// main_window.cpp
//
// MainWindow implementation (see main_window.h).

#include "main_window.h"

#include "depth_code.h"
#include "exposure.h"
#include "fps_spec.h"
#include "ser_writer.h"
#include "frame_view.h"
#include "histogram_widget.h"
#include "mode_toggle.h"
#include "record_button.h"
#include "sequence_button.h"
#include "shutter_button.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFont>
#include <QFontMetrics>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

MainWindow::MainWindow(bool smoke, bool seqtest,
                       double stExp, double stInterval, int stCount,
                       bool vtest, int vtW, int vtH, int vtBits, int vtFps, double vtDur,
                       bool prevtest, int pvW, int pvH, int pvBits, double pvExp, double pvDur,
                       double pvExp2, bool fpstest, int bayerOverride, bool vtSerOut)
{
    // The title names the camera once it answers (cameraReady); until then it
    // stays honest — this build drives any ASI camera, not one model.
    setWindowTitle("ASI Camera");
    resize(1440, 900);
    bayerOverride_ = bayerOverride;
    if (bayerOverride_ >= 0)
        worker_.setBayerOverride(bayerOverride_);   // must be set before the open

    setupUi();
    setupConnections();
    setupInitialState();

    worker_.start();

    if (smoke)   setupSmokeTest();
    if (seqtest) setupSeqTest(stExp, stInterval, stCount);
    if (vtest)   setupVTest(vtW, vtH, vtBits, vtFps, vtDur, vtSerOut);
    if (prevtest) { pvExp2_ = pvExp2; setupPrevTest(pvW, pvH, pvBits, pvExp, pvDur); }
    if (fpstest) setupFpsTest();
}

void MainWindow::setUiMode(int m) { modeToggle_->setMode(m); }

void MainWindow::setupUi()
{
    auto* central = new QWidget(this);
    auto* root = new QHBoxLayout(central);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(12);

    // ---- left: frame display ------------------------------------------
    view_ = new FrameView(central);
    root->addWidget(view_, 1);

    // ---- right: control panel -----------------------------------------
    auto* panel = new QWidget(central);
    panel->setObjectName("panel");
    panel->setFixedWidth(420);
    auto* col = new QVBoxLayout(panel);
    col->setContentsMargins(16, 16, 16, 16);
    col->setSpacing(14);

    modeToggle_ = new ModeToggle(panel);
    col->addWidget(modeToggle_);

    // image size (ROI) + bit depth selectors, side by side without labels
    // (shared by both modes; the combo items speak for themselves)
    auto* roiRow = new QHBoxLayout();
    roiRow->setSpacing(14);   // explicit: syncRoiDepthWidths accounts for it
    roiCombo_ = new QComboBox(panel);
    roiCombo_->setObjectName("roiCombo");
    roiCombo_->setCursor(Qt::PointingHandCursor);
    roiCombo_->addItem("…");              // placeholder until probed
    roiRow->addWidget(roiCombo_, 1);
    // bit depth selector (8-bit -> PNG, 14-bit -> 16-bit TIFF)
    depthCombo_ = new QComboBox(panel);
    depthCombo_->setObjectName("depthCombo");
    depthCombo_->setCursor(Qt::PointingHandCursor);
    depthCombo_->addItem("8-bit  (PNG)", 8);
    depthCombo_->addItem("14-bit  (16-bit TIFF)", 14);
    depthCombo_->setCurrentIndex(0);
    roiRow->addWidget(depthCombo_, 1);
    col->addLayout(roiRow);

    // exposure (shared by both modes; max depends on mode/fps). The range
    // switch takes the place of the old "Exposure" label: it picks the
    // slider's range — off: 32 µs .. 1 s (log), on: 1 .. 60 s (linear,
    // whole-second steps). Shared by photo and interval (the sequence uses
    // the main slider); hidden in video mode (the exposure is capped at the
    // frame period).
    expRangeRow_ = new QWidget(panel);
    auto* expRangeLayout = new QHBoxLayout(expRangeRow_);
    expRangeLayout->setContentsMargins(0, 0, 0, 0);
    auto* expShortBtn = new QPushButton("0-1 s", expRangeRow_);
    expShortBtn->setObjectName("expRangeBtn");
    expShortBtn->setCheckable(true);
    expShortBtn->setChecked(true);
    expShortBtn->setCursor(Qt::PointingHandCursor);
    auto* expLongBtn = new QPushButton("1-60 s", expRangeRow_);
    expLongBtn->setObjectName("expRangeBtn");
    expLongBtn->setCheckable(true);
    expLongBtn->setCursor(Qt::PointingHandCursor);
    auto* expRangeGroup = new QButtonGroup(this);
    expRangeGroup->addButton(expShortBtn, 0);
    expRangeGroup->addButton(expLongBtn, 1);
    expRangeLayout->addWidget(expShortBtn);
    expRangeLayout->addWidget(expLongBtn);
    // (wired here, inside setupUi, because the group is a local: the range
    //  switch is part of the exposure row's construction)
    connect(expRangeGroup, &QButtonGroup::idClicked, this, [this](int id) {
        expLongRange_ = (id == 1);
        onExpRangeChanged();
    });

    auto* expRow = new QHBoxLayout();
    expValue_ = new QLabel(panel);
    expValue_->setObjectName("expValue");
    expRow->addWidget(expRangeRow_);
    expRow->addStretch(1);
    expRow->addWidget(expValue_);
    col->addLayout(expRow);

    expSlider_ = new QSlider(Qt::Horizontal, panel);
    expSlider_->setRange(kSliderMin, kSliderMax);
    expSlider_->setMinimumHeight(44);
    col->addWidget(expSlider_);

    // gain (shared by all modes; 0.1 dB units, e.g. 0..400 = 0.0..40.0 dB)
    auto* gainRow = new QHBoxLayout();
    gainValue_ = new QLabel(panel);
    gainValue_->setObjectName("gainValue");
    gainRow->addStretch(1);
    gainRow->addWidget(gainValue_);
    col->addLayout(gainRow);

    gainSlider_ = new QSlider(Qt::Horizontal, panel);
    gainSlider_->setRange(0, 400);      // default; refined by the gainReady caps
    gainSlider_->setValue(0);
    gainSlider_->setMinimumHeight(44);
    gainValue_->setText(fmtGain(0));
    col->addWidget(gainSlider_);

    // ---- white balance (COLOUR BODIES ONLY) --------------------------------
    // Hidden until cameraReady reports a colour body with WB controls (a mono
    // one never shows this row, and its data stays the neutral 1:1:1 it always
    // was). AWB is CHECKED BY DEFAULT: the camera auto-balances and these two
    // sliders are disabled and show the last sampled gains. Switching AWB off
    // samples and freezes the camera's current gains instead of jumping to
    // model-rounded values. The
    // Kelvin/Tint numbers are the photographic presentation of the raw
    // WB_R/WB_B gains; see white_balance.h for the model.
    // The two manual sliders are INDEPENDENT settings, and so are the values
    // they show: the Temperature never clamps, moves, or disables the Tint
    // (an earlier build made the Tint range per-temperature, which dragged
    // the user's tint whenever the temperature moved), and the labels show
    // EXACTLY the set values — no read-back, no asterisk. Where the body
    // cannot deliver the full balance, the pushed
    // gains clamp PER CHANNEL (wbToGains): the overflowing channel sits at its
    // cap and the free one keeps its temperature-correct value, so the
    // temperature axis stays on blue<->yellow-orange in the image and the
    // body delivers the most of the balance it can.
    wbRow_ = new QWidget(panel);
    auto* wbRowLayout = new QHBoxLayout(wbRow_);
    wbRowLayout->setContentsMargins(0, 0, 0, 0);
    awbCheck_ = new QCheckBox("AWB", wbRow_);
    awbCheck_->setObjectName("awbCheck");
    awbCheck_->setChecked(true);
    awbCheck_->setCursor(Qt::PointingHandCursor);
    wbTempValue_ = new QLabel(wbRow_);
    wbTempValue_->setObjectName("wbTempValue");
    wbTintValue_ = new QLabel(wbRow_);
    wbTintValue_->setObjectName("wbTintValue");
    wbRowLayout->addWidget(awbCheck_);
    wbRowLayout->addStretch(1);
    wbRowLayout->addWidget(wbTempValue_);
    wbRowLayout->addSpacing(12);
    wbRowLayout->addWidget(wbTintValue_);
    col->addWidget(wbRow_);

    wbTempSlider_ = new QSlider(Qt::Horizontal, panel);
    wbTempSlider_->setObjectName("wbTempSlider");
    wbTempSlider_->setRange(kWbTempMinK, kWbTempMaxK);   // value IS the Kelvin
    wbTempSlider_->setTracking(true);
    wbTempSlider_->setValue((int)kWbAnchorK);
    wbTempSlider_->setSingleStep(kWbTempStepK);
    wbTempSlider_->setPageStep(500);
    wbTempSlider_->setMinimumHeight(44);
    wbTempSlider_->setEnabled(false);   // AWB is on: retain the manual setting
    col->addWidget(wbTempSlider_);

    wbTintSlider_ = new QSlider(Qt::Horizontal, panel);
    wbTintSlider_->setObjectName("wbTintSlider");
    // FIXED full range at every temperature — Tint is independent.
    wbTintSlider_->setRange(kWbTintMin, kWbTintMax);     // -100 green .. +100 magenta
    wbTintSlider_->setTracking(true);
    wbTintSlider_->setValue(0);
    wbTintSlider_->setSingleStep(1);
    wbTintSlider_->setPageStep(10);
    wbTintSlider_->setMinimumHeight(44);
    wbTintSlider_->setEnabled(false);
    col->addWidget(wbTintSlider_);

    // No camera (or a mono one) yet: the whole section stays hidden.
    wbRow_->setVisible(false);
    wbTempSlider_->setVisible(false);
    wbTintSlider_->setVisible(false);
    updateWbLabels();

    // ---- stacked action area ------------------------------------------
    stack_ = new QStackedWidget(panel);

    // photo page
    auto* photoPage = new QWidget(stack_);
    auto* photoCol = new QVBoxLayout(photoPage);
    photoCol->setContentsMargins(0, 0, 0, 0);
    photoCol->setSpacing(10);
    photoBtn_ = new ShutterButton(photoPage);
    photoBtn_->setObjectName("photoBtn");
    photoBtn_->setFixedSize(96, 96);    // 20% down from 120 (user request)
    photoCol->addWidget(photoBtn_, 0, Qt::AlignCenter);
    stack_->addWidget(photoPage);

    // video page
    auto* videoPage = new QWidget(stack_);
    auto* videoCol = new QVBoxLayout(videoPage);
    videoCol->setContentsMargins(0, 0, 0, 0);
    // 14, the same rhythm as the shared panel column: this puts the fps
    // slider exactly one "slider pitch" (value row + spacing, as between
    // the exposure and gain sliders) below the gain slider.
    videoCol->setSpacing(14);
    // Frame rate slider: 1 .. max fps achievable for the current ROI. The
    // maximum is recomputed whenever the ROI changes (updateFpsMax), so the
    // top of the slider tracks the ZWO spec ceiling for the selected size.
    auto* fpsRow = new QHBoxLayout();
    fpsValue_ = new QLabel(videoPage);
    fpsValue_->setObjectName("fpsValue");
    fpsValue_->setText("30 fps");
    fpsRow->addStretch(1);
    fpsRow->addWidget(fpsValue_);
    videoCol->addLayout(fpsRow);
    fpsSlider_ = new QSlider(Qt::Horizontal, videoPage);
    fpsSlider_->setObjectName("fpsSlider");
    fpsSlider_->setRange(1, 30);      // full-frame placeholder; refined by updateFpsMax
    fpsSlider_->setValue(30);
    fpsSlider_->setMinimumHeight(44);
    videoCol->addWidget(fpsSlider_);
    connect(fpsSlider_, &QSlider::valueChanged, this, [this](int fps) {
        fpsValue_->setText(QString::number(fps) + " fps");
        onFpsChanged(fps);
    });
    // Center the record button vertically in the space between the fps
    // slider and the bottom of the panel: equal stretches above and below
    // split the stack's surplus height around it.
    videoCol->addStretch(1);
    recBtn_ = new RecordButton(videoPage);
    recBtn_->setObjectName("recBtn");
    recBtn_->setFixedSize(96, 96);      // 20% down from 120 (user request)
    videoCol->addWidget(recBtn_, 0, Qt::AlignHCenter);
    videoCol->addStretch(1);
    // (videoPage is added to the stack AFTER the interval page below, so
    //  the stack indices match the mode numbers: 0 photo, 1 interval, 2 video)

    // interval page (back-to-back long-exposure sequence)
    auto* intervalPage = new QWidget(stack_);
    auto* intervalCol = new QVBoxLayout(intervalPage);
    intervalCol->setContentsMargins(0, 0, 0, 0);
    intervalCol->setSpacing(10);
    auto addSpinRow = [intervalPage, intervalCol](const QString& labelText, QWidget* spin) {
        auto* row = new QHBoxLayout();
        auto* lbl = new QLabel(labelText, intervalPage);
        row->addWidget(lbl);
        row->addStretch(1);
        row->addWidget(spin, 1);
        intervalCol->addLayout(row);
    };

    // No separate exposure control: the sequence uses the main exposure
    // slider above, with the same range switch (0-1 s / 1-60 s) as photo
    // mode. (The old on-page hint label was removed; the README and
    // AGENTS.md still document this behaviour.)

    seqIntervalSpin_ = new QDoubleSpinBox(intervalPage);
    seqIntervalSpin_->setObjectName("seqInterval");
    seqIntervalSpin_->setRange(1.0, 1200.0);
    seqIntervalSpin_->setSingleStep(0.5);
    seqIntervalSpin_->setDecimals(1);
    seqIntervalSpin_->setSuffix("  s");
    seqIntervalSpin_->setValue(5.0);
    seqIntervalSpin_->setCursor(Qt::PointingHandCursor);
    addSpinRow("Interval  (min 1 s)", seqIntervalSpin_);

    seqCountSpin_ = new QSpinBox(intervalPage);
    seqCountSpin_->setObjectName("seqCount");
    seqCountSpin_->setRange(0, 9999);
    seqCountSpin_->setValue(10);
    seqCountSpin_->setCursor(Qt::PointingHandCursor);
    addSpinRow("Images  (0 = continuous)", seqCountSpin_);

    seqBtn_ = new SequenceButton(intervalPage);
    seqBtn_->setObjectName("seqBtn");
    seqBtn_->setFixedHeight(84);   // title line + status band
    intervalCol->addWidget(seqBtn_);
    // Pack the content to the top of the page; the stretch keeps the
    // stack's surplus height below the button (the old separate status
    // label lived in that space).
    intervalCol->addStretch(1);

    stack_->addWidget(intervalPage);
    stack_->addWidget(videoPage);   // stack order == mode order (0, 1, 2)
    col->addWidget(stack_, 1);

    // histogram (128 bins; adapts to 8/14-bit capture)
    histWidget_ = new HistogramWidget(panel);
    histWidget_->setObjectName("histWidget");
    col->addWidget(histWidget_);

    // status
    status_ = new QLabel(panel);
    status_->setObjectName("status");
    status_->setWordWrap(true);
    status_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    // Fixed height for 4 lines — the steady 2 telemetry lines plus the 2
    // live lines (◷ SEQ while a sequence runs, ● REC while recording) —
    // so the window never resizes as text lines appear and disappear.
    // 14 px = the #status CSS font-size; +20 = its 10 px top/bottom
    // padding. Text is top-aligned so the reserved space sits at the bottom.
    {
        QFont f = status_->font();
        f.setPixelSize(14);
        status_->setFixedHeight(QFontMetrics(f).height() * 4 + 20);
    }
    col->addWidget(status_);

    root->addWidget(panel, 0);
    setCentralWidget(central);
}

void MainWindow::setupConnections()
{
    // ---- wiring ---------------------------------------------------------
    connect(modeToggle_, &ModeToggle::modeChanged, this, &MainWindow::onModeChanged);
    connect(expSlider_, &QSlider::valueChanged, this, &MainWindow::onSliderMoved);
    connect(gainSlider_, &QSlider::valueChanged, this, &MainWindow::onGainChanged);

    // White balance (colour bodies only; see setupUi for the design).
    // The sliders own the manual setting even while AWB is running. Send the
    // first change immediately, then at most one latest pair every 80 ms
    // during a drag. Releasing a handle sends its final value immediately.
    wbManualTimer_ = new QTimer(this);
    wbManualTimer_->setSingleShot(false);
    wbManualTimer_->setInterval(80);
    connect(wbManualTimer_, &QTimer::timeout, this, [this] {
        if (!wbShown_ || awbCheck_->isChecked()) { wbManualTimer_->stop(); return; }
        pushWbManual();
        if (!wbTempSlider_->isSliderDown() && !wbTintSlider_->isSliderDown())
            wbManualTimer_->stop();
    });
    connect(awbCheck_, &QCheckBox::toggled, this, [this](bool autoOn) {
        wbManualTimer_->stop();
        updateWbControls();
        if (autoOn)
            worker_.setWhiteBalance(true, wbCurR_, wbCurB_);
        else
            pushWbManual(); // apply the retained manual setting at once
    });
    connect(wbTempSlider_, &QSlider::valueChanged, this, [this](int) {
        if (awbCheck_->isChecked()) return;
        updateWbLabels();
        if (!wbManualTimer_->isActive())
        {
            pushWbManual();
            wbManualTimer_->start();
        }
    });
    connect(wbTintSlider_, &QSlider::valueChanged, this, [this](int) {
        if (awbCheck_->isChecked()) return;
        updateWbLabels();
        if (!wbManualTimer_->isActive())
        {
            pushWbManual();
            wbManualTimer_->start();
        }
    });
    auto commitWbDrag = [this] {
        if (awbCheck_->isChecked()) return;
        wbManualTimer_->stop();
        pushWbManual();
    };
    connect(wbTempSlider_, &QSlider::sliderReleased, this, commitWbDrag);
    connect(wbTintSlider_, &QSlider::sliderReleased, this, commitWbDrag);

    connect(photoBtn_, &QPushButton::clicked, this, &MainWindow::onTakePhoto);
    connect(recBtn_, &QPushButton::clicked, this, &MainWindow::onToggleRecord);
    connect(seqBtn_, &QPushButton::clicked, this, &MainWindow::onToggleSequence);

    connect(&worker_, &CameraWorker::photoResult, this, &MainWindow::onPhotoResult);
    connect(&worker_, &CameraWorker::recordingStarted, this, &MainWindow::onRecordingStarted);
    connect(&worker_, &CameraWorker::recordingStopped, this, &MainWindow::onRecordingStopped);
    connect(&worker_, &CameraWorker::sequenceStarted, this, &MainWindow::onSequenceStarted);
    connect(&worker_, &CameraWorker::sequenceExposing, this, &MainWindow::onSequenceExposing);
    connect(&worker_, &CameraWorker::sequenceShot, this, &MainWindow::onSequenceShot);
    connect(&worker_, &CameraWorker::sequenceWait, this, &MainWindow::onSequenceWait);
    connect(&worker_, &CameraWorker::sequenceDone, this, &MainWindow::onSequenceDone);
    connect(&worker_, &CameraWorker::cameraError, this, &MainWindow::onCameraError);
    connect(&worker_, &CameraWorker::cameraReconnected, this, &MainWindow::onCameraReconnected);
    // Every successful open (first start AND every reconnect, which can bring
    // back a different body): re-read what the camera can do and rebuild the
    // selectors that depend on it.
    connect(&worker_, &CameraWorker::cameraReady, this, [this](const QString& description) {
        applyCameraCaps();
        status_->setStyleSheet("");
        updateStatus();
        if (!description.isEmpty())
            std::fprintf(stderr, "[ui] camera ready: %s\n", description.toUtf8().constData());
    });

    // ROI selector: populate from the probed list, and apply on change
    connect(&worker_, &CameraWorker::roiListReady, this, [this](const QStringList& rois) {
        roiCombo_->blockSignals(true);
        roiCombo_->clear();
        for (const QString& r : rois)
        {
            int x = r.indexOf('x');
            int w = r.left(x).toInt();
            int h = r.mid(x + 1).toInt();
            roiCombo_->addItem(QString("%1×%2").arg(w).arg(h), r);
        }
        // The list opens with the 1:1 base square as its FIRST entry; the
        // default selection is the HIGHEST resolution — the largest-area item
        // (the full sensor on every body), which the worker applies live at
        // open.
        int bestIdx = -1;
        long bestArea = -1;
        for (int i = 0; i < roiCombo_->count(); ++i)
        {
            const QString r = roiCombo_->itemData(i).toString();
            const int x = r.indexOf('x');
            if (x <= 0) continue;
            const long area = r.left(x).toLong() * r.mid(x + 1).toLong();
            if (area > bestArea) { bestArea = area; bestIdx = i; }
        }
        roiCombo_->setCurrentIndex(bestIdx >= 0 ? bestIdx : 0);
        roiCombo_->blockSignals(false);

        // Size the image-size and bit-depth selectors to their labels so
        // the two fit side by side in the panel. The depth selector uses
        // the union of both modes' labels, so its width (and the row's
        // layout) is stable when its items swap on a mode change.
        syncRoiDepthWidths();
        if (smokeActive_) pickSmokeRoi();   // the smoke test records at a probed ROI
        // Seed the fps-slider maximum for the initial ROI (the highest
        // resolution — the full sensor). setCurrentIndex above was
        // signal-blocked, so the ROI handler won't fire for it.
        {
            QString r = roiCombo_->currentData().toString();
            int x = r.indexOf('x');
            if (x > 0)
                updateFpsMax(r.left(x).toInt(), r.mid(x + 1).toInt());
        }
    });
    // Gain: update the slider range from the camera's actual caps and set the
    // position to the camera's current value (signals blocked so this doesn't
    // fire a redundant setGain).
    connect(&worker_, &CameraWorker::gainReady, this, [this](int min, int max, int current) {
        gainSlider_->blockSignals(true);
        gainSlider_->setRange(min, max);
        gainSlider_->setValue(current);
        gainSlider_->blockSignals(false);
        gainValue_->setText(fmtGain(current));
    });
    connect(roiCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        QString r = roiCombo_->currentData().toString();
        int x = r.indexOf('x');
        if (x > 0)
        {
            worker_.setRoi(r.left(x).toInt(), r.mid(x + 1).toInt());
            updateFpsMax(r.left(x).toInt(), r.mid(x + 1).toInt());
        }
    });

    // bit depth selector: apply on change
    connect(depthCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        const int code = depthCombo_->currentData().toInt();
        worker_.setBitDepth(codeDepth(code));
        worker_.setSerMode(codeSer(code));
        updateFpsMax(currentRoiW_, currentRoiH_, /*snapToMax=*/true);   // new format: run it at its spec rate
    });
}

void MainWindow::setupInitialState()
{
    // 30 Hz frame refresh + status updates
    timer_ = new QTimer(this);
    connect(timer_, &QTimer::timeout, this, &MainWindow::onTimer);
    timer_->start(33);

    // initial state
    currentExposure_ = 0.010;
    {
        double tmin, tmax;
        bool linear = false;
        exposureRange(tmin, tmax, &linear);
        expSlider_->setValue(linear ? secondsToSliderLinear(currentExposure_, tmin, tmax)
                                    : secondsToSlider(currentExposure_, tmin, tmax));
    }
    expValue_->setText(fmtExposure(currentExposure_));
    applyMode(0, false);
}

// ---------------------------------------------------------------------------
// Exposure slider
// ---------------------------------------------------------------------------

void MainWindow::exposureRange(double& tmin, double& tmax, bool* linear) const
{
    int m = modeToggle_->mode();
    if (linear) *linear = false;
    if (m == 2) { tmin = kExpMinS; tmax = 1.0 / currentFps_; }   // video
    else if (expLongRange_) { tmin = kExpLongMinS; tmax = kExpLongMaxS; if (linear) *linear = true; }
    else                    { tmin = kExpMinS;     tmax = kExpShortMaxS; }

    // Cut the range down to what the CONNECTED camera can actually do: the
    // 32 µs floor and the 1 s / 60 s ceilings are this family's numbers (probed
    // on the ASI178s: 32 us .. 2000 s), and a body with a longer minimum
    // exposure or a shorter maximum must get a range it can honour. Falls back
    // to the constants before the camera answers.
    if (capsKnown_ && caps_.expMaxUs > caps_.expMinUs)
    {
        const double cmin = (double)caps_.expMinUs * 1e-6;
        const double cmax = (double)caps_.expMaxUs * 1e-6;
        if (tmin < cmin) tmin = cmin;
        if (tmax > cmax) tmax = cmax;
        if (tmax < tmin) tmax = tmin;    // absurd camera: keep a single-valued range
    }
}

void MainWindow::onSliderMoved(int pos)
{
    double tmin, tmax;
    bool linear = false;
    exposureRange(tmin, tmax, &linear);
    currentExposure_ = linear ? sliderToSecondsLinear(pos, tmin, tmax)
                              : sliderToSeconds(pos, tmin, tmax);
    if (linear)
    {
        // Snap the handle to the 1 s step position.
        expSlider_->blockSignals(true);
        expSlider_->setValue(secondsToSliderLinear(currentExposure_, tmin, tmax));
        expSlider_->blockSignals(false);
    }
    expValue_->setText(fmtExposure(currentExposure_));
    worker_.setExposure(currentExposure_);
}

void MainWindow::onExpRangeChanged()
{
    // Photo + interval modes (the switch row is hidden in video mode).
    // Keep the current exposure if it lies in the new range, else clamp to
    // the nearest bound (e.g. 0.5 s -> 1 s when switching to 1-60 s); the
    // long range quantizes to whole seconds.
    double tmin, tmax;
    bool linear = false;
    exposureRange(tmin, tmax, &linear);
    currentExposure_ = std::clamp(currentExposure_, tmin, tmax);
    if (linear) currentExposure_ = (double)std::llround(currentExposure_);
    expSlider_->blockSignals(true);
    expSlider_->setValue(linear ? secondsToSliderLinear(currentExposure_, tmin, tmax)
                                : secondsToSlider(currentExposure_, tmin, tmax));
    expSlider_->blockSignals(false);
    expValue_->setText(fmtExposure(currentExposure_));
    worker_.setExposure(currentExposure_);
}

void MainWindow::onGainChanged(int gain01)
{
    gainValue_->setText(fmtGain(gain01));
    worker_.setGain(gain01);
}

// ---------------------------------------------------------------------------
// White balance (colour bodies only — see white_balance.h for the model)
// ---------------------------------------------------------------------------

void MainWindow::setWbVisible(bool on)
{
    wbShown_ = on;
    // The value labels live inside wbRow_; the two sliders are its siblings.
    wbRow_->setVisible(on);
    wbTempSlider_->setVisible(on);
    wbTintSlider_->setVisible(on);
    updateWbControls();
}

void MainWindow::updateWbControls()
{
    const bool manual = wbShown_ && awbCheck_ && !awbCheck_->isChecked();
    wbTempSlider_->setEnabled(manual);
    wbTintSlider_->setEnabled(manual);
}

void MainWindow::updateWbLabels()
{
    // These are the user's manual settings, retained even while AWB runs.
    // Hardware gain limits affect the image, never these displayed values.
    const int K = wbTempSlider_->value();
    const int t = wbTintSlider_->value();
    wbTempValue_->setText(QString::number(K) + " K");
    wbTintValue_->setText(QString("Tint %1%2")
                              .arg(t < 0 ? QChar('-') : QChar('+'))
                              .arg(t < 0 ? -t : t));
}

void MainWindow::pushWbManual()
{
    int r = 0, b = 0;
    wbToGains(wbTempSlider_->value(), wbTintSlider_->value(), wbCal_, r, b);
    wbCurR_ = r;
    wbCurB_ = b;
    worker_.setWhiteBalance(false, r, b);
}

void MainWindow::syncSliderToExposure()
{
    double tmin, tmax;
    bool linear = false;
    exposureRange(tmin, tmax, &linear);
    currentExposure_ = std::clamp(currentExposure_, tmin, tmax);
    if (linear) currentExposure_ = (double)std::llround(currentExposure_);
    expSlider_->blockSignals(true);
    expSlider_->setValue(linear ? secondsToSliderLinear(currentExposure_, tmin, tmax)
                                : secondsToSlider(currentExposure_, tmin, tmax));
    expSlider_->blockSignals(false);
    expValue_->setText(fmtExposure(currentExposure_));
    worker_.setExposure(currentExposure_);
}

// ---------------------------------------------------------------------------
// Mode / fps
// ---------------------------------------------------------------------------

// The capabilities in force: the connected camera's once it has answered, and
// an ASI178-shaped stand-in before that (the panel is built before the camera
// opens, and the offscreen tests must be deterministic without hardware).
const CameraCaps& MainWindow::effectiveCaps() const
{
    if (capsKnown_) return caps_;
    static const CameraCaps kStandIn = [] {
        CameraCaps c;
        c.name = "ASI camera";
        c.maxW = 3096; c.maxH = 2080;
        c.nativeDepth = 14;
        c.hasRaw8 = true; c.hasRaw16 = true;
        return c;
    }();
    return kStandIn;
}

// One entry of the bit-depth selector. A colour camera's entries SAY RGB,
// because that is what the file will contain (its Bayer readout is demosaiced
// by the app); a mono body's labels come out exactly as they always read.
QString MainWindow::depthLabel(int bits, const QString& container) const
{
    const bool colour = capsKnown_ && caps_.isColor;
    return colour ? QString("%1-bit RGB (%2)").arg(bits).arg(container)
                  : QString("%1-bit  (%2)").arg(bits).arg(container);
}

void MainWindow::syncRoiDepthWidths()
{
    static QFontMetrics fm{QFont()};
    static int qtPad = 0;
    {
        static bool initialized = false;
        if (!initialized)
        {
            QComboBox probe;
            probe.setObjectName("depthCombo");
            const QString ref = "14-bit  (16-bit TIFF)";   // longest of all labels
            probe.addItem(ref);
            probe.ensurePolished();
            fm = probe.fontMetrics();
            qtPad = probe.sizeHint().width() - fm.horizontalAdvance(ref);
            initialized = true;
        }
    }
    auto adv = [](const QString& s) { return fm.horizontalAdvance(s); };

    // The depth selector's width is the longest label it can ever show — over
    // BOTH modes and over the depths the connected camera offers (its item list
    // swaps on a mode change; a stable width keeps the row from reflowing).
    int depthLongest = 0;
    for (int bits : effectiveCaps().depths())
        for (const QString& container : {QStringLiteral("PNG"), QStringLiteral("16-bit TIFF"),
                                         QStringLiteral("H.264"), QStringLiteral(".ser")})
            depthLongest = qMax(depthLongest, adv(depthLabel(bits, container)));

    int roiLongest = 0;
    for (int i = 0; i < roiCombo_->count(); ++i)
        roiLongest = qMax(roiLongest, adv(roiCombo_->itemText(i)));

    int roiW   = roiLongest + qtPad;
    int depthW = depthLongest + qtPad;

    // The row holds both selectors plus its 14 px spacing inside the panel's
    // 388 px content width (420 - 2 x 16 margins); if they ever don't fit,
    // the ROI selector yields first (floored to text + left pad + drop-down
    // so its label isn't clipped).
    const int maxSum = 420 - 32 - 14;
    const int roiFloor = roiLongest + 40;
    if (roiW + depthW > maxSum)
    {
        roiW = qMax(roiW - (roiW + depthW - maxSum), roiFloor);
        if (roiW + depthW > maxSum)
            depthW = qMax(depthW - (roiW + depthW - maxSum), depthLongest + 40);
    }

    roiCombo_->setFixedWidth(roiW);
    depthCombo_->setFixedWidth(depthW);
}

void MainWindow::setDepthComboForMode(int mode)
{
    const int keepCode = depthCombo_->currentData().isValid()
                       ? depthCombo_->currentData().toInt() : 8;
    const bool isVideo = (mode == 2);
    int  keepDepth = codeDepth(keepCode);
    bool keepSer   = codeSer(keepCode);
    if (!isVideo)
        keepSer = false;                        // stills are never .ser

    // The entries come from the CONNECTED camera: a body with no 2-byte readout
    // has no deep entry at all (offering one would only produce a silent
    // fallback), and a colour body's labels say RGB. For the ASI178s this is
    // exactly the old list: 8-bit and 14-bit.
    const std::vector<int> depths = effectiveCaps().depths();   // shallow first

    depthCombo_->blockSignals(true);
    depthCombo_->clear();
    if (isVideo)
    {
        // H.264 only exists for the 8-bit (fast) readout — x264 baseline is
        // 8-bit and the deep readout exists precisely to keep more than 8 bits,
        // so the deep entry is always `.ser`.
        for (int bits : depths)
        {
            if (bits == 8)
                depthCombo_->addItem(depthLabel(bits, "H.264"), depthCode(bits, false));
            depthCombo_->addItem(depthLabel(bits, ".ser"), depthCode(bits, true));
        }
    }
    else
    {
        for (int bits : depths)
            depthCombo_->addItem(depthLabel(bits, bits == 8 ? "PNG" : "16-bit TIFF"),
                                 depthCode(bits, false));
    }
    // Prefer an exact (depth, ser) match, else a depth-only match, else the
    // first entry (the shallower one).
    int idx = 0; bool found = false;
    for (int i = 0; i < depthCombo_->count(); ++i)
    {
        int d = codeDepth(depthCombo_->itemData(i).toInt());
        bool s = codeSer(depthCombo_->itemData(i).toInt());
        if (d == keepDepth && s == keepSer) { idx = i; found = true; break; }
    }
    if (!found)
        for (int i = 0; i < depthCombo_->count(); ++i)
            if (codeDepth(depthCombo_->itemData(i).toInt()) == keepDepth) { idx = i; break; }
    depthCombo_->setCurrentIndex(idx);
    depthCombo_->blockSignals(false);

    const int finalCode = depthCombo_->currentData().toInt();
    worker_.setBitDepth(codeDepth(finalCode));   // keep the capture format in sync
    worker_.setSerMode(codeSer(finalCode));
    syncRoiDepthWidths();
    if (isVideo)
    {
        // Snap to the format's spec rate only when the capture format actually
        // changed with this mode switch (e.g. the user picked 14-bit in photo
        // mode before coming back). On a plain video → photo → video round trip
        // the format is unchanged, so the user's chosen frame rate is KEPT
        // (updateFpsMax still clamps it if the max shrank) — snapping to max on
        // every entry used to silently reset it.
        updateFpsMax(currentRoiW_, currentRoiH_, /*snapToMax=*/(finalCode != keepCode));
    }
}

int MainWindow::currentDepth() const
{
    return codeDepth(depthCombo_->currentData().toInt());
}

bool MainWindow::selectDepthEntry(int bits, bool serMode)
{
    for (int i = 0; i < depthCombo_->count(); ++i)
    {
        const int code = depthCombo_->itemData(i).toInt();
        if (codeDepth(code) == bits && codeSer(code) == serMode)
        {
            depthCombo_->setCurrentIndex(i);   // fires the handler -> worker syncs
            return true;
        }
    }
    return false;      // this camera cannot do that format
}

// Everything the panel derives from the connected camera. Runs after every
// successful open, so swapping the body (or plugging a different model in)
// re-labels the depth entries, drops depths the camera cannot capture, retitles
// the window and re-derives the frame-rate ceiling.
void MainWindow::applyCameraCaps()
{
    caps_ = worker_.caps();
    capsKnown_ = true;
    setWindowTitle(QString("%1 Camera").arg(caps_.name));

    // Depth entries follow the body (a colour body's say RGB, and a body with no
    // 2-byte readout has no deep entry to pick).
    setDepthComboForMode(modeToggle_->mode());

    // A measured frame-rate ceiling belongs to the body that produced it.
    fpsMeasuredMax_ = 0;
    fpsMeasW_ = fpsMeasH_ = fpsMeasBits_ = 0;
    fpsMeasSer_ = false;
    updateFpsMax(currentRoiW_, currentRoiH_);
    syncSliderToExposure();   // the exposure range is the camera's, not a constant

    // ---- white balance: the row belongs to colour bodies alone --------------
    // The model needs this body's NEUTRAL PAIR — the WB_R/WB_B values that
    // actually render a daylight-lit white target neutral — and the factory
    // defaults it reports are NOT that pair (measured: the ASI178MC's defaults
    // sit ~1.8x away in R/B ratio, which is why an earlier build of this GUI
    // read ~8300 K under 6500 K light and could not turn red at all at the top
    // of the slider; see the header of white_balance.h). So: the measured pair
    // when wb_probe has produced one for this body, otherwise (a documented
    // assumption, not a fact) the body's own default gains.
    wbCal_ = WbCal{};
    wbCal_.rMin = caps_.wbRMin; wbCal_.rMax = caps_.wbRMax;
    wbCal_.bMin = caps_.wbBMin; wbCal_.bMax = caps_.wbBMax;
    const bool anchorMeasured = wbMeasuredNeutral(caps_.name.toUtf8().constData(),
                                                   caps_.wbRMax, caps_.wbBMax,
                                                   wbCal_.rNeu, wbCal_.bNeu);
    if (!anchorMeasured)
    {
        wbCal_.rNeu = caps_.wbRDef > 0 ? (double)caps_.wbRDef
                                       : 0.5 * (caps_.wbRMin + caps_.wbRMax);
        wbCal_.bNeu = caps_.wbBDef > 0 ? (double)caps_.wbBDef
                                       : 0.5 * (caps_.wbBMin + caps_.wbBMax);
    }
    const bool showWb = caps_.isColor && caps_.wbControls() && wbValidCal(wbCal_);
    setWbVisible(showWb);
    if (showWb)
    {
        // A body whose caps report no support for the auto flag cannot run
        // AWB: uncheck it and apply the retained manual setting.
        const bool autoOk = caps_.wbRAuto || caps_.wbBAuto;
        awbCheck_->setEnabled(autoOk);
        if (!autoOk && awbCheck_->isChecked())
            awbCheck_->setChecked(false);
        updateWbControls();
        updateWbLabels();
        if (!awbCheck_->isChecked()) pushWbManual();
    }
}

void MainWindow::applyMode(int mode, bool fromUser)
{
    (void)fromUser;
    stack_->setCurrentIndex(mode);
    // The bit-depth row is shown in every mode now: stills offer 8/14-bit,
    // video offers 8/14-bit (8 -> H.264 or .ser, 14 -> uncompressed .ser).
    // Rebuild the selector's items for the mode and keep the row visible.
    setDepthComboForMode(mode);
    if (mode == 2 && recording_)
    {
        worker_.setRecording(false); // stop recording when leaving video mode
    }
    if (mode != 1 && seqRunning_)
    {
        worker_.stopSequence();      // stop a running sequence when leaving interval mode
    }
    // The range switch serves both photo and interval (the sequence uses
    // the main slider); video mode has no switch (exposure <= 1/fps).
    expRangeRow_->setVisible(mode == 0 || mode == 1);
    syncSliderToExposure();
}

void MainWindow::onModeChanged(int mode)
{
    applyMode(mode, true);
}

void MainWindow::onFpsChanged(int fps)
{
    currentFps_ = fps;
    worker_.setFps(fps);
    if (modeToggle_->mode() == 2) syncSliderToExposure();   // video: exposure is capped at 1/fps
}

// Frame-rate ceiling the slider may reach for this ROI + format: the estimate
// probed for the connected camera, raised by the rate the camera is actually
// delivering. The estimate is only that — a datasheet column for the ASI178
// family — so a body that streams faster is allowed to lift its own ceiling
// (fpsMeasuredMax_); the H.264 entry keeps its hard 60 fps codec cap.
int MainWindow::fpsCeilingFor(int w, int h, int bits, bool serMode) const
{
    int ceiling = effectiveCaps().maxFps(w, h, bits, serMode);
    if (serMode && fpsMeasuredMax_ > ceiling &&
        fpsMeasW_ == w && fpsMeasH_ == h && fpsMeasBits_ == bits && fpsMeasSer_ == serMode)
        ceiling = fpsMeasuredMax_;
    return ceiling;
}

void MainWindow::updateFpsMax(int w, int h, bool snapToMax)
{
    currentRoiW_ = w;
    currentRoiH_ = h;
    const int code = depthCombo_->currentData().isValid()
                     ? depthCombo_->currentData().toInt() : 8;
    const int maxFps = fpsCeilingFor(w, h, codeDepth(code), codeSer(code));
    fpsSlider_->blockSignals(true);
    fpsSlider_->setMaximum(maxFps);
    if (fpsSlider_->value() > maxFps)
        fpsSlider_->setValue(maxFps);   // clamp down; fires valueChanged -> onFpsChanged
    else if (snapToMax && fpsSlider_->value() < maxFps)
        fpsSlider_->setValue(maxFps);   // format/mode change: run the new mode at its spec rate
    fpsSlider_->blockSignals(false);
    fpsValue_->setText(QString::number(fpsSlider_->value()) + " fps");
    onFpsChanged(fpsSlider_->value());
}

// ---------------------------------------------------------------------------
// Photo
// ---------------------------------------------------------------------------

void MainWindow::onTakePhoto()
{
    if (photoBusy_) return;
    photoBusy_ = true;
    photoBtn_->setEnabled(false);
    photoBtn_->setBusy(true);   // circle dims while the exposure is in flight
    worker_.setExposure(currentExposure_);
    worker_.requestPhoto();
}

void MainWindow::onPhotoResult(const QString&, bool, const QString&)
{
    photoBusy_ = false;
    photoBtn_->setEnabled(true);
    photoBtn_->setBusy(false);
    updateStatus();
}

// ---------------------------------------------------------------------------
// Recording
// ---------------------------------------------------------------------------

void MainWindow::onToggleRecord()
{
    if (recording_) worker_.setRecording(false);
    else            worker_.setRecording(true);
}

void MainWindow::onRecordingStarted(const QString&)
{
    recording_ = true;
    recStart_ = QDateTime::currentDateTime();
    recBtn_->setRecording(true);
    updateStatus();
}

void MainWindow::onRecordingStopped(const QString&, bool)
{
    recording_ = false;
    recBtn_->setRecording(false);
    updateStatus();
}

uint64_t MainWindow::framesWritten()
{
    return worker_.getTelemetry().videoFramesWritten;
}

// ---------------------------------------------------------------------------
// Interval sequence
// ---------------------------------------------------------------------------

void MainWindow::onToggleSequence()
{
    if (seqRunning_)
    {
        worker_.stopSequence();
        seqBtn_->setStatusLine("Stopping…");
        return;
    }
    // The sequence uses the main exposure slider (no separate control).
    double expS      = currentExposure_;
    double intervalS = std::max(1.0, seqIntervalSpin_->value());
    int    count     = seqCountSpin_->value();
    seqRunning_ = true;
    seqBtn_->setRunning(true);
    seqBtn_->setStatusLine(QString("Starting · %1-bit · %2 exposure · %3 s interval · %4 image(s)")
        .arg(currentDepth())
        .arg(fmtExposure(expS))
        .arg(intervalS, 0, 'f', 1)
        .arg(count == 0 ? QString("continuous") : QString::number(count)));
    worker_.requestSequence(expS, intervalS, count);
}

void MainWindow::onSequenceStarted()
{
    seqStart_ = QDateTime::currentDateTime();
    seqTotal_ = 0;
    seqBtn_->setStatusLine("Capturing…");
}

void MainWindow::onSequenceExposing(int index, int total)
{
    // Exposure in progress (no countdown while the camera is exposing — the
    // interval countdown starts when this shot has finished).
    seqBtn_->setStatusLine((total > 0)
        ? QString("Exposing shot %1 / %2…").arg(index).arg(total)
        : QString("Exposing shot %1…").arg(index));
}

void MainWindow::onSequenceShot(int index, int total, const QString& path)
{
    seqTotal_ = index;
    QString s = (total > 0)
        ? QString("Shot %1 / %2").arg(index).arg(total)
        : QString("Shot %1").arg(index);
    s += path.isEmpty() ? "   — FAILED" : "   saved";
    seqBtn_->setStatusLine(s);
}

void MainWindow::onSequenceWait(double remainingS)
{
    QString s = (seqTotal_ > 0)
        ? QString("Shot %1 done · next in %2 s").arg(seqTotal_).arg(remainingS, 0, 'f', 1)
        : QString("Next shot in %1 s").arg(remainingS, 0, 'f', 1);
    seqBtn_->setStatusLine(s);
}

void MainWindow::onSequenceDone(int total, bool earlyStop)
{
    seqRunning_ = false;
    seqBtn_->setRunning(false);
    int elapsed = seqStart_.secsTo(QDateTime::currentDateTime());
    seqBtn_->setStatusLine(QString("%1: %2 image(s) in %3 s")
        .arg(earlyStop ? "Stopped" : "Done").arg(total).arg(elapsed));
    updateStatus();
}

// ---------------------------------------------------------------------------
// Errors / status
// ---------------------------------------------------------------------------

void MainWindow::onCameraError(const QString& msg)
{
    status_->setStyleSheet("color: #ff8a80; font-size: 16px;");
    status_->setText("⚠ " + msg);
}

void MainWindow::onCameraReconnected()
{
    status_->setStyleSheet("");   // drop the error styling, back to #status
    updateStatus();
}

void MainWindow::updateStatus()
{
    auto t = worker_.getTelemetry();

    // A camera that streams FASTER than the probed estimate raises its own
    // ceiling: the datasheet columns describe the ASI178 family, and the live
    // delivery rate (not recording) is the body's own answer for this exact ROI
    // and format. Only the .ser formats take the raise — the 8-bit H.264 entry
    // is capped by the encoder, not by the camera.
    const int dCode = depthCombo_->currentData().isValid()
                    ? depthCombo_->currentData().toInt() : 8;
    const int dBits = codeDepth(dCode);
    const bool dSer = codeSer(dCode);
    if (!recording_ && t.cameraOk && dSer && t.actualFps > 1.0)
    {
        const bool sameFormat = fpsMeasSer_ == dSer && fpsMeasBits_ == dBits &&
                                fpsMeasW_ == currentRoiW_ && fpsMeasH_ == currentRoiH_;
        const int live = (int)std::llround(t.actualFps * 1.05);   // small headroom
        fpsMeasuredMax_ = sameFormat ? std::max(fpsMeasuredMax_, live) : live;
        fpsMeasSer_ = dSer; fpsMeasBits_ = dBits;
        fpsMeasW_ = currentRoiW_; fpsMeasH_ = currentRoiH_;
        if (fpsCeilingFor(currentRoiW_, currentRoiH_, dBits, dSer) > fpsSlider_->maximum())
            updateFpsMax(currentRoiW_, currentRoiH_);   // raises the max, keeps the value
    }

    QString s;
    int bits = currentDepth();
    // A colour body says what the file will hold, so "14-bit RGB" is not
    // mistaken for the mono body's "14-bit" (they are different files).
    const bool colour = capsKnown_ && caps_.isColor;
    s += QString("Exposure %1   ·   Mode %2   ·   %3-bit%4\n")
             .arg(fmtExposure(currentExposure_),
                  modeToggle_->mode() == 0 ? "Photo" : (modeToggle_->mode() == 1 ? "Interval" : "Video"))
             .arg(bits)
             .arg(colour ? " RGB" : QString());
    if (t.cameraOk)
        s += QString("Frame rate %1 fps   ·   Gain %2   ·   Dropped %3   ·   Sensor %4 °C\n")
                 .arg(t.actualFps, 0, 'f', 1)
                 .arg(fmtGain(gainSlider_->value()))
                 .arg(t.dropped)
                 .arg(t.tempC, 0, 'f', 1);
    if (seqRunning_)
        s += QString("◷ SEQ %1   [%2-bit]\n").arg(seqBtn_->statusLine())
                 .arg(currentDepth());
    if (recording_)
    {
        int sec = (int)recStart_.secsTo(QDateTime::currentDateTime());
        s += QString("● REC  (%1:%2)   [%3 frames]\n")
                 .arg(sec / 60, 2, 10, QChar('0'))
                 .arg(sec % 60, 2, 10, QChar('0'))
                 .arg(t.videoFramesWritten);
    }
    status_->setText(s);
}

void MainWindow::onTimer()
{
    std::vector<unsigned char> buf;
    int w = 0, h = 0, channels = 1;
    int clipCount = worker_.getClipCount();
    // Worker-built display thumbnail (long edge <= kDispMax), grayscale or
    // red-marked RGB when pixels are clipped.
    if (worker_.getLatestFrame(buf, w, h, channels))
        view_->setFrame(buf.data(), buf.size(), w, h, channels);
    std::array<int, kHistBins> hist{};
    if (worker_.getHistogram(hist))
        histWidget_->setHistogram(hist);
    // clipping info: percentage of pixels at the max value, painted in the
    // histogram's top-right corner (no separate label -> no layout jump).
    // clipCount is computed over the RAW frame (full sensor size), so
    // normalize by the raw dimensions — the w/h above are the smaller
    // display thumbnail's, which would let the percentage exceed 100%.
    double clipPct = 0.0;
    if (clipCount > 0)
    {
        double rawTotal = (double)worker_.frameWidth() * worker_.frameHeight();
        // (clamped: a ROI change mid-flight could briefly pair a count from
        // the old frame size with the new one)
        if (rawTotal > 0.0) clipPct = std::min(100.0, 100.0 * clipCount / rawTotal);
    }
    histWidget_->setClipPct(clipPct);
    if (statusTick_++ % 8 == 0) updateStatus(); // ~4 Hz
}

// ---------------------------------------------------------------------------
// Smoke test + hidden sequence diagnostic
// ---------------------------------------------------------------------------

// Largest probed resolution up to ~2.5 MP (see main_window.h). Falls back to the
// geometry already in force when the probe has not answered yet.
void MainWindow::pickSmokeRoi()
{
    int bestW = 0, bestH = 0;
    long bestArea = -1;
    for (int i = 0; i < roiCombo_->count(); ++i)
    {
        const QString r = roiCombo_->itemData(i).toString();
        const int x = r.indexOf('x');
        if (x <= 0) continue;
        const long w = r.left(x).toLong(), h = r.mid(x + 1).toLong();
        const long area = w * h;
        if (area > 2600000) continue;
        if (bestArea < 0 || area > bestArea) { bestArea = area; bestW = int(w); bestH = int(h); }
    }
    if (bestW <= 0) return;   // probe has not answered yet: keep the current ROI
    smokeRoiW_ = bestW;
    smokeRoiH_ = bestH;
    worker_.setRoi(bestW, bestH);
    std::printf("SMOKE ROI %dx%d (largest probed resolution under ~2.5 MP)\n", bestW, bestH);
}

void MainWindow::setupSmokeTest()
{
    smokeActive_ = true;
    smokePhotoOk_ = true;        // AND of every photo result below

    QObject::connect(&worker_, &CameraWorker::photoResult, this,
                     [this](const QString& path, bool ok, const QString&) {
                         smokePhotoOk_ = smokePhotoOk_ && ok;
                         if (ok)
                             // The depth contract of a STILL is its container:
                             // .tif is the 16-bit (deep) output, .png the 8-bit
                             // one. Read it from the path — the GUI's combo at
                             // delivery time is a race (a long still's result
                             // can land after the test already switched the
                             // depth for the next shot; it did, live).
                             smokeStills_.push_back({path.toStdString(),
                                                     path.endsWith(".tif") ? 14 : 8,
                                                     worker_.frameWidth(), worker_.frameHeight()});
                     });
    QObject::connect(&worker_, &CameraWorker::recordingStarted, this,
                     [this](const QString& path) {
                         smokeRecStarted_++;
                         smokeRecPaths_.push_back(path.toStdString());
                         smokeRecDepths_.push_back(currentDepth());
                         smokeRecSize_.push_back({worker_.frameWidth(), worker_.frameHeight()});
                     });
    QObject::connect(&worker_, &CameraWorker::recordingStopped, this,
                     [this](const QString&, bool ok) { if (ok) smokeRecStopped_++; });
    QObject::connect(&worker_, &CameraWorker::sequenceDone, this,
                     [this](int total, bool) { smokeSeqDone_ = true; smokeSeqTotal_ = total; });
    QObject::connect(&worker_, &CameraWorker::sequenceShot, this,
                     [this](int, int, const QString& path) {
                         if (!path.isEmpty())
                             smokeStills_.push_back({path.toStdString(),
                                                     path.endsWith(".tif") ? 14 : 8,
                                                     worker_.frameWidth(), worker_.frameHeight()});
                     });

    // What this test asks for is decided by the CONNECTED camera, not by
    // constants: which depth counts as "deep", which gain value exists, which
    // ROI to record at, and (at the end) whether the files have to hold RGB or a
    // single channel. `./camera_app --smoke` therefore means the same thing on
    // any ASI body — it checks that body's own contract.
    QTimer::singleShot(1000, this, [this] {
        if (!selectDepthEntry(effectiveCaps().deepDepth(), false))
            depthCombo_->setCurrentIndex(0);      // deep-only camera: take what exists
        pickSmokeRoi();
        smokeGain_ = std::min(300, gainSlider_->maximum());
        std::printf("SMOKE camera=%s deep=%d-bit roi=%dx%d gain=%d\n",
                    capsKnown_ ? caps_.name.toUtf8().constData() : "(pending)",
                    effectiveCaps().deepDepth(), smokeRoiW_, smokeRoiH_, smokeGain_);
    });
    QTimer::singleShot(1500, this, [this] { gainSlider_->setValue(smokeGain_); });
    // White balance round trip (COLOUR bodies only — skipped silently on a
    // mono one): uncheck AWB through the real UI, set a distinctive manual
    // tint, then move the Temperature — the camera must apply exactly the
    // gains the sliders computed, and telemetry must report manual mode.
    // The two manual settings are independent, so the
    // temperature move must leave the tint slider ALONE (the old
    // per-temperature window dragged it). AWB is then re-checked, so the
    // saved shots below run in the default (auto) state like a colour
    // user's do.
    QTimer::singleShot(1600, this, [this] {
        if (!(capsKnown_ && caps_.isColor && caps_.wbControls() && wbShown_)) return;
        smokeWbRun_ = true;
        awbCheck_->setChecked(false);       // enables the retained manual settings
    });
    QTimer::singleShot(1750, this, [this] {
        if (!smokeWbRun_) return;
        // Drive the slider's drag path, including sliderReleased, instead of
        // only assigning values programmatically. A disabled handle cannot
        // satisfy this check even if setValue would move it in a test.
        auto drag = [](QSlider* slider) {
            const bool enabled = slider->isVisible() && slider->isEnabled();
            const int before = slider->value();
            const int step = std::max(1, 2 * slider->singleStep());
            const int target = before + step <= slider->maximum()
                             ? before + step : std::max(slider->minimum(), before - step);
            slider->setSliderDown(true);
            slider->setSliderPosition(target);
            slider->setSliderDown(false);
            return enabled && target != before && slider->value() == target;
        };
        const bool tempDrag = drag(wbTempSlider_);
        const bool tintDrag = drag(wbTintSlider_);
        smokeWbDragOk_ = tempDrag && tintDrag;
        std::printf("SMOKE wb handles: temp=%d tint=%d -> %s\n",
                    (int)tempDrag, (int)tintDrag, smokeWbDragOk_ ? "ok" : "FAIL");
        wbTintSlider_->setValue(60);        // a distinctive magenta shift
        wbTempSlider_->setValue(4000);      // the temperature must leave tint at +60
    });
    // The worker receives live manual pairs while the handles move.
    QTimer::singleShot(2300, this, [this] {
        if (!smokeWbRun_) return;
        auto t = worker_.getTelemetry();
        smokeWbOk_ = smokeWbOk_ && smokeWbDragOk_ &&
                    t.wbValid && !t.wbAuto &&
                    std::abs(t.wbR - wbCurR_) <= 3 && std::abs(t.wbB - wbCurB_) <= 3;
        std::printf("SMOKE wb: want R=%d B=%d  applied R=%d B=%d auto=%d -> %s\n",
                    wbCurR_, wbCurB_, t.wbR, t.wbB, (int)t.wbAuto, smokeWbOk_ ? "ok" : "FAIL");
        // The two numbers the user is actually shown — the SET balance
        // (manual labels show exactly what is set; the guard
        // below asserts that, including the absence of any asterisk).
        std::printf("SMOKE wb display (manual): %s  %s\n",
                    wbTempValue_->text().toUtf8().constData(),
                    wbTintValue_->text().toUtf8().constData());
        // And the guard for the coupling the user reported: the Temperature
        // move (whatever the camera was showing -> 4000 K) must have left the
        // Tint SETTING alone — full range, no clamp, value untouched — and the
        // CALCULATED values must be independent too: the labels show EXACTLY
        // what was set (4000 K, Tint +60), no read-back, no asterisk. The
        // body's headroom limits only the delivered image, never the numbers
        // the user is shown.
        const int tv = wbTintSlider_->value();
        const bool independent = (wbTempSlider_->value() == 4000) && (tv == 60);
        const bool labelsSet = (wbTempValue_->text() == QString("4000 K")) &&
                               (wbTintValue_->text() == QString("Tint +60")) &&
                               !wbTintValue_->text().contains('*');
        smokeWbOk_ = smokeWbOk_ && independent && labelsSet;
        std::printf("SMOKE wb values independent: temp=4000 K, tint slider still %+d, "
                    "labels show the set values (no *): %s\n",
                    tv, (independent && labelsSet) ? "ok" : "FAIL");
    });
    QTimer::singleShot(2400, this, [this] {
        if (!smokeWbRun_) return;
        awbCheck_->setChecked(true);        // back to the tested default: AWB on
    });
    // ... and the worker must successfully apply automatic white balance.
    // Check BEFORE still capture: a photo switches ROI/format and the camera
    // drops its WB state temporarily before the worker re-applies it.
    QTimer::singleShot(2850, this, [this] {
        if (!smokeWbRun_) return;
        auto t = worker_.getTelemetry();
        smokeWbOk_ = smokeWbOk_ && t.wbValid && t.wbAuto;
        std::printf("SMOKE wb: auto applied (last R=%d B=%d auto=%d) -> %s\n",
                    t.wbR, t.wbB, (int)t.wbAuto, (t.wbValid && t.wbAuto) ? "ok" : "FAIL");
        // AWB must leave the user's last manual slider positions untouched.
        std::printf("SMOKE wb display (AWB): %s  %s\n",
                    wbTempValue_->text().toUtf8().constData(),
                    wbTintValue_->text().toUtf8().constData());
    });
    // Deep still (14-bit on the ASI178s -> 16-bit TIFF, RGB on a colour body).
    // Starts after the WB auto phase for the reason above.
    QTimer::singleShot(3000, this, [this] { worker_.requestPhoto(); });
    // ... and a shallow one (8-bit -> PNG) so both still paths are checked.
    QTimer::singleShot(4000, this, [this] { selectDepthEntry(8, false); });
    QTimer::singleShot(5000, this, [this] { worker_.requestPhoto(); });
    // Video mode through the GUI toggle; the depth combo is rebuilt and the deep
    // .ser entry picked explicitly (the format switch runs applyRoi).
    QTimer::singleShot(5500, this, [this] {
        modeToggle_->setMode(2);
        worker_.setMode(CameraWorker::Video);
        selectDepthEntry(effectiveCaps().deepDepth(), true);
    });
    QTimer::singleShot(7000,  this, [this] { worker_.setRecording(true);  });
    QTimer::singleShot(9000,  this, [this] { worker_.setRecording(false); });
    // 8-bit .ser: RAW8 + HighSpeedMode (the 10-bit fast readout), copied straight
    // through by SerWriter::push8.
    QTimer::singleShot(9500,  this, [this] {
        if (!selectDepthEntry(8, true))
            std::printf("SMOKE note: this camera offers no 8-bit .ser entry\n");
    });
    QTimer::singleShot(10000, this, [this] { worker_.setRecording(true);  });
    QTimer::singleShot(12000, this, [this] { worker_.setRecording(false); });
    // 1-shot interval sequence (exercises the GUI sequence wiring,
    // including the exposure range switch in interval mode: set 0.5 s on
    // the short log range, then press the "1-60 s" switch, which clamps +
    // quantizes the exposure to 1 s before the sequence starts).
    QTimer::singleShot(12500, this, [this] {
        modeToggle_->setMode(1);                                   // -> interval mode
        expSlider_->setValue(secondsToSlider(0.5, kExpMinS, kExpShortMaxS)); // main slider -> 0.5 s
        expLongRange_ = true;                                       // press the "1-60 s" switch
        onExpRangeChanged();                                        // 0.5 s -> clamped/quantized to 1 s
        seqIntervalSpin_->setValue(5.0);
        seqCountSpin_->setValue(1);
        onToggleSequence();
    });
    // The sequence shot (1 s exposure, started at t=12500) lands at t≈13600;
    // doSequence then re-starts the video capture (ASIStartVideoCapture
    // re-initializes the camera, ~0.5-1.5 s) BEFORE emitting sequenceDone.
    // Check at t=16500 so the emission is never racing the check under load.
    QTimer::singleShot(16500, this, [this] {
        auto t = worker_.getTelemetry();
        const bool colour = capsKnown_ && caps_.isColor;
        const int wantCh = colour ? 3 : 1;

        // Read every file this run wrote back from disk. A colour body must
        // produce 3-channel files and a mono body single-channel ones, at the
        // depth and size they were asked for: this is the user-visible contract,
        // so the test checks the FILES, not the code path that wrote them.
        bool stillsOk = smokeStills_.size() >= 3;   // deep photo + 8-bit photo + seq shot
        for (const auto& s : smokeStills_)
        {
            const cv::Mat m = cv::imread(s.path, cv::IMREAD_UNCHANGED);
            const int wantDepth = (s.bits == 8) ? CV_8U : CV_16U;
            const bool good = !m.empty() && m.channels() == wantCh && m.depth() == wantDepth &&
                              m.cols == s.w && m.rows == s.h;
            std::printf("SMOKE still %s: %dx%d %d-bit %dch -> %s\n",
                        s.path.c_str(), s.w, s.h, s.bits, m.channels(), good ? "ok" : "FAIL");
            stillsOk = stillsOk && good;
        }

        // The recorded clips: v3 header, size for their byte width, and the
        // ColorID this camera implies (100 = 3-plane RGB for a colour body,
        // 0 = MONO for a mono one).
        const int expColorId = colour ? kSerColorRgb : kSerColorMono;
        bool serOk = true;
        int  serCount = 0;
        for (size_t i = 0; i < smokeRecPaths_.size(); ++i)
        {
            const int depth = smokeRecDepths_[i];
            const bool ok = validateSerFile(smokeRecPaths_[i], smokeRecSize_[i].first,
                                            smokeRecSize_[i].second, depth == 8 ? 8 : 16,
                                            expColorId);
            serOk = serOk && ok;
            ++serCount;
        }

        const bool framesOk = t.frameCount > 5;
        const bool seqOk = smokeSeqDone_ && smokeSeqTotal_ >= 1;
        const bool gainOk = (t.gainApplied == smokeGain_);
        const bool recOk = (smokeRecStarted_ >= 2 && smokeRecStopped_ >= 2
                            && smokeRecPaths_.size() >= 2);
        std::printf("SMOKE frames=%llu colour=%d stills=%d rec=%d/%d ser=%d seq=%d(f%d) "
                    "gain=%d wb=%s fps=%.1f temp=%.1fC\n",
                    (unsigned long long)t.frameCount, (int)colour, (int)smokeStills_.size(),
                    smokeRecStarted_, smokeRecStopped_, (int)serOk,
                    (int)smokeSeqDone_, smokeSeqTotal_, t.gainApplied,
                    smokeWbRun_ ? (smokeWbOk_ ? "ok" : "FAIL") : "skipped",
                    t.actualFps, t.tempC);
        qApp->exit(framesOk && smokePhotoOk_ && stillsOk && recOk && serOk && seqOk && gainOk
                   && smokeWbOk_ ? 0 : 1);
    });
}

void MainWindow::setupSeqTest(double expS, double intervalS, int count)
{
    auto t0 = std::chrono::steady_clock::now();
    QObject::connect(&worker_, &CameraWorker::sequenceShot, this, [t0](int index, int total, const QString& path) {
        double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("SEQTEST shot %d/%d completed at t=%.2f s   %s\n",
                    index, total, dt, path.isEmpty() ? "(FAILED)" : qPrintable(path));
    });
    QObject::connect(&worker_, &CameraWorker::sequenceDone, this, [t0](int total, bool earlyStop) {
        double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("SEQTEST done  total=%d  earlyStop=%d  elapsed=%.2f s\n",
                    total, (int)earlyStop, dt);
        qApp->exit(0);
    });
    QTimer::singleShot(2000, this, [this, expS, intervalS, count] {
        worker_.setBitDepth(8);            // force a format switch INSIDE doSequence
        worker_.requestSequence(expS, intervalS, count);
    });
}

// Hidden diagnostic (see header). Records a .ser clip at the requested
// ROI / bit depth / fps, then reports how fast frames actually flowed through
// the whole pipeline (camera -> USB -> worker -> SerWriter -> disk).
void MainWindow::setupVTest(int w, int h, int bits, int fps, double durS, bool serOut)
{
    vtBits_ = bits;
    auto readSerCount = [](const std::string& path) -> uint64_t {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return 0;
        uint32_t cnt = 0;
        std::fseek(f, 38, SEEK_SET);
        if (std::fread(&cnt, 1, 4, f) != 4) cnt = 0;
        std::fclose(f);
        return cnt;
    };

    QObject::connect(&worker_, &CameraWorker::recordingStarted, this,
                     [this](const QString& path) {
        vtRecPath_ = path.toStdString();
        vtRecStart_ = std::chrono::steady_clock::now();
        std::fprintf(stderr, "[vtest] recording started %s\n", qPrintable(path));
    });
    // The stop is processed by the worker thread; give it a moment to
    // finalize the file (close() patches the frame count), then read the
    // frame count out of the header and report the measured rate.
    auto summarize = [this, w, h, bits, fps, serOut, readSerCount] {
        auto t1 = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(t1 - vtRecStart_).count();
        auto t = worker_.getTelemetry();
        // .ser: the header's frame count is authoritative. MP4: count what the
        // encoder says it encoded, and check the file is actually there.
        uint64_t frames = serOut ? readSerCount(vtRecPath_) : t.videoFramesWritten;
        std::error_code ec;
        const auto bytes = serOut ? 0ULL : static_cast<uint64_t>(std::filesystem::file_size(vtRecPath_, ec));
        std::fprintf(stderr,
                     "[vtest] %dx%d %d-bit %s target=%d fps  written=%llu  measured=%.1f fps "
                     "ema=%.1f fps  camDropped=%llu appDropped=%llu  bytes=%llu  path=%s\n",
                     w, h, bits, serOut ? "ser" : "mp4", fps,
                     (unsigned long long)frames, frames / std::max(elapsed, 0.1),
                     t.actualFps,
                     (unsigned long long)t.dropped,
                     (unsigned long long)t.videoFramesDropped,
                     (unsigned long long)bytes,
                     vtRecPath_.c_str());
        std::fflush(stderr);
    };

    QTimer::singleShot(2000, this, [this, w, h, bits, fps, durS, serOut] {
        worker_.setMode(CameraWorker::Video);
        worker_.setRoi(w, h);
        worker_.setBitDepth(bits);
        worker_.setSerMode(serOut);        // .ser, or 8-bit H.264 -> MP4 with --vtestser 0
        worker_.setFps(fps);
    });
    QTimer::singleShot(4500, this, [this] { worker_.setRecording(true);  });
    QTimer::singleShot((int)((4.5 + (std::max(durS, 1.0))) * 1000), this,
                       [this, summarize] {
        worker_.setRecording(false);
        QTimer::singleShot(2500, this, summarize);   // after the file is finalized
    });
    QTimer::singleShot((int)((5.0 + (std::max(durS, 1.0))) * 1000) + 2500, this,
                       [] { qApp->exit(0); });
}

// Hidden diagnostic (see header). Drives the photo-mode SLOW preview and
// measures the real preview update rate over a window that is fully inside the
// slow regime (full exposure on the video stream; the camera delivers at
// 1/exposure). With --prevexp2 it also tests the live slow<->fast transition.
void MainWindow::setupPrevTest(int w, int h, int bits, double expS, double durS)
{
    // Drive the REAL GUI controls (mode, ROI, depth, exposure slider) so the
    // GUI state (currentExposure_, fps snap) and the worker stay in sync — a
    // direct worker_.setExposure() gets clobbered by the GUI's exposure push.
    QTimer::singleShot(2000, this, [this, w, h, bits, expS] {
        modeToggle_->setMode(CameraWorker::Photo);        // photo mode (rebuilds depth combo)
        // select the ROI (combo data is "WxH")
        for (int i = 0; i < roiCombo_->count(); ++i)
            if (roiCombo_->itemData(i).toString() == QString("%1x%2").arg(w).arg(h))
                { roiCombo_->setCurrentIndex(i); break; }
        // Name the format rather than trusting an index (how many entries the
        // selector has depends on the camera).
        if (!selectDepthEntry(bits, false))
            depthCombo_->setCurrentIndex(0);
        double tmin, tmax; bool linear = false;
        exposureRange(tmin, tmax, &linear);
        double clamped = std::clamp(expS, tmin, tmax);
        int pos = linear ? secondsToSliderLinear(clamped, tmin, tmax)
                         : secondsToSlider(clamped, tmin, tmax);
        expSlider_->setValue(pos);                         // fires onSliderMoved -> worker.setExposure
        std::fprintf(stderr, "[prevtest] GUI: photo %d-bit %dx%d exposure=%.4f s slider=%d range=%.1fus..%gs linear=%d\n",
                     bits, w, h, clamped, pos, tmin * 1e6, tmax, (int)linear);
        std::fflush(stderr);
    });
    // 2. baseline once the slow preview has settled (1.5 s after config)
    QTimer::singleShot(3500, this, [this] {
        pvStart_ = std::chrono::steady_clock::now();
        pvBase_ = worker_.getTelemetry().frameCount;
    });
    // 2b. optional: switch to a second exposure at the window midpoint (tests
    //     the slow<->fast transition live)
    if (pvExp2_ > 0.0)
    {
        QTimer::singleShot((int)(3500 + durS * 500), this, [this] {
            double tmin, tmax; bool linear = false;
            exposureRange(tmin, tmax, &linear);
            double clamped = std::clamp(pvExp2_, tmin, tmax);
            int pos = linear ? secondsToSliderLinear(clamped, tmin, tmax)
                             : secondsToSlider(clamped, tmin, tmax);
            expSlider_->setValue(pos);
            pvMid_ = std::chrono::steady_clock::now();
            pvMidFrames_ = worker_.getTelemetry().frameCount;
            std::fprintf(stderr, "[prevtest] mid: exposure -> %.3f s (slider=%d)\n", clamped, pos);
            std::fflush(stderr);
        });
    }
    // 3. measure the preview rate over the window
    QTimer::singleShot((int)(3500 + durS * 1000), this, [this, bits, expS] {
        auto t1 = std::chrono::steady_clock::now();
        auto tele = worker_.getTelemetry();
        if (pvMid_ != std::chrono::steady_clock::time_point{})
        {
            // two phases: [start, mid] and [mid, end]
            double p1 = std::chrono::duration<double>(pvMid_ - pvStart_).count();
            double p2 = std::chrono::duration<double>(t1 - pvMid_).count();
            uint64_t f1 = pvMidFrames_ - pvBase_;
            uint64_t f2 = tele.frameCount - pvMidFrames_;
            std::fprintf(stderr,
                         "[prevtest] phase1 %d-bit exp=%.3f s window=%.2f s frames=%llu measured=%.2f fps expected=%.2f fps\n",
                         bits, expS, p1, (unsigned long long)f1, f1 / std::max(p1, 0.1), 1.0 / expS);
            std::fprintf(stderr,
                         "[prevtest] phase2 %d-bit exp=%.3f s window=%.2f s frames=%llu measured=%.2f fps expected=%.2f fps ema=%.2f fps\n",
                         bits, pvExp2_, p2, (unsigned long long)f2, f2 / std::max(p2, 0.1), 1.0 / pvExp2_, tele.actualFps);
        }
        else
        {
            double elapsed = std::chrono::duration<double>(t1 - pvStart_).count();
            uint64_t frames = tele.frameCount - pvBase_;
            double measured = frames / std::max(elapsed, 0.1);
            std::fprintf(stderr,
                         "[prevtest] %d-bit exposure=%.3f s  window=%.2f s  preview-frames=%llu  "
                         "measured=%.2f fps  expected(1/exp)=%.2f fps  ema=%.2f fps\n",
                         bits, expS, elapsed, (unsigned long long)frames, measured,
                         1.0 / expS, tele.actualFps);
        }
        std::fflush(stderr);
        qApp->exit(0);
    });
}

void MainWindow::setupFpsTest()
{
    // GUI-driven regression test for the video frame-rate slider (2026-09-16):
    // the fps chosen in video mode must SURVIVE a video -> photo -> video round
    // trip — entering video mode used to snap the slider to the spec max
    // unconditionally, silently resetting the user's choice. A genuine format
    // change (14-bit picked in photo mode before coming back) must still re-snap
    // to the new format's spec rate, so that path is asserted too.
    // 50: valid under the full-res 8-bit H.264 max (60) but ABOVE the
    // full-res 14-bit .ser spec max (30) — so check A (round trip keeps 50)
    // and check B (format change snaps DOWN to 30) are both discriminating.
    const int SET_FPS = 50;
    QTimer::singleShot(2000, this, [this] {
        modeToggle_->setMode(CameraWorker::Video);   // GUI -> video (8-bit H.264, full ROI)
        worker_.setMode(CameraWorker::Video);
    });
    QTimer::singleShot(2500, this, [this, SET_FPS] {
        fpsSlider_->setValue(SET_FPS);               // user picks 50 fps
    });
    QTimer::singleShot(3000, this, [this] {
        modeToggle_->setMode(CameraWorker::Photo);   // -> photo (same 8-bit format)
        worker_.setMode(CameraWorker::Photo);
    });
    QTimer::singleShot(3500, this, [this] {
        modeToggle_->setMode(CameraWorker::Video);   // -> back to video (format unchanged)
        worker_.setMode(CameraWorker::Video);
    });
    QTimer::singleShot(4000, this, [this, SET_FPS] {
        const int depthIdx = depthCombo_->currentIndex();   // video: 0 = 8-bit (H.264)
        fpstestRoundTripOk_ = (fpsSlider_->value() == SET_FPS) && (depthIdx == 0);
        std::fprintf(stderr,
                     "[fpstest] round-trip: slider=%d (want %d) depthIdx=%d (want 0) -> %s\n",
                     fpsSlider_->value(), SET_FPS, depthIdx,
                     fpstestRoundTripOk_ ? "ok" : "FAIL");
        // Now the format-changing round trip: 14-bit in photo mode, then back
        // to video (14-bit .ser) — the slider must snap to that format's max.
        modeToggle_->setMode(CameraWorker::Photo);
        worker_.setMode(CameraWorker::Photo);
        depthCombo_->setCurrentIndex(1);           // photo: 1 = 14-bit (16-bit TIFF)
    });
    QTimer::singleShot(4500, this, [this] {
        modeToggle_->setMode(CameraWorker::Video);   // -> video: 14-bit .ser
        worker_.setMode(CameraWorker::Video);
    });
    QTimer::singleShot(5000, this, [this] {
        const int code = depthCombo_->currentData().toInt();
        const int expect = maxFpsForMode(currentRoiW_, currentRoiH_,
                                         codeDepth(code), codeSer(code));
        fpstestSnapOk_ = (depthCombo_->currentIndex() == 2) && (fpsSlider_->value() == expect);
        std::fprintf(stderr,
                     "[fpstest] format-change: slider=%d (want %d = 14-bit .ser max) -> %s\n",
                     fpsSlider_->value(), expect, fpstestSnapOk_ ? "ok" : "FAIL");
        std::fflush(stderr);
        const bool ok = fpstestRoundTripOk_ && fpstestSnapOk_;
        std::fprintf(stderr, "FPSTEST %s\n", ok ? "PASS" : "FAIL");
        qApp->exit(ok ? 0 : 1);
    });
}
