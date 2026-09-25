// exposure.h
//
// Logarithmic exposure mapping and value formatting.
//
//   slider position 0..1000  ->  t = tmin * (tmax/tmin)^(pos/1000)
//   tmin = 32 µs (the camera's documented minimum)
//   (tmin, tmax) per mode, see MainWindow::exposureRange():
//     photo    32 µs .. 1 s   (range switch off)  or  1 .. 60 s (switch on,
//              LINEAR in whole-second steps — sliderToSecondsLinear)
//     video    32 µs .. 1/fps (exposure must fit in one frame)
//     interval same ranges + switch as photo mode (the sequence takes its
//              exposure from the main slider; the worker still clamps to the
//              SDK max kExpSdkMaxS = 2000 s as a safety bound)
#pragma once

#include <QString>

constexpr int    kSliderMin      = 0;
constexpr int    kSliderMax      = 1000;
constexpr double kExpMinS        = 32e-6;   // 32 µs - SDK/camera documented minimum
constexpr double kExpShortMaxS   = 1.0;     // photo, range switch off: 32 µs .. 1 s
constexpr double kExpLongMinS    = 1.0;     // photo, range switch on:  1 .. 60 s
constexpr double kExpLongMaxS    = 60.0;
constexpr double kExpSdkMaxS     = 2000.0;  // 2000 s - SDK Exposure MaxValue (worker-side safety clamp)

double sliderToSeconds(int pos, double tmin, double tmax);
int    secondsToSlider(double t, double tmin, double tmax);

// Photo long range (1..60 s): LINEAR, quantized to whole-second steps
// (pos 0 -> tmin, pos 1000 -> tmax; ~17 slider ticks per 1 s).
double sliderToSecondsLinear(int pos, double tmin, double tmax);
int    secondsToSliderLinear(double t, double tmin, double tmax);

QString fmtExposure(double t);

// Format a gain value (0.1 dB units, as the SDK reports it) as dB.
// e.g. 120 -> "12.0 dB", 0 -> "0.0 dB", 400 -> "40.0 dB".
QString fmtGain(int gain01);
