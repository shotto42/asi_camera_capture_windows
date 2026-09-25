// fps_spec.h
//
// Frame-rate ceilings for the video frame-rate slider.
//
// The ZWO datasheet gives the ASI178's ceiling as two ADC columns keyed by
// pixel count (ascending):
//   - 14-bit column: the RAW16 output path (the deep .ser mode). HSM is
//     ignored on this path — the sensor always reads out at full depth.
//   - 10-bit column: HSM=1 + RAW8 OUTPUT (every 8-bit output — 8-bit H.264 and
//     8-bit .ser). The 10-bit "high speed" readout, delivered 1 byte/px.
// An arbitrary ROI is linearly interpolated between the two bracketing entries
// and clamped to the endpoints outside the range; the result is floored to an
// integer fps (>= 1).
//
// The same two columns also serve as the ESTIMATE for any other ASI body
// (maxFpsEstimate): a column-ADC sensor spends its readout budget per pixel, so
// the same ROI costs about the same on another model; past the top of the table
// the estimate falls off as 1/pixels and it is clamped by the measured USB
// stream bandwidth. An estimate is what a slider ceiling needs to be: the GUI
// watches the rate the camera actually delivers and raises the ceiling when a
// body streams faster than the model (MainWindow::fpsCeilingFor).
#pragma once

struct FpsPoint { long px; double fps; };

// Payload ceiling for the estimate below, from the ASI178 family on this link
// with the camera's BandWidth governor at 100: the biggest rate ever measured
// here is 3096x2080 RGB24 at 28.7 fps = 554 MB/s, while the datasheet's own
// full-frame pair (60 fps RAW8 = 386 MB/s, 30 fps RAW16 = 387 MB/s) has to stay
// unclipped. So this is deliberately generous (USB3 SuperSpeed carries ~500-600
// MB/s in practice): it exists only to stop the model proposing a rate the
// cable cannot carry — e.g. a 41 MP body's full frame — not to trim the
// datasheet numbers.
constexpr double kUsbStreamBytesPerSec = 550.0e6;

// Spec-ceiling max fps for a video format at the current ROI (ASI178 columns):
//   - 8-bit H.264 (MP4) : hard-capped at 60 fps.
//   - 8-bit .ser        : the 10-bit ADC column (RAW8 + HSM=1 readout).
//   - deep (14-bit) .ser: the 14-bit ADC column (RAW16 readout; HSM is
//     ignored on the RAW16 output path).
// These are the spec ceilings; the real rate is ~80-95% of the column on a
// healthy USB3 link, and lower when the camera's BandWidth governor is below
// 100 (it persists a setting — 60 on this unit, which throttled 480x320 to
// ~111 fps; the app sets 100 on open) or when the USB connection is
// bandwidth-limited.
int maxFpsForMode(int w, int h, int bitDepth, bool serMode);

// The same ceiling as an ESTIMATE for any ASI body (the table above was
// measured on the ASI178 family and both its columns hold for the colour
// ASI178MC too, since colour costs no extra readout time — the app captures
// the Bayer mosaic, not the camera's RGB24):
//   * the datasheet columns at the ROI's ABSOLUTE pixel count (a column-ADC
//     sensor spends its readout budget per pixel, so the same ROI costs about
//     the same on a different body),
//   * beyond the top of the table (a sensor bigger than the ASI178) falling off
//     as 1/pixels, which is what a column-limited readout does,
//   * clamped by the USB payload rate for the format's real bytes/pixel,
// and floored to >= 1.
// A body other than the ASI178 family may still stream faster or slower than
// this says; the app raises the frame-rate slider to the rate the camera is
// actually delivering (see MainWindow::fpsCeilingFor).
int maxFpsEstimate(int w, int h, int bitDepth, bool serMode);

// Bytes per pixel the CAMERA STREAMS for this capture depth: 1 for the 8-bit
// (fast) readout, 2 for the deep one. This is a property of the readout, not of
// the file: a colour camera streams its Bayer MOSAIC at these same widths and
// the RGB it saves (3 channels — interleaved R,G,B in `.ser`/MP4, planes in a
// PNG/TIFF) is produced on
// the host — see colour.h, which is also why the app never asks the camera for
// its RGB24 readout (3 bytes/px at the SLOW rate: strictly worse than 1 at fast).
int captureBytesPerPixel(int bitDepth);
