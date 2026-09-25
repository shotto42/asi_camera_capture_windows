// main_window.h
//
// MainWindow: the application window — frame display on the left, all controls
// in a touch-friendly side panel on the right, the 33 ms display timer, and
// the GUI-side wiring to the CameraWorker.
#pragma once

#include <QDateTime>
#include <QMainWindow>

#include <string>
#include <vector>

#include "camera_worker.h"
#include "white_balance.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QSlider;
class QSpinBox;
class QStackedWidget;
class QTimer;
class QWidget;
class FrameView;
class HistogramWidget;
class ModeToggle;
class RecordButton;
class SequenceButton;
class ShutterButton;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(bool smoke, bool seqtest = false,
                        double stExp = 0.5, double stInterval = 5.0, int stCount = 3,
                        bool vtest = false, int vtW = 480, int vtH = 320,
                        int vtBits = 8, int vtFps = 404, double vtDur = 4.0,
                        bool prevtest = false, int pvW = 480, int pvH = 320,
                        int pvBits = 14, double pvExp = 0.1, double pvDur = 6.0,
                        double pvExp2 = 0.0, bool fpstest = false,
                        int bayerOverride = -1, bool vtSerOut = true);

    ~MainWindow() override
    {
        worker_.requestQuit();
        worker_.wait(8000);
    }

    // Test hook for offscreen screenshots: switch to a given mode
    // (0 = photo, 1 = interval, 2 = video).
    void setUiMode(int m);

private:
    // ---- construction (called in order from the ctor) --------------------
    void setupUi();              // central layout + all widgets
    void setupConnections();     // signal/slot wiring
    void setupInitialState();    // timer, initial exposure/mode, worker start

    // ---- exposure slider ----------------------------------------------------
    // Slider range (tmin, tmax) for the current mode + range switch:
    //   photo    switch off -> 32 µs .. 1 s;  switch on -> 1 .. 60 s (linear,
    //            whole-second steps)
    //   video    32 µs .. frame period (exposure must fit in one frame)
    //   interval SAME ranges + switch as photo mode (the sequence takes its
    //            exposure from this slider — there is no separate control)
    void exposureRange(double& tmin, double& tmax, bool* linear = nullptr) const;

    void onSliderMoved(int pos);
    void onExpRangeChanged();
    void onGainChanged(int gain01);
    void syncSliderToExposure();

    // ---- white balance (colour bodies only; see white_balance.h) ------------
    // The row (AWB checkbox + Temperature/Tint sliders) exists only for a
    // colour body: it stays hidden until cameraReady reports IsColorCam with
    // WB controls, and a mono body never sees it. AWB is checked by default;
    // while it is, the sliders are disabled and retain the user's last manual
    // Temperature/Tint values. Switching AWB off reapplies those values.
    void setWbVisible(bool on);              // whole WB row (3 widgets)
    void updateWbLabels();                   // slider values -> value labels
    // Enabled states only; the independent slider values are never set by AWB.
    void updateWbControls();
    void pushWbManual();                     // sliders -> gains -> worker (manual)

    // ---- mode / fps ----------------------------------------------------------
    // Fixed widths for the ROI and bit-depth selectors, side by side in the
    // 420 px panel. Each selector is sized to its own longest label — the ROI
    // one to its probed items, the depth one to the union of BOTH modes'
    // labels (its items swap on a mode change; the union keeps the width
    // stable and the rows from reflowing).
    //
    // The label font and the combos' non-text chrome (left pad + drop-down)
    // are measured once on a throwaway, polished combo. The live combos can't
    // be the reference: the first call happens during construction, before
    // they're polished (font and sizeHint are still the unstyled defaults),
    // and their sizeHints stay cached across item rebuilds — both produced
    // bogus padding.
    void syncRoiDepthWidths();

    // Rebuild the bit-depth selector for the given mode and restore the
    // selection where it's still valid. Item data encodes depth + the .ser flag
    // (see depthCode): video offers 8-bit H.264, 8-bit .ser, 14-bit .ser;
    // stills offer 8/14-bit (never .ser). The .ser flag is dropped when it
    // can't be represented (stills). The worker is synced explicitly because
    // the item changes are signal-blocked.
    void setDepthComboForMode(int mode);

    // Bit depth of the current combo selection (strips the .ser flag).
    int currentDepth() const;

    // Pick a depth entry by what it MEANS (depth + .ser flag) rather than by
    // index: how many entries the selector has depends on the connected camera,
    // so "index 1" is only meaningful while one specific model is attached.
    // Returns false if the camera cannot do that format (nothing selected).
    bool selectDepthEntry(int bits, bool serMode);

    void applyMode(int mode, bool fromUser);
    void onModeChanged(int mode);
    void onFpsChanged(int fps);

    // Recompute the fps-slider maximum for the current ROI + video format
    // (the ZWO spec ceiling, mode-aware) and clamp the current value into range.
    // When snapToMax is true (bit-depth / format / mode change), the slider is
    // also RAISED to the new maximum so the new mode runs at its spec rate —
    // a leftover value from the previous mode would pace the faster readout
    // down and drop frames (e.g. 14-bit 30 fps carried into 8-bit, which can
    // run to 60 at full res).
    void updateFpsMax(int w, int h, bool snapToMax = false);

    // The smoke test's ROI: the largest probed resolution that stays under
    // ~2.5 MP, so the two .ser clips keep a sane size on any body (the probed
    // list only contains sizes the camera accepted, so the pick is always
    // legal). Falls back to the current geometry before the probe answers.
    void pickSmokeRoi();

    // ---- the connected camera ---------------------------------------------
    // Everything the UI adapts to — the camera's name for the title, whether it
    // is colour or mono (which decides whether the files say RGB), the bit
    // depths it can capture at all, its exposure limits and frame-rate ceiling —
    // comes from CameraCaps. This runs after EVERY successful camera open, so
    // plugging in a different body re-labels and re-ranges the selectors.
    void applyCameraCaps();

    // Label for one bit-depth entry: "8-bit  (PNG)" for a mono body and
    // "8-bit RGB (PNG)" for a colour one — `container` is what the file will be
    // ("PNG", "16-bit TIFF", "H.264", ".ser"). Saying RGB out loud is the point:
    // it is what lands on disk, and the two are not interchangeable.
    QString depthLabel(int bits, const QString& container) const;

    // The camera's capabilities, or a stand-in used until the first camera open
    // so the panel is fully populated (and the offscreen tests deterministic)
    // before the hardware answers.
    const CameraCaps& effectiveCaps() const;

    // Frame-rate ceiling the slider may reach for this ROI + format: the
    // estimate probed from the camera, raised by the rate the camera is
    // actually delivering (see fpsMeasuredMax_).
    int fpsCeilingFor(int w, int h, int bits, bool serMode) const;

    // ---- photo ---------------------------------------------------------------
    void onTakePhoto();
    void onPhotoResult(const QString&, bool, const QString&);

    // ---- recording -------------------------------------------------------------
    void onToggleRecord();
    void onRecordingStarted(const QString&);
    void onRecordingStopped(const QString&, bool);

    uint64_t framesWritten();

    // ---- interval sequence -------------------------------------------------------
    void onToggleSequence();
    void onSequenceStarted();
    void onSequenceExposing(int index, int total);
    void onSequenceShot(int index, int total, const QString& path);
    void onSequenceWait(double remainingS);
    void onSequenceDone(int total, bool earlyStop);

    // ---- errors / status --------------------------------------------------------
    void onCameraError(const QString& msg);
    void onCameraReconnected();
    void updateStatus();
    void onTimer();

    // ---- smoke test ---------------------------------------------------------------
    // Drives the app end-to-end offscreen: a 14-bit still, a 14-bit .ser clip and
    // an 8-bit .ser clip (both validated against the v3 spec), a gain change, and a
    // 1-shot interval sequence. Exits 0 only if every check passes.
    void setupSmokeTest();

    // Hidden diagnostic: run a timed interval sequence offscreen and print the
    // per-shot completion times so the spacing can be verified (the interval
    // always elapses between shots: completion-to-completion ≈ exposure +
    // interval, independent of the exposure length).
    //   ./camera_app --seqtest --seqexp 0.5 --seqinterval 5 --seqcount 3
    void setupSeqTest(double expS, double intervalS, int count);

    // Hidden diagnostic: record a .ser clip for a fixed duration at the given
    // ROI / bit depth / fps and print the achieved rate (frames written,
    // measured fps, EMA fps, camera-side + app-side dropped frames). With
    // CAMDBG=1 the worker's [perf] lines add the per-stage timing breakdown
    // (grab / disp+hist / period). Used to separate USB bandwidth limits from
    // app-side (display/encoding/disk) limits:
    //   ./camera_app --vtest --vtestroi 480x320 --vtestbits 8 --vtestfps 404 --vtestdur 4
    // --vtest: record one clip and report the achieved rate. With serOut the
    // clip is an uncompressed .ser (rate read back from its header); without it
    // the clip is 8-bit H.264/MP4, whose frames are counted from telemetry and
    // whose file is only checked to be non-trivial (the colour of its frames is
    // the demosaic the headless --colourtest already pins down).
    void setupVTest(int w, int h, int bits, int fps, double durS, bool serOut = true);

    // Hidden diagnostic: drive the photo-mode SLOW preview (`slowPreview_` —
    // the video stream kept running with the FULL exposure set, so the camera
    // stretches the frame period) and measure the real preview update rate.
    // Expected: the preview updates at 1/exposure (physics). A rate stuck well
    // below 1/exposure indicates a fixed per-frame cost in the slow path.
    //   ./camera_app --prevtest --prevexp 0.1 --prevbits 14 --prevroi 480x320 --prevdur 6
    void setupPrevTest(int w, int h, int bits, double expS, double durS);

    // Hidden regression test (GUI-driven, real camera): the frame rate chosen
    // in video mode must SURVIVE a video -> photo -> video round trip (it used
    // to snap back to the max on every entry into video mode), while a genuine
    // format change (14-bit picked in photo mode) still re-snaps to the new
    // format's spec rate.
    //   ./camera_app --fpstest
    void setupFpsTest();

    // members
    CameraWorker worker_;
    // What the connected camera can do, mirrored from the worker on every
    // cameraReady (the first open and every reconnect). See camera_caps.h.
    CameraCaps caps_;
    bool capsKnown_ = false;
    int bayerOverride_ = -1;        // --bayer rggb|bggr|grbg|gbrg (-1 = the camera's own)
    // The live delivery rate the camera reached for one ROI + format, which can
    // be above the probed estimate (an estimate is a floor for the slider, not
    // a hard ceiling). Reset whenever the ROI or format changes.
    int fpsMeasuredMax_ = 0;
    int fpsMeasW_ = 0, fpsMeasH_ = 0, fpsMeasBits_ = 0;
    bool fpsMeasSer_ = false;
    FrameView* view_ = nullptr;
    ModeToggle* modeToggle_ = nullptr;
    QComboBox* roiCombo_ = nullptr;
    QComboBox* depthCombo_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    QSlider* expSlider_ = nullptr;
    QLabel* expValue_ = nullptr;
    QWidget* expRangeRow_ = nullptr;     // exposure range switch (sits in the exposure row; photo + interval modes)
    bool expLongRange_ = false;          // false: 32 µs..1 s, true: 1..60 s
    QSlider* gainSlider_ = nullptr;
    QLabel* gainValue_ = nullptr;
    // ---- white balance (colour bodies only; hidden otherwise) ----
    QWidget* wbRow_ = nullptr;          // "AWB" checkbox + the two value labels
    QCheckBox* awbCheck_ = nullptr;     // automatic white balance, checked by default
    QSlider* wbTempSlider_ = nullptr;   // colour temperature (value IS the Kelvin)
    QSlider* wbTintSlider_ = nullptr;   // green(-)/magenta(+) tint
    QTimer* wbManualTimer_ = nullptr;    // pace live manual updates while dragging
    QLabel* wbTempValue_ = nullptr;
    QLabel* wbTintValue_ = nullptr;
    bool wbShown_ = false;              // a colour body with WB controls answered
    WbCal wbCal_;                       // the connected camera's WB model input
    int wbCurR_ = 0, wbCurB_ = 0;       // last manual gains sent to the worker
    ShutterButton* photoBtn_ = nullptr;
    RecordButton* recBtn_ = nullptr;
    QSlider* fpsSlider_ = nullptr;
    QLabel* fpsValue_ = nullptr;
    QDoubleSpinBox* seqIntervalSpin_ = nullptr;
    QSpinBox* seqCountSpin_ = nullptr;
    SequenceButton* seqBtn_ = nullptr;
    QLabel* status_ = nullptr;
    HistogramWidget* histWidget_ = nullptr;
    QTimer* timer_ = nullptr;

    double currentExposure_ = 0.010;
    int currentFps_ = 30;
    int currentRoiW_ = 3096;     // for recomputing the fps-slider maximum on ROI change
    int currentRoiH_ = 2080;
    bool recording_ = false;
    bool photoBusy_ = false;
    QDateTime recStart_;
    bool seqRunning_ = false;
    QDateTime seqStart_;
    int seqTotal_ = 0;
    int statusTick_ = 0;

    // smoke
    bool smokePhotoOk_ = false;
    int  smokeRecStarted_ = 0;      // count of recordingStarted signals (2: 14-bit + 8-bit .ser)
    int  smokeRecStopped_ = 0;      // count of recordingStopped signals
    // What the test asked each file to be, so the checks that run at the end can
    // verify what the CONNECTED camera actually produced (a colour body must
    // produce RGB files, a mono body single-channel ones, at the size asked).
    struct SmokeStill { std::string path; int bits; int w; int h; };
    std::vector<SmokeStill> smokeStills_;
    std::vector<std::string> smokeRecPaths_;   // one per recorded clip
    std::vector<int> smokeRecDepths_;          // the depth each clip was asked for
    std::vector<std::pair<int,int>> smokeRecSize_;  // and the ROI it was asked at
    bool smokeActive_ = false;    // --smoke: the panel follows the camera, not constants
    bool smokeStarted_ = false;   // the timeline starts when the camera answers
    int smokeGain_ = 300;         // gain value the smoke test drives
    int smokeWbR_ = 0, smokeWbB_ = 0;   // manual WB gains the smoke test drives (colour only)
    bool smokeWbRun_ = false;     // the WB step ran (colour body with WB controls)
    bool smokeWbOk_ = true;       // its read-back verdict (true when skipped)
    bool smokeWbDragOk_ = false;   // both enabled sliders accepted a drag
    int smokeRoiW_ = 0, smokeRoiH_ = 0;   // ROI the smoke test records at
    bool smokeSeqDone_ = false;
    int  smokeSeqTotal_ = 0;

    // vtest
    std::chrono::steady_clock::time_point vtRecStart_{};
    std::string vtRecPath_;
    int vtBits_ = 8;

    // prevtest
    std::chrono::steady_clock::time_point pvStart_{};
    uint64_t pvBase_ = 0;
    double pvExp2_ = 0.0;   // if > 0: switch to this exposure at the window midpoint
    std::chrono::steady_clock::time_point pvMid_{};
    uint64_t pvMidFrames_ = 0;

    // fpstest
    bool fpstestRoundTripOk_ = true;  // video->photo->video kept the chosen fps
    bool fpstestSnapOk_ = true;       // a real format change re-snapped to spec max
};
