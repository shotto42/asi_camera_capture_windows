// colour_test.cpp — headless colour pipeline self-test (`--colourtest`).
//
// No camera needed. It pins down the things the colour path can get wrong
// silently — i.e. with pictures that LOOK fine — and that a hardware run would
// not catch cheaply:
//
//   1. the Bayer -> RGB mapping (a swapped R/B is invisible on a grey scene and
//      turns every red planet blue),
//   2. the SER frame layout: ColorID 100 frames must be PER-PIXEL INTERLEAVED
//      R,G,B on disk (the layout Siril and Ser-Player decode; full planes
//      RRR...GGG...BBB — what an earlier revision wrongly wrote — plays back
//      as a 3x3 mosaic of the scene in both players, and byte-swap/planar
//      mistakes are otherwise silent because all layouts have equal size),
//   3. that a MONO body still writes exactly what it wrote before (ColorID 0,
//      one plane, byte-for-byte),
//   4. the colour thumbnail + clip mask, including the 16-bit -> 8-bit scaling
//      that once made the mono preview pure white, and the RGB BYTE ORDER of
//      the preview buffer (a BGR-ordered thumbnail is invisible in the
//      imwrite/H.264 file paths — they consume BGR — and in the .ser path it
//      would swap red and blue in every recorded frame, but shows live as an
//      R/B-swapped preview), pinned for BOTH thumbnail paths.
//
// The mapping test builds, for each of the four patterns, a mosaic whose R
// sites hold 200, G sites 100 and B sites 50 (every site of one colour carries
// the same value), so the DEMOSAICED channel means must come out 200/100/50.
// Any wrong placement of R or B — swap, transpose, or the wrong OpenCV naming
// convention — moves those means.

#include "colour.h"
#include "ser_writer.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
// ---- helpers ---------------------------------------------------------------
// ASI_BAYER_PATTERN values, spelled out locally so the test does not need the
// SDK header (these are the four values --bayer accepts).
const int kPatRG = 0;   // RGGB
const int kPatBG = 1;   // BGGR
const int kPatGR = 2;   // GRBG
const int kPatGB = 3;   // GBRG

struct Site { int x, y; };

Site rSiteOf(int pat)
{
    switch (pat)
    {
    case kPatBG: return {1, 1};
    case kPatGR: return {1, 0};
    case kPatGB: return {0, 1};
    default:     return {0, 0};       // kPatRG
    }
}
Site bSiteOf(int pat)
{
    switch (pat)
    {
    case kPatBG: return {0, 0};
    case kPatGR: return {0, 1};
    case kPatGB: return {1, 0};
    default:     return {1, 1};
    }
}

bool approx(double a, double b, double tol) { return a > b - tol && a < b + tol; }

// Mean of one channel of an interleaved 8-bit BGR image.
double meanChan(const unsigned char* bgr, int w, int h, int chan)
{
    const size_t n = (size_t)w * h;
    double s = 0;
    for (size_t i = 0; i < n; ++i) s += bgr[i * 3 + chan];
    return s / (double)n;
}

void report(const char* what, bool pass)
{
    std::printf("[colour] %s %s\n", pass ? "OK  " : "FAIL", what);
}

// Fill an 8-bit mosaic for `pat` with R sites = rv, B sites = bv, G sites = gv.
void fillMosaic8(std::vector<unsigned char>& m, int w, int h, int pat,
                 unsigned char rv, unsigned char gv, unsigned char bv)
{
    const Site r = rSiteOf(pat), b = bSiteOf(pat);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            const int sx = x & 1, sy = y & 1;
            unsigned char v = gv;
            if (sx == r.x && sy == r.y) v = rv;
            else if (sx == b.x && sy == b.y) v = bv;
            m[(size_t)y * w + x] = v;
        }
}

// ---- 1. Bayer -> RGB mapping ----------------------------------------------
bool testMapping8()
{
    const int W = 16, H = 16;
    const int pats[4] = {kPatRG, kPatBG, kPatGR, kPatGB};
    bool ok = true;
    for (int pat : pats)
    {
        std::vector<unsigned char> mos((size_t)W * H);
        fillMosaic8(mos, W, H, pat, 200, 100, 50);
        std::vector<unsigned char> bgr((size_t)W * H * 3, 0);
        if (!demosaicBayerToBgr(mos.data(), W, H, 1, pat, bgr.data()))
        {
            report("8-bit demosaic call", false);
            ok = false;
            continue;
        }
        // OpenCV channel order is B,G,R. The 1-px border is smeared by the
        // interpolation, hence the slack on an otherwise exact 200/100/50.
        const double mB = meanChan(bgr.data(), W, H, 0);
        const double mG = meanChan(bgr.data(), W, H, 1);
        const double mR = meanChan(bgr.data(), W, H, 2);
        const bool pass = approx(mR, 200, 8) && approx(mG, 100, 8) && approx(mB, 50, 8);
        std::printf("[colour] %s %-4s demosaic: R=%.1f G=%.1f B=%.1f (want 200/100/50)\n",
                    pass ? "OK  " : "FAIL", bayerPatternName(pat), mR, mG, mB);
        ok = ok && pass;
    }
    return ok;
}

// The deep (16-bit) readout must map the same way AND keep its full depth: the
// test mosaic uses values far apart so a >> 8 anywhere shows up.
bool testMapping16()
{
    const int W = 16, H = 16;
    bool ok = true;
    std::vector<unsigned char> mos((size_t)W * H * 2);
    uint16_t* p = (uint16_t*)mos.data();
    const Site r = rSiteOf(kPatRG), b = bSiteOf(kPatRG);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
        {
            const int sx = x & 1, sy = y & 1;
            uint16_t v = 25600;                      // G
            if (sx == r.x && sy == r.y) v = 51200;   // R
            else if (sx == b.x && sy == b.y) v = 12800;   // B
            p[(size_t)y * W + x] = v;
        }
    std::vector<unsigned char> out((size_t)W * H * 3 * 2, 0);
    if (!demosaicBayerToBgr(mos.data(), W, H, 2, kPatRG, out.data()))
    {
        report("16-bit demosaic call", false);
        return false;
    }
    const uint16_t* o = (const uint16_t*)out.data();
    const size_t n = (size_t)W * H;
    double smB = 0, smG = 0, smR = 0;
    for (size_t i = 0; i < n; ++i) { smB += o[i * 3]; smG += o[i * 3 + 1]; smR += o[i * 3 + 2]; }
    const double mB = smB / n, mG = smG / n, mR = smR / n;
    const bool pass = approx(mR, 51200, 2000) && approx(mG, 25600, 2000) && approx(mB, 12800, 2000);
    std::printf("[colour] %s 16-bit demosaic keeps depth: R=%.0f G=%.0f B=%.0f (want 51200/25600/12800)\n",
                pass ? "OK  " : "FAIL", mR, mG, mB);
    ok = ok && pass;

    // ... and the 8-bit variant of the SAME frame reduces it (51200 >> 8 = 200)
    // instead of clipping everything to 255 (the old mono preview bug).
    ColourScratch scratch;
    std::vector<unsigned char> bgr8((size_t)W * H * 3, 0);
    if (!demosaicBayerToBgr8(mos.data(), W, H, 2, kPatRG, bgr8.data(), scratch))
    {
        report("16 -> 8-bit demosaic call", false);
        return false;
    }
    const double r8 = meanChan(bgr8.data(), W, H, 2), g8 = meanChan(bgr8.data(), W, H, 1),
                 b8 = meanChan(bgr8.data(), W, H, 0);
    const bool pass8 = approx(r8, 200, 12) && approx(g8, 100, 12) && approx(b8, 50, 12);
    std::printf("[colour] %s 16 -> 8-bit reduction: R=%.1f G=%.1f B=%.1f (want 200/100/50, NOT 255)\n",
                pass8 ? "OK  " : "FAIL", r8, g8, b8);
    return ok && pass8;
}

// ---- 2. SER: colour planes and mono regression -----------------------------
std::vector<unsigned char> readFile(const std::string& path)
{
    std::vector<unsigned char> out;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return out;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize((size_t)n);
    if (n > 0 && std::fread(out.data(), 1, (size_t)n, f) != (size_t)n) out.clear();
    std::fclose(f);
    return out;
}

uint32_t le32(const unsigned char* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// 8-bit RGB .ser: 2 frames of per-pixel varying RGB triples. Pins the on-disk
// frame layout as PER-PIXEL INTERLEAVED RGBRGB — the layout Siril and
// Ser-Player de-interleave (see ser_writer.h). A planar layout (all R, then
// all G, then all B — what an earlier revision wrongly wrote, and what makes
// both players show a 3x3 mosaic) fails these checks: under it the frame's
// second byte would be R again, not G.
bool testSerRgb8()
{
    const int W = 4, H = 3, N = W * H;
    const std::string path = "samples/colourtest_rgb8.ser";
    {
        SerWriter w;
        if (!w.open(path, W, H, 8, "colourtest", kSerColorRgb))
        {
            report("open 8-bit RGB .ser", false);
            return false;
        }
        const bool meta = (w.colorId() == kSerColorRgb && w.planes() == 3);
        report("ColorID 100 implies 3 channels", meta);
        bool ok = meta;
        for (int f = 0; f < 2; ++f)
        {
            std::vector<unsigned char> rgb((size_t)N * 3);
            for (int i = 0; i < N; ++i)   // per-pixel varying: no two channels
            {                             // interchangeable
                rgb[i * 3 + 0] = (unsigned char)(10 + f + i);
                rgb[i * 3 + 1] = (unsigned char)(20 + f + i);
                rgb[i * 3 + 2] = (unsigned char)(30 + f + i);
            }
            if (!w.pushRgb8(rgb.data())) { report("pushRgb8", false); ok = false; break; }
        }
        if (ok && w.frameCount() != 2) { report("frame count", false); ok = false; }
    }   // scope end closes + patches the count
    bool ok = validateSerFile(path, W, H, 8, kSerColorRgb);
    auto bytes = readFile(path);
    if (bytes.size() != 178 + 2 * (size_t)N * 3) { report("8-bit RGB file size", false); ok = false; }
    else
    {
        // Every pixel triple in file order: (R,G,B) per pixel, R first.
        bool interleaveOk = true;
        for (int f = 0; f < 2 && interleaveOk; ++f)
        {
            const size_t base = 178 + (size_t)f * N * 3;
            for (int i = 0; i < N; ++i)
            {
                if (bytes[base + i * 3 + 0] != (unsigned char)(10 + f + i) ||
                    bytes[base + i * 3 + 1] != (unsigned char)(20 + f + i) ||
                    bytes[base + i * 3 + 2] != (unsigned char)(30 + f + i)) { interleaveOk = false; break; }
            }
        }
        report("interleaved RGBRGB order in file bytes", interleaveOk);
        ok = ok && interleaveOk;
        // Header: ColorID at 18, width/height/depth, endianness field 0.
        const bool hdr = le32(&bytes[18]) == 100 && le32(&bytes[22]) == 0 &&
                         le32(&bytes[26]) == (uint32_t)W && le32(&bytes[30]) == (uint32_t)H &&
                         le32(&bytes[34]) == 8 && le32(&bytes[38]) == 2;
        report("RGB header fields (ColorID=100, LE=0, count=2)", hdr);
        ok = ok && hdr;
    }
    return ok;
}

// 14-bit RGB .ser: interleaved 16-bit channel samples, values left-justified.
bool testSerRgb16()
{
    const int W = 4, H = 3, N = W * H;
    const std::string path = "samples/colourtest_rgb14.ser";
    const uint16_t rv = 51200, gv = 25600, bv = 12800;
    {
        SerWriter w;
        if (!w.open(path, W, H, 14, "colourtest", kSerColorRgb))
        {
            report("open 14-bit RGB .ser", false);
            return false;
        }
        std::vector<uint16_t> rgb((size_t)N * 3);
        for (int i = 0; i < 2; ++i)
        {
            for (int k = 0; k < N; ++k) { rgb[k * 3 + 0] = rv; rgb[k * 3 + 1] = gv; rgb[k * 3 + 2] = bv; }
            if (!w.pushRgb(rgb.data())) { report("pushRgb", false); return false; }
        }
    }
    bool ok = validateSerFile(path, W, H, 16, kSerColorRgb);   // depth declared 16
    auto bytes = readFile(path);
    const size_t frameBytes = (size_t)N * 3 * 2;
    if (bytes.size() != 178 + 2 * frameBytes) { report("14-bit RGB file size", false); ok = false; }
    else
    {
        const uint16_t* px = (const uint16_t*)&bytes[178];
        const bool vals = px[0] == rv && px[1] == gv && px[2] == bv &&
                          px[N * 3] == rv;   // frame 1 starts back at R
        report("14-bit RGB interleaved samples (as-is, left-justified)", vals);
        ok = ok && vals;
    }
    return ok;
}

// End-to-end: a raw Bayer mosaic demosaiced and recorded as an RGB .ser must
// land on disk with R in byte 0 of every pixel triple — the .ser path
// demosaices straight to interleaved RGB (demosaicBayerToRgb), so a wrong
// OpenCV code direction (BGR instead of RGB) or a planar regression shows up
// here, at the one point where the colour mapping and the frame layout meet.
bool testSerRgbEndToEnd()
{
    const int W = 16, H = 16, N = W * H;
    const int pats[4] = {kPatRG, kPatBG, kPatGR, kPatGB};
    const std::string path = "samples/colourtest_rgbE2E.ser";
    bool ok = true;
    for (int pat : pats)
    {
        std::vector<unsigned char> mos((size_t)N);
        fillMosaic8(mos, W, H, pat, 200, 100, 50);
        std::vector<unsigned char> rgb((size_t)N * 3, 0);
        bool frameOk = false;
        {
            SerWriter w;
            if (!w.open(path, W, H, 8, "colourtest", kSerColorRgb))
            {
                report("open E2E RGB .ser", false);
                return false;
            }
            frameOk = demosaicBayerToRgb(mos.data(), W, H, 1, pat, rgb.data()) &&
                      w.pushRgb8(rgb.data());
        }
        auto bytes = readFile(path);
        // Interior pixels only (the 1-px border is smeared by interpolation):
        // byte 0 of every pixel triple = the R sites' value, byte 2 = B's.
        int bad = 0;
        for (int y = 2; y < H - 2 && !bad; ++y)
            for (int x = 2; x < W - 2; ++x)
            {
                const size_t off = 178 + ((size_t)y * W + x) * 3;
                if (bytes[off + 0] != 200 || bytes[off + 1] != 100 || bytes[off + 2] != 50) { bad = 1; break; }
            }
        const bool pass = frameOk && !bad && bytes.size() == 178 + (size_t)N * 3;
        std::printf("[colour] %s .ser end-to-end, pattern %s: file pixel = (R,G,B) = (200,100,50)\n",
                    pass ? "OK  " : "FAIL", bayerPatternName(pat));
        ok = ok && pass;
    }
    return ok;
}

// A MONO body must still produce exactly what it produced before colour
// support existed: ColorID 0, one plane, the readout verbatim.
bool testSerMonoUnchanged()
{
    const int W = 4, H = 3, N = W * H;
    const std::string path = "samples/colourtest_mono.ser";
    {
        SerWriter w;
        if (!w.open(path, W, H, 14, "ZWO ASI178MM")) { report("open mono .ser", false); return false; }
        if (w.colorId() != kSerColorMono || w.planes() != 1)
        {
            report("mono ColorID 0 implies 1 plane", false);
            return false;
        }
        std::vector<uint16_t> fr(N);
        for (int i = 0; i < N; ++i) fr[i] = (uint16_t)(i * 4);       // 14-bit readout style
        if (!w.push(fr.data())) { report("mono push", false); return false; }
    }
    bool ok = validateSerFile(path, W, H, 16, kSerColorMono);
    auto bytes = readFile(path);
    if (bytes.size() != 178 + (size_t)N * 2) { report("mono file size (1 plane)", false); ok = false; }
    else
    {
        const uint16_t* px = (const uint16_t*)&bytes[178];
        bool same = true;
        for (int i = 0; i < N; ++i) if (px[i] != (uint16_t)(i * 4)) same = false;
        report("mono data verbatim (no plane, no shift)", same);
        ok = ok && same && le32(&bytes[18]) == 0;
    }
    // A mono writer must reject the colour entry points rather than silently
    // write three planes into a one-plane file (and vice versa).
    {
        SerWriter w;
        w.open(path, W, H, 8, "mono");
        std::vector<unsigned char> rgb((size_t)N * 3, 7);
        const bool rejected = !w.pushRgb8(rgb.data());
        report("pushRgb8 on a mono file is rejected", rejected);
        ok = ok && rejected;
    }
    return ok;
}

// ---- 3. colour thumbnail ---------------------------------------------------
bool testThumbnail()
{
    const int W = 8, H = 8;
    bool ok = true;
    ColourScratch scratch;

    // 8-bit: uniform RGB-ish mosaic -> every thumbnail pixel must carry the
    // same colour (the quad demosaic averages each 2x2 block: R, (G+G)/2, B) —
    // AND the thumbnail buffer must be R,G,B BYTE ORDER: the GUI shows it as a
    // QImage Format_RGB888 (byte 0 = R) and paintClipRed paints its red flag at
    // byte 0. A BGR-ordered thumbnail passes every file-side test while showing
    // an R/B-swapped PREVIEW, so it is pinned here explicitly.
    std::vector<unsigned char> mos((size_t)W * H);
    fillMosaic8(mos, W, H, kPatRG, 200, 100, 50);
    std::vector<unsigned char> rgb((size_t)4 * 4 * 3, 0), mask(4 * 4, 0);
    bool anyClip = false;
    colourDisplayFrame(mos.data(), W, H, 1, true, kPatRG, rgb.data(), mask.data(), 4, 4,
                       anyClip, scratch);
    const bool colours = rgb[0] == 200 && rgb[1] == 100 && rgb[2] == 50;
    std::printf("[colour] %s thumbnail quad-demosaic colour (RGB byte order): R=%d G=%d B=%d (want 200/100/50)\n",
                colours ? "OK  " : "FAIL", rgb[0], rgb[1], rgb[2]);
    ok = ok && colours && !anyClip;

    // Clip mask: saturate ONE 2x2 block; the thumbnail (4x4 = one pixel per
    // block here) must flag exactly that block and nothing else.
    fillMosaic8(mos, W, H, kPatRG, 200, 100, 50);
    for (int y = 2; y < 4; ++y)
        for (int x = 2; x < 4; ++x) mos[(size_t)y * W + x] = 255;
    std::fill(rgb.begin(), rgb.end(), 0);
    std::fill(mask.begin(), mask.end(), 0);
    colourDisplayFrame(mos.data(), W, H, 1, true, kPatRG, rgb.data(), mask.data(), 4, 4,
                       anyClip, scratch);
    // A 2x2 mosaic block is ONE thumbnail pixel in the quad path: mosaic
    // x/y 2..3 is thumbnail (x=1, y=1).
    const int clipIdx = 1 * 4 + 1;
    int flagged = 0;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            if (mask[y * 4 + x]) ++flagged;
    const bool maskOk = anyClip && mask[clipIdx] && flagged == 1;
    std::printf("[colour] %s clip flags exactly the saturated block (%d of 16 flagged)\n",
                maskOk ? "OK  " : "FAIL", flagged);
    ok = ok && maskOk;

    // A clipped block is painted pure red in the preview buffer — the same call
    // the preview builder makes (colourDisplayFrame only reports the mask; the
    // paint is a separate step, so it is checked through the shared helper).
    if (anyClip) paintClipRed(rgb.data(), mask.data(), mask.size());
    const bool red = rgb[clipIdx * 3] == 255 && rgb[clipIdx * 3 + 1] == 0 &&
                     rgb[clipIdx * 3 + 2] == 0;
    report("clipped block painted red", red);
    ok = ok && red;

    // Byte order pinned for BOTH thumbnail paths and ALL FOUR Bayer patterns.
    // The same uniform mosaic (R sites 200, G 100, B 50): a quad-block or a
    // full-demosaic pixel must read rgb = [200,100,50]. Any wrong mosaic
    // placement OR a BGR-ordered preview buffer moves 200 into rgb[2], which
    // the file-side tests cannot see (they consume BGR) but the preview shows
    // as exchanged red/blue. 8x8 -> 4x4 takes the quad path (w >= 2*dispW);
    // 8x8 -> 6x6 is not boxable and takes the full-demosaic fallback, where
    // OpenCV's BGR output must be swapped to display order.
    const int pats4[4] = {kPatRG, kPatBG, kPatGR, kPatGB};
    for (int pat : pats4)
    {
        fillMosaic8(mos, W, H, pat, 200, 100, 50);
        bool pathOk = true;
        struct { int dw, dh; const char* name; } paths[] = {
            {4, 4, "quad"}, {6, 6, "fallback"}};
        for (auto& p : paths)
        {
            std::vector<unsigned char> rgbT((size_t)p.dw * p.dh * 3, 0), maskT((size_t)p.dw * p.dh, 0);
            bool clipT = true;
            colourDisplayFrame(mos.data(), W, H, 1, true, pat, rgbT.data(), maskT.data(),
                               p.dw, p.dh, clipT, scratch);
            for (int i = 0; i < p.dw * p.dh && pathOk; ++i)
                pathOk = rgbT[i * 3] == 200 && rgbT[i * 3 + 1] == 100 && rgbT[i * 3 + 2] == 50;
            pathOk = pathOk && !clipT;
            if (!pathOk)
                std::printf("        (%s path, pattern %d: rgb[0]=%d rgb[2]=%d)\n",
                            p.name, pat, rgbT[0], rgbT[2]);
        }
        std::printf("[colour] %s thumbnail RGB byte order, pattern %d (both paths)\n",
                    pathOk ? "OK  " : "FAIL", pat);
        ok = ok && pathOk;
    }

    // 16-bit source: mid-grey 32896 (=128<<8) must display as 128, and a
    // 65528 block must be flagged (the 14-bit saturation value).
    std::vector<unsigned char> mos16((size_t)W * H * 2, 0);
    uint16_t* p16 = (uint16_t*)mos16.data();
    for (size_t i = 0; i < (size_t)W * H; ++i) p16[i] = 32896;
    for (int y = 4; y < 6; ++y)
        for (int x = 4; x < 6; ++x) p16[(size_t)y * W + x] = 65528;
    std::fill(rgb.begin(), rgb.end(), 0);
    std::fill(mask.begin(), mask.end(), 0);
    colourDisplayFrame(mos16.data(), W, H, 2, true, kPatRG, rgb.data(), mask.data(), 4, 4,
                       anyClip, scratch);
    const bool grey = rgb[0] == 128 && rgb[1] == 128 && rgb[2] == 128;
    std::printf("[colour] %s 16-bit thumbnail scales (>>8): %d %d %d (want 128, not 255)\n",
                grey ? "OK  " : "FAIL", rgb[0], rgb[1], rgb[2]);
    // The 14-bit saturation value (65528) must land on the RIGHT thumbnail
    // pixel: mosaic x/y 4..5 is quad-block (2,2).
    int flagged16 = 0;
    for (int i = 0; i < 16; ++i) if (mask[i]) ++flagged16;
    const bool clip16 = anyClip && mask[2 * 4 + 2] && flagged16 == 1;
    std::printf("[colour] %s 14-bit clip flagged on the right block (%d of 16)\n",
                clip16 ? "OK  " : "FAIL", flagged16);
    ok = ok && grey && clip16;

    // A mono body through the same call: grey triple, clip at 65528 flagged.
    std::fill(rgb.begin(), rgb.end(), 0);
    std::fill(mask.begin(), mask.end(), 0);
    colourDisplayFrame(mos16.data(), W, H, 2, false, kPatRG, rgb.data(), mask.data(), 4, 4,
                       anyClip, scratch);
    const bool monoThumb = rgb[0] == 128 && rgb[1] == 128 && rgb[2] == 128;
    report("mono source via the same call = grey triple", monoThumb);
    int flaggedMono = 0;
    for (int i = 0; i < 16; ++i) if (mask[i]) ++flaggedMono;
    report("mono clip mask stays aligned to the same block", anyClip && flaggedMono == 1);
    ok = ok && monoThumb && anyClip && flaggedMono == 1;
    return ok;
}
} // namespace

bool runColourSelfTest()
{
    bool ok = true;
    ok = testMapping8()   && ok;
    ok = testMapping16()  && ok;
    ok = testSerRgb8()    && ok;
    ok = testSerRgb16()   && ok;
    ok = testSerRgbEndToEnd() && ok;
    ok = testSerMonoUnchanged() && ok;
    ok = testThumbnail()  && ok;
    std::printf("%s\n", ok ? "COLOURTEST PASS" : "COLOURTEST FAIL");
    return ok;
}
