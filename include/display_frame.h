// display_frame.h
//
// Worker-side display thumbnail building (see the function comment).
#pragma once

// Build the display thumbnail (worker-side, long edge <= kDispMax) from a raw
// frame: write 8-bit gray to disp8; any target block containing a clipped
// pixel is flagged in clipM (and anyClip). 16-bit sources are shifted >> 8
// per pixel first (65528 -> 255) and downscaled in the 8-bit domain via
// cv::resize INTER_AREA — averaging a 16-bit frame at full depth and THEN
// clamping/shift-averaging to 8-bit instead turns any 14-bit scene with
// values above 256 into a pure-white image (this bug: the old box-average
// path did exactly that, so switching 8-bit -> 14-bit blew the whole preview
// white while only the true clips showed red). disp8/clipM are sized
// dispW*dispH; clipM is zeroed here.
void buildDisplayFrame(const unsigned char* src, int w, int h, int bpp,
                       unsigned char* disp8, unsigned char* clipM,
                       int dispW, int dispH, bool& anyClip);

// Exact 2x2 box downsample in ONE memory-bound pass, straight from the raw
// frame (no intermediate copy): dst8/clipMask are sized (w/2)*(h/2), one
// byte each. 16-bit sources are shifted >> 8 per pixel and averaged in the
// 8-bit domain — the same domain buildDisplayFrame's INTER_AREA path uses
// (differs from a 16-bit-domain average by at most 1 grey level). clipMask
// is set for every output pixel whose 2x2 source block contains a clipped
// pixel (>= 255 / >= 65528). For w >= 2*dispW and h >= 2*dispH this plus a
// small cv::resize to the exact thumbnail size replaces the full-frame copy
// + INTER_AREA at ~7x lower cost (~1.5 ms vs ~12 ms at 6.4 MP) — that cost
// lives on the preview builder thread and, measured, stole ~25 fps of write
// capacity from the drain loop at 6.4 MP 60 fps.
void boxDownsample2x(const unsigned char* src, int w, int h, int bpp,
                     unsigned char* dst8, unsigned char* clipMask);
