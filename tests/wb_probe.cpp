// wb_probe.cpp — standalone white-balance MEASUREMENT probe (no Qt, no display).
//
// Answers the questions the Planckian model in white_balance.cpp cannot answer
// by itself:
//
//   1. What do the body's WB_R/WB_B caps really say (min/max/default/auto)?
//   2. Is the delivered RAW Bayer data white-balanced IN the camera, and is the
//      control value a LINEAR multiplier (the model assumes it is)? Measured by
//      pinning one channel, sweeping the other, and reading the Bayer channel
//      means back out of real frames -> d ln(R/G) / d ln(WB_R).
//   3. NEUTRAL (--neutral): with a white/grey target in view, the gains that
//      actually render THAT TARGET neutral are measured directly —
//      r* = rRef * (G/R), b* = bRef * (G/B) — and verified by applying them and
//      measuring again. This is the body's real 6500 K anchor, which the
//      factory default gains are NOT (ASI178MC, window daylight: neutral is
//      (93, 65); the defaults (70, 90) leave the same target visibly blue —
//      R/G 0.76, B/G 1.36).
//   4. Where does the body's AWB converge (auto on WB_R only = the ZWO demo
//      pattern, vs the flag on both channels), and is the auto flag per channel
//      or global?
//   5. What colour shift does each slider temperature actually DELIVER on this
//      body, measured as the rendered R/B ratio against the neutral point?
//
// Every measured point is also run through the app's model (white_balance.cpp
// is linked in) so the printed KELVIN is exactly what the GUI would display.
//
// Build:  CMake -DBUILD_PROBES=ON (or by hand: cl /O2 /EHsc /std:c++17 /utf-8
//          tests\wb_probe.cpp src\white_balance.cpp /Fe:wb_probe.exe
//          -Iinclude -IASI_SDK /link ASI_SDK\x64\ASICamera2.lib)
//
// Usage:  wb_probe.exe [--neutral|--law] [--roi WxH] [--exp us] [--gain n]
//                    [--frames n] [--center pct] [--secs n] [--manual]
//          --neutral   only the neutral-reference measurement (fast) — hold a
//                      white/grey target FILLING the frame in the light you want
//                      to call neutral, and this prints the pair that
//                      wbMeasuredNeutral() stores
//          --law       only the caps + gain-law sweeps (no target needed)
//          --tint      only the tint-liveness measurement (does the Tint slider
//                      change the image inside / outside its deliverable window)
//          --manual    leave the controls manual at the camera defaults at the
//                      end instead of re-enabling auto
//
// NOTE: opens the camera itself — stop the GUI first (one process per camera).

#include <ASICamera2.h>
#include "white_balance.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

// ---------------------------------------------------------------- Bayer means
// Means of the sampled Bayer positions, three ways:
//   full   = the whole field (gray-world estimate)
//   band   = only samples whose value is within +/-20%% of the median -> the
//            DOMINANT surface (a white/grey sheet that fills the frame), with
//            highlights and background rejected. This is the trustworthy one.
//   bright = the top decile (useless when a lamp/lit window is in view)
struct Means
{
    double r = 0, g1 = 0, g2 = 0, b = 0, n = 0, sat = 0;
    double br = 0, bg1 = 0, bg2 = 0, bb = 0, bn = 0;
    double mr = 0, mg1 = 0, mg2 = 0, mb = 0, mn = 0;   // median band
    double bandFrac = 0;
    double g()  const { return 0.5 * (g1 + g2); }
    double bg() const { return 0.5 * (bg1 + bg2); }
    double mg() const { return 0.5 * (mg1 + mg2); }
};

// The 2x2 mosaic for a Bayer pattern, indexed [row&1][col&1].
static void patternMap(ASI_BAYER_PATTERN p, char map[4])
{
    map[0] = 'R'; map[1] = 'G'; map[2] = 'G'; map[3] = 'B';         // RGGB
    if (p == ASI_BAYER_BG) { map[0] = 'G'; map[1] = 'B'; map[2] = 'R'; map[3] = 'G'; }
    else if (p == ASI_BAYER_GR) { map[0] = 'G'; map[1] = 'R'; map[2] = 'B'; map[3] = 'G'; }
    else if (p == ASI_BAYER_GB) { map[0] = 'B'; map[1] = 'G'; map[2] = 'G'; map[3] = 'R'; }
}

int main(int argc, char** argv)
{
    bool neutralOnly = false;
    bool lawOnly = false;           // --law: caps + gain law only, then leave
    bool tintOnly = false;          // --tint: tint-liveness measurement only
    int  w = 800, h = 600;
    int  expUs = 0;                 // 0 = bracket for the best level
    int  gainVal = -1;              // -1 = camera default
    int  framesPerPoint = 10;
    double centerPct = 35.0;        // measure the central N%% of the field
    double settleSecs = 6.0;        // per auto-WB convergence wait
    bool leaveManual = false;
    bool calMeasured = false;       // does the app already have a measured anchor?

    for (int i = 1; i < argc; ++i)
    {
        if (!std::strcmp(argv[i], "--neutral")) neutralOnly = true;
        if (!std::strcmp(argv[i], "--law"))     lawOnly = true;
        if (!std::strcmp(argv[i], "--tint"))    tintOnly = true;
        else if (!std::strcmp(argv[i], "--center") && i + 1 < argc) centerPct = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--roi") && i + 1 < argc)
        {
            // Both forms work: "--roi 1280x800" and "--roi 1280 800". (The one
            // -arg form is what people type; accepting only two args silently
            // read "800x600" as 800x0 on the first run of this probe.)
            const char* a = argv[++i];
            const char* xx = std::strchr(a, 'x');
            if (xx) { w = std::atoi(a); h = std::atoi(xx + 1); }
            else if (i + 1 < argc) { w = std::atoi(a); h = std::atoi(argv[++i]); }
        }
        else if (!std::strcmp(argv[i], "--exp") && i + 1 < argc) expUs = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--gain") && i + 1 < argc) gainVal = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) framesPerPoint = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--secs") && i + 1 < argc) settleSecs = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--manual")) leaveManual = true;
    }

    const int n = ASIGetNumOfConnectedCameras();
    if (n <= 0) { std::fprintf(stderr, "no camera\n"); return 1; }

    ASI_CAMERA_INFO info = {};
    if (ASIGetCameraProperty(&info, 0) != ASI_SUCCESS) { std::fprintf(stderr, "property failed\n"); return 1; }
    std::printf("camera: %s  %ldx%ld  IsColorCam=%d\n", info.Name, info.MaxWidth, info.MaxHeight,
                (int)info.IsColorCam);
    if (!info.IsColorCam) { std::fprintf(stderr, "this probe needs a colour body\n"); return 1; }

    if (ASIOpenCamera(info.CameraID) != ASI_SUCCESS)
    {
        std::fprintf(stderr, "open failed (another process holds the camera?)\n");
        return 1;
    }
    ASIInitCamera(info.CameraID);

    // ---- 1. caps ----------------------------------------------------------
    int nControls = 0;
    ASIGetNumOfControls(info.CameraID, &nControls);
    long rMin = 1, rMax = 99, rDef = 70, bMin = 1, bMax = 99, bDef = 90, gainDef = 210;
    bool rAuto = false, bAuto = false;
    std::printf("--- control caps (WB + friends) ---\n");
    for (int c = 0; c < nControls; ++c)
    {
        ASI_CONTROL_CAPS cap = {};
        if (ASIGetControlCaps(info.CameraID, c, &cap) != ASI_SUCCESS) continue;
        if (cap.ControlType != ASI_WB_R && cap.ControlType != ASI_WB_B &&
            cap.ControlType != ASI_GAMMA && cap.ControlType != ASI_GAIN &&
            cap.ControlType != ASI_HIGH_SPEED_MODE) continue;
        std::printf("  %-18s min=%-6ld max=%-6ld def=%-6ld auto=%d writable=%d\n",
                    cap.Name, cap.MinValue, cap.MaxValue, cap.DefaultValue,
                    (int)cap.IsAutoSupported, (int)cap.IsWritable);
        if (cap.ControlType == ASI_WB_R) { rMin = cap.MinValue; rMax = cap.MaxValue; rDef = cap.DefaultValue; rAuto = cap.IsAutoSupported; }
        if (cap.ControlType == ASI_WB_B) { bMin = cap.MinValue; bMax = cap.MaxValue; bDef = cap.DefaultValue; bAuto = cap.IsAutoSupported; }
        if (cap.ControlType == ASI_GAIN) gainDef = cap.DefaultValue;
    }

    auto getWb = [&](long& r, long& b, bool& ar, bool& ab) {
        ASI_BOOL a1 = ASI_FALSE, a2 = ASI_FALSE;
        const ASI_ERROR_CODE e1 = ASIGetControlValue(info.CameraID, ASI_WB_R, &r, &a1);
        const ASI_ERROR_CODE e2 = ASIGetControlValue(info.CameraID, ASI_WB_B, &b, &a2);
        ar = (e1 == ASI_SUCCESS && a1 == ASI_TRUE);
        ab = (e2 == ASI_SUCCESS && a2 == ASI_TRUE);
        return e1 == ASI_SUCCESS && e2 == ASI_SUCCESS;
    };
    auto setWb = [&](long r, long b) {
        ASISetControlValue(info.CameraID, ASI_WB_R, r, ASI_FALSE);
        ASISetControlValue(info.CameraID, ASI_WB_B, b, ASI_FALSE);
    };

    WbCal cal;
    cal.rMin = (int)rMin; cal.rMax = (int)rMax; cal.bMin = (int)bMin; cal.bMax = (int)bMax;
    // The anchor the APP would use for this body: the stored measurement when
    // wbMeasuredNeutral() has one, else the factory defaults. Printed here so a
    // fresh measurement can be compared against it (that is what this probe is
    // for: it produces and validates the numbers in wbMeasuredNeutral()).
    calMeasured = wbMeasuredNeutral(info.Name, (int)rMax, (int)bMax, cal.rNeu, cal.bNeu);
    if (!calMeasured)
    {
        cal.rNeu = (double)rDef;
        cal.bNeu = (double)bDef;
    }
    std::printf("  anchor in the app: (%.1f, %.1f) %s\n", cal.rNeu, cal.bNeu,
                calMeasured ? "[measured]" : "[FALLBACK: the factory defaults - measure this body!]");
    long r0 = 0, b0 = 0; bool ar0 = false, ab0 = false;
    if (getWb(r0, b0, ar0, ab0))
        std::printf("  live at connect: WB_R=%ld (auto %d)  WB_B=%ld (auto %d)  -> model %d K\n",
                    r0, (int)ar0, b0, (int)ab0, [&] { int K = 0, T = 0; wbFromGains((int)r0, (int)b0, cal, K, T); return K; }());

    // ---- capture setup ----------------------------------------------------
    if (w % 8) w -= w % 8;
    if (h % 2) h -= h % 2;
    if (ASISetROIFormat(info.CameraID, w, h, 1, ASI_IMG_RAW8) != ASI_SUCCESS)
        std::fprintf(stderr, "  (ROI %dx%d refused, using the camera's current one)\n", w, h);
    int aw = 0, ah = 0, ab = 0; ASI_IMG_TYPE ait = ASI_IMG_END;
    ASIGetROIFormat(info.CameraID, &aw, &ah, &ab, &ait);
    std::printf("  ROI: %dx%d bin%d fmt%d\n", aw, ah, ab, (int)ait);

    if (gainVal < 0) gainVal = (int)gainDef;
    ASISetControlValue(info.CameraID, ASI_GAIN, gainVal, ASI_FALSE);
    char bmap[4]; patternMap(info.BayerPattern, bmap);
    std::printf("  bayer: %c%c/%c%c\n", bmap[0], bmap[1], bmap[2], bmap[3]);

    std::vector<unsigned char> buf((size_t)aw * ah);
    if (ASIStartVideoCapture(info.CameraID) != ASI_SUCCESS)
    {
        std::fprintf(stderr, "start capture failed\n");
        ASICloseCamera(info.CameraID);
        return 1;
    }

    auto drain = [&](int nFrames) {
        for (int i = 0; i < nFrames; ++i)
            ASIGetVideoData(info.CameraID, buf.data(), (long)buf.size(), 1000);
    };

    // Sampled pixels: a 2x2 block at every 4th position of both phases (so all
    // four Bayer positions), kept so a second pass can average selectively.
    struct Smp { unsigned char v; char ch; int row; };
    const double cFrac = centerPct / 100.0;

    auto measure = [&](const char* label, int warmupFrames, bool verbose = true) -> Means
    {
        drain(warmupFrames);
        Means m;
        const int y0 = (int)(ah * (1.0 - cFrac) / 2.0) & ~3;
        const int y1 = ah - y0;
        const int x0 = (int)(aw * (1.0 - cFrac) / 2.0) & ~3;
        const int x1 = aw - x0;
        std::vector<Smp> smp;
        smp.reserve((size_t)(aw / 4) * (ah / 4) * 2);
        for (int f = 0; f < framesPerPoint; ++f)
        {
            if (ASIGetVideoData(info.CameraID, buf.data(), (long)buf.size(), 1000) != ASI_SUCCESS) continue;
            for (int y = y0; y + 1 < y1; y += 4)        // rows y and y+1 -> both row phases
            {
                const unsigned char* r0p = buf.data() + (size_t)y * aw;
                const unsigned char* r1p = r0p + aw;
                for (int x = x0; x + 1 < x1; x += 4)    // cols x and x+1 -> both col phases
                {
                    for (int sub = 0; sub < 2; ++sub)
                    {
                        const unsigned char* row = sub ? r1p : r0p;
                        for (int sc = 0; sc < 2; ++sc)
                        {
                            const int xx = x + sc;
                            const unsigned char v = row[xx];
                            const int yy = y + sub;
                            const char ch = bmap[(yy & 1) * 2 + (xx & 1)];
                            switch (ch)
                            {
                            case 'R': m.r += v; break;
                            case 'B': m.b += v; break;
                            default:  (yy & 1) ? (m.g2 += v) : (m.g1 += v); break;
                            }
                            if (v >= 250) m.sat += 1.0;
                            m.n += 1.0;
                            smp.push_back({ v, ch, (yy & 1) });
                        }
                    }
                }
            }
        }
        if (m.n < 1) return m;
        m.r /= m.n; m.g1 /= m.n; m.g2 /= m.n; m.b /= m.n; m.sat = 100.0 * m.sat / m.n;

        std::vector<unsigned char> vals; vals.reserve(smp.size());
        for (const auto& s : smp) vals.push_back(s.v);
        // the dominant surface: within +/-20% of the median value
        std::vector<unsigned char> tmp = vals;
        std::nth_element(tmp.begin(), tmp.begin() + tmp.size() / 2, tmp.end());
        const unsigned char med = tmp[tmp.size() / 2];
        const int lo = (int)(med * 0.8), hi = (int)(med * 1.2) + 1;
        for (const auto& s : smp)
        {
            if (s.v < lo || s.v > hi) continue;
            switch (s.ch)
            {
            case 'R': m.mr += s.v; break;
            case 'B': m.mb += s.v; break;
            default:  s.row ? (m.mg2 += s.v) : (m.mg1 += s.v); break;
            }
            m.mn += 1.0;
        }
        if (m.mn > 0) { m.mr /= m.mn; m.mg1 /= m.mn; m.mg2 /= m.mn; m.mb /= m.mn; m.bandFrac = m.mn / (double)smp.size(); }
        else m.mr = m.mg1 = m.mg2 = m.mb = 0;
        // brightest decile
        std::nth_element(vals.begin(), vals.begin() + vals.size() * 9 / 10, vals.end());
        const unsigned char thr = vals[vals.size() * 9 / 10];
        for (const auto& s : smp)
        {
            if (s.v < thr) continue;
            switch (s.ch)
            {
            case 'R': m.br += s.v; break;
            case 'B': m.bb += s.v; break;
            default:  s.row ? (m.bg2 += s.v) : (m.bg1 += s.v); break;
            }
            m.bn += 1.0;
        }
        if (m.bn > 0) { m.br /= m.bn; m.bg1 /= m.bn; m.bg2 /= m.bn; m.bb /= m.bn; }

        long rr = 0, bb = 0; bool a1 = false, a2 = false;
        getWb(rr, bb, a1, a2);
        int K = 0, T = 0;
        wbFromGains((int)rr, (int)bb, cal, K, T);
        if (verbose)
        {
            const double g = m.g();
            std::printf("  %-26s r=%-4ld(auto %d) b=%-4ld(auto %d) | full R=%6.1f G=%6.1f B=%6.1f R/G=%5.3f B/G=%5.3f "
                        "| band(med %3d, %3.0f%%) R=%6.1f G=%6.1f B=%6.1f R/G=%5.3f B/G=%5.3f | sat=%4.1f%% | model %5d K tint %+d\n",
                        label, rr, (int)a1, bb, (int)a2,
                        m.r, g, m.b, g > 0 ? m.r / g : 0.0, g > 0 ? m.b / g : 0.0,
                        (int)med, 100.0 * m.bandFrac,
                        m.mr, m.mg(), m.mb, m.mg() > 0 ? m.mr / m.mg() : 0.0, m.mg() > 0 ? m.mb / m.mg() : 0.0,
                        m.sat, K, T);
        }
        return m;
    };

    // ---- exposure bracketing at the reference gains -----------------------
    if (expUs <= 0)
    {
        const int tries[] = { 1000, 2000, 4000, 8000, 16000, 32000, 64000 };
        setWb(rDef, bDef);
        int best = tries[0];
        for (int e : tries)
        {
            ASISetControlValue(info.CameraID, ASI_EXPOSURE, e, ASI_FALSE);
            const Means m = measure("", 3, false);
            std::printf("  exposure %6d us -> full R=%6.1f G=%6.1f B=%6.1f dominant-surface G=%6.1f sat=%4.1f%%\n",
                        e, m.r, m.g(), m.b, m.mg(), m.sat);
            if (m.mg() < 205.0 && m.sat < 2.0) best = e;
        }
        expUs = best;
        std::printf("  chosen exposure %d us\n", expUs);
    }
    ASISetControlValue(info.CameraID, ASI_EXPOSURE, expUs, ASI_FALSE);
    std::printf("  exposure=%d us, gain=%d\n", expUs, gainVal);

    // ---- NEUTRAL reference: the measured 6500 K anchor -------------------
    auto neutralPoint = [&](const char* label) -> bool
    {
        setWb(rDef, bDef);
        const Means ref = measure(label, 6);
        if (ref.g() < 8) { std::printf("  (too dark for a neutral measurement)\n"); return false; }
        // The gains are linear multipliers (verified by the sweep), so the
        // gains that render a target neutral are rRef*G/R and bRef*G/B.
        auto est = [&](double R, double G, double B, const char* what) {
            if (G < 5 || R < 1 || B < 1) { std::printf("  %-16s : (unusable)\n", what); return; }
            const double rRaw = (double)rDef * G / R;
            const double bRaw = (double)bDef * G / B;
            const long rN = (long)std::clamp(rRaw, (double)rMin, (double)rMax);
            const long bN = (long)std::clamp(bRaw, (double)bMin, (double)bMax);
            int K = 0, T = 0;
            wbFromGains((int)rN, (int)bN, cal, K, T);
            // How far this fresh measurement sits from the anchor the app
            // carries. An R/B ratio error IS a temperature error: p(T) moves at
            // about -1.754e-4 per kelvin around daylight.
            const double dRatio = std::log(((double)rN * cal.bNeu) / ((double)bN * cal.rNeu));
            std::printf("  %-16s : neutral at r=%6.1f b=%6.1f -> (%3ld,%3ld) | app anchor (%.0f,%.0f): "
                        "ratio x%.3f = %+.0f K off | app displays %5d K tint %+d\n",
                        what, rRaw, bRaw, rN, bN, cal.rNeu, cal.bNeu,
                        std::exp(dRatio), dRatio / 1.754e-4, K, T);
        };
        est(ref.r,  ref.g(),  ref.b,  "full field");
        est(ref.mr, ref.mg(), ref.mb, "dominant surf.");
        // verify the estimate that matters most (the dominant surface)
        if (ref.mg() > 5 && ref.mr > 1 && ref.mb > 1)
        {
            const long rN = (long)std::clamp((double)rDef * ref.mg() / ref.mr, (double)rMin, (double)rMax);
            const long bN = (long)std::clamp((double)bDef * ref.mg() / ref.mb, (double)bMin, (double)bMax);
            setWb(rN, bN);
            measure("verified at neutral", 6);
        }
        return true;
    };

    // Leave the body in a sane state: the gains it had when the probe read them
    // (or the factory defaults with --manual), the auto flags as the caps
    // advertise them, stream stopped, camera released for the next process.
    auto leaveSane = [&] {
        long r = 0, b = 0; bool x1 = false, x2 = false;
        getWb(r, b, x1, x2);
        if (leaveManual) setWb(rDef, bDef);
        else
        {
            ASISetControlValue(info.CameraID, ASI_WB_R, r, rAuto ? ASI_TRUE : ASI_FALSE);
            ASISetControlValue(info.CameraID, ASI_WB_B, b, bAuto ? ASI_TRUE : ASI_FALSE);
        }
        ASIStopVideoCapture(info.CameraID);
        ASICloseCamera(info.CameraID);
    };

    if (neutralOnly)
    {
        neutralPoint("neutral ref @defaults");
        leaveSane();
        return 0;
    }

    // ---- 2. gain law ------------------------------------------------------
    // A WB control is a LINEAR multiplier on its own channel, so with the other
    // channel pinned ln(R/G) must move 1:1 with ln(WB_R) — slope 1.000. The
    // slope is measured between ADJACENT sweep points: relating every point to
    // the pin it is swept past goes singular there, and the near-black low end
    // of the sweep biases the ratio hard (a channel reading 3 counts is mostly
    // floor, not signal), so intervals are only taken where both channels sit
    // above 8 counts.
    const long steps[] = { 10, 20, 35, 50, 70, 85, 99 };
    auto sweepLaw = [&](bool sweepR) {
        const long vmax = sweepR ? rMax : bMax;
        std::printf("--- gain law: %s swept, %s pinned at %ld (slope 1.000 = linear multiplier) ---\n",
                    sweepR ? "WB_R" : "WB_B", sweepR ? "WB_B" : "WB_R", sweepR ? bDef : rDef);
        double prevRatio = -1.0, prevV = 0.0;
        double sum = 0.0; int n = 0; double lo = 1e9, hi = -1e9;
        for (long v : steps)
        {
            if (v > vmax) continue;
            if (sweepR) setWb(v, bDef); else setWb(rDef, v);
            const Means m = measure(sweepR ? "R sweep" : "B sweep", 4);
            const double num = sweepR ? m.r : m.b;
            const double ratio = m.g() > 0 ? num / m.g() : -1.0;
            if (num > 8.0 && m.g() > 8.0 && prevRatio > 0 && prevV > 0 && prevV != (double)v)
            {
                const double s = std::log(ratio / prevRatio) / std::log((double)v / prevV);
                std::printf("        %ld -> %ld: d ln(%s/G) / d ln(%s) = %.3f\n",
                            (long)prevV, v, sweepR ? "R" : "B", sweepR ? "WB_R" : "WB_B", s);
                sum += s; ++n; lo = std::min(lo, s); hi = std::max(hi, s);
            }
            prevRatio = ratio; prevV = (double)v;
        }
        if (n)
            std::printf("        LINEAR-LAW CHECK %s: mean slope %.3f over %d intervals (min %.3f, max %.3f)\n",
                        sweepR ? "WB_R" : "WB_B", sum / n, n, lo, hi);
    };
    sweepLaw(true);
    sweepLaw(false);
    if (lawOnly) { leaveSane(); return 0; }

    // Does the TINT slider actually change the IMAGE? Inside the window the GUI
    // offers it must; past the window the delivered pair is identical by
    // construction, which is exactly the dead travel the window exists to hide.
    auto tintLiveness = [&] {
        std::printf("--- tint liveness: inside vs beyond the deliverable window ---\n");
        const int tintK[] = { 10000, 6500 };
        for (int K : tintK)
        {
            int lo = 0, hi = 0;
            wbTintRangeForK(K, cal, lo, hi);
            std::printf("  %d K: tint window %+d..%+d\n", K, lo, hi);
            const int pts[] = { lo, (lo + hi) / 2, hi, hi + 25 };
            for (int t : pts)
            {
                int r = 0, b = 0;
                wbToGains(K, t, cal, r, b);
                setWb(r, b);
                char label[64];
                std::snprintf(label, sizeof label, "  t=%+d r=%d b=%d%s", t, r, b,
                              (t > hi || t < lo) ? " [OUTSIDE THE WINDOW]" : "");
                measure(label, 3);
            }
        }
    };
    if (tintOnly) { tintLiveness(); leaveSane(); return 0; }

    // ---- 3. neutral reference --------------------------------------------
    neutralPoint("neutral ref @defaults");

    // ---- 4. where the camera's AWB converges ------------------------------
    // (a) the ZWO demo pattern: auto flag on WB_R only, WB_B left manual
    ASISetControlValue(info.CameraID, ASI_WB_B, bDef, ASI_FALSE);
    ASISetControlValue(info.CameraID, ASI_WB_R, rDef, ASI_TRUE);
    std::printf("--- AWB (auto on WB_R only, WB_B manual) --- polling %.0f s\n", settleSecs);
    for (int i = 0; i < (int)(settleSecs / 0.5); ++i)
    {
        long r = 0, b = 0; bool a1 = false, a2 = false;
        getWb(r, b, a1, a2);
        std::printf("  t+%2.1fs r=%-4ld(auto %d) b=%-4ld(auto %d)\n", i * 0.5, r, (int)a1, b, (int)a2);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    measure("after AWB(R only)", 4);

    // (b) auto on BOTH channels (what the app does when both caps say auto)
    long rNow = 0, bNow = 0; bool a1 = false, a2 = false;
    getWb(rNow, bNow, a1, a2);
    ASISetControlValue(info.CameraID, ASI_WB_R, rNow, ASI_TRUE);
    ASISetControlValue(info.CameraID, ASI_WB_B, bNow, ASI_TRUE);
    std::printf("--- AWB (auto on BOTH channels) --- polling %.0f s\n", settleSecs);
    for (int i = 0; i < (int)(settleSecs / 0.5); ++i)
    {
        long r = 0, b = 0; bool x1 = false, x2 = false;
        getWb(r, b, x1, x2);
        std::printf("  t+%2.1fs r=%-4ld(auto %d) b=%-4ld(auto %d)\n", i * 0.5, r, (int)x1, b, (int)x2);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    measure("after AWB(both)", 4);

    // ---- 5. measured authority of the slider ends -------------------------
    std::printf("--- applied + measured: what each slider K actually delivers ---\n");
    const int probeK[] = { 2500, 3200, 4000, 5000, 6500, 8000, 8300, 9000, 10000 };
    for (int K : probeK)
    {
        int r = 0, b = 0;
        wbToGains(K, 0, cal, r, b);
        setWb(r, b);
        // u/v is the colour the pair delivers against the anchor's neutral:
        // > 1 red, < 1 blue. The whole point of the slider's cool end.
        const double uv = ((double)r / cal.rNeu) / ((double)b / cal.bNeu);
        const double auth = wbAuthorityScale(K, 0, cal);
        char label[64];
        std::snprintf(label, sizeof label, "%d K r=%d b=%d u/v=%.2f auth=%.2f", K, r, b, uv, auth);
        measure(label, 3);
    }
    std::printf("  caps: R %ld..%ld def %ld (auto %d)   B %ld..%ld def %ld (auto %d)\n",
                rMin, rMax, rDef, (int)rAuto, bMin, bMax, bDef, (int)bAuto);

    leaveSane();
    return 0;
}
