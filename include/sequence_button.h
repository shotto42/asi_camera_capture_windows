// sequence_button.h
//
// SequenceButton — the interval-mode START/STOP button with the sequence
// status line painted INSIDE it (the old separate status label below the
// button is gone). Title line on top, status line in a band at the bottom;
// the colors match the former #seqBtn stylesheet (purple idle, red running).
#pragma once

#include <QPushButton>

class SequenceButton : public QPushButton
{
    Q_OBJECT
public:
    explicit SequenceButton(QWidget* parent = nullptr);

    void setStatusLine(const QString& s) { statusLine_ = s; update(); }
    QString statusLine() const { return statusLine_; }

    void setRunning(bool r);

protected:
    void paintEvent(QPaintEvent*) override;

private:
    bool running_ = false;
    QString statusLine_ = "Ready";
};
