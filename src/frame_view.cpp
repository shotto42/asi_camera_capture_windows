// frame_view.cpp
//
// FrameView implementation (see frame_view.h).

#include "frame_view.h"

#include <QFont>
#include <QPainter>

#include <cstdio>

FrameView::FrameView(QWidget* parent) : QWidget(parent)
{
    setMinimumSize(480, 360);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

QSize FrameView::sizeHint() const { return QSize(900, 600); }

void FrameView::setFrame(const unsigned char* data, size_t dataSize, int w, int h, int channels)
{
    // Never build a QImage that extends past the data actually provided:
    // a channel/size mismatch would make QImage::copy() read out of bounds
    // and segfault the app. (This guard made the stale-latestCh_ photo bug
    // impossible to crash on again: it would just degrade instead.)
    const size_t need1 = (size_t)w * (size_t)h;
    const size_t need3 = need1 * 3;
    if (dataSize < need1)
    {
        static int warned = 0;
        if (++warned <= 3)
            std::fprintf(stderr, "[frame] buffer too small (%zu bytes for %dx%d) - keeping previous image\n",
                         dataSize, w, h);
        return;
    }
    if (channels == 3 && dataSize < need3)
    {
        static int warned2 = 0;
        if (++warned2 <= 3)
            std::fprintf(stderr, "[frame] channel mismatch (3 requested, %zu bytes for %dx%d) - showing as grayscale\n",
                         dataSize, w, h);
        channels = 1;   // the data is actually grayscale
    }
    if (channels == 3)
        img_ = QImage((const uchar*)data, w, h, w * 3, QImage::Format_RGB888).copy();
    else
        img_ = QImage((const uchar*)data, w, h, w, QImage::Format_Grayscale8).copy();
    update();
}

void FrameView::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    if (img_.isNull())
    {
        QFont f = font();
        f.setPointSize(16);
        p.setFont(f);
        p.setPen(QColor(0x80, 0x80, 0x80));
        p.drawText(rect(), Qt::AlignCenter, "Waiting for camera...");
        return;
    }
    QSize s = img_.size().scaled(size(), Qt::KeepAspectRatio);
    QPoint pos((width() - s.width()) / 2, (height() - s.height()) / 2);
    p.drawImage(QRect(pos, s), img_, img_.rect());
}
