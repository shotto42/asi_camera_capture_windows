// mode_toggle.cpp
//
// ModeToggle implementation (see mode_toggle.h).

#include "mode_toggle.h"

#include <QFont>
#include <QMouseEvent>
#include <QPainter>

#include <algorithm>

ModeToggle::ModeToggle(QWidget* parent) : QWidget(parent)
{
    setMinimumHeight(60);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void ModeToggle::setMode(int m)
{
    if (m == mode_) return;
    mode_ = std::clamp(m, 0, kNumModes - 1);
    update();
    emit modeChanged(mode_);
}

void ModeToggle::mousePressEvent(QMouseEvent* e)
{
    int m = std::clamp((int)(e->position().x() / (width() / (double)kNumModes)), 0, kNumModes - 1);
    if (m != mode_) setMode(m);
}

void ModeToggle::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    qreal h = height();
    qreal r = h / 2.0;
    qreal segW = width() / (double)kNumModes;

    // track
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x3a, 0x3a, 0x3a));
    p.drawRoundedRect(QRectF(0, 0, width(), h), r, r);

    // knob over the active segment
    QColor knob = mode_ == 0 ? QColor(0x00, 0x89, 0x7b)
               : mode_ == 1 ? QColor(0x19, 0x76, 0xd2)
                            : QColor(0x5e, 0x35, 0xb1);
    QRectF knobRect(5.0 + mode_ * segW, 5, segW - 10, h - 10);
    p.setBrush(knob);
    p.drawRoundedRect(knobRect, r - 5, r - 5);

    // labels (one per segment)
    QFont f = font();
    f.setPointSize(15);
    f.setBold(true);
    p.setFont(f);
    for (int m = 0; m < kNumModes; ++m)
    {
        QRectF seg(m * segW, 0, segW, h);
        p.setPen(m == mode_ ? Qt::white : QColor(0x9e, 0x9e, 0x9e));
        p.drawText(seg, Qt::AlignCenter, QString::fromLatin1(label(m)));
    }
}
