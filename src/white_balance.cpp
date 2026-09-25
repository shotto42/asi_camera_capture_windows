// white_balance.cpp
//
// The Planckian-locus WB model (see white_balance.h). Pure math, no SDK, no
// Qt — headlessly tested by ./camera_app --wbtest.

#include "white_balance.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <vector>

namespace
{

// ---------------------------------------------------------------------------
// Planckian locus — Kim et al. 2002 cubic splines in CIE 1931 xy.
// (From Wikipedia, "Planckian locus"; the standard published approximation.
// Verified against standard illuminant A: xy(2856) = (0.4471, 0.4075) vs the
// CIE value (0.4476, 0.4074) — |d| < 0.0005, --wbtest pins this.)
// ---------------------------------------------------------------------------
double locusX(double T)
{
    if (T < 4000.0)
        return -0.2661239e9  / (T * T * T) - 0.2343589e6 / (T * T)
             +  0.8776956e3 / T + 0.179910;
    return -3.0258469e9  / (T * T * T) + 2.1070379e6 / (T * T)
         +  0.2226347e3 / T + 0.240390;
}

double locusY(double T, double x)
{
    if (T <= 2222.0)
        return -1.1063814 * x * x * x - 1.34811020 * x * x
             + 2.18555832 * x - 0.20219683;
    if (T <= 4000.0)
        return -0.9549476 * x * x * x - 1.37418593 * x * x
             + 2.09137015 * x - 0.16748867;
    return 3.0817580 * x * x * x - 5.87338670 * x * x
         + 3.75112997 * x - 0.37001483;
}

// Full-table range for the K inverse: the slider spans 2500..10000 K; the
// table runs 500 K wider on each side so read-back gains slightly beyond the
// slider interpolate instead of snapping.
constexpr double kInvTMin  = 2000.0;
constexpr double kInvTMax  = 12000.0;
constexpr double kInvStepK = 25.0;

// p(T) = ln(nb) - ln(nr): the temperature signature of the WB gain pair.
// Strictly decreasing in T (checked by --wbtest).
double pOfT(double T)
{
    double nr, nb;
    wbNormGains(T, nr, nb);
    return std::log(nb) - std::log(nr);
}

// q(T) = (ln nr + ln nb) / 2: the tint (green/magenta) offset at T.
double qOfT(double T)
{
    double nr, nb;
    wbNormGains(T, nr, nb);
    return 0.5 * (std::log(nr) + std::log(nb));
}

struct InverseTable
{
    std::vector<double> T, p;   // p parallel to T, strictly decreasing
};

const InverseTable& inverseTable()
{
    static const InverseTable tab = [] {
        InverseTable t;
        for (double T = kInvTMin; T <= kInvTMax + 0.5; T += kInvStepK)
        {
            t.T.push_back(T);
            t.p.push_back(pOfT(T));
        }
        return t;
    }();
    return tab;
}

// Invert p -> Kelvin (piecewise-linear over the table, clamped to the table).
double kelvinFromP(double p)
{
    const InverseTable& tab = inverseTable();
    const size_t n = tab.T.size();
    if (p >= tab.p.front()) return tab.T.front();     // warmer than the table
    if (p <= tab.p.back())  return tab.T.back();      // cooler
    size_t lo = 0, hi = n - 1;                        // p[lo] >= p >= p[hi]
    while (hi - lo > 1)
    {
        const size_t mid = (lo + hi) / 2;
        if (tab.p[mid] > p) lo = mid; else hi = mid;
    }
    const double p0 = tab.p[lo], p1 = tab.p[hi];
    const double f = (p0 == p1) ? 0.0 : (p - p0) / (p1 - p0);
    return tab.T[lo] + f * (tab.T[hi] - tab.T[lo]);
}

// Tint scale: +/-100 tint multiplies BOTH gains by e^(+/-0.4054651) = 1.5x.
constexpr double kTintLogPerUnit = 0.4054651 / 100.0;

// Case-insensitive substring test (the SDK reports "ZWO ASI178MC" on one
// build and "ASI178MC" on another, so a fragment match is the honest test).
bool nameHas(const char* name, const char* fragment)
{
    std::string hay = name ? name : "";
    std::string needle = fragment ? fragment : "";
    for (auto& c : hay)     c = (char)std::tolower((unsigned char)c);
    for (auto& c : needle)  c = (char)std::tolower((unsigned char)c);
    return !needle.empty() && hay.find(needle) != std::string::npos;
}

} // namespace

// See white_balance.h: the bodies whose NEUTRAL pair has actually been
// measured (tests/wb_probe.cpp). Adding a body means holding a white/grey
// target in the light you call daylight and running that probe — the numbers
// here are measurements of light on a sensor, not vendor constants.
bool wbMeasuredNeutral(const char* cameraName, int rMax, int bMax,
                       double& rNeu, double& bNeu)
{
    struct Measured
    {
        const char* fragment;   // case-insensitive substring of the SDK name
        int rMax, bMax;         // the caps the measurement was taken under
        double rNeu, bNeu;
    };
    static const Measured kMeasured[] = {
        // ASI178MC, 2026-09-23: white sheet filling the frame, window daylight
        // (reported 6500 K). Gray-world on the target: (92.0, 66.0); the
        // camera's own AWB converged to (93, 64) with the target measuring
        // R/G 1.02, B/G 0.96. Anchor = the mean of the two estimators.
        // Rounded to whole control units (residuals vs the two estimators:
        // <= 1.1% in R, <= 1.6% in B, i.e. about +/-120 K of the ~175 K the
        // slider's own 50 K steps already smear).
        { "178mc", 99, 99, 93.0, 65.0 },
    };
    for (const auto& m : kMeasured)
    {
        if (nameHas(cameraName, m.fragment) && rMax == m.rMax && bMax == m.bMax)
        {
            rNeu = m.rNeu;
            bNeu = m.bNeu;
            return true;
        }
    }
    return false;
}

void wbPlanckXy(double kelvin, double& x, double& y)
{
    const double T = std::clamp(kelvin, 1667.0, 25000.0);
    x = locusX(T);
    y = locusY(T, x);
}

void wbModelGains(double kelvin, double& r, double& b)
{
    double x, y;
    wbPlanckXy(kelvin, x, y);
    // xy (Y = 1) -> linear sRGB (D65 primaries).
    const double X = x / y, Y = 1.0, Z = (1.0 - x - y) / y;
    const double rs =  3.2406 * X - 1.5372 * Y - 0.4986 * Z;
    const double gs = -0.9689 * X + 1.8758 * Y + 0.0415 * Z;
    const double bs =  0.0557 * X - 0.2040 * Y + 1.0570 * Z;
    // Gains that neutralize this white point, normalized to G = 1.
    r = gs / rs;
    b = gs / bs;
}

void wbNormGains(double kelvin, double& nr, double& nb)
{
    static const auto anchor = [] {
        double r, b;
        wbModelGains(kWbAnchorK, r, b);
        return std::pair(r, b);
    }();
    double r, b;
    wbModelGains(kelvin, r, b);
    nr = r / anchor.first;
    nb = b / anchor.second;
}

namespace {
// How far the model pair (r, b) must be scaled to fit the body's caps, capped at
// 1.0 (== the tightest per-channel fit: wbToGains clamps each channel to its
// cap and this says how far the WORST channel is from its ideal value; 1.0
// when both channels fit — the full requested balance is deliverable). Also
// returns the effective cap window (min/max order normalized). If the box is
// so tight that nothing fits, the smallest legal scale is returned instead:
// that pair is the body's best effort.
double wbFitScale(double r, double b, const WbCal& cal,
                  int& rLo, int& rHi, int& bLo, int& bHi)
{
    rLo = std::min(cal.rMin, cal.rMax); rHi = std::max(cal.rMin, cal.rMax);
    bLo = std::min(cal.bMin, cal.bMax); bHi = std::max(cal.bMin, cal.bMax);
    const double lo = std::max((double)rLo / r, (double)bLo / b);
    const double hi = std::min(std::min((double)rHi / r, (double)bHi / b), 1.0);
    return (hi >= lo) ? hi : lo;
}
} // namespace

void wbToGains(int kelvin, int tint, const WbCal& cal, int& rGain, int& bGain)
{
    const double K  = (double)std::clamp(kelvin, kWbTempMinK, kWbTempMaxK);
    const double t  = (double)std::clamp(tint,  kWbTintMin,   kWbTintMax);
    double nr, nb;
    wbNormGains(K, nr, nb);
    const double s = std::exp(kTintLogPerUnit * t);   // tint: both gains ×same
    // The body's measured neutral pair is the origin of the whole scale.
    const double r = cal.rNeu * nr * s;
    const double b = cal.bNeu * nb * s;
    // Fit into the camera's caps by clamping each channel INDEPENDENTLY
    // (2026-09-23, user-reported: "reducing the temperature should shift the
    // image blue, it shifts green"). When one channel overflows its cap it
    // sits AT the cap and the OTHER keeps its ideal (temperature-correct)
    // value. The earlier build scaled the whole pair down to keep the R/B
    // ratio exact — but under a warm light (e.g. a 3000 K source, where the
    // B photodiode sees ~0.5% of the green signal and this body's B gain tops
    // out at 1.52x its neutral) that dragged the free R channel below neutral
    // too: (23, 99) rendered the 3000 K light as a GREEN image instead of
    // cooling it along the temperature axis. Per-channel clamping is what
    // real AWB hardware does: the free channel carries the temperature
    // (blue<->yellow-orange), the pinned one delivers the most of its axis
    // the body can. Inside the caps both channels are exact (unchanged).
    // Real case on the ASI178MC, whose neutral pair measures 93/65: WB_B tops
    // out at 99 = 1.52x of it, so every request cooler than ~4500 K overflows
    // B (it stays at 99, R holds its temperature value), and R overflows at
    // ~7300 K.
    const int rLo = std::min(cal.rMin, cal.rMax), rHi = std::max(cal.rMin, cal.rMax);
    const int bLo = std::min(cal.bMin, cal.bMax), bHi = std::max(cal.bMin, cal.bMax);
    rGain = std::clamp((int)std::llround(r), rLo, rHi);
    bGain = std::clamp((int)std::llround(b), bLo, bHi);
}

double wbAuthorityScale(int kelvin, int tint, const WbCal& cal)
{
    const double K = (double)std::clamp(kelvin, kWbTempMinK, kWbTempMaxK);
    const double t = (double)std::clamp(tint,  kWbTintMin,   kWbTintMax);
    double nr = 1.0, nb = 1.0;
    wbNormGains(K, nr, nb);
    const double s = std::exp(kTintLogPerUnit * t);
    int rLo = 0, rHi = 0, bLo = 0, bHi = 0;
    return wbFitScale(cal.rNeu * nr * s, cal.bNeu * nb * s, cal, rLo, rHi, bLo, bHi);
}

void wbTintRangeForK(int kelvin, const WbCal& cal, int& tLo, int& tHi)
{
    // "Live" means wbToGains does not have to scale this pair down: the moment
    // it scales, the scale factor cancels the tint factor exactly (both act as
    // one factor on the pair), so all values past the boundary deliver the same
    // gains and the slider would be inert. The live set is an interval — tint is
    // one factor, monotone in t — so walking in from both ends finds it.
    auto live = [&](int t) { return wbAuthorityScale(kelvin, t, cal) >= 1.0 - 1e-12; };
    int a = kWbTintMin, b = kWbTintMax;
    while (a <= b && !live(a)) ++a;
    while (b >= a && !live(b)) --b;
    if (a > b)
    {
        // No tint at this temperature is deliverable unscaled (the temperature
        // slider is already pushing a channel to its cap): report a dead range
        // and let the GUI disable the control rather than move nothing.
        tLo = tHi = 0;
        return;
    }
    tLo = a; tHi = b;
}

void wbFromGains(int rGain, int bGain, const WbCal& cal, int& kelvin, int& tint)
{
    // (rNeu/bNeu are > 0 by the valid-cal contract; gains read back from the
    // SDK can be 0 before the first apply, hence the max(1)).
    const double u = std::log(std::max(1.0, (double)rGain) / cal.rNeu);
    const double v = std::log(std::max(1.0, (double)bGain) / cal.bNeu);

    // The b/r ratio pins the temperature; the tint factor cancels in v - u.
    const double p = v - u;
    double K = kelvinFromP(p);
    kelvin = (int)std::llround(K / kWbTempStepK) * kWbTempStepK;
    kelvin = std::clamp(kelvin, kWbTempMinK, kWbTempMaxK);

    // The remaining degree of freedom is the green/magenta offset, measured
    // against the (quantized) slider temperature so the displayed pair is
    // exactly what the sliders would send back.
    const double q = 0.5 * (u + v) - qOfT((double)kelvin);
    tint = (int)std::llround(q / kTintLogPerUnit);
    tint = std::clamp(tint, kWbTintMin, kWbTintMax);
}

void wbAuthorityRangeK(const WbCal& cal, int& kLo, int& kHi)
{
    // Which slider temperatures need NO scale-down at tint 0, i.e. both gains
    // of the model pair already sit inside the body's caps. The fitting set is
    // contiguous (the pair runs monotonically in both channels along the
    // locus), so first/last bound the range.
    int firstFit = -1, lastFit = -1;
    for (int K = kWbTempMinK; K <= kWbTempMaxK; K += kWbTempStepK)
    {
        double nr, nb;
        wbNormGains((double)K, nr, nb);
        const double r = cal.rNeu * nr, b = cal.bNeu * nb;
        if (r < (double)cal.rMin || r > (double)cal.rMax ||
            b < (double)cal.bMin || b > (double)cal.bMax) continue;
        if (firstFit < 0) firstFit = K;
        lastFit = K;
    }
    kLo = (firstFit < 0) ? kWbTempMinK : firstFit;
    kHi = (lastFit  < 0) ? kWbTempMaxK : lastFit;
}
