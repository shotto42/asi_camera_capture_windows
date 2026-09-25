// camera_caps.h
//
// CameraCaps: what the CONNECTED camera can actually do, probed once when the
// camera is opened (and re-probed if a different body is plugged in later).
//
// The app started life as an ASI178MM tool with everything (ROI list, bit
// depths, gain range, frame-rate ceilings) hard-coded for that sensor. This
// module replaces those assumptions with the values the SDK reports for the
// camera in front of it, so any ASI camera in the family drives the UI:
//
//   * sensor size            -> the resolution selector (see roiCandidates())
//   * IsColorCam + BayerPattern -> RGB output vs single-channel output
//   * SupportedVideoFormat   -> which bit depths exist at all
//   * BitDepth               -> the label + container width of the deep mode
//   * control caps           -> gain range, exposure range, HighSpeedMode
//
// Only probeCameraCaps() touches the SDK; everything else is pure and
// headlessly testable (./camera_app --capstest).
#pragma once

#include <QString>
#include <QStringList>
#include <vector>

// A candidate capture window (pixels). Both numbers are aligned so the camera
// accepts them (see roiCandidates) and, for a Bayer sensor, so the colour
// filter array stays phase-aligned (even width AND height).
struct RoiSize
{
    int w = 0, h = 0;
    bool operator==(const RoiSize& o) const { return w == o.w && h == o.h; }
};

struct CameraCaps
{
    QString name = "ASI camera";  // as the SDK reports it ("ZWO ASI178MC")
    int maxW = 0, maxH = 0;       // sensor maximum (full ROI)
    bool isColor = false;         // Bayer colour camera (vs mono)
    int bayer = 0;                // ASI_BAYER_PATTERN value; meaningful if isColor
    int nativeDepth = 8;          // sensor readout depth (info.BitDepth, 8..16)

    // SupportedVideoFormat, normalised to what the app can use.
    bool hasRaw8 = false;         // 1 byte/px  (mono: gray, colour: Bayer mosaic)
    bool hasRaw16 = false;        // 2 bytes/px (mono: gray, colour: Bayer mosaic)
    bool hasRgb24 = false;        // 3 bytes/px, camera-demosaiced 8-bit colour
    bool hasY8 = false;           // 1 byte/px luminance-style output

    int  gainMin = 0, gainMax = 400;      // ASI_GAIN units (0.1 dB each)
    long expMinUs = 32L;                  // ASI_EXPOSURE caps, microseconds
    long expMaxUs = 2000000000L;
    bool hasHighSpeedMode = false;        // ASI_HIGH_SPEED_MODE control present
    bool capsReadable = false;            // ASIGetControlCaps worked at all

    // ---- white balance (ASI_WB_R / ASI_WB_B, only meaningful on colour) ----
    // Probed ranges, the factory default, and whether each control supports the
    // SDK auto flag (the camera's automatic white balance). The defaults are
    // NOT the neutral anchor any more: measured (tests/wb_probe.cpp) they sit
    // ~1.8x away in R/B ratio from the pair that renders a daylight-lit white
    // target neutral, so the GUI's anchor comes from wbMeasuredNeutral() and
    // only falls back to these (see white_balance.h).
    bool hasWbR = false, hasWbB = false;
    bool wbRAuto = false, wbBAuto = false;
    int  wbRMin = 0, wbRMax = 0, wbRDef = 0;
    int  wbBMin = 0, wbBMax = 0, wbBDef = 0;
    // Both controls exist with usable caps -> the WB UI may be shown (and the
    // worker may touch the controls). Mono bodies never see either decision.
    bool wbControls() const
    {
        return hasWbR && hasWbB && wbRMax > wbRMin && wbBMax > wbBMin;
    }

    // ---- derived: what the UI may offer ----------------------------------
    // The deep mode exists only when the SDK can stream 2 bytes/pixel; its bit
    // count is the sensor's BitDepth (14 on the ASI178, 12 on older sensors).
    int deepDepth() const { return hasRaw16 ? nativeDepth : 8; }
    bool supportsDepth(int bits) const { return bits == 8 ? hasRaw8 || hasY8 : (hasRaw16 && bits == deepDepth()); }
    // Depths to offer, shallow first (8 always when a 1-byte readout exists).
    std::vector<int> depths() const;
    // Save format of one captured frame at this depth: bytes per pixel in the
    // RAW capture buffer (1 for 8-bit, 2 for the deep mode).
    int rawBytesPerPixel(int bits) const { return (bits == 8) ? 1 : 2; }

    // The resolution selector for THIS sensor, aligned + de-duplicated: the
    // 1:1 base square (baseSquare()) FIRST — the entry the selector opens on
    // — then the full frame, integer sensor bins and the aspect-preserving
    // width ladder, largest first behind it. Every entry is still verified
    // against the camera before it is published (probeRois) — an alignment
    // guess the camera rejects simply drops out of the list (and the full
    // frame then takes the first seat).
    std::vector<RoiSize> roiCandidates() const;

    // The 1:1 base entry the resolution selector opens on: a true square
    // whose side is the sensor's y-resolution (MaxHeight — the short side on
    // every landscape body the SDK reports, so the largest 1:1 window that
    // fits), aligned to the 8-px readout granularity. {0,0} when that
    // aligned square would be smaller than the minimum ROI.
    RoiSize baseSquare() const;

    // Frame-rate ceiling for this ROI + capture format (see fps_spec.h for the
    // model; the camera's own measured rate can raise it at runtime).
    int maxFps(int w, int h, int bitDepth, bool serMode) const;

    // One-line description for the window title / log.
    QString describe() const;
    // "colour RGGB" / "mono"
    QString kindText() const;
};

// Read the capabilities of an ALREADY OPENED camera (camId = ASI_CAMERA_INFO
// CameraID). Falls back to documented defaults where the SDK refuses to
// answer (the ASI178MM rejects ASIGetControlCaps for every index, so the gain
// range falls back to 0..400 there). Returns false if the SDK cannot even read
// the camera property.
bool probeCameraCaps(int camId, CameraCaps& out);

// Bayer pattern override ("rggb"/"bggr"/"grbg"/"gbrg", case-insensitive);
// returns the ASI_BAYER_PATTERN value, or -1 if the text is not a pattern.
// Escape hatch for the (documented) case where a body reports a pattern that
// does not match its data stream: --bayer <pattern>.
int parseBayerPattern(const QString& text);
