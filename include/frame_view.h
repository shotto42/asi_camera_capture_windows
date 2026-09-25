// frame_view.h
//
// FrameView: paints the latest camera frame scaled to fit.
#pragma once

#include <cstddef>
#include <QImage>
#include <QWidget>

class FrameView : public QWidget
{
public:
    explicit FrameView(QWidget* parent = nullptr);

    // channels: 1 = grayscale, 3 = RGB (red-marked preview). dataSize is the
    // actual number of valid bytes in `data`.
    void setFrame(const unsigned char* data, size_t dataSize, int w, int h, int channels = 1);

    QSize sizeHint() const override;

    // Test hooks (used by --frametest).
    bool hasImage() const { return !img_.isNull(); }
    int imageChannels() const { return img_.isNull() ? 0 : (img_.format() == QImage::Format_RGB888 ? 3 : 1); }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QImage img_;
};
