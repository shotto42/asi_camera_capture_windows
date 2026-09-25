// histogram_widget.h
//
// HistogramWidget: 128-bin brightness histogram of the current frame.
// The worker fills the bins from the raw capture (8-bit or 14-bit), so the
// shape reflects the full dynamic range of the selected bit depth.
#pragma once

#include <array>
#include <QWidget>

#include "constants.h"

class HistogramWidget : public QWidget
{
public:
    explicit HistogramWidget(QWidget* parent = nullptr);

    void setHistogram(const std::array<int, kHistBins>& h)
    {
        hist_ = h;
        update();
    }

    // Clip info drawn in the top-right corner (percentage of pixels at the max
    // value; 0 hides it). Painted inside this fixed-size widget rather than a
    // separate label so the panel never reflows when clipping starts/stops.
    void setClipPct(double pct)
    {
        if (pct == clipPct_) return;
        clipPct_ = pct;
        update();
    }

    QSize sizeHint() const override { return QSize(600, 120); }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    std::array<int, kHistBins> hist_{};
    double clipPct_ = 0.0;
};
