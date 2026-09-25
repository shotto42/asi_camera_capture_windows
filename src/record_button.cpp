// record_button.cpp
//
// RecordButton implementation (see record_button.h).

#include "record_button.h"

#include <QFont>
#include <QPainter>

#include <algorithm>

RecordButton::RecordButton(QWidget* parent)
    : QPushButton(parent)
{
    setAccessibleName("Record");
    setFocusPolicy(Qt::NoFocus);    // no focus ring around the circle
    setCursor(Qt::PointingHandCursor);
}

void RecordButton::setRecording(bool rec)
{
    if (rec_ == rec) return;
    rec_ = rec;
    update();
}

void RecordButton::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);

    // Centered circle sized by the shorter side.
    const double d = std::min((double)width(), (double)height());
    const QRectF outer((width() - d) / 2.0, (height() - d) / 2.0, d, d);
    const double ring = d * 0.07;                            // white ring
    const QRectF inner = outer.adjusted(ring, ring, -ring, -ring);

    QColor ringCol, fillCol;
    if (!isEnabled())
    {
        ringCol = QColor(0x9e, 0x9e, 0x9e);                  // dim when disabled
        fillCol = QColor(0x61, 0x61, 0x61);
    }
    else if (rec_)
    {
        ringCol = Qt::white;
        fillCol = QColor(0x3a, 0x3a, 0x3a);                  // dark while recording
    }
    else
    {
        ringCol = Qt::white;
        fillCol = isDown()   ? QColor(0xc6, 0x28, 0x28)      // pressed: dark red
                 : underMouse() ? QColor(0xf4, 0x43, 0x36)   // hover: light red
                                : QColor(0xe5, 0x39, 0x35);  // idle
    }
    p.setBrush(ringCol);
    p.drawEllipse(outer);
    p.setBrush(fillCol);
    p.drawEllipse(inner);

    if (rec_ && isEnabled())
    {
        const double dotD = d * 0.42;                        // the REC dot
        p.setBrush(QColor(0xe5, 0x39, 0x35));
        p.drawEllipse(QRectF((width() - dotD) / 2.0, (height() - dotD) / 2.0, dotD, dotD));
    }
}
