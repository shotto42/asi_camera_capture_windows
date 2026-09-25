// sequence_button.cpp
//
// SequenceButton implementation (see sequence_button.h).

#include "sequence_button.h"

#include <QFont>
#include <QFontMetrics>
#include <QPainter>

SequenceButton::SequenceButton(QWidget* parent)
    : QPushButton(parent)
{
    setAccessibleName("Start sequence");
    setFocusPolicy(Qt::NoFocus);
    setCursor(Qt::PointingHandCursor);
}

void SequenceButton::setRunning(bool r)
{
    if (running_ == r) return;
    running_ = r;
    setAccessibleName(r ? "Stop sequence" : "Start sequence");
    update();
}

void SequenceButton::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);

    QColor bg = running_ ? (isDown()   ? QColor(0xb7, 0x1c, 0x1c)
                       : underMouse() ? QColor(0xd3, 0x2f, 0x2f)
                                      : QColor(0xc6, 0x28, 0x28))
                         : (isDown()   ? QColor(0x45, 0x27, 0xa0)
                       : underMouse() ? QColor(0x6a, 0x42, 0xc0)
                                      : QColor(0x5e, 0x35, 0xb1));
    if (!isEnabled()) bg = QColor(0x2e, 0x2e, 0x2e);
    p.setBrush(bg);
    p.drawRoundedRect(QRectF(rect()), 10, 10);

    // title line: vertically centered in the area above the status band
    const int statusBand = 26;
    QFont tf = font();
    tf.setBold(true);
    tf.setPixelSize(21);
    p.setFont(tf);
    p.setPen(isEnabled() ? Qt::white : QColor(0x77, 0x77, 0x77));
    p.drawText(rect().adjusted(0, 0, 0, -statusBand), Qt::AlignCenter,
               running_ ? QStringLiteral("■  STOP") : QStringLiteral("▶  START SEQUENCE"));

    // status line: centered in the bottom band, elided if too long
    // (bold for readability against the button background)
    QFont sf = font();
    sf.setBold(true);
    sf.setPixelSize(13);
    p.setFont(sf);
    p.setPen(isEnabled() ? QColor(0xff, 0xff, 0xff, 190) : QColor(0x77, 0x77, 0x77));
    const QRectF band(12, rect().height() - statusBand, rect().width() - 24, statusBand);
    p.drawText(band, Qt::AlignCenter,
               QFontMetrics(sf).elidedText(statusLine_, Qt::ElideRight, int(band.width())));
}
