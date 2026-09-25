// camera_worker.h
//
// CameraWorker: owns the ASI SDK and the video encoder.
// All SDK calls happen on this thread.
#pragma once

#include <QThread>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "camera_caps.h"
#include "colour.h"
#include "constants.h"
#include "gst_video_encoder.h"
#include "ser_writer.h"

class CameraWorker : public QThread
{
    Q_OBJECT
public:
    // Mode order matches the GUI toggle (0 = photo, 1 = interval, 2 = video).
    enum Mode { Photo = 0, Interval = 1, Video = 2 };

    struct Telemetry
    {
        double actualFps = 0.0;
        int dropped = 0;
        double tempC = 0.0;
        bool cameraOk = false;
        uint64_t frameCount = 0;
        uint64_t videoFramesWritten = 0;
        uint64_t videoFramesDropped = 0;
        uint64_t previewBuilds = 0;  // display thumbnails built by the preview builder (30 Hz)
        int gainApplied = 0;      // last gain value applied to the camera (0.1 dB)
        // White balance (colour bodies): gains sampled at open or last sent
        // successfully in manual mode. AWB does not change manual GUI values;
        // periodic WB SDK reads are avoided in the video drain loop.
        int  wbR = 0, wbB = 0;
        bool wbAuto = false;
        bool wbValid = false;
    };

    CameraWorker() = default;

    // Stops the preview builder thread (idempotent; also runs from run()'s
    // exit path so the join is prompt).
    ~CameraWorker()
    {
        previewQuit_ = true;
        if (previewThread_.joinable()) previewThread_.join();
    }

    // ---- config setters (GUI thread) -------------------------------------
    void setMode(int mode)          { std::lock_guard<std::mutex> l(cfgMtx_); cfg_.mode = mode; }
    void setExposure(double s)      { std::lock_guard<std::mutex> l(cfgMtx_); cfg_.exposureS = s; }
    void setFps(int fps)            { std::lock_guard<std::mutex> l(cfgMtx_); cfg_.fps = std::clamp(fps, 1, 1000); }
    void setRoi(int w, int h)       { std::lock_guard<std::mutex> l(cfgMtx_); cfg_.roiW = w; cfg_.roiH = h; }
    void setBitDepth(int bits)      { std::lock_guard<std::mutex> l(cfgMtx_); cfg_.bitDepth = (bits == 14) ? 14 : (bits == 10) ? 10 : 8; }
    void setSerMode(bool ser)       { std::lock_guard<std::mutex> l(cfgMtx_); cfg_.serMode = ser; }
    void setGain(int gain)          { std::lock_guard<std::mutex> l(cfgMtx_); cfg_.gain = gain; }
    // White balance (colour bodies only — a mono body's WB controls are never
    // touched, keeping its data neutral as before). Sent as one triple so a
    // mode change and the manual gains can never be applied out of order:
    // autoWb = the camera's automatic white balance (SDK auto flags on the WB
    // controls); with it off, wbR/wbB are the manual gains (raw SDK units,
    // clamped to the probed caps on apply). The GUI's Kelvin/Tint sliders are
    // converted to the gains by white_balance.h — the worker only ever sees
    // raw values.
    void setWhiteBalance(bool autoWb, int wbR, int wbB)
    {
        std::lock_guard<std::mutex> l(cfgMtx_);
        cfg_.autoWb = autoWb; cfg_.wbR = wbR; cfg_.wbB = wbB;
    }
    void setRecording(bool on)      { std::lock_guard<std::mutex> l(cfgMtx_); cfg_.recording = on; }
    void requestPhoto()             { std::lock_guard<std::mutex> l(cfgMtx_); cfg_.photoRequested = true; }
    void requestQuit()              { std::lock_guard<std::mutex> l(cfgMtx_); cfg_.quitting = true; }
    // Interval sequence: start a back-to-back long-exposure sequence. The next
    // shot starts `interval` (>= 1 s) after the previous shot FINISHED — the
    // interval is the time between images and always elapses, independent of
    // the exposure (a long exposure is followed by the full interval).
    void requestSequence(double expS, double intervalS, int count)
    {
        std::lock_guard<std::mutex> l(cfgMtx_);
        cfg_.sequenceRequested = true;
        cfg_.sequenceExpS = expS;
        cfg_.sequenceIntervalS = intervalS;
        cfg_.sequenceCount = count;   // 0 = continuous
    }
    void stopSequence()             { seqStop_ = true; }

    // ---- data getters (GUI thread) ---------------------------------------
    // Display thumbnail built by the worker (long edge <= kDispMax): 1-channel
    // grayscale, or 3-channel RGB with clipped pixels marked red.
    bool getLatestFrame(std::vector<unsigned char>& out, int& w, int& h, int& channels)
    {
        std::lock_guard<std::mutex> l(frameMtx_);
        if (latest_.empty()) return false;
        out = latest_;
        w = latestW_;
        h = latestH_;
        channels = latestCh_;
        return true;
    }

    bool getHistogram(std::array<int, kHistBins>& out)
    {
        std::lock_guard<std::mutex> l(frameMtx_);
        out = hist_;
        return true;
    }

    int getClipCount()
    {
        std::lock_guard<std::mutex> l(frameMtx_);
        return clipCount_;
    }

    Telemetry getTelemetry()
    {
        std::lock_guard<std::mutex> l(teleMtx_);
        return tele_;
    }

    // What the CONNECTED camera can do. Valid once cameraReady has fired (and
    // re-read after a reconnect, which may bring back a different body). The
    // GUI builds its resolution list, bit-depth entries, gain range and
    // frame-rate ceiling from this.
    CameraCaps caps() const
    {
        std::lock_guard<std::mutex> l(capsMtx_);
        return caps_;
    }
    bool isColorCamera() const { return isColor_; }
    // Escape hatch: use a Bayer pattern other than the one the SDK reports
    // (--bayer rggb|bggr|grbg|gbrg; -1 = use the camera's own). See colour.h.
    void setBayerOverride(int pattern) { bayerOverride_ = pattern; }

    int frameWidth()  const { return w_; }
    int frameHeight() const { return h_; }

signals:
    void photoResult(const QString& path, bool ok, const QString& err);
    void recordingStarted(const QString& path);
    void recordingStopped(const QString& path, bool ok);
    void cameraError(const QString& msg);
    void cameraReconnected();   // the camera came back after being lost
    // Fired after every successful open (first start AND every reconnect): the
    // camera may be a different model, so the GUI re-reads caps()/roiListReady
    // and rebuilds its selectors when this arrives.
    void cameraReady(const QString& description);
    void roiListReady(const QStringList& rois); // entries are "WxH"
    void gainReady(int min, int max, int current); // gain caps (0.1 dB units) + current value
    void sequenceStarted();
    void sequenceExposing(int index, int total);  // a shot's exposure just started
    void sequenceShot(int index, int total, const QString& path);
    void sequenceWait(double remainingS);
    void sequenceDone(int total, bool earlyStop);

protected:
    void run() override;

private:
    bool openCamera();                        // find + open + probe (also re-opened after a loss)
    bool handleCameraLost(const char* why);   // drop the dead handle, wait, re-open
    bool nap(int ms);                         // sleep in 50 ms slices; true if quitting meanwhile
    void doPhoto(double expS);
    void doSequence();
    void startRecording(double expS, int fps);
    void stopRecording();
    void updateTelemetry();
    // 128-bin histogram + clipping of one frame. `raw` may be a DOWNSCALED view
    // (the preview builder's 2x2 box buffer) — `rawPixels` is the true frame's
    // pixel count so the clipping estimate is reported on the frame, not on the
    // view it was measured from.
    void computeHistogram(const unsigned char* raw, int w, int h, int bpp,
                          long rawPixels = 0, bool colour = false);
    bool applyRoi(int w, int h, int dbpp, bool restartVideo = true); // false on failure/camera lost
    void probeRois();
    // Apply the white-balance config (colour bodies only; no-op when the
    // body has no WB controls). autoWb sets the SDK auto flags on the WB
    // controls (the camera's automatic white balance); otherwise the clamped
    // manual gains are written. Updates the applied-state trackers on
    // success. Returns the SDK error code of the last write attempted
    // (ASI_SUCCESS when nothing had to be done) — the callers check it for
    // ASI_ERROR_CAMERA_REMOVED like every other control write here.
    int applyWhiteBalanceNow(bool autoWb, int r, int b);
    // Save one captured frame: 1 byte/px -> PNG, 2 bytes/px -> 16-bit TIFF;
    // a colour camera's frame is demosaiced to RGB first (mono stays single
    // channel). Returns the written path, or "" on failure (err filled in).
    QString saveStill(const QString& path, const unsigned char* raw, int w, int h, int bpp,
                      QString& err);
    // Publish a still frame into the live preview buffer (the capture thread
    // owns the camera during a photo/sequence, so the builder is idle).
    void publishStillPreview(const unsigned char* raw, int w, int h, int bpp);
    // Hand one grabbed frame to the active writer (H.264 or .ser), converting
    // a colour camera's Bayer readout to RGB as needed. False = dropped/failed.
    bool pushToWriter(const unsigned char* raw, int w, int h, int bpp);
    // Preview builder thread body (see the class comment below the raw
    // buffers): builds the display thumbnail + histogram at 30 Hz, off the
    // drain loop.
    void previewBuilderLoop();

    struct Config
    {
        int mode = Mode::Photo;
        double exposureS = 0.010;
        int fps = 30;
        int roiW = 0, roiH = 0;   // user-selected ROI (0 = full)
        int bitDepth = 8;         // 8, or the camera's deep readout depth (14 on the
                                  // ASI178; from caps). 10 only via --vtest.
        bool serMode = false;     // true for every .ser video (8-bit and deep). The deep
                                  // depth is always .ser; for 8-bit it selects .ser over
                                  // H.264. 8-bit H.264 and stills leave it false. Every
                                  // 8-bit output (H.264 and .ser) captures the 1-byte
                                  // readout (RAW8) on the 10-bit HSM mode; the deep
                                  // depth captures the 2-byte readout (RAW16).
        int gain = 0;             // SDK gain value (0.1 dB units; 0..400 = 0.0..40.0 dB)
        // White balance (colour bodies; the worker ignores it for mono ones).
        // autoWb = the camera's automatic white balance — the GUI default.
        // wbR/wbB are the manual gains used while autoWb is false.
        bool autoWb = true;
        int  wbR = 0, wbB = 0;
        bool recording = false;
        bool photoRequested = false;
        bool quitting = false;
        // interval sequence
        bool sequenceRequested = false;
        double sequenceExpS = 10.0;    // per-shot exposure (s)
        double sequenceIntervalS = 5.0; // time between shots (s; clamped to >= 1 in doSequence; always elapses after each shot, independent of the exposure)
        int sequenceCount = 10;        // 0 = continuous
    };

    Config cfg_;
    std::mutex cfgMtx_;

    std::mutex frameMtx_;
    std::vector<unsigned char> latest_;         // display thumbnail (grayscale or RGB, see latestCh_)
    int latestW_ = 0, latestH_ = 0;             // thumbnail size
    int latestCh_ = 1;                          // 1 = grayscale, 3 = RGB (red-marked clips)
    std::array<int, kHistBins> hist_{};   // 128-bin histogram of current frame (full resolution)
    int clipCount_ = 0;                   // pixels at the max value (clipped)

    std::mutex teleMtx_;
    Telemetry tele_;

    int cam_ = -1;
    int fullW_ = 0, fullH_ = 0;   // sensor max size (full ROI)
    int w_ = 0, h_ = 0;           // current ROI size (worker thread writes)
    int bpp_ = 1;
    std::vector<std::pair<int,int>> validRois_; // probed valid ROIs (w,h)

    // ---- raw frame buffers (worker thread + preview builder) -------------
    // Sized ONCE in openCamera() to the sensor maximum (fullW_ x fullH_ x 2
    // bytes, the wide-readout worst case) and never resized again, so the
    // .data() pointers are stable for the life of the camera.
    //
    // The builder claims readingIdx_ under frameMtx_ before touching a raw
    // slot and releases it after the thumbnail AND histogram are complete.
    // The worker selects a write slot different from both readingIdx_ and
    // publishedIdx_. Three slots guarantee one is available even when a
    // preview build lasts longer than a camera frame period. The raw read is
    // lock-free while claimed; a reconnect waits for that claim before resize.
    std::array<std::vector<unsigned char>, 3> raw_;
    size_t maxRawBytes_ = 0;
    int activeIdx_ = 0;         // worker's selected SDK write target
    int publishedIdx_ = -1;     // latest completed frame, -1 before first grab
    int publishedW_ = 0, publishedH_ = 0, publishedBpp_ = 1;
    int readingIdx_ = -1;       // builder's claimed raw frame (under frameMtx_)
    std::condition_variable frameCv_;
    std::atomic<uint64_t> frameSeq_{0}; // completed video frames published so far

    int selectWritableRawSlot(); // worker thread: never selects published/reading

    // ---- the connected camera (probed at open) ----------------------------
    // caps_ is guarded by capsMtx_ for the GUI; the per-frame hot paths read
    // the cached atomics below instead of copying the struct.
    CameraCaps caps_;
    mutable std::mutex capsMtx_;
    std::atomic<bool> isColor_{false};        // colour body: everything saves RGB
    std::atomic<int>  bayer_{0};              // Bayer pattern in force (ASI_BAYER_*)
    std::atomic<int>  bayerOverride_{-1};     // --bayer rggb|bggr|grbg|gbrg
    std::atomic<bool> hasRaw8_{true};         // 1-byte readout exists (else Y8-only)
    std::atomic<bool> hasRaw16_{true};        // 2-byte readout exists (the deep mode)
    std::atomic<bool> hasHsm_{true};          // ASI_HIGH_SPEED_MODE control present

    // ---- white balance state (colour bodies only) --------------------------
    std::atomic<bool> hasWb_{false};          // colour body with WB_R + WB_B caps
    std::atomic<bool> wbAutoR_{false}, wbAutoB_{false}; // SDK auto flag supported
    int wbRMin_ = 0, wbRMax_ = 0, wbBMin_ = 0, wbBMax_ = 0; // clamp range in force
    int lastWbAuto_ = -1;    // applied state: -1 unknown/reset, 0 manual, 1 auto
    int lastWbR_ = -1, lastWbB_ = -1;   // applied manual gains (-1 = force)
    std::chrono::steady_clock::time_point wbRetryAfter_{}; // avoid retrying a failed SDK write every frame

    // Colour conversion scratch. One per thread: the preview builder thread and
    // the capture thread both convert frames and must not share buffers.
    ColourScratch dispColour_;                // preview builder thread only
    std::vector<unsigned char> dispRgb_;      // its RGB thumbnail (dispW*dispH*3)
    std::vector<unsigned char> dispMask_;     // its per-thumbnail-pixel clip mask
    ColourScratch capColour_;                 // capture thread (saves + recording)
    std::vector<unsigned char> bgr8Buf_;      // capture thread: 8-bit BGR (H.264)
    std::vector<unsigned char> serRgbBuf_;    // capture thread: interleaved RGB (.ser)

    long lastExpApplied_ = -1;
    int lastGainApplied_ = -1;
    int gainMin_ = 0, gainMax_ = 400;  // SDK gain caps (0.1 dB units), queried at start
    bool videoActive_ = false;    bool slowPreview_ = false;     // photo mode, exposure > frame period: full exposure on the video stream (camera delivers at 1/exposure)
    bool everOpened_ = false;      // true after the first successful openCamera()

    // recording
    GstVideoEncoder encoder_;
    SerWriter serWriter_;        // for .ser (14-bit: RAW16 readout; 8-bit: RAW8 HSM readout)
    bool videoIsSer_ = false;    // true while the active recording is a .ser
    bool recordingActive_ = false;
    std::string videoPath_;
    double writerFps_ = 0.0;
    uint64_t framesWritten_ = 0;
    uint64_t encoderDropped_ = 0;
    std::chrono::steady_clock::time_point recT0_{};

    // pacing / fps measurement (gates the RECORDING writer only — the preview
    // runs on the builder thread at a fixed 30 Hz, independent of cfg.fps)
    std::chrono::steady_clock::time_point lastFrameTime_{};
    std::chrono::steady_clock::time_point nextAllowed_{};
    double emaFps_ = 0.0;    // accepted rate: frames WRITTEN while recording,
                             // the live stream's delivery rate otherwise
    int droppedPaced_ = 0;   // frames the camera delivered that the file did not get
    std::chrono::steady_clock::time_point lastTele_{};
    std::atomic<uint64_t> frameCount_{0};   // frames grabbed from the camera (live rate)
    std::atomic<bool> seqStop_{false};      // request to stop a running sequence

    // ---- preview builder thread ------------------------------------------
    // Owns the 30 Hz preview clock: builds the display thumbnail + histogram
    // at most every 33 ms from the newest published raw frame, so the preview
    // updates at 30 fps in every mode (photo / interval / video), independent
    // of the record fps — and never faster than that (the GUI repaints at
    // 33 ms anyway; there is no point building more). Running off the drain
    // loop keeps the ~12 ms 6.4 MP build from stalling the frame drain
    // (#38: a 30 ms in-loop gate overflowed the camera's USB buffer at the
    // 56 fps 10-bit HSM full-res readout).
    std::thread previewThread_;
    std::atomic<bool> previewQuit_{true};  // false while the builder runs
    std::atomic<uint64_t> previewBuilds_{0};
};
