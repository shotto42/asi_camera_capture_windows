// shutter_button.h
//
// ShutterButton — photo button drawn as a red-filled circle with a white ring.
#pragma once

#include <QPushButton>

class ShutterButton : public QPushButton
{
    Q_OBJECT
public:
    explicit ShutterButton(QWidget* parent = nullptr);

    // Dim the circle while an exposure is in flight.
    void setBusy(bool busy) { busy_ = busy; update(); }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    bool busy_ = false;
};
