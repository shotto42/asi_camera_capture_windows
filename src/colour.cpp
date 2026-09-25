// colour.cpp
//
// Bayer -> RGB conversion and colour preview thumbnails (see colour.h).

#include "colour.h"

#include <ASICamera2.h>   // ASI_BAYER_* enum values only

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cstring>
#include <utility>

int rawMaxValue(int bpp) { return (bpp == 1) ? 255 : 65528; }

const char* bayerPatternName(int asiPattern)
{
    switch (asiPattern)
    {
    case ASI_BAYER_RG: return "RGGB";
    case ASI_BAYER_BG: return "BGGR";
    case ASI_BAYER_GR: return "GRBG";
    case ASI_BAYER_GB: return "GBRG";
    default:           return "RGGB";
    }
}

int bayerToBgrCode(int asiPattern)
{
    // The two naming schemes do NOT read the mosaic the same way, and the
    // cross-over is not the naive same-name match (that is how R and B end up
    // swapped in a picture that still looks like a picture). Verified on
    // hardware-shaped data by `./camera_app --colourtest`, which paints each of
    // the four datasheet arrangements and checks which OpenCV code recovers the
    // painted R and B sites:
    //   RGGB (R at 0,0)  -> COLOR_BayerBG2BGR     BGGR (R at 1,0) -> COLOR_BayerRG2BGR
    //   GRBG (R at 0,1)  -> COLOR_BayerGB2BGR     GBRG (R at 1,1) -> COLOR_BayerGR2BGR
    // i.e. OpenCV's name is the datasheet name read down the first COLUMN, not
    // across the rows. This table must agree with bayerPhase() below (the
    // thumbnail path), which locates R and B in the datasheet arrangement.
    switch (asiPattern)
    {
    case ASI_BAYER_RG: return cv::COLOR_BayerBG2BGR;   // RGGB
    case ASI_BAYER_BG: return cv::COLOR_BayerRG2BGR;   // BGGR
    case ASI_BAYER_GR: return cv::COLOR_BayerGB2BGR;   // GRBG
    case ASI_BAYER_GB: return cv::COLOR_BayerGR2BGR;   // GBRG
    default:           return cv::COLOR_BayerBG2BGR;
    }
}

namespace
{
// Which position of the 2x2 block (a=(0,0), b=(0,1), c=(1,0), d=(1,1)) carries
// R, and which pair carries the two greens. Derived from the same pattern names
// bayerToBgrCode uses:
//   RGGB: R=a G=(b+c)/2 B=d      BGGR: R=d G=(b+c)/2 B=a
//   GRBG: R=b G=(a+d)/2 B=c      GBRG: R=c G=(a+d)/2 B=b
enum GreenPair { GREEN_BC, GREEN_AD };
struct BayerPhase { int rIdx; int bIdx; GreenPair green; };

BayerPhase bayerPhase(int asiPattern)
{
    switch (asiPattern)
    {
    case ASI_BAYER_BG: return {3, 0, GREEN_BC};
    case ASI_BAYER_GR: return {1, 2, GREEN_AD};
    case ASI_BAYER_GB: return {2, 1, GREEN_AD};
    case ASI_BAYER_RG:
    default:           return {0, 3, GREEN_BC};
    }
}

// One 2x2 block -> one RGB pixel. `v` holds the block's four samples in
// row-major order, already reduced to 8 bits; `out` is written R,G,B — the
// BYTE ORDER the display expects (QImage Format_RGB888 reads byte 0 as R, and
// paintClipRed paints its red flag at byte 0). The FILE paths stay BGR
// (demosaicBayerToBgr / the H.264 appsrc); the .ser colour path demosaices
// straight to interleaved RGB (demosaicBayerToRgb); only the preview
// buffer uses this order.
inline void quadToRgb(const BayerPhase& ph, const unsigned int v[4], unsigned char* out)
{
    const unsigned int g = (ph.green == GREEN_BC) ? (v[1] + v[2] + 1) / 2
                                                  : (v[0] + v[3] + 1) / 2;
    out[0] = (unsigned char)v[ph.rIdx];
    out[1] = (unsigned char)g;
    out[2] = (unsigned char)v[ph.bIdx];
}

// Nearest-neighbour mapping of the source clip mask to the thumbnail grid.
// Each output pixel maps to one source mask pixel. Precomputing X avoids two
// integer divisions and nested loops for every displayed pixel.
void expandClipMask(const unsigned char* mask, int sw, int sh,
                    unsigned char* dst, int dispW, int dispH, bool& anyClip)
{
    if (sw == dispW && sh == dispH)
    {
        const size_t n = (size_t)dispW * dispH;
        std::memcpy(dst, mask, n);
        for (size_t i = 0; i < n; ++i) anyClip = anyClip || (dst[i] != 0);
        return;
    }
    std::vector<int> srcX(dispW);
    for (int x = 0; x < dispW; ++x)
        srcX[x] = std::min(sw - 1, (int)((long long)x * sw / dispW));
    for (int y = 0; y < dispH; ++y)
    {
        const int sy = std::min(sh - 1, (int)((long long)y * sh / dispH));
        const unsigned char* row = mask + (size_t)sy * sw;
        unsigned char* out = dst + (size_t)y * dispW;
        for (int x = 0; x < dispW; ++x)
        {
            const unsigned char c = row[srcX[x]];
            out[x] = c;
            anyClip |= c != 0;
        }
    }
}
} // namespace

int bayerToRgbCode(int asiPattern)
{
    // The verified pattern table of bayerToBgrCode(), emitting RGB instead of
    // BGR (SER ColorID 100 wants R,G,B per pixel — see ser_writer.h).
    switch (asiPattern)
    {
    case ASI_BAYER_RG: return cv::COLOR_BayerBG2RGB;   // RGGB
    case ASI_BAYER_BG: return cv::COLOR_BayerRG2RGB;   // BGGR
    case ASI_BAYER_GR: return cv::COLOR_BayerGB2RGB;   // GRBG
    case ASI_BAYER_GB: return cv::COLOR_BayerGR2RGB;   // GBRG
    default:           return cv::COLOR_BayerBG2RGB;
    }
}

bool demosaicBayerToRgb(const unsigned char* src, int w, int h, int bpp, int asiPattern,
                        unsigned char* dstRgb)
{
    if (!src || !dstRgb || w <= 0 || h <= 0) return false;
    if (bpp == 1)
    {
        cv::Mat in(h, w, CV_8UC1, const_cast<unsigned char*>(src));
        cv::Mat out(h, w, CV_8UC3, dstRgb);
        cv::demosaicing(in, out, bayerToRgbCode(asiPattern));
        return true;
    }
    if (bpp == 2)
    {
        cv::Mat in(h, w, CV_16UC1, const_cast<unsigned char*>(src));
        cv::Mat out(h, w, CV_16UC3, dstRgb);
        cv::demosaicing(in, out, bayerToRgbCode(asiPattern));
        return true;
    }
    return false;
}

bool demosaicBayerToBgr(const unsigned char* src, int w, int h, int bpp, int asiPattern,
                        unsigned char* dstBgr)
{
    if (!src || !dstBgr || w <= 0 || h <= 0) return false;
    if (bpp == 1)
    {
        cv::Mat in(h, w, CV_8UC1, const_cast<unsigned char*>(src));
        cv::Mat out(h, w, CV_8UC3, dstBgr);
        cv::demosaicing(in, out, bayerToBgrCode(asiPattern));
        return true;
    }
    if (bpp == 2)
    {
        cv::Mat in(h, w, CV_16UC1, const_cast<unsigned char*>(src));
        cv::Mat out(h, w, CV_16UC3, dstBgr);
        cv::demosaicing(in, out, bayerToBgrCode(asiPattern));
        return true;
    }
    return false;
}

void bgrTo8(const unsigned char* src, int w, int h, int bpp, unsigned char* dst8)
{
    const size_t n = (size_t)w * h * 3;
    if (bpp == 1) { std::memcpy(dst8, src, n); return; }
    const uint16_t* p = (const uint16_t*)src;
    for (size_t i = 0; i < n; ++i) dst8[i] = (unsigned char)(p[i] >> 8);
}

bool demosaicBayerToBgr8(const unsigned char* src, int w, int h, int bpp, int asiPattern,
                         unsigned char* dstBgr8, ColourScratch& scratch)
{
    if (bpp == 1) return demosaicBayerToBgr(src, w, h, 1, asiPattern, dstBgr8);
    if (bpp != 2) return false;

    const size_t bytes = (size_t)w * h * 3 * 2;
    scratch.rgb16.resize(bytes);
    if (!demosaicBayerToBgr(src, w, h, 2, asiPattern, scratch.rgb16.data())) return false;
    bgrTo8(scratch.rgb16.data(), w, h, 2, dstBgr8);
    return true;
}

// ---------------------------------------------------------------------------
// Colour thumbnail
// ---------------------------------------------------------------------------

void colourDisplayFrame(const unsigned char* src, int w, int h, int bpp, bool bayer,
                        int asiPattern, unsigned char* rgb, unsigned char* clipM,
                        int dispW, int dispH, bool& anyClip, ColourScratch& scratch)
{
    const size_t nOut = (size_t)dispW * dispH;
    std::memset(clipM, 0, nOut);
    anyClip = false;
    if (!src || dispW <= 0 || dispH <= 0) return;
    const int maxv = rawMaxValue(bpp);
    const bool boxable = ((w & 1) == 0 && (h & 1) == 0 &&
                          w >= 2 * dispW && h >= 2 * dispH);

    unsigned char* srcRgb = nullptr;    // 8-bit RGB image the thumbnail scales from
    const unsigned char* srcMask = nullptr;
    int sw = 0, sh = 0;

    if (bayer && boxable)
    {
        // One pass over the mosaic: every 2x2 block IS an RGB pixel (R, G, G, B).
        const int hw = w / 2, hh = h / 2;
        scratch.rgbHalf.resize((size_t)hw * hh * 3);
        scratch.maskHalf.assign((size_t)hw * hh, 0);
        const BayerPhase ph = bayerPhase(asiPattern);
        unsigned char* out = scratch.rgbHalf.data();
        unsigned char* mask = scratch.maskHalf.data();
        if (bpp == 1)
        {
            const uint8_t* p = (const uint8_t*)src;
            for (int y = 0; y < h; y += 2, out += (size_t)hw * 3, mask += hw)
            {
                const uint8_t* r0 = p + (size_t)y * w;
                const uint8_t* r1 = r0 + w;
                for (int x = 0; x < w; x += 2)
                {
                    const unsigned int v[4] = { r0[x], r0[x + 1], r1[x], r1[x + 1] };
                    quadToRgb(ph, v, out + (size_t)(x / 2) * 3);
                    if (v[0] >= (unsigned)maxv || v[1] >= (unsigned)maxv ||
                        v[2] >= (unsigned)maxv || v[3] >= (unsigned)maxv)
                        mask[x / 2] = 1;
                }
            }
        }
        else
        {
            const uint16_t* p = (const uint16_t*)src;
            for (int y = 0; y < h; y += 2, out += (size_t)hw * 3, mask += hw)
            {
                const uint16_t* r0 = p + (size_t)y * w;
                const uint16_t* r1 = r0 + w;
                for (int x = 0; x < w; x += 2)
                {
                    const unsigned int v[4] = { (unsigned)(r0[x] >> 8), (unsigned)(r0[x + 1] >> 8),
                                                (unsigned)(r1[x] >> 8), (unsigned)(r1[x + 1] >> 8) };
                    quadToRgb(ph, v, out + (size_t)(x / 2) * 3);
                    if (r0[x] >= (uint16_t)maxv || r0[x + 1] >= (uint16_t)maxv ||
                        r1[x] >= (uint16_t)maxv || r1[x + 1] >= (uint16_t)maxv)
                        mask[x / 2] = 1;
                }
            }
        }
        srcRgb = scratch.rgbHalf.data();
        srcMask = scratch.maskHalf.data();
        sw = hw; sh = hh;
    }
    else
    {
        // 8-bit RGB at source resolution, then one resize to the thumbnail.
        scratch.rgbFull8.resize((size_t)w * h * 3);
        if (bayer)
        {
            if (!demosaicBayerToBgr8(src, w, h, bpp, asiPattern, scratch.rgbFull8.data(), scratch))
                return;
            // OpenCV wrote B,G,R; the display buffer is R,G,B (see quadToRgb).
            // One swap pass over the interleaved triples (the fallback path only
            // runs on frames smaller than 2x the thumbnail, and cv::demosaicing
            // already dominated the cost).
            unsigned char* q = scratch.rgbFull8.data();
            const size_t npx = (size_t)w * h;
            for (size_t i = 0; i < npx; ++i)
                std::swap(q[3 * i], q[3 * i + 2]);
        }
        else if (bpp == 1)
        {
            const uint8_t* p = (const uint8_t*)src;
            for (size_t i = 0, n = (size_t)w * h; i < n; ++i)
            {
                const unsigned char v = p[i];
                unsigned char* o = scratch.rgbFull8.data() + i * 3;
                o[0] = v; o[1] = v; o[2] = v;
            }
        }
        else
        {
            const uint16_t* p = (const uint16_t*)src;
            for (size_t i = 0, n = (size_t)w * h; i < n; ++i)
            {
                const unsigned char v = (unsigned char)(p[i] >> 8);
                unsigned char* o = scratch.rgbFull8.data() + i * 3;
                o[0] = v; o[1] = v; o[2] = v;
            }
        }
        // Clip mask straight from the RAW samples (exact per channel).
        scratch.maskFull.assign((size_t)w * h, 0);
        if (bpp == 1)
        {
            const uint8_t* p = (const uint8_t*)src;
            for (size_t i = 0, n = (size_t)w * h; i < n; ++i)
                if (p[i] >= 255) scratch.maskFull[i] = 1;
        }
        else
        {
            const uint16_t* p = (const uint16_t*)src;
            for (size_t i = 0, n = (size_t)w * h; i < n; ++i)
                if (p[i] >= (uint16_t)65528) scratch.maskFull[i] = 1;
        }
        srcRgb = scratch.rgbFull8.data();
        srcMask = scratch.maskFull.data();
        sw = w; sh = h;
    }

    // Scale the 8-bit RGB image to the thumbnail.
    if (sw == dispW && sh == dispH)
        std::memcpy(rgb, srcRgb, nOut * 3);
    else
    {
        cv::Mat in(sh, sw, CV_8UC3, srcRgb);
        cv::Mat out(dispH, dispW, CV_8UC3, rgb);
        const bool bigShrink = (sw >= 2 * dispW && sh >= 2 * dispH);
        cv::resize(in, out, cv::Size(dispW, dispH), 0, 0, bigShrink ? cv::INTER_AREA : cv::INTER_LINEAR);
    }
    expandClipMask(srcMask, sw, sh, clipM, dispW, dispH, anyClip);
}
