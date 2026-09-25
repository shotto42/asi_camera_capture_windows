// exposure.cpp
//
// Logarithmic exposure mapping and value formatting (see exposure.h).

#include "exposure.h"

#include <algorithm>
#include <cmath>

double sliderToSeconds(int pos, double tmin, double tmax)
{
    double f = (double)pos / kSliderMax;
    return tmin * std::pow(tmax / tmin, f);
}

int secondsToSlider(double t, double tmin, double tmax)
{
    t = std::clamp(t, tmin, tmax);
    double f = std::log(t / tmin) / std::log(tmax / tmin);
    return (int)std::lround(std::clamp(f, 0.0, 1.0) * kSliderMax);
}

double sliderToSecondsLinear(int pos, double tmin, double tmax)
{
    double f = (double)pos / kSliderMax;
    double t = tmin + (tmax - tmin) * f;
    return (double)std::clamp(std::llround(t), (long long)tmin, (long long)tmax);
}

int secondsToSliderLinear(double t, double tmin, double tmax)
{
    t = std::clamp(t, tmin, tmax);
    double f = (t - tmin) / (tmax - tmin);
    return (int)std::lround(std::clamp(f, 0.0, 1.0) * kSliderMax);
}

QString fmtExposure(double t)
{
    if (t < 0.001) return QString::number(t * 1e6, 'f', 0) + " µs";  // < 1 ms -> µs
    if (t < 0.01)  return QString::number(t * 1000.0, 'f', 2) + " ms";
    if (t < 1.0)   return QString::number(t * 1000.0, 'f', 1) + " ms";
    if (t < 3600.0 && std::abs(t - std::llround(t)) < 1e-9)
        return QString::number((long)std::llround(t)) + " s";       // whole seconds (1 s steps)
    if (t < 3600.0) return QString::number(t, 'f', 1) + " s";
    return QString::number(t / 60.0, 'f', 1) + " min";              // >= 1 h
}

QString fmtGain(int gain01)
{
    return QString::number(gain01 / 10.0, 'f', 1) + " dB";
}
