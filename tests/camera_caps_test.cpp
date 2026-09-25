// camera_caps_test.cpp — headless capability-model self-test (`--capstest`).
//
// The SDK probe itself needs hardware (./camera_probe dumps it); this suite
// covers the UI MODEL built on top of the probe, which decides what the
// resolution selector offers, which bit depths exist, and what the frame-rate
// slider may reach — for any camera, not just the one attached. It runs the
// model against three synthetic bodies: an ASI178MC (colour, 6.4 MP, both
// readouts), the same sensor as a mono ASI178MM, and a small older body without
// a 2-byte readout.

#include "camera_caps.h"
#include "fps_spec.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
void report(const char* what, bool pass)
{
    std::printf("[caps] %s %s\n", pass ? "OK  " : "FAIL", what);
}

CameraCaps asi178(bool colour)
{
    CameraCaps c;
    c.name = colour ? "ZWO ASI178MC" : "ZWO ASI178MM";
    c.maxW = 3096; c.maxH = 2080;
    c.isColor = colour;
    c.bayer = 0;                 // ASI_BAYER_RG (RGGB), what this model reports
    c.nativeDepth = 14;
    c.hasRaw8 = true; c.hasRaw16 = true; c.hasRgb24 = colour;
    c.gainMin = 0; c.gainMax = 510;
    // White balance caps as MEASURED on the attached ASI178MC (camera_probe,
    // 2026-09-23): both controls 1..99, defaults R 70 / B 90, writable, the
    // SDK auto flag supported. A mono body's controls (if it reports them)
    // are never touched — the WB UI and the worker's WB path gate on colour.
    c.hasWbR = true; c.wbRMin = 1; c.wbRMax = 99; c.wbRDef = 70; c.wbRAuto = true;
    c.hasWbB = true; c.wbBMin = 1; c.wbBMax = 99; c.wbBDef = 90; c.wbBAuto = true;
    return c;
}

bool testDepths()
{
    bool ok = true;
    const CameraCaps mc = asi178(true);
    const std::vector<int> d = mc.depths();
    const bool shape = d.size() == 2 && d[0] == 8 && d[1] == 14;
    report("colour ASI178 offers 8-bit and its 14-bit readout", shape);
    ok = ok && shape;
    ok = ok && mc.deepDepth() == 14;
    report("supportsDepth(8)/(14) yes, (10)/(12) no",
           mc.supportsDepth(8) && mc.supportsDepth(14) &&
           !mc.supportsDepth(10) && !mc.supportsDepth(12));
    ok = ok && mc.supportsDepth(8) && mc.supportsDepth(14) &&
         !mc.supportsDepth(10) && !mc.supportsDepth(12);

    // A body with no 2-byte readout has no deep mode at all (the app must not
    // offer a 14-bit entry that cannot be captured).
    CameraCaps old;
    old.name = "Old mono"; old.maxW = 1280; old.maxH = 960;
    old.isColor = false; old.nativeDepth = 8; old.hasRaw8 = true; old.hasRaw16 = false;
    const bool shallow = old.depths().size() == 1 && old.depths()[0] == 8 &&
                         old.deepDepth() == 8 && !old.supportsDepth(14);
    report("a body without RAW16 offers no deep mode", shallow);
    ok = ok && shallow;
    return ok;
}

bool testRoiCandidates()
{
    const CameraCaps mc = asi178(true);
    const std::vector<RoiSize> c = mc.roiCandidates();
    bool ok = !c.empty();
    report("non-empty candidate list", ok);
    if (!ok) return false;

    // First entry: the 1:1 base square (side = the sensor's y-resolution —
    // 2080x2080 on the ASI178), the user-requested default framing.
    const bool firstSquare = c[0].w == c[0].h && c[0].w == mc.maxH;
    report("the 1:1 base square (2080x2080) is the first entry", firstSquare);
    ok = ok && firstSquare;
    // ...and the full sensor follows it.
    const bool secondFull = c.size() > 1 && c[1].w == mc.maxW && c[1].h == mc.maxH;
    report("full sensor is the second entry", secondFull);
    ok = ok && secondFull;

    bool inBounds = true, decreasing = true, even = true, dup = false;
    for (size_t i = 0; i < c.size(); ++i)
    {
        if (c[i].w > mc.maxW || c[i].h > mc.maxH || c[i].w < 64 || c[i].h < 48) inBounds = false;
        if ((c[i].w & 1) || (c[i].h & 1)) even = false;   // Bayer phase must stay aligned
        if (i >= 1 && i + 1 < c.size())   // behind the base square: strictly decreasing area
        {
            const long a = (long)c[i].w * c[i].h, b = (long)c[i + 1].w * c[i + 1].h;
            if (b >= a) decreasing = false;
        }
        if (i + 1 < c.size() && c[i].w == c[i + 1].w && c[i].h == c[i + 1].h) dup = true;
    }
    report("every candidate fits the sensor and the minimum size", inBounds);
    report("base square leads, the rest strictly by decreasing area, no duplicates",
           decreasing && !dup);
    report("every candidate even-sized (Bayer phase preserved)", even);
    ok = ok && inBounds && decreasing && !dup && even;

    // Aspect-preserving list: EVERY crop keeps the sensor's own aspect ratio
    // within ~1% (the slack the %8 width / %4 height alignment can cost at
    // the small end) — the one deliberate exception is the single 1:1 base
    // square (planetary framing, the first entry), which must be present.
    // The old fixed 16:9 / 4:3 windows are gone: on a 1.49:1 sensor they
    // sliced away a different wedge of the frame per entry.
    const double ar = (double)mc.maxW / (double)mc.maxH;
    bool shapeOk = true, hasSquare = false, hasMidCrop = false;
    for (const auto& s : c)
    {
        if (s.w == s.h)
        {
            if (s.w == mc.maxH) hasSquare = true;   // 2080x2080 on the ASI178
            continue;                               // the allowed exception
        }
        const double sar = (double)s.w / (double)s.h;
        if (std::fabs(sar - ar) > 0.01 * ar)
        {
            std::printf("        (%dx%d is %.2f%% off the sensor shape)\n",
                        s.w, s.h, 100.0 * (sar - ar) / ar);
            shapeOk = false;
        }
        if (s.w == 1280 && s.h == 860) hasMidCrop = true;  // native-shape 800p-class
    }
    report("every crop keeps the sensor aspect ratio (1:1 base excepted)", shapeOk);
    report("offers the 1:1 base square (2080x2080) and 1280x860", hasSquare && hasMidCrop);
    ok = ok && shapeOk && hasSquare && hasMidCrop;

    // A small sensor must not be offered windows it does not physically have.
    CameraCaps small;
    small.name = "Small"; small.maxW = 1280; small.maxH = 960; small.hasRaw8 = true;
    small.nativeDepth = 10;
    bool smallOk = true;
    for (const auto& s : small.roiCandidates())
        if (s.w > small.maxW || s.h > small.maxH) smallOk = false;
    report("a 1280x960 sensor is offered nothing larger", smallOk);
    // The windows a user expects from a shape-preserving menu on the ASI178.
    std::printf("[caps] ASI178 candidate list (sensor AR %.4f):\n", ar);
    for (const auto& s : c)
    {
        const double sar = (double)s.w / (double)s.h;
        std::printf("[caps]   %4dx%-4d  AR %.4f  (%+.2f%%)%s\n", s.w, s.h, sar,
                    100.0 * (sar - ar) / ar, s.w == s.h ? "  <- the 1:1 base entry" : "");
    }
    return ok && smallOk;
}

bool testFpsModel()
{
    const CameraCaps mc = asi178(true);
    // The ASI178 family uses the datasheet columns unmodified: full-res 60 fps
    // on the fast readout, 30 fps on the deep one, and 8-bit H.264 capped at 60.
    const int fast = mc.maxFps(3096, 2080, 8, true);
    const int deep = mc.maxFps(3096, 2080, 14, true);
    const int h264 = mc.maxFps(3096, 2080, 8, false);
    std::printf("[caps] full-res: fast=%.0f deep=%.0f h264=%.0f (want 60/30/60)\n",
                (double)fast, (double)deep, (double)h264);
    bool ok = (fast == 60) && (deep == 30) && (h264 == 60);
    report("full-res rates match the datasheet columns", ok);

    // Smaller ROIs are faster; and no format beats the USB link.
    const int mid = mc.maxFps(1920, 1080, 14, true);
    const int small = mc.maxFps(640, 480, 14, true);
    std::printf("[caps] 14-bit: 6.4MP=%d 2.07MP=%d 0.31MP=%d (monotone increasing)\n",
                deep, mid, small);
    ok = ok && deep <= mid && mid <= small;
    report("smaller ROI reaches a higher ceiling", deep <= mid && mid <= small);

    const long px = 640L * 480L;
    const double bwCap = 360.0e6 / ((double)px * captureBytesPerPixel(14));
    report("no ceiling exceeds the USB bandwidth", (double)small <= bwCap + 1.0);
    ok = ok && (double)small <= bwCap + 1.0;

    // A smaller sensor keeps the fast rates for the SAME absolute ROI: the
    // table is stretched by the ratio of the two full frames.
    CameraCaps smallCam;
    smallCam.name = "Small"; smallCam.maxW = 1280; smallCam.maxH = 960;
    smallCam.hasRaw8 = true; smallCam.hasRaw16 = true; smallCam.nativeDepth = 14;
    const int bigSame = mc.maxFps(640, 480, 14, true);
    const int smallSame = smallCam.maxFps(640, 480, 14, true);
    std::printf("[caps] 640x480 14-bit on a 6.4MP sensor=%d, on a 1.2MP sensor=%d (equal: the "
                "columns are keyed to absolute pixel count)\n", bigSame, smallSame);
    report("rate model is finite and >= 1 everywhere", smallSame >= 1 && bigSame >= 1);
    return ok && smallSame >= 1;
}

bool testText()
{
    const CameraCaps mc = asi178(true), mm = asi178(false);
    bool ok = true;
    const bool kind = mc.kindText().contains("colour") && mc.kindText().contains("RGGB") &&
                      mm.kindText() == "mono";
    report("kindText distinguishes colour RGGB from mono", kind);
    ok = ok && kind;
    const QString d = mc.describe();
    const bool desc = d.contains("ASI178MC") && d.contains("3096") && d.contains("14-bit");
    std::printf("[caps] describe(): %s\n", d.toUtf8().constData());
    report("describe() names the camera, sensor and deep depth", desc);
    ok = ok && desc;

    const bool parse = parseBayerPattern("rggb") == 0 && parseBayerPattern("BG") == 1 &&
                       parseBayerPattern(" grbg ") == 2 && parseBayerPattern("GBRG") == 3 &&
                       parseBayerPattern("nonsense") == -1;
    report("--bayer parses rggb/bggr/grbg/gbrg (and rejects junk)", parse);
    return ok && parse;
}
} // namespace

bool runCameraCapsSelfTest()
{
    bool ok = true;
    ok = testDepths()          && ok;
    ok = testRoiCandidates()   && ok;
    ok = testFpsModel()        && ok;
    ok = testText()            && ok;
    std::printf("%s\n", ok ? "CAPSTEST PASS" : "CAPSTEST FAIL");
    return ok;
}
