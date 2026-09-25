// camera_caps.cpp
//
// Camera capability probe + derived UI model (see camera_caps.h).

#include "camera_caps.h"

#include "fps_spec.h"

#include <ASICamera2.h>

#include <algorithm>
#include <cmath>

// ---------------------------------------------------------------------------
// ROI candidate generation
// ---------------------------------------------------------------------------

namespace
{
constexpr int kRoiAlignW = 8;   // measured on the ASI178MC: 1548/774/618/516 wide
                               // ROIs are rejected (ASI_ERROR_INVALID_SIZE) while
                               // 1032 = 8*129 is accepted -> the width granularity
                               // is 8 pixels, not 2.
constexpr int kRoiAlignH = 4;   // 692 = 4*173 is accepted; keep 4 to stay safe on
                               // every model (the full sensor height is aligned
                               // separately and probed unmodified).
constexpr int kMinRoiW = 64, kMinRoiH = 48;

int alignDown(int v, int a) { return (v / a) * a; }
int alignNearest(int v, int a) { return ((v + a / 2) / a) * a; }

void pushCandidate(std::vector<RoiSize>& v, int w, int h, int maxW, int maxH)
{
    if (w < kMinRoiW || h < kMinRoiH) return;
    if (w > maxW || h > maxH) return;
    v.push_back({w, h});
}
} // namespace

// The 1:1 base entry: a true square whose side is the sensor's y-resolution
// (MaxHeight — the short side on every landscape body the SDK reports, so the
// largest 1:1 window that fits), aligned to the 8-px readout granularity.
RoiSize CameraCaps::baseSquare() const
{
    const int s = alignDown(std::min(maxW, maxH), kRoiAlignW);
    if (s < kMinRoiW || s < kMinRoiH) return {0, 0};
    return {s, s};
}

std::vector<RoiSize> CameraCaps::roiCandidates() const
{
    std::vector<RoiSize> rest;

    // 1) the full sensor, exactly as the SDK reports it (never aligned: this is
    //    the native size, and a camera that reports it accepts it).
    pushCandidate(rest, maxW, maxH, maxW, maxH);

    // 2) 1/b-scale windows (the list's historical "bin2..bin6" entries; the
    //    SDK bin control stays at 1, so these are exact centre crops at 1/b
    //    of the sensor, not subsamples). The width is aligned down to the
    //    8-px readout granularity FIRST, then the height is re-derived from
    //    that width at the sensor's aspect ratio — dividing both sides by b
    //    and aligning each independently lets the edges drift apart (bin4:
    //    768x520 = 0.77% off vs 768x516 = exact).
    for (int b = 2; b <= 6; ++b)
    {
        const int bw = alignDown(maxW / b, kRoiAlignW);
        if (bw > maxW || bw < kMinRoiW) continue;
        const int bh = alignNearest((int)((long long)bw * maxH / maxW), kRoiAlignH);
        pushCandidate(rest, bw, bh, maxW, maxH);
    }

    // 3) aspect-preserving crops: a ladder of familiar WIDTHS, but every
    //    height comes from the SENSOR's own aspect ratio (rounded to the %4
    //    height alignment) — the old fixed 16:9 / 4:3 windows cropped away a
    //    different slice of the frame per entry. The rounding costs at most
    //    ~0.5% of the ratio at the small end; wider than the sensor is
    //    skipped. On a 4:3 or 16:9 sensor this ladder lands on the usual
    //    video windows exactly (1280x960 sensor -> 800x600, 640x480, 480x360).
    static const int kCropWidths[] = {3840, 2560, 1920, 1600, 1280, 1024, 800, 640, 480};
    for (int w : kCropWidths)
    {
        if (w % kRoiAlignW != 0 || w > maxW) continue;
        const int h = alignNearest((int)((long long)w * maxH / maxW), kRoiAlignH);
        pushCandidate(rest, w, h, maxW, maxH);
    }

    // de-duplicate, largest area first (ties: the wider window first)
    std::stable_sort(rest.begin(), rest.end(), [](const RoiSize& a, const RoiSize& b) {
        const long aa = (long)a.w * a.h, bb = (long)b.w * b.h;
        if (aa != bb) return aa > bb;
        return a.w > b.w;
    });
    std::vector<RoiSize> uniq;
    for (const auto& c : rest)
        if (std::find(uniq.begin(), uniq.end(), c) == uniq.end())
            uniq.push_back(c);
    if (uniq.empty()) uniq.push_back({maxW, maxH});

    // 4) the 1:1 base square (planetary framing; the user-requested default)
    //    LEADS the list — it is the entry the selector opens on — with the
    //    full frame and the crops behind it, largest first. On a square
    //    sensor the full frame already IS that square and stays first.
    const RoiSize base = baseSquare();
    if (base.w > 0 && !(uniq.front() == base))
    {
        std::vector<RoiSize> out;
        out.push_back(base);
        for (const auto& u : uniq)
            if (!(u == base)) out.push_back(u);
        return out;
    }
    return uniq;
}

// ---------------------------------------------------------------------------
// Depth + fps
// ---------------------------------------------------------------------------

std::vector<int> CameraCaps::depths() const
{
    std::vector<int> d;
    if (hasRaw8 || hasY8) d.push_back(8);
    if (hasRaw16 && nativeDepth > 8) d.push_back(nativeDepth);
    if (d.empty()) d.push_back(8);       // should not happen: guarded at open
    return d;
}

int CameraCaps::maxFps(int w, int h, int bitDepth, bool serMode) const
{
    // The datasheet columns are keyed by ABSOLUTE pixel count (verified on both
    // ASI178 bodies: the same ROI costs the same readout time whether it is
    // carrying mono or Bayer samples), extrapolated past the top of the table
    // and clamped by the USB payload rate — see fps_spec.h. Always an estimate
    // for another model: the GUI watches the delivered rate and raises the
    // frame-rate ceiling when the camera actually streams faster.
    return maxFpsEstimate(w, h, bitDepth, serMode);
}

// ---------------------------------------------------------------------------
// Description
// ---------------------------------------------------------------------------

QString CameraCaps::kindText() const
{
    if (!isColor) return "mono";
    switch (bayer)
    {
    case ASI_BAYER_RG: return "colour RGGB";
    case ASI_BAYER_BG: return "colour BGGR";
    case ASI_BAYER_GR: return "colour GRBG";
    case ASI_BAYER_GB: return "colour GBRG";
    }
    return "colour";
}

QString CameraCaps::describe() const
{
    // "ZWO ASI178MC · colour RGGB · 3096x2080 · 14-bit"
    return QString("%1 · %2 · %3x%4 · %5-bit")
        .arg(name, kindText())
        .arg(maxW).arg(maxH)
        .arg(deepDepth());
}

int parseBayerPattern(const QString& text)
{
    const QString t = text.trimmed().toUpper();
    if (t == "RGGB" || t == "RG") return ASI_BAYER_RG;
    if (t == "BGGR" || t == "BG") return ASI_BAYER_BG;
    if (t == "GRBG" || t == "GR") return ASI_BAYER_GR;
    if (t == "GBRG" || t == "GB") return ASI_BAYER_GB;
    return -1;
}

// ---------------------------------------------------------------------------
// SDK probe
// ---------------------------------------------------------------------------

bool probeCameraCaps(int camId, CameraCaps& out)
{
    ASI_CAMERA_INFO info = {};
    if (ASIGetCameraProperty(&info, camId) != ASI_SUCCESS) return false;

    out = CameraCaps{};
    out.name = QString::fromLatin1(info.Name[0] ? info.Name : "ASI camera");
    out.maxW = (int)std::max(1L, info.MaxWidth);
    out.maxH = (int)std::max(1L, info.MaxHeight);
    out.isColor = (info.IsColorCam == ASI_TRUE);
    out.bayer = (int)info.BayerPattern;
    out.nativeDepth = std::clamp(info.BitDepth > 0 ? info.BitDepth : 8, 8, 16);

    for (int f = 0; f < 8 && info.SupportedVideoFormat[f] != ASI_IMG_END; ++f)
    {
        switch (info.SupportedVideoFormat[f])
        {
        case ASI_IMG_RAW8:  out.hasRaw8 = true;  break;
        case ASI_IMG_RAW16: out.hasRaw16 = true; break;
        case ASI_IMG_RGB24: out.hasRgb24 = true; break;
        case ASI_IMG_Y8:    out.hasY8 = true;    break;
        default: break;
        }
    }
    // A colour body that only offers the camera-demosaiced stream still counts
    // as colour; a body with no 1-byte readout at all is handled by the caller
    // (a clear startup error), so keep the flags as probed here.

    // Control caps. ASIGetControlCaps takes the control's LIST INDEX (not the
    // type), so scan the indices and match on ControlType. Some bodies (the
    // ASI178MM among them) answer ASI_ERROR_INVALID_CONTROL_TYPE for every
    // index, in which case the documented defaults above stay in force; the
    // ASI178MC answers normally (gain 0..510, exposure 32 us..2000 s).
    int nControls = 0;
    if (ASIGetNumOfControls(camId, &nControls) == ASI_SUCCESS && nControls > 0)
    {
        for (int i = 0; i < nControls && i < 64; ++i)
        {
            ASI_CONTROL_CAPS cap = {};
            if (ASIGetControlCaps(camId, i, &cap) != ASI_SUCCESS) break;
            out.capsReadable = true;
            switch (cap.ControlType)
            {
            case ASI_GAIN:
                if (cap.MaxValue > cap.MinValue)
                {
                    out.gainMin = (int)cap.MinValue;
                    out.gainMax = (int)cap.MaxValue;
                }
                break;
            case ASI_EXPOSURE:
                if (cap.MaxValue > cap.MinValue)
                {
                    out.expMinUs = cap.MinValue;
                    out.expMaxUs = cap.MaxValue;
                }
                break;
            case ASI_HIGH_SPEED_MODE:
                out.hasHighSpeedMode = (cap.MaxValue >= 1);
                break;
            case ASI_WB_R:
                out.hasWbR  = true;
                out.wbRMin  = (int)cap.MinValue;
                out.wbRMax  = (int)cap.MaxValue;
                out.wbRDef  = std::clamp((int)cap.DefaultValue, (int)cap.MinValue, (int)cap.MaxValue);
                out.wbRAuto = (cap.IsAutoSupported == ASI_TRUE);
                break;
            case ASI_WB_B:
                out.hasWbB  = true;
                out.wbBMin  = (int)cap.MinValue;
                out.wbBMax  = (int)cap.MaxValue;
                out.wbBDef  = std::clamp((int)cap.DefaultValue, (int)cap.MinValue, (int)cap.MaxValue);
                out.wbBAuto = (cap.IsAutoSupported == ASI_TRUE);
                break;
            default:
                break;
            }
        }
    }

    // Even when the caps cannot be read the CURRENT value usually can; a body
    // that reports no readable caps keeps the documented 0..400 range.
    long cur = 0; ASI_BOOL isAuto = ASI_FALSE;
    if (ASIGetControlValue(camId, ASI_GAIN, &cur, &isAuto) == ASI_SUCCESS)
    {
        if (!out.capsReadable)
            out.gainMax = std::max(out.gainMax, (int)cur);   // never below the live value
        out.gainMin = std::min(out.gainMin, (int)cur);
    }
    return true;
}
