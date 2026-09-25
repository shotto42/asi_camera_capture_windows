// white_balance.h
//
// The mapping between the photographic white-balance controls the GUI shows —
// colour TEMPERATURE (K) and green/magenta TINT — and the camera's raw
// WB_R/WB_B gain controls.
//
// The SDK has no notion of a colour temperature: its whole white-balance API
// is two channel gains relative to green (ASI_WB_R / ASI_WB_B, integer units)
// plus the auto flag of ASISetControlValue (see §6.9 — measured: that flag is
// camera-GLOBAL, the camera then computes both channels). The GUI speaks
// K + Tint because that is what a white balance means to a photographer, so
// this module converts one to the other through a physical model:
//
//   * a blackbody at T K has a CIE 1931 xy chromaticity on the Planckian
//     locus (Kim et al. 2002 cubic splines, the standard approximation — it
//     reproduces standard illuminant A (2856 K) to |dx|,|dy| < 0.0005);
//   * that chromaticity expressed in linear sRGB (D65 matrix, Y = 1) is the
//     scene's RGB white point, and the gains that neutralize it are
//     proportional to the reciprocals of its channels;
//   * the model is NORMALIZED at kWbAnchorK, and that point is identified with
//     the body's MEASURED NEUTRAL PAIR (WbCal::rNeu/bNeu) — the WB_R/WB_B
//     values that actually render a daylight-lit white target neutral on THAT
//     body. Everything else follows from the probe: the sliders only ever move
//     *ratios* of that pair.
//
//   Temperature  = moving along the Planckian locus: the b/r gain RATIO,
//                  (ln b − ln r) monotonically decreasing with K.
//   Tint         = the green/magenta axis: both gains scaled by the same
//                  factor around the locus. +100 multiplies both by ×1.5,
//                  positive adds MAGENTA (the Lightroom convention — the
//                  correction for a green cast).
//
// WHY THE MEASURED PAIR AND NOT THE FACTORY DEFAULTS
// --------------------------------------------------
// The first version of this model assumed "the camera's default WB gains are
// the factory daylight balance". That assumption is FALSE, and it was the
// whole reason the GUI showed ~8300 K for a 6500 K scene while the manual
// slider's top end (10000 K) was not red at all: on the ASI178MC the defaults
// (R 70, B 90) are ~1.8x away in R/B ratio from the pair that actually
// neutralizes a white daylight target. Measured with tests/wb_probe.cpp
// (2026-09-23, a white sheet filling the frame in window daylight):
//
//   gray-world on the target at the default gains   -> neutral at R 92.0 B 66.0
//   the camera's own AWB convergence               -> R 93, B 64, and the
//       target then measures R/G 1.02 B/G 0.96 (i.e. AWB really does balance)
//   the body's default pair (70, 90)               -> the target measures
//       R/G 0.76 B/G 1.36 — visibly BLUE, not neutral
//
// So WbCal carries the neutral pair, wbMeasuredNeutral() supplies the measured
// one for the bodies we have measured, and an unmeasured body falls back to
// its factory defaults (a documented assumption, not a fact). The Kelvin
// numbers are still ESTIMATES — they model the body as a trichromatic sensor
// with sRGB-matching primaries, which is what every camera's WB UI does — but
// the anchor they hang on is now a measurement, the round trip is exact, and
// the AWB follow lands on 6500 K under 6500 K light.
#pragma once

// Slider ranges — the slider VALUES are the displayed units.
constexpr int kWbTempMinK  = 2500;   // slider range: value IS the Kelvin
constexpr int kWbTempMaxK  = 10000;
constexpr int kWbTempStepK = 50;
constexpr int kWbTintMin   = -100;   // slider range: −100 green … +100 magenta
constexpr int kWbTintMax   = 100;

// The temperature the neutral pair is identified with (daylight).
constexpr double kWbAnchorK = 6500.0;

// The calibration one body needs: its neutral anchor + the caps to clamp to.
// All four bounds must come from the probe (wbValidCal() below); the anchor
// falls back to the mid-range when the camera reports no default.
struct WbCal
{
    // The gains that render a kWbAnchorK scene NEUTRAL on this body — the
    // model's origin. Prefer wbMeasuredNeutral(); the caller falls back to the
    // camera's default gains when no measurement exists.
    double rNeu = 100.0, bNeu = 100.0;
    int    rMin = 1,   rMax = 99;           // WB_R caps from the probe (ASI178MC: 1..99)
    int    bMin = 1,   bMax = 99;           // WB_B caps from the probe (ASI178MC: 1..99)
};
inline bool wbValidCal(const WbCal& c)
{
    return c.rMax > c.rMin && c.bMax > c.bMin && c.rNeu > 0.0 && c.bNeu > 0.0;
}

// The MEASURED neutral pair of bodies we have probed with tests/wb_probe.cpp.
// `cameraName` is what the SDK reports (matched case-insensitively), and the
// measurement is only applied when the body's reported caps still match the
// ones it was taken with — a body that reports something else gets no anchor
// and keeps the caller's fallback. Returns true when an anchor was applied.
//
// Measured so far (2026-09-23, white target filling the frame, window daylight
// reported as 6500 K; the two independent estimators agreed within 1.6%):
//   ASI178MC  (R 1..99, B 1..99) -> rNeu 93, bNeu 65
bool wbMeasuredNeutral(const char* cameraName, int rMax, int bMax,
                       double& rNeu, double& bNeu);

// raw gain values -> (kelvin, tint): the exact inverse of the model, then
// quantized to the slider steps (kWbTempStepK / 1 tint unit) and clamped to the
// slider ranges. Places camera gains sampled at open or at the AWB-to-manual
// handoff onto the sliders. The manual labels show the SET values.
void wbFromGains(int rGain, int bGain, const WbCal& cal, int& kelvin, int& tint);

// (kelvin, tint) -> raw SDK gain values, rounded and clamped to the caps —
// PER CHANNEL (2026-09-23, user-reported: the temperature axis must stay on
// blue<->yellow-orange in the image). A channel that overflows its cap sits AT
// the cap; the other keeps its ideal (temperature-correct) value. Inside the
// caps both channels are exact. Kelvin outside [kWbTempMinK, kWbTempMaxK] and
// tint outside the slider range are clamped first — the slider ends pin the
// gains, never overflow them.
void wbToGains(int kelvin, int tint, const WbCal& cal, int& rGain, int& bGain);

// The colour-temperature range THIS body can reach at tint 0 with both gains
// still inside its caps — measured on the model's own scale and quantized to
// the slider step. Outside it one WB channel is pinned at its cap and the
// other carries the temperature (that is the body running out of WB authority,
// not a bug: the ASI178MC's neutral pair sits at 93 of a 99 WB_R ceiling and
// 65 of a 99 WB_B one, so it holds the full balance at tint 0 only across
// roughly 4500..7300 K — under a 3000 K light, for example, the best it can
// render is a slightly warm white, because its B gain tops out at 1.52x its
// neutral). The GUI's labels never show this — they show the set values.
void wbAuthorityRangeK(const WbCal& cal, int& kLo, int& kHi);

// How much of the requested balance the body can actually deliver at
// (kelvin, tint): 1.0 when both gains fit inside the caps, < 1.0 when a channel
// is pinned at its cap (this is the WORST channel's fit; the free channel keeps
// its ideal value, so the shortfall shows up in the delivered image only, not
// in the GUI's labels). Used by --wbtest and the probes, not by the GUI display.
double wbAuthorityScale(int kelvin, int tint, const WbCal& cal);

// The TINT window this body can actually deliver UNSCALED at `kelvin`: the
// values whose gain pair fits its caps, both channels.
// This describes the BODY, not the GUI: the manual Temperature and Tint
// settings are independent (the Temperature never clamps, moves, or disables
// the Tint), so the Tint slider keeps its full -100..+100 range at every
// temperature (this window is documentation only — the GUI has no tooltips).
// What it still measures is real: outside it one channel is pinned at its cap
// and the OTHER one moves with the tint — the free channel still carries the
// balance, but the body's headroom limits how far the delivered image can move
// on that axis. (Measured on the model with the ASI178MC's numbers — caps
// 1..99 both, neutral 93/65: at 10000 K the window is -100..-31; above -31, R
// sits at its ceiling and only B (48 at tint 0, rising with the tint) is free;
// at 2500 K the window is empty — B is pinned at 99 for the whole slider and
// only R is free.) tLo == tHi == 0 means
// "no tint authority at this temperature".
void wbTintRangeForK(int kelvin, const WbCal& cal, int& tLo, int& tHi);

// ---- pure model pieces, exposed for --wbtest -------------------------------

// Planckian-locus chromaticity (Kim et al. 2002 cubic splines, CIE 1931 xy;
// valid 1667..25000 K, clamped outside).
void wbPlanckXy(double kelvin, double& x, double& y);

// The model's WB gains (relative to G = 1) that neutralize a blackbody at
// `kelvin`: (Gs/Rs, Gs/Bs) from its linear-sRGB representation.
void wbModelGains(double kelvin, double& r, double& b);

// The model gains normalized so wbModelGains(kWbAnchorK) maps to (1, 1).
void wbNormGains(double kelvin, double& nr, double& nb);
