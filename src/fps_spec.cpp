// fps_spec.cpp
//
// Frame-rate ceiling model (see fps_spec.h).

#include "fps_spec.h"

#include <algorithm>
#include <cmath>

// ZWO ASI178 datasheet table, keyed by pixel count (ascending). Two ADC readouts:
//
//  * 14-bit column — the RAW16 output path. ASI_HIGH_SPEED_MODE is IGNORED on
//    this path (verified 2025-09-15: identical 14-bit data and fps for
//    HSM=0/1 at RAW16), so the deep .ser capture (RAW16 readout) tops out at
//    the 14-bit column.
//
//  * 10-bit ("high speed mode") column — HSM=1 combined with RAW8 OUTPUT
//    switches the sensor to the 10-bit fast readout, delivered 1 byte/px.
//    That is what every 8-bit output (8-bit H.264 and 8-bit .ser) runs on
//    (the app sets HSM=1 whenever the format is RAW8). Measured 2025-09-15 on
//    the ASI178MM: 351.3 fps @ 480x320 (spec ~404), 384.7 @ 320x240 (spec
//    479.7), 56.0 @ 3096x2080 (spec 60) — ~80–93% of the column, the same
//    efficiency the 14-bit readout shows.
//
// Re-measured 2026-09-22 on the ASI178MC (the COLOUR body on the same sensor,
// BandWidth governor at 100, exposure 2 ms, 0 dropped frames) — the two
// columns hold for the colour readout too, so the table is shared:
//   ROI          RAW8 HSM=1   RAW8 HSM=0   RGB24    RAW16
//   3096x2080      56.7         28.7        28.7     28.0
//   1920x1080     110.0         55.0        55.0     55.0
//   1280x720      163.3         81.7        81.7     81.7
//   640x480       240.7        120.3       120.3    120.3
//   320x240       456.0        228.0       228.0    228.0
// Note the colour camera's own RGB24 output runs at the SLOW (14-bit) column
// while carrying 3 bytes/pixel: capturing RGB24 from the camera is strictly
// worse than capturing Bayer RAW8 and demosaicing on the host (2.0 ms/frame at
// 6.4 MP, measured) — so the app never asks for RGB24 (see colour.h).
static const FpsPoint kFps10[] = {
    {   76800L, 479.7 },  // 320x240
    {  307200L, 253.1 },  // 640x480
    {  480000L, 204.7 },  // 800x600
    { 1228800L, 130.0 },  // 1280x960
    { 2211840L, 116.0 },  // 2048x1080
    { 5242880L,  62.0 },  // 2560x2048
    { 6439680L,  60.0 },  // 3096x2080
};

static const FpsPoint kFps14[] = {
    {   76800L, 239.8 },  // 320x240
    {  307200L, 126.5 },  // 640x480
    {  480000L, 102.3 },  // 800x600
    { 1228800L,  65.0 },  // 1280x960
    { 2211840L,  58.0 },  // 2048x1080
    { 5242880L,  31.0 },  // 2560x2048
    { 6439680L,  30.0 },  // 3096x2080
};

// The ASI178 full frame the columns above are keyed to.
static const long kAsi178Px = 3096L * 2080L;

static int interpFps(const FpsPoint* t, int n, long px)
{
    if (px <= t[0].px)      return std::max(1, (int)std::floor(t[0].fps));
    if (px >= t[n - 1].px)  return std::max(1, (int)std::floor(t[n - 1].fps));
    for (int i = 0; i < n - 1; ++i)
        if (px >= t[i].px && px <= t[i + 1].px)
        {
            const double f = (double)(px - t[i].px) / (double)(t[i + 1].px - t[i].px);
            return std::max(1, (int)std::floor(t[i].fps + f * (t[i + 1].fps - t[i].fps)));
        }
    return 30;
}

int captureBytesPerPixel(int bitDepth)
{
    return (bitDepth == 8) ? 1 : 2;
}

int maxFpsForMode(int w, int h, int bitDepth, bool serMode)
{
    if (!serMode) return 60;   // 8-bit H.264 (MP4)
    const long px = (long)w * h;
    // The 8-bit .ser is captured on the 10-bit HSM readout (RAW8 output +
    // ASI_HIGH_SPEED_MODE=1; the app sets both) -> 10-bit column. The deep
    // .ser is captured on the full-depth RAW16 readout (HSM is ignored on that
    // path) -> 14-bit column.
    return (bitDepth == 8)
        ? interpFps(kFps10, int(sizeof(kFps10) / sizeof(kFps10[0])), px)
        : interpFps(kFps14, int(sizeof(kFps14) / sizeof(kFps14[0])), px);
}

int maxFpsEstimate(int w, int h, int bitDepth, bool serMode)
{
    const long px = std::max(1L, (long)w * (long)h);

    // 8-bit H.264 is a codec ceiling, not a sensor ceiling: the encode path is
    // the limit and it is 60 fps regardless of the body.
    if (!serMode && bitDepth == 8) return 60;

    int fps = maxFpsForMode(w, h, bitDepth, serMode);

    // The columns stop at the ASI178 full frame (their verified floor, and the
    // ASI178's own ceiling). Past that — a bigger sensor's full frame, which
    // this table cannot speak for — a column-limited readout falls off as
    // 1/pixels, so extrapolate on that law instead of repeating the last entry
    // (kAsi178Px is that last entry: 3096x2080).
    const long topPx = kAsi178Px;
    if (px > topPx)
        fps = std::max(1, (int)std::llround((double)fps * (double)topPx / (double)px));

    // Bandwidth ceiling: no format streams faster than the link carries.
    const int bpp = captureBytesPerPixel(bitDepth);
    const double bwFps = kUsbStreamBytesPerSec / ((double)px * bpp);
    if (bwFps < (double)fps) fps = (int)std::floor(bwFps);

    return std::clamp(fps, 1, 2000);
}
