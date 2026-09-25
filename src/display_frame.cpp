// display_frame.cpp
//
// Worker-side display thumbnail building (see display_frame.h).

#include "display_frame.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <cstring>
#include <vector>

void buildDisplayFrame(const unsigned char* src, int w, int h, int bpp,
                       unsigned char* disp8, unsigned char* clipM,
                       int dispW, int dispH, bool& anyClip)
{
    const size_t npx = (size_t)dispW * dispH;
    std::memset(clipM, 0, npx);
    anyClip = false;
    if (dispW == w && dispH == h)
    {
        // Small ROI: no scaling, just convert (16->8) and flag clips.
        if (bpp == 1)
        {
            for (size_t i = 0; i < npx; ++i)
            {
                disp8[i] = src[i];
                if (src[i] >= 255) { clipM[i] = 1; anyClip = true; }
            }
        }
        else
        {
            const unsigned short* p = (const unsigned short*)src;
            for (size_t i = 0; i < npx; ++i)
            {
                disp8[i] = (unsigned char)(p[i] >> 8);
                if (p[i] >= 65528) { clipM[i] = 1; anyClip = true; }
            }
        }
        return;
    }
    // Large downscale. The old approach — one nested per-target-pixel block
    // loop over the FULL source (per-pixel branches + a float reciprocal
    // multiply) — cost ~25 ms at 6.4 MP and capped the whole frame loop
    // (~37 fps at full res) below the 10-bit HSM readout rate (~56), so the
    // camera's USB buffer overflowed and frames dropped. This is now:
    //
    //  1. cv::resize INTER_AREA — a SIMD-optimized exact box downscale
    //     (~4 ms at 6.4 MP vs ~16-25 ms for the hand-rolled passes). 16-bit
    //     sources are mapped to 8-bit first (>>8): downsampling in the 8-bit
    //     domain differs from the 16-bit-domain average by at most 1 grey
    //     level (invisible in a live preview).
    //  2. A clip-flag pass that checks the CORNERS of each target pixel's
    //     source block (bounds precomputed once, no per-pixel division) —
    //     ~4 sampled reads per target pixel instead of a full per-source
    //     pixel scan. A clipped block is still marked red, as before; this
    //     is a live-preview indicator, and the un-sampled interior pixels of
    //     a block can only hide a clip, never fake one.
    //
    // Only the worker thread ever calls this (frame loop / vtest); the
    // statics are reused scratch, no per-frame allocation.
    static std::vector<unsigned char> g8;   // 16-bit -> 8-bit map (bpp == 2 only)

    cv::Mat srcMat;
    if (bpp == 1)
    {
        srcMat = cv::Mat(h, w, CV_8UC1, const_cast<unsigned char*>(src));
    }
    else
    {
        const unsigned short* p16 = (const unsigned short*)src;
        const int n = w * h;
        g8.resize(n);
        unsigned char* d = g8.data();
        for (int i = 0; i < n; ++i) d[i] = (unsigned char)(p16[i] >> 8);
        srcMat = cv::Mat(h, w, CV_8UC1, d);
    }
    cv::Mat dst(dispH, dispW, CV_8UC1);
    cv::resize(srcMat, dst, cv::Size(dispW, dispH), 0, 0, cv::INTER_AREA);
    std::memcpy(disp8, dst.data, npx);

    // Clip flags from the block corners (see above).
    std::vector<int> bx0(dispW), bx1(dispW), by0(dispH), by1(dispH);
    for (int tx = 0; tx < dispW; ++tx)
    {
        bx0[tx] = tx * w / dispW;
        bx1[tx] = (tx + 1) * w / dispW;
        if (bx1[tx] <= bx0[tx]) bx1[tx] = bx0[tx] + 1;
        bx1[tx]--;   // inclusive right/bottom corner
    }
    for (int ty = 0; ty < dispH; ++ty)
    {
        by0[ty] = ty * h / dispH;
        by1[ty] = (ty + 1) * h / dispH;
        if (by1[ty] <= by0[ty]) by1[ty] = by0[ty] + 1;
        by1[ty]--;
    }
    if (bpp == 1)
    {
        for (int ty = 0; ty < dispH; ++ty)
        {
            const int y0 = by0[ty], y1 = by1[ty];
            const unsigned char* r0 = src + (size_t)y0 * w;
            const unsigned char* r1 = src + (size_t)y1 * w;
            unsigned char* c = clipM + (size_t)ty * dispW;
            for (int tx = 0; tx < dispW; ++tx)
            {
                const int x0 = bx0[tx], x1 = bx1[tx];
                if (r0[x0] >= 255 || r0[x1] >= 255 || r1[x0] >= 255 || r1[x1] >= 255)
                {
                    c[tx] = 1;
                    anyClip = true;
                }
            }
        }
    }
    else
    {
        const unsigned short* p16 = (const unsigned short*)src;
        for (int ty = 0; ty < dispH; ++ty)
        {
            const int y0 = by0[ty], y1 = by1[ty];
            const unsigned short* r0 = p16 + (size_t)y0 * w;
            const unsigned short* r1 = p16 + (size_t)y1 * w;
            unsigned char* c = clipM + (size_t)ty * dispW;
            for (int tx = 0; tx < dispW; ++tx)
            {
                const int x0 = bx0[tx], x1 = bx1[tx];
                if (r0[x0] >= 65528 || r0[x1] >= 65528 || r1[x0] >= 65528 || r1[x1] >= 65528)
                {
                    c[tx] = 1;
                    anyClip = true;
                }
            }
        }
    }
}

void boxDownsample2x(const unsigned char* src, int w, int h, int bpp,
                     unsigned char* dst8, unsigned char* clipMask)
{
    const int ow = w / 2, oh = h / 2;
    if (bpp == 1)
    {
        for (int y = 0; y < oh; ++y)
        {
            const unsigned char* r0 = src + (size_t)(2 * y) * w;
            const unsigned char* r1 = src + (size_t)(2 * y + 1) * w;
            unsigned char* o = dst8 + (size_t)y * ow;
            unsigned char* m = clipMask + (size_t)y * ow;
            for (int x = 0; x < ow; ++x)
            {
                const unsigned int a = r0[2 * x], b = r0[2 * x + 1];
                const unsigned int c = r1[2 * x], d = r1[2 * x + 1];
                o[x] = (unsigned char)((a + b + c + d) >> 2);
                m[x] = (a == 255 || b == 255 || c == 255 || d == 255) ? 1 : 0;
            }
        }
    }
    else
    {
        const unsigned short* s16 = (const unsigned short*)src;
        for (int y = 0; y < oh; ++y)
        {
            const unsigned short* r0 = s16 + (size_t)(2 * y) * w;
            const unsigned short* r1 = s16 + (size_t)(2 * y + 1) * w;
            unsigned char* o = dst8 + (size_t)y * ow;
            unsigned char* m = clipMask + (size_t)y * ow;
            for (int x = 0; x < ow; ++x)
            {
                // >> 8 per pixel, average in the 8-bit domain (matches
                // buildDisplayFrame's INTER_AREA path).
                const unsigned int a = r0[2 * x] >> 8, b = r0[2 * x + 1] >> 8;
                const unsigned int c = r1[2 * x] >> 8, d = r1[2 * x + 1] >> 8;
                o[x] = (unsigned char)((a + b + c + d) >> 2);
                m[x] = (r0[2 * x] >= 65528 || r0[2 * x + 1] >= 65528 ||
                         r1[2 * x] >= 65528 || r1[2 * x + 1] >= 65528) ? 1 : 0;
            }
        }
    }
}
