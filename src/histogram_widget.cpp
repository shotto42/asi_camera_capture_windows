// histogram_widget.cpp
//
// HistogramWidget implementation (see histogram_widget.h).

#include "histogram_widget.h"

#include <QFont>
#include <QPainter>

HistogramWidget::HistogramWidget(QWidget* parent) : QWidget(parent)
{
    setFixedHeight(120);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void HistogramWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(0x1e, 0x1e, 0x1e));
    int w = width(), h = height();
    if (w <= 0 || h <= 0) return;
    int mx = 0;
    for (int c : hist_) if (c > mx) mx = c;
    if (mx <= 0) return;
    int barW = w / kHistBins;
    if (barW < 1) barW = 1;
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x00, 0xac, 0xf0));
    for (int i = 0; i < kHistBins; ++i)
    {
        int barH = (int)((double)hist_[i] / mx * (h - 4));
        if (barH < 0) barH = 0;
        p.drawRect(i * barW, h - barH, barW - 1, barH);
    }
    if (clipPct_ > 0.0)
    {
        QFont f = font();
        f.setBold(true);
        f.setPixelSize(14);
        p.setFont(f);
        p.setPen(QColor(0xef, 0x53, 0x50));   // warning red on the dark background
        p.drawText(rect().adjusted(0, 4, -8, 0),
                   Qt::AlignTop | Qt::AlignRight,
                   QString("⚠  CLIPPED %1%").arg(clipPct_, 0, 'f', 2));
    }
}
