// colour.h
//
// Colour (Bayer) handling: turning a raw CFA frame into RGB for saving, and
// into an RGB preview thumbnail.
//
// A colour ASI camera streams its MOSAIC, not RGB: ASI_IMG_RAW8 / ASI_IMG_RAW16
// carry one sample per pixel in the colour filter array pattern the SDK reports
// in ASI_CAMERA_INFO.BayerPattern. The camera can also stream ASI_IMG_RGB24
// (its own demosaiced 8-bit output), but measured on the ASI178MC that runs at
// the SLOW readout rate while carrying 3 bytes/pixel (28.7 fps at full res vs
// 56.7 fps for RAW8+HSM), so the app never asks for it: the Bayer readout is
// cheaper on the wire and demosaicing on the host costs 2.0 ms (8-bit) /
// 4.2 ms (16-bit) at 6.4 MP (measured — see fps_spec.cpp). The one thing RGB24
// would buy is the camera's white balance, and that is deliberately left out:
// RAW with no in-camera balance is what stacking/colour-calibration tools
// expect, and applying the camera's WB to saved pixels cannot be undone.
//
// MONO cameras never reach this module: their output stays single-channel.
//
// OpenCV names its Bayer conversions by a convention that is NOT the sensor
// datasheet naming — see the bayerToBgrCode table for the verified mapping.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Sensor max value for a raw capture of this width (14-bit saturates at 65528).
int rawMaxValue(int bpp);

// "RGGB" / "BGGR" / "GRBG" / "GBRG" for an ASI_BAYER_PATTERN value: the four
// samples of the top-left 2x2 block, read row-major from (0,0).
const char* bayerPatternName(int asiPattern);

// OpenCV conversion code (COLOR_BayerXX2BGR) for a ZWO camera reporting
// `asiPattern`. Verified two ways:
//
//  1. OpenCV's own convention, pinned down with a synthetic mosaic whose four
//     2x2 positions hold distinct markers (250/200/150/10) — the position
//     OpenCV feeds to the R output says what it thinks (0,0) is:
//        COLOR_BayerBG2BGR -> (0,0)=R (0,1)=G (1,0)=G (1,1)=B   = RGGB
//        COLOR_BayerRG2BGR -> (0,0)=B ...            (1,1)=R   = BGGR
//        COLOR_BayerGR2BGR -> (0,1)=B (1,0)=R                   = GRBG
//        COLOR_BayerGB2BGR -> (0,1)=R (1,0)=B                   = GBRG
//     i.e. OpenCV's two-letter name describes the pattern read from the SECOND
//     row, so the datasheet-style names cross over (RGGB <-> "BayerBG") — the
//     classic trap: a naive RGGB -> COLOR_BayerRG2BGR swap red and blue.
//  2. Against the ASI178MC's own RGB24 output of the same scene (the camera's
//     internal demosaic as the reference): `./camera_probe --bayer`.
// ZWO documents ASI_BAYER_RG as the RGGB mosaic, hence the crossed table below.
int bayerToBgrCode(int asiPattern);

// Caller-owned scratch: the preview builder thread and the capture thread both
// build thumbnails, so they must not share static buffers.
struct ColourScratch
{
    std::vector<unsigned char> rgb16;     // full-res 16-bit BGR intermediate
    std::vector<unsigned char> rgbFull8;  // full-res 8-bit: BGR from OpenCV, B/R
                                          // swapped in place to RGB for display
    std::vector<unsigned char> rgbHalf;   // half-res 8-bit RGB (quad-demosaic path)
    std::vector<unsigned char> maskHalf;  // clip mask at half resolution
    std::vector<unsigned char> maskFull;  // clip mask at source resolution
};

// Demosaic a raw Bayer frame to interleaved BGR (the order OpenCV and the
// H.264 pipeline expect).
//   bpp == 1: src is w*h bytes (8-bit mosaic) -> dst is w*h*3 bytes (8UC3)
//   bpp == 2: src is w*h*2 bytes (16-bit mosaic, sensor-depth left-justified)
//             -> dst is w*h*3*2 bytes (16UC3)
// Returns false for an unsupported bpp — the caller then saves the raw frame
// rather than write an image with the wrong colours.
bool demosaicBayerToBgr(const unsigned char* src, int w, int h, int bpp, int asiPattern,
                        unsigned char* dstBgr);

// OpenCV conversion code (COLOR_BayerXX2RGB) for a ZWO camera reporting
// `asiPattern` — the same verified table as bayerToBgrCode(), emitting RGB
// instead of BGR (used by the .ser colour path, whose SER frames are
// interleaved R,G,B).
int bayerToRgbCode(int asiPattern);

// Same, always delivering 8-bit BGR (for H.264, which is 8-bit, and for the
// preview). A 16-bit source is demosaiced at FULL depth first and only then
// reduced per pixel (>> 8, so 65528 -> 255): reducing the mosaic before
// interpolating would drop the low bits and band the result.
bool demosaicBayerToBgr8(const unsigned char* src, int w, int h, int bpp, int asiPattern,
                         unsigned char* dstBgr8, ColourScratch& scratch);

// Demosaic a raw Bayer frame to interleaved RGB (R,G,B per pixel — the SER
// ColorID 100 frame layout; see ser_writer.h). Element width follows bpp:
//   bpp == 1: dst is w*h*3 bytes;  bpp == 2: dst is w*h*3*2 bytes.
bool demosaicBayerToRgb(const unsigned char* src, int w, int h, int bpp, int asiPattern,
                        unsigned char* dstRgb);

// Reduce interleaved BGR (8- or 16-bit elements) to 8-bit BGR (>> 8 for the
// 16-bit case). dst is w*h*3 bytes.
void bgrTo8(const unsigned char* src, int w, int h, int bpp, unsigned char* dst8);

// ---- colour display thumbnail --------------------------------------------
// Build an 8-bit RGB thumbnail (dispW x dispH, caller passes the aspect-
// correct size) plus a per-output-pixel clip mask (set when any channel of
// that block is at sensor max: >= 255 for a 1-byte source, >= 65528 for a
// 2-byte one — read from the RAW frame, so it is exact per channel).
//
// BYTE ORDER: the thumbnail is R,G,B — byte 0 is R. This is the order the GUI
// displays it in (QImage Format_RGB888 in FrameView) and the order paintClipRed
// paints its red flag in. The H.264 and PNG/TIFF file paths use interleaved
// BGR (demosaicBayerToBgr -> imwrite, the H.264 appsrc); the .ser colour path
// demosaices straight to interleaved RGB (demosaicBayerToRgb -> SerWriter, the
// SER ColorID 100 order). Getting these mixed up shows up as an R/B-swapped
// PREVIEW with correct files — verified by `--colourtest`.
//
//   bayer == true:  the cheap path is a 2x2 QUAD demosaic — every 2x2 mosaic
//                   block holds R, G, G, B, so each block yields a real RGB
//                   pixel at half resolution in one memory-bound pass with no
//                   interpolation at all; a small OpenCV resize then reaches the
//                   exact thumbnail size. Frames that are not at least 2x the
//                   thumbnail fall back to a full demosaic.
//   bayer == false: single-channel source (mono body); the output is the grey
//                   triple, so one code path feeds the RGB preview canvas and
//                   clipped blocks can still be marked red.
void colourDisplayFrame(const unsigned char* src, int w, int h, int bpp, bool bayer,
                        int asiPattern, unsigned char* rgb, unsigned char* clipM,
                        int dispW, int dispH, bool& anyClip, ColourScratch& scratch);

// Paint the clipped blocks pure red in an 8-bit RGB display buffer (mask from
// colourDisplayFrame, one byte per output pixel). Both bodies mark clips this
// way in the preview, so the paint lives next to the mask that produces it —
// and is the one thing a colour/mono preview has in common. Covered by
// `./camera_app --colourtest`.
inline void paintClipRed(unsigned char* rgb, const unsigned char* mask, size_t nPixels)
{
    for (size_t i = 0; i < nPixels; ++i)
    {
        if (!mask[i]) continue;
        unsigned char* o = rgb + i * 3;
        o[0] = 255; o[1] = 0; o[2] = 0;
    }
}
