// record_button.h
//
// RecordButton — video-mode record toggle in the same circle language as the
// ShutterButton: idle = red circle (press to record); recording = dark circle
// with a red dot in the center (the classic REC indicator, standing in for the
// old "● RECORD" text); disabled = dimmed like the shutter.
#pragma once

#include <QPushButton>

class RecordButton : public QPushButton
{
    Q_OBJECT
public:
    explicit RecordButton(QWidget* parent = nullptr);

    void setRecording(bool rec);

protected:
    void paintEvent(QPaintEvent*) override;

private:
    bool rec_ = false;
};
