// mode_toggle.h
//
// ModeToggle: large touch-friendly PHOTO | INTERVAL | VIDEO switch.
#pragma once

#include <QWidget>

class ModeToggle : public QWidget
{
    Q_OBJECT
public:
    static constexpr int kNumModes = 3;
    static const char* label(int m)
    {
        switch (m) { case 1: return "INTERVAL"; case 2: return "VIDEO"; default: return "PHOTO"; }
    }
    explicit ModeToggle(QWidget* parent = nullptr);

    int mode() const { return mode_; }
    void setMode(int m);

signals:
    void modeChanged(int mode);

protected:
    void mousePressEvent(QMouseEvent* e) override;
    void paintEvent(QPaintEvent*) override;

private:
    int mode_ = 0;
};
