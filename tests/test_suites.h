// test_suites.h
//
// Headless self-test suites (no camera needed), dispatched from main() via
// CLI flags:
//   ./camera_app --sertest     runSerWriterSelfTest()
//   ./camera_app --frametest   runFrameViewSelfTest()
//   ./camera_app --capstest    runCameraCapsSelfTest()
//   ./camera_app --colourtest  runColourSelfTest()
//   ./camera_app --wbtest      runWhiteBalanceSelfTest()
#pragma once

// Writes 8/14-bit .ser ramp samples into samples/ and round-trip-validates
// the header + every pixel value. Returns true on success.
bool runSerWriterSelfTest();

// FrameView regression suite (offscreen Qt): the stale-latestCh_ photo
// scenario that used to SIGSEGV in QImage::copy, plus the 14-bit display
// downscale regression. Returns true on success.
bool runFrameViewSelfTest();

// Capability-model suite: the resolution candidates, offered bit depths and
// frame-rate ceilings the app derives from what a camera reports — verified for
// an ASI178MC/MM and for bodies that cannot do what the ASI178 can. Returns
// true on success.
bool runCameraCapsSelfTest();

// Colour pipeline suite: the Bayer->RGB mapping for all four patterns (8- and
// 16-bit), the RGB .ser frame order (per-pixel interleaved R,G,B, pinned
// byte-for-byte + end to end), the fact that a MONO body still writes
// exactly what it wrote before, and the colour thumbnail + clip mask. Returns
// true on success.
bool runColourSelfTest();

// White-balance model suite (no camera): the Planckian-locus anchors, the
// monotonic temperature signature, the neutral anchor at the camera's default
// WB gains, both slider directions, exact (K, tint) <-> gain round trips, cap
// clamping, and the AWB-follow quantization. Returns true on success.
bool runWhiteBalanceSelfTest();
