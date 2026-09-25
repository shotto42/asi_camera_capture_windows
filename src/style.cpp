// style.cpp
//
// The app's dark-theme stylesheet (see style.h).

#include "style.h"

const char* kStyleSheet = R"CSS(
QMainWindow { background: #232323; }
QWidget#panel { background: #2b2b2b; border-radius: 14px; }
QLabel { color: #e0e0e0; font-size: 15px; }
QLabel#expValue { font-size: 19px; font-weight: bold; color: #80d8ff; }
QLabel#gainValue { font-size: 19px; font-weight: bold; color: #ffd54f; }
QLabel#fpsValue { font-size: 19px; font-weight: bold; color: #aed581; }
QLabel#hint { color: #9e9e9e; font-size: 13px; }
QLabel#status { color: #cfcfcf; font-size: 14px; background: #262626;
                border-radius: 10px; padding: 10px; }
QPushButton {
    min-height: 56px; background: #3a3a3a; color: #ffffff;
    border: none; border-radius: 10px; font-size: 17px; font-weight: 600;
}
QPushButton:hover { background: #474747; }
QPushButton:pressed { background: #555555; }
QPushButton:disabled { background: #2e2e2e; color: #777777; }
/* photo button: a ShutterButton (red circle with white ring) painted in code;
   the size rules override the generic QPushButton min-height so the circle
   keeps its square 96x96 shape (20% down from 120, user request) */
QPushButton#photoBtn { min-width: 96px; min-height: 96px;
                       max-width: 96px; max-height: 96px; }
/* record button: a RecordButton (circle with REC dot) painted in code, like
   the photo shutter; state comes from the recording signal, not the style.
   The min/max size rules (matching #photoBtn) are what keep it a 96x96
   square (20% down from 120, user request): added with AlignHCenter, the
   layout would otherwise size it to its small sizeHint (setFixedSize alone
   does not fix that). */
QPushButton#recBtn { min-width: 96px; min-height: 96px;
                     max-width: 96px; max-height: 96px; }
QPushButton#recBtn[recording="true"]:hover { background: #d32f2f; }
QPushButton#recBtn[recording="true"]:pressed { background: #b71c1c; }
QSlider::groove:horizontal { height: 10px; background: #3a3a3a; border-radius: 5px; }
QSlider::sub-page:horizontal { background: #1976d2; border-radius: 5px; }
QSlider::add-page:horizontal { background: #3a3a3a; border-radius: 5px; }
QSlider::handle:horizontal {
    width: 36px; height: 36px; margin: -13px 0;
    border-radius: 18px; background: #eceff1;
}
QSlider::handle:horizontal:hover { background: #ffffff; }
/* exposure range switch (photo + interval modes): 0-1 s / 1-60 s */
QPushButton#expRangeBtn {
    min-height: 44px; background: #3a3a3a; color: #e0e0e0;
    border: none; border-radius: 10px; font-size: 15px; font-weight: 600;
    padding: 0 16px;
}
QPushButton#expRangeBtn:hover { background: #474747; }
QPushButton#expRangeBtn:checked { background: #1976d2; color: #ffffff; }
QPushButton#expRangeBtn:checked:hover { background: #1e88e5; }
/* 16 px (was 18) + tighter right chrome: at the old size the two selectors'
   natural widths exceeded the 388 px panel row and the ROI combo overlapped
   the bit-depth one. */
QComboBox#roiCombo, QComboBox#depthCombo {
    min-height: 52px; background: #3a3a3a; color: #ffffff;
    border: none; border-radius: 10px; font-size: 16px; font-weight: 600;
    padding-left: 14px; padding-right: 20px;
}
QComboBox#roiCombo:hover, QComboBox#depthCombo:hover { background: #474747; }
QComboBox#roiCombo::drop-down, QComboBox#depthCombo::drop-down { border: none; width: 24px; }
QComboBox#roiCombo::down-arrow, QComboBox#depthCombo::down-arrow {
    image: none; width: 0; height: 0; margin-right: 6px;
    border-left: 6px solid transparent; border-right: 6px solid transparent;
    border-top: 8px solid #80d8ff;
}
QComboBox#roiCombo QAbstractItemView, QComboBox#depthCombo QAbstractItemView {
    background: #2e2e2e; color: #ffffff; border: none;
    selection-background-color: #1976d2; outline: none;
}
/* white balance (colour bodies only): the AWB checkbox and the two value
   labels; the sliders are the shared QSlider style. While AWB is checked the
   sliders are disabled (Qt dims them automatically). */
QCheckBox#awbCheck {
    color: #e0e0e0; font-size: 17px; font-weight: 600; spacing: 10px;
    background: transparent;
}
QCheckBox#awbCheck::indicator {
    width: 26px; height: 26px; border-radius: 6px; background: #3a3a3a;
}
QCheckBox#awbCheck::indicator:hover { background: #474747; }
QCheckBox#awbCheck::indicator:checked { background: #1976d2; }
QCheckBox#awbCheck:disabled { color: #777777; }
QLabel#wbTempValue { font-size: 19px; font-weight: bold; color: #ffab91; }
QLabel#wbTintValue { font-size: 19px; font-weight: bold; color: #ce93d8; }
/* interval sequence controls */
QSpinBox#seqCount, QDoubleSpinBox#seqInterval {
    min-height: 52px; background: #3a3a3a; color: #ffffff;
    border: none; border-radius: 10px; font-size: 18px; font-weight: 600;
    padding-left: 14px; padding-right: 10px;
}
QSpinBox#seqCount:hover, QDoubleSpinBox#seqInterval:hover { background: #474747; }
QSpinBox#seqCount::up-button, QDoubleSpinBox#seqInterval::up-button,
QSpinBox#seqCount::down-button, QDoubleSpinBox#seqInterval::down-button {
    background: #2e2e2e; border: none; width: 28px;
}
QSpinBox#seqCount::up-arrow, QDoubleSpinBox#seqInterval::up-arrow,
QSpinBox#seqCount::down-arrow, QDoubleSpinBox#seqInterval::down-arrow {
    image: none; width: 0; height: 0; margin-right: 9px;
    border-left: 5px solid transparent; border-right: 5px solid transparent;
}
QSpinBox#seqCount::up-arrow, QDoubleSpinBox#seqInterval::up-arrow { border-bottom: 7px solid #80d8ff; }
QSpinBox#seqCount::down-arrow, QDoubleSpinBox#seqInterval::down-arrow { border-top: 7px solid #80d8ff; }
/* sequence button: a SequenceButton painted in code (title + status line
   inside the button, like the ShutterButton/RecordButton circles); the old
   #seqBtn background rules and the separate #seqStatus label are gone. Its
   fixed height is set in code (84 px). */
)CSS";
