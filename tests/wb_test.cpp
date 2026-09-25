// wb_test.cpp — headless white-balance model self-test (`--wbtest`).
//
// Pins down white_balance.h: the Planckian-locus anchors (the Kim splines must
// reproduce standard illuminant A, or every Kelvin number the GUI shows drifts),
// the monotonicity of the temperature signature p(T) = ln(b/r) that the
// inverse slider mapping relies on, the exact neutral anchor at the body's
// MEASURED neutral pair, both slider directions (warmer -> more R / less B
// gain; positive tint -> BOTH gains up), an exact round trip gain->(K,tint)->gain
// over the whole slider surface while nothing clamps, the measured ASI178MC
// anchor (the regression test for "AWB reads 8300 K under 6500 K light" and
// "10000 K is not red at all" — both came from anchoring on the factory
// default gains), and the clamping behaviour at the caps.

#include "white_balance.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>

namespace
{
void report(const char* what, bool pass)
{
    std::printf("[wb] %s %s\n", pass ? "OK  " : "FAIL", what);
}

// A wide-open calibration: nothing clamps, so the round trip must be exact.
WbCal openCal()
{
    WbCal c;
    c.rMin = 1;     c.rMax = 100000;
    c.bMin = 1;     c.bMax = 100000;
    c.rNeu = 1000;  c.bNeu = 1000;
    return c;
}

// The connected ASI178MC: its probed caps (2026-09-23) + the anchor that
// wbMeasuredNeutral() supplies for it.
WbCal mcCal()
{
    WbCal c;
    c.rMin = 1; c.rMax = 99;
    c.bMin = 1; c.bMax = 99;
    double r = 0, b = 0;
    if (!wbMeasuredNeutral("ZWO ASI178MC", 99, 99, r, b)) { r = 93; b = 65; }
    c.rNeu = r; c.bNeu = b;
    return c;
}

bool near(double a, double b, double eps) { return std::fabs(a - b) <= eps; }

// The colour the pair actually delivers, as the rendered channel ratios against
// the neutral point: u = R/G, v = B/G. u/v > 1 is RED (the temperature axis),
// u*v is the level/magenta axis. This is the only honest way to ask "is the top
// of the slider red?".
struct Delivered { double u, v; };
Delivered delivered(int K, int tint, const WbCal& c)
{
    int r = 0, b = 0;
    wbToGains(K, tint, c, r, b);
    return Delivered{ (double)r / c.rNeu, (double)b / c.bNeu };
}

bool testLocus()
{
    bool ok = true;

    // Standard illuminant A = a blackbody at 2856 K: CIE xy = (0.4476, 0.4074).
    double x, y;
    wbPlanckXy(2856.0, x, y);
    const bool aOk = near(x, 0.4476, 0.002) && near(y, 0.4074, 0.002);
    std::printf("[wb]      A 2856 K -> (%.4f, %.4f), CIE (0.4476, 0.4074)\n", x, y);
    report("Planckian locus hits illuminant A", aOk);
    ok = ok && aOk;

    // The Planckian point at 6500 K (NOT D65): x 0.3136, y 0.3236.
    wbPlanckXy(6500.0, x, y);
    const bool dOk = near(x, 0.3136, 0.002) && near(y, 0.3236, 0.002);
    std::printf("[wb]      6500 K   -> (%.4f, %.4f), blackbody (0.3136, 0.3236)\n", x, y);
    report("Planckian locus hits 6500 K", dOk);
    ok = ok && dOk;
    return ok;
}

bool testMonotonic()
{
    // p(T) = ln(nb/nr) must strictly decrease over the whole inverse table
    // range (2000..12000 K at 25 K steps — kelvinFromP interpolates it).
    bool ok = true;
    double prev = 0.0;
    bool first = true;
    for (int T = 2000; T <= 12000; T += 25)
    {
        double nr, nb;
        wbNormGains((double)T, nr, nb);
        const double p = std::log(nb) - std::log(nr);
        if (!first && !(p < prev)) ok = false;
        prev = p;
        first = false;
    }
    report("p(T) = ln(nb/nr) strictly decreasing over the inverse range", ok);
    return ok;
}

bool testAnchor()
{
    bool ok = true;
    // The model is normalized to (1, 1) at the anchor...
    double nr, nb;
    wbNormGains(kWbAnchorK, nr, nb);
    const bool nOk = near(nr, 1.0, 1e-12) && near(nb, 1.0, 1e-12);
    report("model gains are (1,1) at the anchor temperature", nOk);
    ok = ok && nOk;

    // ...so at (anchor K, tint 0) the raw gains ARE the body's neutral pair.
    WbCal c = openCal();
    c.rNeu = 777; c.bNeu = 432;
    int r = 0, b = 0;
    wbToGains((int)kWbAnchorK, 0, c, r, b);
    const bool aOk = r == 777 && b == 432;
    std::printf("[wb]      anchor gains (%d, %d), want (777, 432)\n", r, b);
    report("anchor (6500 K, tint 0) maps to the body's neutral pair", aOk);
    ok = ok && aOk;

    // ...and the inverse lands there too.
    int K = 0, tint = 0;
    wbFromGains(777, 432, c, K, tint);
    const bool iOk = K == (int)kWbAnchorK && tint == 0;
    std::printf("[wb]      anchor inverse (%d K, tint %d), want (6500, 0)\n", K, tint);
    report("neutral pair maps back to (6500 K, tint 0)", iOk);
    ok = ok && iOk;
    return ok;
}

bool testDirections()
{
    const WbCal c = openCal();
    int rLo, bLo, rMid, bMid, rHi, bHi;
    wbToGains(3000, 0, c, rLo, bLo);      // warm
    wbToGains(6500, 0, c, rMid, bMid);    // anchor
    wbToGains(9000, 0, c, rHi, bHi);      // cool
    // Warmer light: the camera needs LESS R gain and MORE B gain (to damp the
    // red and boost the blue of a tungsten white). Colder: the opposite.
    const bool warm = rLo < rMid && bLo > bMid;
    std::printf("[wb]      3000K (%d,%d) 6500K (%d,%d) 9000K (%d,%d)\n",
                rLo, bLo, rMid, bMid, rHi, bHi);
    report("temperature direction: warmer -> lower R gain, higher B gain", warm);

    // Positive tint scales BOTH gains up (magenta = R+B pushed against G).
    int r0, b0, rp, bp;
    wbToGains(6500, 0, c, r0, b0);
    wbToGains(6500, +50, c, rp, bp);
    const bool tint = rp > r0 && bp > b0;
    report("tint direction: positive tint raises BOTH gains (magenta)", tint);

    // Tint is a pure product: (r*r)/(...) — the b/r ratio must not move.
    // In floats: r(6500,+50)/r(6500,-50) == b(6500,+50)/b(6500,-50).
    int rm, bm;
    wbToGains(6500, -50, c, rm, bm);
    const double rr = (double)rp / (double)rm, rb = (double)bp / (double)bm;
    const bool prod = near(rr, rb, 0.05) && rr > 1.2;
    std::printf("[wb]      tint ratio r-side %.4f vs b-side %.4f\n", rr, rb);
    report("tint moves both gains by the same factor (pure green/magenta axis)", prod);
    return warm && tint && prod;
}

bool testRoundTrip()
{
    bool ok = true;
    const WbCal c = openCal();
    int worstK = 0, worstT = 0;
    bool clamped = false;
    for (int K = kWbTempMinK; K <= kWbTempMaxK; K += kWbTempStepK)
    {
        for (int t = kWbTintMin; t <= kWbTintMax; t += 25)
        {
            int r = 0, b = 0;
            wbToGains(K, t, c, r, b);
            if (r <= c.rMin + 1 || r >= c.rMax - 1 || b <= c.bMin + 1 || b >= c.bMax - 1)
            { clamped = true; continue; }     // exactness only asserted unclamped
            int K2 = 0, t2 = 0;
            wbFromGains(r, b, c, K2, t2);
            const int dK = std::abs(K2 - K), dT = std::abs(t2 - t);
            if (dK > worstK) worstK = dK;
            if (dT > worstT) worstT = dT;
            // K comes back within one table-interpolation step; tint within
            // 2 units (integer gain rounding around large anchors).
            if (dK > kWbTempStepK || dT > 2)
            {
                std::printf("[wb] round-trip FAIL at (%d K, tint %d): gains (%d, %d) -> (%d K, tint %d)\n",
                            K, t, r, b, K2, t2);
                ok = false;
            }
        }
    }
    std::printf("[wb] round trip: worst |dK| = %d, worst |dtint| = %d %s\n",
                worstK, worstT, clamped ? "(some clamps skipped)" : "(no clamps hit)");
    report("(K, tint) -> gains -> (K, tint) round trip", ok);
    return ok;
}

// The measured anchor: the regression test for the two reported symptoms.
// Both came from the model hanging its 6500 K point on the camera's DEFAULT
// gains (ASI178MC: R 70, B 90) — which do NOT render a daylight-lit white
// target neutral (measured: that target sits at R/G 0.76, B/G 1.36 there,
// visibly blue). Consequences: the camera's correct AWB gains read thousands
// of K too high, and the top of the temperature slider was not red at all.
bool testMeasuredAnchor()
{
    bool ok = true;

    // wbMeasuredNeutral answers for the body we measured, and refuses when the
    // caps no longer match (a body that reports other caps must not inherit
    // someone else's anchor).
    double r = 0, b = 0;
    const bool hit  = wbMeasuredNeutral("ZWO ASI178MC", 99, 99, r, b);
    const bool hit2 = wbMeasuredNeutral("asi178mc", 99, 99, r, b);   // case-insensitive
    const bool missName = !wbMeasuredNeutral("ZWO ASI2600MC", 99, 99, r, b);
    const bool missCaps = !wbMeasuredNeutral("ZWO ASI178MC", 210, 210, r, b);
    std::printf("[wb]      measured anchor for ASI178MC: (%.1f, %.1f)\n", r, b);
    report("measured anchor: found by name, refused on unknown name/other caps",
           hit && hit2 && missName && missCaps);
    ok = ok && hit && hit2 && missName && missCaps;

    const WbCal mc = mcCal();

    // 1. The measured neutral pair IS 6500 K, tint 0 (that is what the anchor
    //    means), and it lands within a slider step / a couple of tint units.
    int K = 0, t = 0;
    wbFromGains((int)mc.rNeu, (int)mc.bNeu, mc, K, t);
    const bool anchorOk = std::abs(K - (int)kWbAnchorK) <= kWbTempStepK && std::abs(t) <= 2;
    std::printf("[wb]      pair (%d, %d) reads (%d K, tint %d), want 6500 K / ~0\n",
                (int)mc.rNeu, (int)mc.bNeu, K, t);
    report("measured neutral pair displays as 6500 K", anchorOk);
    ok = ok && anchorOk;

    // 2. The camera's AWB convergence measured on the white target (R 93/B 64)
    //    must display as daylight, NOT as the 8300-10000 K the old anchor showed.
    wbFromGains(93, 64, mc, K, t);
    const bool awbOk = std::abs(K - (int)kWbAnchorK) <= 3 * kWbTempStepK;
    std::printf("[wb]      measured AWB gains (93, 64) read %d K (was 10000 K before)\n", K);
    report("camera AWB gains under 6500 K light display as ~6500 K", awbOk);
    ok = ok && awbOk;

    // 3. The slider ends must be RED / BLUE relative to neutral. Per-channel
    //    clamping delivers the body's MAXIMUM red/blue at the ends: the top of
    //    the slider pushes R to its cap (above neutral) with B below neutral,
    //    the bottom pins B at its cap (far above neutral) with R below neutral.
    const Delivered cool = delivered(kWbTempMaxK, 0, mc);
    const Delivered warm = delivered(kWbTempMinK, 0, mc);
    const Delivered neut = delivered((int)kWbAnchorK, 0, mc);
    const bool redEnd   = cool.u > 1.05 && cool.v < 0.80;
    const bool blueEnd  = warm.v > 1.40 && warm.u < 0.50;
    const bool neutralMid = near(neut.u, 1.0, 0.02) && near(neut.v, 1.0, 0.02);
    std::printf("[wb]      delivered R/G,B/G: %d K -> (%.3f, %.3f) u/v=%.2f | %d K -> u/v=%.3f | "
                "6500 K -> (%.3f, %.3f)\n",
                kWbTempMaxK, cool.u, cool.v, cool.u / cool.v, kWbTempMinK, warm.u / warm.v,
                neut.u, neut.v);
    report("top of the slider is RED (R above neutral, B below) and the bottom is BLUE (B far above neutral, R below)",
           redEnd && blueEnd);
    report("6500 K delivers an actually neutral pair", neutralMid);
    ok = ok && redEnd && blueEnd && neutralMid;

    // 4. The body's own authority, straight from the caps: the ASI178MC holds
    //    tint 0 only over part of the slider, and the GUI says so.
    int kLo = 0, kHi = 0;
    wbAuthorityRangeK(mc, kLo, kHi);
    const bool authOk = kLo > kWbTempMinK && kHi < kWbTempMaxK && kLo > 4000 && kHi < 8000;
    std::printf("[wb]      ASI178MC tint-0 authority: %d..%d K of %d..%d\n",
                kLo, kHi, kWbTempMinK, kWbTempMaxK);
    report("authority range reported (the MC is capped at both ends)", authOk);
    ok = ok && authOk;

    // 5. wbAuthorityScale: 1.0 where
    //    the pair fits, and everywhere else exactly the shortfall the delivered
    //    pair shows (the GUI must not quote a level the gains do not bear out).
    const double sMid  = wbAuthorityScale(6500, 0, mc);
    const double sWarm = wbAuthorityScale(kWbTempMaxK, 0, mc);
    const double sCool = wbAuthorityScale(kWbTempMinK, 0, mc);
    double nr = 1.0, nb = 1.0;
    wbNormGains((double)kWbTempMaxK, nr, nb);
    const double scaleSeen = cool.u / nr;   // delivered/ideal on the R side
    const bool scaleOk = near(sMid, 1.0, 1e-9) && sWarm < 1.0 && sCool < 1.0
                      && near(sWarm, scaleSeen, 0.02);
    std::printf("[wb]      authority scale: 6500 K %.2f, %d K %.2f (pair says %.2f), %d K %.2f\n",
                sMid, kWbTempMaxK, sWarm, scaleSeen, kWbTempMinK, sCool);
    report("authority scale is 1.0 inside the range and matches the delivered pair outside",
           scaleOk);
    ok = ok && scaleOk;

    return ok;
}

// The deliverable Tint WINDOW this body can deliver UNSCALED at the current
// temperature (both channels inside their caps — the full requested balance).
// The manual sliders themselves are independent (fixed full Tint range; the
// Temperature never clamps or moves the Tint), so this window is what the Tint
// TOOLTIP states as information, not the slider's range. Outside it one
// channel is pinned at its cap and the free one keeps moving with the tint
// (per-channel clamping, wbToGains): the delivered image still moves, but only
// along whatever axis the body has headroom for.
bool testTintRange()
{
    bool ok = true;
    const WbCal mc = mcCal();

    // 1. Mid-range: a real window, containing neutral, narrower than the slider.
    int lo = 0, hi = 0;
    wbTintRangeForK(6500, mc, lo, hi);
    const bool midWindow = lo <= -50 && hi > 0 && hi < kWbTintMax;
    std::printf("[wb]      tint window @6500 K: %d..%+d of %+d..%+d\n",
                lo, hi, kWbTintMin, kWbTintMax);
    report("mid-range tint window is real, holds 0, and is narrower than the slider",
           midWindow);

    // 2. Inside the window every step must be DELIVERABLE (no clamping) and
    //    the pair must actually move; outside it the CAPPED channel must be
    //    pinned at its cap (constant) while the FREE one keeps tracking the
    //    tint — the delivered image still moves along the axis the body has
    //    headroom for (per-channel clamping, 2026-09-23: the earlier pair-scale
    //    froze the whole pair outside the window, which under a warm light read
    //    back as a GREEN image instead of a cool one). The GUI's labels show
    //    the SET values; the headroom is documentation only (no tooltips).
    int prevR = -1, prevB = -1, moved = 0;
    bool allFit = true;
    for (int t = lo; t <= hi; ++t)
    {
        if (wbAuthorityScale(6500, t, mc) < 1.0 - 1e-12) allFit = false;
        int r = 0, b = 0;
        wbToGains(6500, t, mc, r, b);
        if (r != prevR || b != prevB) ++moved;
        prevR = r; prevB = b;
    }
    int rAt = 0, bAt = 0;
    wbToGains(6500, hi, mc, rAt, bAt);
    // Above the 6500 K window the R cap binds: R must stay at its cap, and B
    // (the free channel) must keep taking distinct values with the tint.
    bool pinnedAbove = true;
    int distinctAbove = 0, prevFree = -1;
    for (int t = hi + 1; t <= kWbTintMax; ++t)
    {
        int r = 0, b = 0;
        wbToGains(6500, t, mc, r, b);
        if (r != rAt) pinnedAbove = false;
        if (b != prevFree) { ++distinctAbove; prevFree = b; }
    }
    int rLo = 0, bLo = 0;
    wbToGains(6500, lo, mc, rLo, bLo);
    const int win = hi - lo + 1;
    // Integer gain units quantize the tint axis (one unit is ×1.004, and 0.4 of a
    // count on R changes nothing), so "every value moves the pair" is the wrong
    // bar — the delivered COLOUR spanning the window is the right one.
    const double uLo = (double)rLo / mc.rNeu, uHi = (double)rAt / mc.rNeu;
    const bool live = allFit && moved >= 20 && uHi / uLo >= 1.4;
    const bool outside = pinnedAbove && distinctAbove >= 10;
    std::printf("[wb]      window: %d values, %d distinct pairs, all fit=%d | R/neutral "
                "%.3f..%.3f (x%.2f) | above window: R pinned at %d, B takes %d distinct values\n",
                win, moved, (int)allFit, uLo, uHi, uHi / uLo, rAt, distinctAbove);
    report("inside the window the gains move; outside, the capped channel is pinned and the free one tracks the tint",
           live && outside);

    // 3. At the saturated ends the window shrinks to what is left (the top of the
    //    temperature slider has NO magenta left on this body: WB_R is at its cap).
    wbTintRangeForK(kWbTempMaxK, mc, lo, hi);
    const bool warmEnd = hi < 0;
    std::printf("[wb]      tint window @%d K: %d..%+d (no magenta above neutral left)\n",
                kWbTempMaxK, lo, hi);
    wbTintRangeForK(kWbTempMinK, mc, lo, hi);
    const bool coldEnd = lo == 0 && hi == 0;    // nothing deliverable at all -> disabled
    std::printf("[wb]      tint window @%d K: %d..%d (dead: WB_B pinned)\n",
                kWbTempMinK, lo, hi);
    report("saturated ends report a narrowed / dead tint window", warmEnd && coldEnd);

    // 4. The narrowing is the BODY, not the model: a camera with room to spare
    //    keeps the whole slider.
    WbCal big = mc;
    big.rMax = 4096; big.bMax = 4096;
    int blo = 0, bhi = 0;
    wbTintRangeForK(kWbTempMaxK, big, blo, bhi);
    const bool roomy = blo == kWbTintMin && bhi == kWbTintMax;
    std::printf("[wb]      same model, caps 1..4096: window %d..%+d at %d K\n",
                blo, bhi, kWbTempMaxK);
    report("a body with WB headroom keeps the full tint range", roomy);

    ok = ok && midWindow && live && outside && warmEnd && coldEnd && roomy;
    return ok;
}

bool testClamping()
{
    bool ok = true;

    // A deliberately tiny cap window around the anchor: every model move,
    // warm or cool, must pin the gains at the caps — the UI sliders must
    // never ask the camera for a value it cannot take.
    WbCal c;
    c.rMin = 100; c.rMax = 120; c.rNeu = 110;
    c.bMin = 50;  c.bMax = 60;  c.bNeu = 55;
    int r = 0, b = 0;
    wbToGains(2500, 0, c, r, b);       // warm: r wants 44, b wants 317
    const bool warm = r == 100 && b == 60;
    std::printf("[wb]      clamp warm: (%d, %d), want (100, 60)\n", r, b);
    report("gains clamp at the caps towards the warm end", warm);

    wbToGains(10000, 0, c, r, b);      // cool: r wants 133, b wants 40
    const bool cool = r == 120 && b == 50;
    std::printf("[wb]      clamp cool: (%d, %d), want (120, 50)\n", r, b);
    report("gains clamp at the caps towards the cool end", cool);

    // Out-of-range slider INPUTS are clamped to the slider ranges first —
    // the result must equal the clamped-slider call, never a wild gain.
    int r1 = 0, b1 = 0;
    wbToGains(999999, 999999, c, r, b);
    wbToGains(kWbTempMaxK, kWbTintMax, c, r1, b1);
    const bool inHi = r == r1 && b == b1;
    wbToGains(-999999, -999999, c, r, b);
    wbToGains(kWbTempMinK, kWbTintMin, c, r1, b1);
    const bool inLo = r == r1 && b == b1;
    report("out-of-range K/tint inputs clamp first", inHi && inLo);

    // The inverse never reports outside the slider ranges, whatever comes
    // back from a wild camera (extreme gains, even a zero before the first
    // apply).
    int K = 0, t = 0;
    wbFromGains(c.rMax, c.bMin, c, K, t);
    const bool oob1 = K >= kWbTempMinK && K <= kWbTempMaxK &&
                      t >= kWbTintMin && t <= kWbTintMax;
    wbFromGains(c.rMin, c.bMax, c, K, t);
    const bool oob2 = K >= kWbTempMinK && K <= kWbTempMaxK &&
                      t >= kWbTintMin && t <= kWbTintMax;
    wbFromGains(0, 0, c, K, t);
    const bool oob3 = K >= kWbTempMinK && K <= kWbTempMaxK &&
                      t >= kWbTintMin && t <= kWbTintMax;
    std::printf("[wb]      inverse zero gains -> (%d K, %d)\n", K, t);
    report("inverse clamps into the slider ranges", oob1 && oob2 && oob3);
    ok = ok && warm && cool && inHi && inLo && oob1 && oob2 && oob3;

    // The REAL ASI178MC caps (probed 2026-09-23: R 1..99, B 1..99) around the
    // MEASURED neutral pair (93, 65): the blue channel tops out at 99 = 1.52x
    // of the neutral, the red one at 99 = 1.06x, so BOTH ends of the slider
    // overflow. 2026-09-23, user: "reducing the temperature should shift the
    // image blue, it shifts green." The old build scaled the whole pair to keep
    // the R/B ratio exact, but under a warm light that dragged the free R
    // channel below neutral too (at 3000 K: (23, 99) rendered the 3000 K light
    // as a GREEN image). wbToGains now clamps each channel to its cap
    // independently: the overflowing channel sits at the cap, the free channel
    // keeps its ideal (temperature-correct) value — the temperature axis stays
    // on blue<->yellow-orange in the image, and the apparent balance is pulled
    // towards the full-balance band (the authority range).
    const WbCal mc = mcCal();
    double nrm, nbm;
    for (int Kx : { kWbTempMinK, kWbTempMaxK })
    {
        wbToGains(Kx, 0, mc, r, b);
        wbNormGains((double)Kx, nrm, nbm);
        const double wantR = mc.rNeu * nrm, wantB = mc.bNeu * nbm;
        const int rCap = std::max(mc.rMin, mc.rMax), bCap = std::max(mc.bMin, mc.bMax);
        // The free channel keeps the ideal (within rounding); the pinned one
        // sits at its cap.
        const int rWant = (wantR > rCap) ? rCap : (int)std::llround(wantR);
        const int bWant = (wantB > bCap) ? bCap : (int)std::llround(wantB);
        const bool perChannelOk = std::abs(r - rWant) <= 1 && std::abs(b - bWant) <= 1;
        int K2 = 0, t2 = 0;
        wbFromGains(r, b, mc, K2, t2);
        // The apparent temperature is pulled towards the authority band: at
        // the cool end the pinned B makes the pair look WARMER than set
        // (K2 >= Kx), at the warm end the pinned R makes it look COOLER.
        const bool pulled = (Kx < kWbAnchorK) ? (K2 >= Kx) : (K2 <= Kx);
        std::printf("[wb]      MC caps @ %5d K -> (%3d, %3d): free channel keeps its ideal "
                    "(%.1f/%.1f), pinned at the cap; reads back as (%d K, tint %+d)\n",
                    Kx, r, b, wantR, wantB, K2, t2);
        report("measured MC caps: per-channel clamp, the free channel holds the temperature",
               perChannelOk && pulled);
        ok = ok && perChannelOk && pulled;
    }

    return ok;
}

bool testAwbFollow()
{
    // The GUI's follow path: an arbitrary pair of camera gains maps to a
    // (K, tint) display state whose sliders would send NEARBY gains back —
    // the hand-over on an AWB toggle switches to wbCurR_/wbCurB_ (exact),
    // but a user nudging a slider afterwards starts from the quantized state,
    // so the quantization must be tight: the followed anchor is at most one
    // step + the quantization error away.
    bool ok = true;
    const WbCal c = openCal();
    for (int rNeu : {1000, 1234}) for (int bNeu : {1000, 876})
    {
        WbCal cc = c; cc.rNeu = rNeu; cc.bNeu = bNeu;
        for (int K = 3000; K <= 9000; K += 1500) for (int t : {-40, 0, 70})
        {
            int r = 0, b = 0;
            wbToGains(K, t, cc, r, b);
            int K2 = 0, t2 = 0;
            wbFromGains(r, b, cc, K2, t2);
            // Re-quantized display: within one slider step in K and 2 in tint.
            if (std::abs(K2 - K) > kWbTempStepK || std::abs(t2 - t) > 2)
            {
                std::printf("[wb] follow FAIL (%d K, %d) @neu(%d,%d): (%d,%d) -> (%d, %d)\n",
                            K, t, rNeu, bNeu, r, b, K2, t2);
                ok = false;
            }
        }
    }
    report("AWB follow: any camera gain pair lands the sliders at its own state", ok);
    return ok;
}
} // namespace

bool runWhiteBalanceSelfTest()
{
    std::printf("==== white-balance model self-test ====\n");
    bool ok = true;
    ok = testLocus()           && ok;
    ok = testMonotonic()       && ok;
    ok = testAnchor()          && ok;
    ok = testDirections()      && ok;
    ok = testRoundTrip()       && ok;
    ok = testMeasuredAnchor()  && ok;
    ok = testTintRange()        && ok;
    ok = testClamping()        && ok;
    ok = testAwbFollow()       && ok;
    std::printf("WBTEST %s\n", ok ? "PASS" : "FAIL");
    return ok;
}
