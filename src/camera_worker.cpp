// camera_worker.cpp
//
// CameraWorker implementation (see camera_worker.h). Owns the ASI SDK and the
// video encoder; all SDK calls happen on this thread.

#include "camera_worker.h"

#include <ASICamera2.h>

#include <opencv2/opencv.hpp>

#include <display_frame.h>
#include <exposure.h>
#include <util.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <thread>

// Sleep in 50 ms slices so a quit request is noticed quickly. Returns true if
// quitting was requested while sleeping.
bool CameraWorker::nap(int ms)
{
    for (int i = 0; i < ms / 50; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::lock_guard<std::mutex> l(cfgMtx_);
        if (cfg_.quitting) return true;
    }
    return false;
}

int CameraWorker::selectWritableRawSlot()
{
    std::lock_guard<std::mutex> l(frameMtx_);
    for (int i = 0; i < (int)raw_.size(); ++i)
    {
        const int slot = (activeIdx_ + i) % (int)raw_.size();
        if (slot != publishedIdx_ && slot != readingIdx_)
        {
            activeIdx_ = slot;
            return slot;
        }
    }
    // A builder claims at most one slot, and one other slot is published.
    // With three buffers this cannot occur.
    std::abort();
}

// Find, open and initialize the camera, probe its ROIs and query the gain
// control. Used at startup AND after the camera was lost (see
// handleCameraLost): on this machine the USB can re-enumerate the camera at
// any time, which kills the SDK handle — calling the SDK with the dead handle
// risks segfaulting the process, so the worker re-opens a fresh handle
// instead of running on.
bool CameraWorker::openCamera()
{
    if (ASIGetNumOfConnectedCameras() <= 0) return false;
    ASI_CAMERA_INFO info = {};
    if (ASIGetCameraProperty(&info, 0) != ASI_SUCCESS) return false;
    int id = info.CameraID;
    if (ASIOpenCamera(id) != ASI_SUCCESS) return false;
    if (ASIInitCamera(id) != ASI_SUCCESS) { ASICloseCamera(id); return false; }
    cam_ = id;

    // Everything the app adapts to — sensor size, mono vs colour, which readouts
    // exist, the gain/exposure ranges — comes from this probe. A reconnect can
    // bring back a DIFFERENT body, so it is re-run on every open.
    CameraCaps caps;
    if (!probeCameraCaps(id, caps))
    {
        ASICloseCamera(cam_);
        cam_ = -1;
        return false;
    }
    if (!caps.hasRaw8 && !caps.hasY8 && !caps.hasRaw16)
    {
        std::fprintf(stderr, "[cam] %s offers no readable pixel format (SupportedVideoFormat empty)\n",
                     caps.name.toLatin1().constData());
        emit cameraError(QString("%1 streams no pixel format this app can read "
                                 "(needs RAW8 or RAW16).").arg(caps.name));
        ASICloseCamera(cam_);
        cam_ = -1;
        return false;
    }
    {
        std::lock_guard<std::mutex> l(capsMtx_);
        caps_ = caps;
    }
    fullW_ = caps.maxW;
    fullH_ = caps.maxH;
    isColor_   = caps.isColor;
    hasRaw8_   = caps.hasRaw8;
    hasRaw16_  = caps.hasRaw16;
    hasHsm_    = caps.hasHighSpeedMode;
    const int bayerOv = bayerOverride_;
    bayer_ = (bayerOv >= 0) ? bayerOv : caps.bayer;
    std::fprintf(stderr, "[cam] %s%s%s\n", caps.name.toLatin1().constData(),
                 caps.isColor ? "" : " (mono output)", "");
    if (caps.isColor)
        std::fprintf(stderr, "[cam] colour body: Bayer %s (SDK) -> saving RGB%s\n",
                     bayerPatternName((int)caps.bayer),
                     (bayerOv >= 0 && bayerOv != (int)caps.bayer) ? " USING --bayer OVERRIDE" : "");
    gainMin_ = caps.gainMin;
    gainMax_ = caps.gainMax;

    // White balance: colour bodies only. A mono body's WB controls are never
    // touched, so mono captures stay the neutral 1:1:1 data they always were
    // (§9). For a colour body, record the caps + auto support, seed the
    // manual-gain config from the camera's CURRENT values on the first open
    // (so toggling AWB off at startup continues from where the camera is,
    // not from an arbitrary constant), and invalidate the applied-state
    // trackers — a fresh handle starts at the firmware's settings.
    hasWb_    = caps.isColor && caps.wbControls();
    wbAutoR_  = caps.wbRAuto;
    wbAutoB_  = caps.wbBAuto;
    wbRMin_ = caps.wbRMin; wbRMax_ = caps.wbRMax;
    wbBMin_ = caps.wbBMin; wbBMax_ = caps.wbBMax;
    lastWbAuto_ = -1; lastWbR_ = -1; lastWbB_ = -1;
    wbRetryAfter_ = {};
    {
        std::lock_guard<std::mutex> l(teleMtx_);
        tele_.wbValid = false;      // nothing read back on this handle yet
    }
    if (hasWb_)
    {
        long r0 = caps.wbRDef, b0 = caps.wbBDef; ASI_BOOL ar = ASI_FALSE, ab = ASI_FALSE;
        const bool rRead = ASIGetControlValue(cam_, ASI_WB_R, &r0, &ar) == ASI_SUCCESS;
        const bool bRead = ASIGetControlValue(cam_, ASI_WB_B, &b0, &ab) == ASI_SUCCESS;
        if (!rRead) r0 = caps.wbRDef;
        if (!bRead) b0 = caps.wbBDef;
        r0 = std::clamp<long>(r0, wbRMin_, wbRMax_);
        b0 = std::clamp<long>(b0, wbBMin_, wbBMax_);
        if (rRead && bRead)
        {
            std::lock_guard<std::mutex> l(teleMtx_);
            tele_.wbR = (int)r0;
            tele_.wbB = (int)b0;
            tele_.wbAuto = ar == ASI_TRUE || ab == ASI_TRUE;
            tele_.wbValid = true;
        }
        if (!everOpened_)
        {
            std::lock_guard<std::mutex> l(cfgMtx_);
            cfg_.wbR = (int)r0;
            cfg_.wbB = (int)b0;
        }
        std::fprintf(stderr, "[wb] R %d..%d def %d (now %d, auto %s) · B %d..%d def %d (now %d, auto %s)%s\n",
                     wbRMin_, wbRMax_, caps.wbRDef, (int)r0, wbAutoR_ ? "yes" : "no",
                     wbBMin_, wbBMax_, caps.wbBDef, (int)b0, wbAutoB_ ? "yes" : "no",
                     everOpened_ ? "  (reconnected; user setting reapplied by the loop)" : "");
    }

    // Size the raw buffers to the sensor maximum (the 2-byte readout worst
    // case). A reconnect with a different body may resize them; wait until
    // the builder releases its current raw frame first.
    {
        const size_t need = (size_t)fullW_ * (size_t)fullH_ * 2;
        std::unique_lock<std::mutex> l(frameMtx_);
        frameCv_.wait(l, [this] { return readingIdx_ < 0; });
        publishedIdx_ = -1; // old handle's frame must not be built after a reconnect
        if (need != maxRawBytes_)
        {
            maxRawBytes_ = need;
            for (auto& slot : raw_) slot.resize(need);
        }
    }

    // The camera persists a BandWidth governor (ASI_BANDWIDTHOVERLOAD,
    // "fraction of the total bandwidth the camera may use", 40..100,
    // persisted 60 on this unit) that throttles video far below what the
    // USB3 link can carry: at 60, 480x320 RAW16 caps at ~105-111 fps
    // (32.5 MB/s), while at 100 the camera delivers ~95% of the 14-bit
    // datasheet column (176 fps @ 480x320, 228 @ 320x240; measured
    // 2025-09-15, /tmp/bw_probe*). Raise it to the max on every open so
    // recording is not silently limited to the persisted setting.
    {
        ASI_ERROR_CODE e = ASISetControlValue(cam_, ASI_BANDWIDTHOVERLOAD, 100, ASI_FALSE);
        long rb = -1; ASI_BOOL rbAuto = ASI_FALSE;
        if (ASIGetControlValue(cam_, ASI_BANDWIDTHOVERLOAD, &rb, &rbAuto) == ASI_SUCCESS)
            std::fprintf(stderr, "[bandwidth] set=100 err=%d readback=%ld\n", (int)e, rb);
        else
            std::fprintf(stderr, "[bandwidth] set=100 err=%d readback FAILED\n", (int)e);
    }

    // output directories for photos / videos (idempotent)
    std::error_code ec;
    std::filesystem::create_directories("photos", ec);
    std::filesystem::create_directories("videos", ec);
    std::filesystem::create_directories("sequences", ec);

    // Probe the ROIs the camera actually accepts, apply the default (full)
    // and publish the list to the GUI for the ROI selector (also (re)starts
    // the live capture).
    probeRois();
    if (w_ == 0)
    {
        ASICloseCamera(cam_);
        cam_ = -1;
        return false;
    }

    // Query the gain control so the GUI slider spans the camera's REAL range:
    // the ASI178MC answers ASIGetControlCaps (0..510 = 0.0..51.0 dB) while the
    // ASI178MM rejects it and keeps the documented 0..400 — probeCameraCaps()
    // resolved that, gainMin_/gainMax_ already hold the range in force.
    {
        long curGain = 0; ASI_BOOL autoGain = ASI_FALSE;
        if (ASIGetControlValue(cam_, ASI_GAIN, &curGain, &autoGain) != ASI_SUCCESS)
            curGain = gainMin_;
        curGain = std::clamp<long>(curGain, (long)gainMin_, (long)gainMax_);
        if (!everOpened_)
        {
            // Cold start: seed cfg_.gain with the camera's current value so
            // startup doesn't jump the gain, and sanity-check that the control
            // is writable and round-trips.
            ASISetControlValue(cam_, ASI_GAIN, curGain, ASI_FALSE);
            long rb = 0; ASI_BOOL rbAuto = ASI_FALSE;
            if (ASIGetControlValue(cam_, ASI_GAIN, &rb, &rbAuto) == ASI_SUCCESS)
                std::fprintf(stderr, "[gain] caps %d..%d (0.1 dB), current %d, readback %d %s\n",
                             gainMin_, gainMax_, (int)curGain, (int)rb,
                             (rb == curGain) ? "OK" : "MISMATCH");
            else
                std::fprintf(stderr, "[gain] caps %d..%d (0.1 dB), current %d, readback FAILED\n",
                             gainMin_, gainMax_, (int)curGain);
            {
                std::lock_guard<std::mutex> l(cfgMtx_);
                cfg_.gain = (int)curGain;
            }
            emit gainReady(gainMin_, gainMax_, (int)curGain);
        }
        else
        {
            int userGain = 0;
            { std::lock_guard<std::mutex> l(cfgMtx_); userGain = cfg_.gain; }
            std::fprintf(stderr, "[cam] reconnected (sensor gain %d; user setting %d is reapplied by the loop)\n",
                         (int)curGain, userGain);
            emit gainReady(gainMin_, gainMax_, (int)userGain);
        }
        everOpened_ = true;
    }

    // The fresh handle starts at default settings: make the main loop
    // re-apply the current exposure and gain to it (and, if the user's ROI
    // differs from the full 8-bit default, the ROI as well).
    lastExpApplied_ = -1;
    lastGainApplied_ = -1;
    slowPreview_ = false;    // probeRois left the live capture running
    emaFps_ = 0.0;
    lastFrameTime_ = std::chrono::steady_clock::time_point{};
    nextAllowed_ = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> l(teleMtx_);
        tele_.cameraOk = true;
    }
    return true;
}

// The camera's USB device is gone (e.g. re-enumeration): the SDK handle is
// dead and any further call into the SDK on it risks crashing the process.
// Drop the handle WITHOUT calling the SDK on it, finalize an active
// recording, then wait — retrying every second — until the camera can be
// opened again (or the app quits). Returns false only when quitting.
bool CameraWorker::handleCameraLost(const char* why)
{
    std::fprintf(stderr, "[cam] lost (%s) - waiting for the camera to come back\n", why);
    cam_ = -1;
    videoActive_ = false;   // never call the SDK on the dead handle
    if (recordingActive_)
        stopRecording();    // finalize the partially recorded file
    {
        std::lock_guard<std::mutex> l(teleMtx_);
        tele_.cameraOk = false;
        tele_.wbValid = false;    // the read-back values belong to the dead handle
    }
    emit cameraError("Camera lost - reconnecting...");

    for (;;)
    {
        if (nap(1000)) return false;
        if (openCamera())
        {
            std::fprintf(stderr, "[cam] reconnected\n");
            emit cameraReconnected();
            return true;
        }
    }
}

void CameraWorker::run()
{
    using Clock = std::chrono::steady_clock;
    const bool dbg = isDebugRecording();

    // Open the camera, retrying while it is not (yet) reachable: on this
    // machine the USB node can appear late or the camera can re-enumerate,
    // so waiting for it beats dead-ending with an error.
    bool reported = false;
    while (!openCamera())
    {
        if (!reported)
        {
            emit cameraError("No ASI camera found yet - waiting (check the USB connection; "
                             "if the /dev node is missing create it with mknod, "
                             "major:minor from /sys/bus/usb/devices/<usbX>/<port>/dev)");
            reported = true;
        }
        if (nap(1000)) return;
    }

    nextAllowed_ = Clock::now();
    lastTele_ = Clock::now();

    // OpenCV's default parallel resize starved the SDK drain on this host:
    // the camera stopped delivering for ~650 ms every few seconds while the
    // preview builder ran, although each resize itself took only ~10 ms.
    // One OpenCV worker keeps the full-resolution colour preview and the
    // camera stream steady without changing the app's own thread separation.
    cv::setNumThreads(1);

    // Start the preview builder: it owns the 30 Hz preview clock (thumbnail +
    // histogram) OFF the drain loop, so the preview updates at 30 fps in
    // every mode regardless of the record fps. The builder stays off the
    // drain loop, and its OpenCV work is bounded above to one worker thread.
    previewQuit_ = false;
    previewThread_ = std::thread(&CameraWorker::previewBuilderLoop, this);

    while (true)
    {
        Config cfg;
        {
            std::lock_guard<std::mutex> l(cfgMtx_);
            cfg = cfg_;
        }
        if (cfg.quitting) break;

        // ---- single photo (snap mode) ------------------------------------
        if (cfg.photoRequested)
        {
            {
                std::lock_guard<std::mutex> l(cfgMtx_);
                cfg_.photoRequested = false;
            }
            doPhoto(cfg.exposureS);
            continue;
        }

        // ---- recording start/stop (encoder lives on this thread) ----------
        // Handled before the sequence so a pending recording-stop is never
        // deferred behind a sequence that pauses the live loop.
        if (cfg.recording && !recordingActive_) startRecording(cfg.exposureS, cfg.fps);
        if (!cfg.recording && recordingActive_) stopRecording();

        // ---- interval sequence (blocks the live loop while it runs) -------
        if (cfg.sequenceRequested)
        {
            {
                std::lock_guard<std::mutex> l(cfgMtx_);
                cfg_.sequenceRequested = false;
            }
            seqStop_ = false;
            doSequence();
            continue;
        }

        // ---- preview mode decision -------------------------------------------
        // A manual exposure longer than one frame period stretches the frame
        // period, so the camera delivers stream frames at 1/exposure (the
        // sensor simply can't expose faster than the exposure). In PHOTO mode
        // the preview therefore runs at 1/exposure (physics: the preview
        // builder shows every frame the camera delivers, at most 30 fps — and
        // 1/exposure is below 30 fps in this regime): keep the video stream
        // running and set the FULL exposure on it — the camera naturally slows
        // to 1/exposure on the fast video path. (The old single-shot path — one
        // standalone ASIStartExposure per frame — added a fixed ~250 ms
        // camera-side cost per frame and made the preview update at
        // 1/(exposure+250 ms); it is no longer used for the preview. doPhoto()/
        // doSequence() still use standalone exposures for their saved shots.)
        // Video mode's slider never exceeds 1/fps, and interval mode keeps a
        // fast preview (its sequence shots use the standalone path too).
        double framePeriodS = 1.0 / cfg.fps;
        bool wantSlow = (cfg.mode == Mode::Photo) && (cfg.exposureS > framePeriodS);
        if (wantSlow != slowPreview_)
        {
            if (wantSlow)
            {
                // Video stream STAYS running for the slow preview: a long
                // exposure simply stretches the frame period, so the camera
                // delivers frames at 1/exposure on the (fast) video path.
                // (No stop/restart needed.)
            }
            else
            {
                // Clean restart: doPhoto()/doSequence() may have left the stream
                // running (possibly at a long exposure), so always re-init it.
                if (videoActive_)
                {
                    ASI_ERROR_CODE e = ASIStopVideoCapture(cam_);
                    videoActive_ = false;
                    if (e == ASI_ERROR_CAMERA_REMOVED) { handleCameraLost("stop video"); continue; }
                }
                ASI_ERROR_CODE e = ASIStartVideoCapture(cam_);
                if (e == ASI_ERROR_CAMERA_REMOVED) { handleCameraLost("start video"); continue; }
                if (e == ASI_SUCCESS) videoActive_ = true;
            }
            slowPreview_ = wantSlow;
            nextAllowed_ = Clock::now();
            lastFrameTime_ = Clock::time_point{};
        }

        // ---- apply the user-selected ROI + bit depth (never mid-recording) -
        if (!recordingActive_)
        {
            int dw = (cfg.roiW > 0) ? cfg.roiW : fullW_;
            int dh = (cfg.roiH > 0) ? cfg.roiH : fullH_;
            // A body without the 2-byte readout cannot do the deep mode, and a
            // Y8-only body cannot do the 1-byte RAW8 one: the ROI probe already
            // restricted the sizes, this restricts the depth (the GUI only
            // offers depths caps_.supportsDepth() accepts).
            int dbpp = (cfg.bitDepth == 8 || !hasRaw16_) ? 1 : 2;
            if (dw != w_ || dh != h_ || dbpp != bpp_)
            {
                if (dbg) std::fprintf(stderr, "[roi] switching to %dx%d %d-bit %s (mode=%d fps=%d)\n", dw, dh, cfg.bitDepth, cfg.serMode ? "ser" : "raw8", cfg.mode, cfg.fps);
                // The live preview (fast OR slow) runs on the video stream, so a
                // format change must leave it running — restart it here.
                if (!applyRoi(dw, dh, dbpp, /*restartVideo=*/true))
                {
                    if (cam_ < 0) { handleCameraLost("roi switch"); continue; }
                    // non-removed failure (already reported): back off so a
                    // failing camera is not hammered every loop iteration
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    continue;
                }
            }
        }

        // ---- exposure ------------------------------------------------------
        // Fast preview: the stream only ever sees a frame-period-safe exposure.
        // (Single-shot preview applies the FULL user exposure itself, below.)
        if (!wantSlow)
        {
            long us = (long)std::llround(std::min(cfg.exposureS, framePeriodS) * 1e6);
            if (us != lastExpApplied_)
            {
                ASI_ERROR_CODE e = ASISetControlValue(cam_, ASI_EXPOSURE, us, ASI_FALSE);
                if (e == ASI_ERROR_CAMERA_REMOVED) { handleCameraLost("set exposure"); continue; }
                lastExpApplied_ = us;
            }
        }

        // ---- gain (soft control; applies to subsequent frames) --------------
        if (cfg.gain != lastGainApplied_)
        {
            ASI_ERROR_CODE e = ASISetControlValue(cam_, ASI_GAIN, cfg.gain, ASI_FALSE);
            if (e == ASI_ERROR_CAMERA_REMOVED) { handleCameraLost("set gain"); continue; }
            if (e == ASI_SUCCESS)
            {
                lastGainApplied_ = cfg.gain;
                std::lock_guard<std::mutex> tl(teleMtx_);
                tele_.gainApplied = cfg.gain;
            }
            else
                std::fprintf(stderr, "[gain] failed to set gain %d (0.1 dB)\n", cfg.gain);
        }

        // ---- white balance (colour bodies only; soft controls like gain) ----
        // Applied on any change of the auto/manual mode or (in manual) of the
        // gain values. While auto is on the camera balances continuously and
        // nothing is re-sent; updateTelemetry reads the live gains back for
        // the GUI's follow.
        if (hasWb_)
        {
            const int wbR = std::clamp(cfg.wbR, wbRMin_, wbRMax_);
            const int wbB = std::clamp(cfg.wbB, wbBMin_, wbBMax_);
            const bool modeChanged = lastWbAuto_ != (cfg.autoWb ? 1 : 0);
            const bool valsChanged = !cfg.autoWb && (wbR != lastWbR_ || wbB != lastWbB_);
            if ((modeChanged || valsChanged) && Clock::now() >= wbRetryAfter_)
            {
                const int we = applyWhiteBalanceNow(cfg.autoWb, wbR, wbB);
                if (we == (int)ASI_ERROR_CAMERA_REMOVED) { handleCameraLost("set WB"); continue; }
                // A rejected control must not be re-sent on every frame: that
                // blocks the camera's video drain loop and can fill its USB
                // buffer. Retry after a pause in case the failure is transient.
                wbRetryAfter_ = we == (int)ASI_SUCCESS ? Clock::time_point{}
                    : Clock::now() + std::chrono::seconds(1);
            }
        }

        // ---- grab one frame ------------------------------------------------
        // The loop always pulls from the continuous video stream as fast as
        // the camera delivers — INDEPENDENT of cfg.fps. The record fps only
        // paces the writer (below); the preview runs on the builder thread at
        // a fixed 30 Hz from whatever the loop drains here. A fixed SHORT wait
        // (not framePeriodMs/4, which was tied to the record fps — a 1 fps
        // recording would have polled at 250 ms and stalled the live view):
        // the loop spins fast and drains the camera's USB frame buffer as soon
        // as frames arrive. A long wait makes each timeout block the full
        // duration, slowing the loop so the camera's buffer fills up and it
        // drops frames (bursty delivery, long gaps).
        auto tPerf0 = dbg ? Clock::now() : Clock::time_point{};
        static std::chrono::steady_clock::time_point sPrevEnd{};
        double gapMs = 0.0;
        if (dbg && sPrevEnd != std::chrono::steady_clock::time_point{})
            gapMs = std::chrono::duration<double, std::milli>(tPerf0 - sPrevEnd).count();
        // per-phase dbg timings (filled inside the writer push)
        double tPushMs = 0.0;
        // Protect the frame the builder is still reading, even if that build
        // runs longer than one camera frame period.
        const int grabSlot = selectWritableRawSlot();
        const long frameBytes = (long)w_ * h_ * bpp_;
        // Slow preview: same video stream, but the FULL exposure is set, so the
        // camera stretches the frame period and delivers once per exposure
        // (physics: a 60 s exposure takes 60 s). Poll with a short wait for a
        // frame to arrive.
        auto now = Clock::now();
        if (slowPreview_)
        {
            // Slow preview: keep the video stream running and set the FULL
            // selected exposure on it. A long exposure simply stretches the
            // frame period, so the camera delivers frames at 1/exposure on the
            // fast video path — no standalone-exposure overhead (which added a
            // fixed ~250 ms camera-side cost per frame). Poll with a short wait
            // so the loop stays responsive (the grab block below).
            if (!videoActive_)
            {
                ASI_ERROR_CODE e = ASIStartVideoCapture(cam_);
                if (e == ASI_ERROR_CAMERA_REMOVED) { handleCameraLost("slow preview"); continue; }
                if (e == ASI_SUCCESS) videoActive_ = true;
            }
            long us = (long)std::llround(cfg.exposureS * 1e6);
            if (us != lastExpApplied_)
            {
                ASI_ERROR_CODE e = ASISetControlValue(cam_, ASI_EXPOSURE, us, ASI_FALSE);
                if (e == ASI_ERROR_CAMERA_REMOVED) { handleCameraLost("slow preview"); continue; }
                lastExpApplied_ = us;
            }
            // Block a SHORT time per grab so the loop stays responsive (it can
            // still see a quit/photo/sequence request between grabs) and the
            // polling granularity (<< exposure) doesn't limit the delivered
            // rate. Cap at 50 ms so a long exposure (up to 60 s) doesn't block
            // the loop — the frame simply isn't ready yet and we retry.
            int expMs = (int)std::llround(cfg.exposureS * 1000);
            int waitMs = std::min(50, std::max(10, expMs / 4));
            ASI_ERROR_CODE err = ASIGetVideoData(cam_, raw_[grabSlot].data(), frameBytes, waitMs);
            now = Clock::now();
            if (err == ASI_ERROR_CAMERA_REMOVED) { handleCameraLost("slow preview"); continue; }
            if (err != ASI_SUCCESS) continue;   // timeout - no frame this round
        }
        else
        {
            ASI_ERROR_CODE err = ASIGetVideoData(cam_, raw_[grabSlot].data(), frameBytes, 10);
            now = Clock::now();

            if (err == ASI_ERROR_CAMERA_REMOVED)
            {
                handleCameraLost("video grab");
                continue;
            }
            if (err != ASI_SUCCESS)
                continue; // timeout - no frame this round
        }
        auto tPerf1 = dbg ? Clock::now() : Clock::time_point{};

        // ---- publish the frame to the preview builder ----------------------
        // Geometry (w_, h_, bpp_) was set by applyRoi before the grab.
        // publishedIdx_ is published under frameMtx_ (the builder reads it
        // there) and frameSeq_ LAST: a builder that observes the new
        // sequence also observes the new index and the fully written frame
        // (release/acquire on frameSeq_ orders the SDK's buffer write).
        {
            std::lock_guard<std::mutex> l(frameMtx_);
            publishedIdx_ = grabSlot;
            publishedW_ = w_; publishedH_ = h_; publishedBpp_ = bpp_;
            frameSeq_.fetch_add(1, std::memory_order_release);
        }

        // ---- record pacing: gate ONLY the writer ---------------------------
        // The preview (builder thread) already saw this frame at 30 Hz no
        // matter the record fps; the RECORDED file paces to cfg.fps. Surplus
        // frames the camera delivered are dropped here (droppedPaced_) — the
        // writer still gets exactly the selected record rate.
        bool paceOk = true;
        if (recordingActive_)
        {
            if (now < nextAllowed_)
            {
                droppedPaced_++;
                paceOk = false;
                if (dbg) std::fprintf(stderr, "[rec] t=%.3f PACE-DROP (arrived early)\n", std::chrono::duration<double>(now - recT0_).count());
            }
            else
            {
                if (std::chrono::duration<double>(now - nextAllowed_).count() > 0.5)
                    nextAllowed_ = now; // resync after long stalls (e.g. photo exposure)
                nextAllowed_ += std::chrono::duration_cast<Clock::duration>(
                                    std::chrono::duration<double>(1.0 / cfg.fps));
            }
        }

        // fps measurement: EMA of the ACCEPTED rate — while recording, the
        // pace-accepted (written) rate, i.e. what the file plays back at;
        // otherwise the live stream's delivery rate. (A pre-pacing "arrival
        // rate" EMA spikes to the loop's own speed whenever the loop drains
        // the camera's USB buffer in bursts — #38.) In the slow regime the
        // accepted rate is a stable 1/exposure (any transition burst is
        // already paced down to cfg.fps), so a faster blend lets the displayed
        // fps settle quickly after an exposure change; in the fast regime keep
        // the heavy smoothing so bursty buffer-drain reads don't show up as
        // spikes.
        if (!recordingActive_ || paceOk)
        {
            if (lastFrameTime_ != Clock::time_point{})
            {
                double dt = std::chrono::duration<double>(now - lastFrameTime_).count();
                if (dt > 0.0005)
                {
                    double inst = 1.0 / dt;
                    double blend = slowPreview_ ? 0.5 : 0.9;
                    emaFps_ = emaFps_ > 0 ? emaFps_ * blend + inst * (1.0 - blend) : inst;
                }
            }
            lastFrameTime_ = now;
        }

        // (The display thumbnail + histogram are built off this loop by the
        // preview builder thread at a fixed 30 Hz — see previewBuilderLoop.
        // This keeps the ~12 ms 6.4 MP build from stalling the frame drain:
        // in-loop, a 30 ms gate left a build frame (~28 ms incl. scheduling)
        // longer than the 17.9 ms frame period of the 56 fps 10-bit HSM
        // full-res readout, so the camera's USB buffer overflowed and frames
        // dropped (#38).)
        frameCount_++;
        if (recordingActive_ && paceOk)
        {
            // Push the just-grabbed frame (raw_[grabSlot] — the active slot;
            // the builder is on the published one). pushToWriter() handles the
            // mono/colour × .ser/H.264 matrix (a colour body's Bayer readout is
            // demosaiced to RGB here, so the FILE is RGB; a mono body's stays
            // single-channel).
            auto tP0 = dbg ? Clock::now() : Clock::time_point{};
            if (pushToWriter(raw_[grabSlot].data(), w_, h_, bpp_))
                framesWritten_++;
            else
                encoderDropped_++;
            if (dbg) tPushMs = std::chrono::duration<double, std::milli>(Clock::now() - tP0).count();
            if (dbg) std::fprintf(stderr, "[rec] t=%.3f push=%s written=%llu dropped=%llu\n",
                std::chrono::duration<double>(now - recT0_).count(),
                encoderDropped_ ? "DROP" : "ok",
                (unsigned long long)framesWritten_, (unsigned long long)encoderDropped_);
        }
        if (dbg && (frameCount_.load() % 50 == 0))
        {
            unsigned long n = frameCount_.load();
            auto ms = [](auto a, auto b) {
                return (int)std::llround(std::chrono::duration<double, std::milli>(b - a).count());
            };
            std::fprintf(stderr, "[perf] n=%lu roi=%dx%d bpp=%d disp=%dx%d gap=%.1fms grab=%dms push=%.1fms period=%dms emaFps=%.1f\n",
                         n, w_, h_, bpp_, latestW_, latestH_,
                         gapMs, ms(tPerf0, tPerf1),
                         tPushMs,
                         ms(tPerf0, Clock::now()), emaFps_);
            std::fflush(stderr);
        }
        if (dbg) sPrevEnd = Clock::now();
        if (std::chrono::duration<double>(now - lastTele_).count() > 0.5)
        {
            lastTele_ = now;
            updateTelemetry();
        }
    }

    // ---- cleanup -----------------------------------------------------------
    previewQuit_ = true;   // the builder drains its last frame and exits;
                           // the destructor joins it
    if (recordingActive_)
    {
        encoder_.stop();
        recordingActive_ = false;
        emit recordingStopped(QString::fromStdString(videoPath_), true);
    }
    if (videoActive_ && cam_ >= 0) ASIStopVideoCapture(cam_);
    if (cam_ >= 0)                 ASICloseCamera(cam_);
}

// Preview builder thread: the app's 30 Hz preview clock. See the member
// comment on previewThread_ in camera_worker.h for the design rationale
// (fixed 30 fps preview in every mode, independent of the record fps; the
// build runs off the drain loop so it can never overflow the camera's USB
// buffer — #38).
void CameraWorker::previewBuilderLoop()
{
    using Clock = std::chrono::steady_clock;
    const bool dbg = isDebugRecording();

    // Private scratch (reused, no per-frame allocation).
    std::vector<unsigned char> boxBuf8;   // 2x2 box of the published frame
    std::vector<unsigned char> boxClip;   // clip mask at box resolution
    std::vector<int>           mx, my;    // NN mask mapping (disp -> box)
    std::vector<unsigned char> disp8;     // thumbnail
    std::vector<unsigned char> clipM;     // clip flags
    std::vector<unsigned char> rgb;       // red-marked RGB
    auto lastBuild = Clock::time_point{};
    auto tPrev = Clock::time_point{};
    uint64_t seen = 0;
    uint64_t buildCount = 0;
    int lastW = -1, lastH = -1, lastBpp = -1;

    while (true)
    {
        if (previewQuit_) break;
        const uint64_t seq = frameSeq_.load(std::memory_order_acquire);
        if (seq == seen)
        {
            // No new frame yet (or none at all — sequence running / stream
            // stopped): a SHORT re-check nap. A long nap here would skip the
            // 30 Hz slot: the next frame arrives within one frame period
            // (<= ~36 ms at full-res 14-bit), so waiting >33 ms guarantees a
            // missed slot and a 66 ms inter-build gap (measured).
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        // Read geometry for the 30 Hz scheduling decision. The raw slot is
        // claimed only after this decision so waiting for the next slot never
        // prevents the worker from reusing an old frame.
        int idx = 0, w = 0, h = 0, bpp = 1;
        {
            std::lock_guard<std::mutex> l(frameMtx_);
            idx = publishedIdx_; w = publishedW_; h = publishedH_; bpp = publishedBpp_;
        }
        if (idx < 0) { seen = seq; continue; }
        const bool geometryChanged = (w != lastW || h != lastH || bpp != lastBpp);
        const auto now = Clock::now();
        // 30 fps cap: rebuild at most every 33 ms — the preview updates at
        // 30 fps, never faster (the GUI repaints at 33 ms anyway). A
        // ROI/format change forces an immediate build so the new preview
        // shows at once.
        if (!geometryChanged && lastBuild != Clock::time_point{} &&
            now < lastBuild + std::chrono::milliseconds(33))
        {
            // Sleep PRECISELY until the next 30 Hz slot: a 3 ms poll loop
            // here loses ~15 ms/cycle to wakeup quantization and drifts the
            // rate to ~20 Hz. lastBuild is the build START, so the slots
            // stay anchored even when a build (up to ~10 ms at 6.4 MP 14-bit)
            // runs long — the average rate is 30 fps, not 30 minus the build.
            std::this_thread::sleep_for(lastBuild + std::chrono::milliseconds(33) - now);
            continue;
        }
        // Claim the newest completed frame. The worker can continue grabbing
        // into either of the other two raw slots while this build runs.
        {
            std::lock_guard<std::mutex> l(frameMtx_);
            idx = publishedIdx_; w = publishedW_; h = publishedH_; bpp = publishedBpp_;
            seen = frameSeq_.load(std::memory_order_acquire);
            if (idx >= 0) readingIdx_ = idx;
        }
        if (idx < 0) continue;
        lastBuild = now;
        lastW = w; lastH = h; lastBpp = bpp;

        // ---- thumbnail (long edge <= kDispMax) -----------------------------
        // The claimed raw frame is read WITHOUT the lock. The worker excludes
        // readingIdx_ from SDK writes, and camera reconnect waits for this
        // claim to be released before resizing the buffers.
        int dispW, dispH;
        if (w <= kDispMax && h <= kDispMax) { dispW = w; dispH = h; }
        else if (w >= h) { dispW = kDispMax; dispH = (int)((long)h * kDispMax / w); }
        else             { dispH = kDispMax; dispW = (int)((long)w * kDispMax / h); }
        bool anyClip = false;
        // Fast path when the frame is >= 2x the thumbnail (the 6.4 MP corner):
        // a one-pass 2x2 box straight from raw_ + a small linear resize + an
        // NN clip-mask pass — ~2 ms at 6.4 MP, vs ~13 ms for the full-frame
        // copy + INTER_AREA. Measured: that extra cost on this thread stole
        // ~25 fps of write capacity from the drain loop at 6.4 MP 60 fps, so
        // the downscale budget lives here. A colour frame takes the same
        // shape of fast path: its 2x2 block IS one RGB pixel (quad demosaic).
        const bool boxable = ((w & 1) == 0 && (h & 1) == 0 &&
                              w >= 2 * dispW && h >= 2 * dispH);
        // The histogram ALWAYS comes from the RAW published frame: the box
        // buffer is 8-bit domain, so a deep frame's per-channel clipping
        // (any of the 4 samples at max) would be measured as "all 4 saturated"
        // — a ~4x under-report — and a colour frame's mosaic would mix three
        // channels into one grey histogram. Sampling 1/4 of the raw pixels
        // (computeHistogram strides 2x2) keeps that cheap and depth-exact.
        const unsigned char* histSrc = raw_[idx].data();
        const int histW = w, histH = h, histBpp = bpp;
        auto tB0 = dbg ? Clock::now() : Clock::time_point{};
        if (isColor_)
        {
            // Colour body: an RGB thumbnail (a colour camera's preview must not
            // be a grey mosaic average), with clipped blocks marked red.
            dispRgb_.resize((size_t)dispW * dispH * 3);
            dispMask_.resize((size_t)dispW * dispH);
            colourDisplayFrame(raw_[idx].data(), w, h, bpp, true, bayer_,
                               dispRgb_.data(), dispMask_.data(), dispW, dispH,
                               anyClip, dispColour_);
            if (anyClip)
                paintClipRed(dispRgb_.data(), dispMask_.data(), dispMask_.size());
            std::lock_guard<std::mutex> l(frameMtx_);
            latest_.swap(dispRgb_);
            latestCh_ = 3;
            latestW_ = dispW;
            latestH_ = dispH;
        }
        else
        {
            // Mono body: single-channel preview, upgraded to an RGB buffer only
            // when there are clipped blocks to mark red.
            disp8.assign((size_t)dispW * dispH, 0);
            clipM.assign((size_t)dispW * dispH, 0);
            if (boxable)
            {
                const int bw = w / 2, bh = h / 2;
                boxBuf8.resize((size_t)bw * bh);
                boxClip.resize((size_t)bw * bh);
                boxDownsample2x(raw_[idx].data(), w, h, bpp, boxBuf8.data(), boxClip.data());
                cv::Mat boxM(bh, bw, CV_8UC1, boxBuf8.data());
                cv::Mat dstM(dispH, dispW, CV_8UC1, disp8.data());
                if (bw == dispW && bh == dispH)
                    std::memcpy(disp8.data(), boxBuf8.data(), (size_t)bw * bh);
                else
                    cv::resize(boxM, dstM, cv::Size(dispW, dispH), 0, 0, cv::INTER_LINEAR);
                // NN the box-res clip mask to thumbnail resolution (one box
                // pixel covers one 2x2 raw block).
                mx.resize(dispW);
                my.resize(dispH);
                for (int x = 0; x < dispW; ++x) mx[x] = x * bw / dispW;
                for (int y = 0; y < dispH; ++y) my[y] = y * bh / dispH;
                for (int y = 0; y < dispH; ++y)
                {
                    const unsigned char* srcM = boxClip.data() + (size_t)my[y] * bw;
                    unsigned char* c = clipM.data() + (size_t)y * dispW;
                    for (int x = 0; x < dispW; ++x)
                        if (srcM[mx[x]]) { c[x] = 1; anyClip = true; }
                }
            }
            else
            {
                buildDisplayFrame(raw_[idx].data(), w, h, bpp, disp8.data(), clipM.data(),
                                  dispW, dispH, anyClip);
            }
            {
                std::lock_guard<std::mutex> l(frameMtx_);
                if (anyClip)
                {
                    rgb.assign((size_t)dispW * dispH * 3, 0);
                    for (size_t i = 0; i < disp8.size(); ++i)
                    {
                        size_t o = i * 3;
                        if (clipM[i]) { rgb[o] = 255; rgb[o + 1] = 0; rgb[o + 2] = 0; }
                        else          { rgb[o] = disp8[i]; rgb[o + 1] = disp8[i]; rgb[o + 2] = disp8[i]; }
                    }
                    latest_ = std::move(rgb);
                    latestCh_ = 3;
                }
                else
                {
                    latest_ = std::move(disp8);
                    latestCh_ = 1;
                }
                latestW_ = dispW;
                latestH_ = dispH;
            }
        }
        auto tB1 = dbg ? Clock::now() : Clock::time_point{};

        auto tH0 = dbg ? Clock::now() : Clock::time_point{};
        computeHistogram(histSrc, histW, histH, histBpp, (long)histW * histH, isColor_);
        auto tH1 = dbg ? Clock::now() : Clock::time_point{};

        {
            std::lock_guard<std::mutex> l(frameMtx_);
            readingIdx_ = -1;
        }
        frameCv_.notify_one();

        ++buildCount;
        previewBuilds_.fetch_add(1, std::memory_order_relaxed);
        if (dbg && buildCount % 50 == 0)
        {
            static auto t0 = Clock::now();
            std::fprintf(stderr, "[perf-prev] t=%.3fs builds=%llu roi=%dx%d bpp=%d disp=%dx%d downscale=%.1fms hist=%.1fms lastgap=%dms\n",
                         std::chrono::duration<double>(now - t0).count(),
                         (unsigned long long)buildCount, w, h, bpp, dispW, dispH,
                         std::chrono::duration<double, std::milli>(tB1 - tB0).count(),
                         std::chrono::duration<double, std::milli>(tH1 - tH0).count(),
                         tPrev != Clock::time_point{}
                             ? (int)std::llround(std::chrono::duration<double, std::milli>(now - tPrev).count())
                             : 0);
            std::fflush(stderr);
        }
        tPrev = now;
    }
}

// Single photo (snap): one standalone exposure saved to photos/.
// Every SDK call checks for ASI_ERROR_CAMERA_REMOVED: on this machine the USB
// can re-enumerate the camera at any time, and calling the SDK with the dead
// handle risks segfaulting the process. On a loss the pending photoResult is
// emitted (so the UI un-dims the button) and the worker waits for the camera
// to come back (handleCameraLost).
void CameraWorker::doPhoto(double expS)
{
    const bool dbg = isDebugRecording();
    long us = (long)std::llround(expS * 1e6);
    bool ok = false;
    std::string path;
    std::string err = "exposure failed or file could not be written";
    QString errQt;                     // filled by saveStill on a write failure
    auto finishErr = [&]() { if (!errQt.isEmpty()) err = errQt.toStdString(); };

    // Stage markers (CAMDBG=1): on a crash, the last printed line names the
    // SDK call the worker was in.
    if (dbg) std::fprintf(stderr, "[photo] start exp=%ldus roi=%dx%d bpp=%d cam=%d videoActive=%d raw=%zu\n",
                          us, w_, h_, bpp_, cam_, (int)videoActive_, maxRawBytes_);

    auto lost = [this](const char* why) {
        emit photoResult(QString(), false, "camera disconnected");
        handleCameraLost(why);
    };

    // Ensure the selected ROI + bit depth are applied before capturing (they
    // reset to the defaults on a reconnect, and a photo queued in the same
    // loop iteration as an ROI change would otherwise use the old format).
    Config cfgSnap;
    {
        std::lock_guard<std::mutex> l(cfgMtx_);
        cfgSnap = cfg_;
    }
    int dw   = (cfgSnap.roiW > 0) ? cfgSnap.roiW : fullW_;
    int dh   = (cfgSnap.roiH > 0) ? cfgSnap.roiH : fullH_;
    int dbpp = (cfgSnap.bitDepth == 8 || !hasRaw16_) ? 1 : 2;
    if (dw != w_ || dh != h_ || dbpp != bpp_)
    {
        if (!applyRoi(dw, dh, dbpp, /*restartVideo=*/false))
        {
            if (cam_ < 0) { lost("photo: roi switch"); return; }
            // non-removed failure: capture at the current format (the error
            // was already reported by applyRoi)
        }
        else
        {
            // The format change reset the camera's gain to the firmware
            // default. Re-apply the user's gain so this capture isn't taken at
            // the wrong gain (exposure is re-applied below). Only reachable in
            // the same-iteration case (photo + format change together); the
            // main loop otherwise re-applies both before the photo fires.
            ASI_ERROR_CODE ge = ASISetControlValue(cam_, ASI_GAIN, cfgSnap.gain, ASI_FALSE);
            if (ge == ASI_ERROR_CAMERA_REMOVED) { lost("photo: set gain"); return; }
            if (ge == ASI_SUCCESS)
            {
                lastGainApplied_ = cfgSnap.gain;
                std::lock_guard<std::mutex> tl(teleMtx_);
                tele_.gainApplied = cfgSnap.gain;
            }
            // White balance is reset by the same firmware behaviour; apply it
            // here too (applyRoi only invalidates the trackers, which re-applies
            // on the loop's NEXT iteration — this photo happens NOW, and must
            // not be taken without the user's WB; no-op on mono bodies).
            const int we = applyWhiteBalanceNow(cfgSnap.autoWb, cfgSnap.wbR, cfgSnap.wbB);
            if (we == (int)ASI_ERROR_CAMERA_REMOVED) { lost("photo: set WB"); return; }
        }
    }

    ASI_ERROR_CODE e = ASIStopVideoCapture(cam_);
    videoActive_ = false;
    if (dbg) std::fprintf(stderr, "[photo] video stopped (e=%d)\n", (int)e);
    if (e == ASI_ERROR_CAMERA_REMOVED) { lost("photo: stop video"); return; }

    e = ASISetControlValue(cam_, ASI_EXPOSURE, us, ASI_FALSE);
    if (dbg) std::fprintf(stderr, "[photo] exposure set (e=%d)\n", (int)e);
    if (e == ASI_ERROR_CAMERA_REMOVED)
    {
        lost("photo: set exposure");
        return;
    }
    lastExpApplied_ = us;

    e = ASIStartExposure(cam_, ASI_FALSE);
    if (dbg) std::fprintf(stderr, "[photo] exposure started (e=%d)\n", (int)e);
    if (e == ASI_SUCCESS)
    {
        using Clock = std::chrono::steady_clock;
        auto deadline = Clock::now() + std::chrono::duration<double>(expS + 5.0);
        ASI_EXPOSURE_STATUS st = ASI_EXP_WORKING;
        while (Clock::now() < deadline)
        {
            ASI_ERROR_CODE se = ASIGetExpStatus(cam_, &st);
            if (se == ASI_ERROR_CAMERA_REMOVED) { lost("photo: get status"); return; }
            if (se != ASI_SUCCESS || st != ASI_EXP_WORKING) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (dbg) std::fprintf(stderr, "[photo] exposure finished (st=%d)\n", (int)st);
        if (st == ASI_EXP_SUCCESS)
        {
            // The stream is stopped, but a previous preview build may still
            // be reading an older frame. Choose a free raw slot for the shot.
            selectWritableRawSlot();
            ASI_ERROR_CODE ge = ASIGetDataAfterExp(cam_, raw_[activeIdx_].data(), (long)maxRawBytes_);
            if (dbg) std::fprintf(stderr, "[photo] data fetched (ge=%d)\n", (int)ge);
            if (ge == ASI_ERROR_CAMERA_REMOVED) { lost("photo: get data"); return; }
            if (ge == ASI_SUCCESS)
            {
                // A colour camera saves RGB (3 channels); a mono body's frame
                // goes to disk exactly as it came out of the sensor. The
                // container follows the depth either way: 1 byte/px -> PNG,
                // 2 bytes/px -> 16-bit TIFF (see saveStill).
                path = saveStill(QString("photos/photo_%1%2")
                                     .arg(timestamp())
                                     .arg(bpp_ == 1 ? ".png" : ".tif"),
                                 raw_[activeIdx_].data(), w_, h_, bpp_, errQt).toStdString();
                ok = !path.empty();
                if (ok) publishStillPreview(raw_[activeIdx_].data(), w_, h_, bpp_);
            }
        }
    }

    if (dbg) std::fprintf(stderr, "[photo] saved ok=%d %s\n", (int)ok, path.c_str());

    // resume live preview
    e = ASIStartVideoCapture(cam_);
    if (dbg) std::fprintf(stderr, "[photo] preview resumed (e=%d)\n", (int)e);
    if (e == ASI_SUCCESS) videoActive_ = true;
    nextAllowed_ = std::chrono::steady_clock::now();
    lastFrameTime_ = std::chrono::steady_clock::time_point{};

    finishErr();
    emit photoResult(QString::fromStdString(path), ok,
                     ok ? QString() : QString::fromStdString(err));
}

// Interval sequence: back-to-back long exposures. The next shot starts exactly
// `interval` (>= 1 s) AFTER the previous shot finished — the interval is the
// time between images and always elapses, independent of the exposure (a long
// exposure is followed by the full interval, not shortened by it).
// Captures to sequences/seq_<ts>/NNNN.<ext>.
void CameraWorker::doSequence()
{
    using Clock = std::chrono::steady_clock;

    Config cfg;
    {
        std::lock_guard<std::mutex> l(cfgMtx_);
        cfg = cfg_;
    }
    // Exposure comes from the main exposure slider (clamped to the SDK range);
    // the interval is clamped to a 1 s minimum. count=0 runs continuously.
    double expS      = std::clamp(cfg.sequenceExpS, 0.001, kExpSdkMaxS);
    double intervalS = std::max(cfg.sequenceIntervalS, 1.0);  // enforce 1 s minimum
    int count        = cfg.sequenceCount;                      // 0 = continuous

    // A sequence exclusively owns the camera: stop any active recording first.
    if (recordingActive_) stopRecording();

    // The GUI already flipped the button to "running" before the request
    // reached the worker, so EVERY exit path emits sequenceDone.
    // The camera's USB device can disappear mid-sequence (re-enumeration):
    // on a loss we must stop calling the SDK on the dead handle (it risks
    // segfaulting) and wait for the camera to come back (handleCameraLost).
    auto lost = [this]() {
        handleCameraLost("sequence");
    };

    // Ensure the selected ROI + bit depth are applied before capturing.
    int dw   = (cfg.roiW > 0) ? cfg.roiW : fullW_;
    int dh   = (cfg.roiH > 0) ? cfg.roiH : fullH_;
    int dbpp = (cfg.bitDepth == 8 || !hasRaw16_) ? 1 : 2;
    if (dw != w_ || dh != h_ || dbpp != bpp_)
    {
        if (!applyRoi(dw, dh, dbpp, /*restartVideo=*/false))
        {
            if (cam_ < 0) { emit sequenceDone(0, true); lost(); return; }
            // non-removed failure: proceed with the current format (the error
            // was already reported by applyRoi)
        }
        else
        {
            // The format change reset the camera's gain to the firmware
            // default; re-apply the user's gain so the shots aren't taken at
            // the wrong gain (exposure is re-applied below).
            ASI_ERROR_CODE ge = ASISetControlValue(cam_, ASI_GAIN, cfg.gain, ASI_FALSE);
            if (ge == ASI_ERROR_CAMERA_REMOVED) { emit sequenceDone(0, true); lost(); return; }
            if (ge == ASI_SUCCESS)
            {
                lastGainApplied_ = cfg.gain;
                std::lock_guard<std::mutex> tl(teleMtx_);
                tele_.gainApplied = cfg.gain;
            }
            // White balance: same firmware reset, same reasoning as doPhoto —
            // the shots must carry the user's WB (no-op on mono bodies).
            const int we = applyWhiteBalanceNow(cfg.autoWb, cfg.wbR, cfg.wbB);
            if (we == (int)ASI_ERROR_CAMERA_REMOVED) { emit sequenceDone(0, true); lost(); return; }
        }
    }

    // One sub-folder per sequence: sequences/seq_<ts>/NNNN.<ext>
    std::string dir = "sequences/seq_" + timestamp().toStdString();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    // Dedicate the camera to the sequence (pause the live preview).
    if (videoActive_)
    {
        ASI_ERROR_CODE e = ASIStopVideoCapture(cam_);
        videoActive_ = false;
        if (e == ASI_ERROR_CAMERA_REMOVED) { emit sequenceDone(0, true); lost(); return; }
    }

    long us = (long)std::llround(expS * 1e6);
    ASI_ERROR_CODE e = ASISetControlValue(cam_, ASI_EXPOSURE, us, ASI_FALSE);
    lastExpApplied_ = us;
    if (e == ASI_ERROR_CAMERA_REMOVED) { emit sequenceDone(0, true); lost(); return; }

    emit sequenceStarted();

    int i = 0;
    bool earlyStop = false;
    bool camLost = false;
    while (true)
    {
        if (seqStop_)                  { earlyStop = true; break; }
        if (count > 0 && i >= count)   break;

        i++;

        // Status: this shot's exposure is running (keeps the button line
        // current through long exposures instead of freezing on the last
        // countdown value).
        emit sequenceExposing(i, count);

        // Start the exposure and wait for it to finish.
        bool haveFrame = false, exposureDone = false;
        ASI_ERROR_CODE se0 = ASIStartExposure(cam_, ASI_FALSE);
        if (se0 == ASI_ERROR_CAMERA_REMOVED)
            camLost = true;
        else if (se0 == ASI_SUCCESS)
        {
            auto deadline = Clock::now() + std::chrono::duration<double>(expS + 10.0);
            ASI_EXPOSURE_STATUS st = ASI_EXP_WORKING;
            while (Clock::now() < deadline && !seqStop_)
            {
                ASI_ERROR_CODE se = ASIGetExpStatus(cam_, &st);
                if (se == ASI_ERROR_CAMERA_REMOVED) { camLost = true; break; }
                if (st != ASI_EXP_WORKING) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            exposureDone = (st == ASI_EXP_SUCCESS);
            if (exposureDone)
            {
                // A previous preview build may still be reading a raw slot;
                // choose a free one before the standalone shot is fetched.
                selectWritableRawSlot();
                ASI_ERROR_CODE ge = ASIGetDataAfterExp(cam_, raw_[activeIdx_].data(), (long)maxRawBytes_);
                if (ge == ASI_ERROR_CAMERA_REMOVED) { camLost = true; exposureDone = false; }
                else if (ge == ASI_SUCCESS) haveFrame = true;
            }
        }
        if (camLost || seqStop_)
        {
            if (!exposureDone && !camLost) ASIStopExposure(cam_);  // cancel in-progress exposure
            earlyStop = true;
            break;
        }

        if (haveFrame)
        {
            // Save (RGB for a colour body, single-channel for a mono one;
            // 1 byte/px -> PNG, 2 bytes/px -> 16-bit TIFF) + refresh preview.
            char name[16];
            std::snprintf(name, sizeof name, "%04d", i);
            QString errQt;
            const QString saved = saveStill(QString("%1/%2%3")
                                                .arg(QString::fromStdString(dir))
                                                .arg(name)
                                                .arg(bpp_ == 1 ? ".png" : ".tif"),
                                            raw_[activeIdx_].data(), w_, h_, bpp_, errQt);
            const bool ok = !saved.isEmpty();
            if (ok) publishStillPreview(raw_[activeIdx_].data(), w_, h_, bpp_);
            emit sequenceShot(i, count, ok ? saved : QString());
        }
        else
        {
            emit sequenceShot(i, count, QString()); // failed shot
        }

        // If this was the last shot, stop immediately (no trailing interval wait).
        if (count > 0 && i >= count) break;

        // The interval is the time BETWEEN two captured images — it is added
        // on top of the exposure, not folded into it, so it always elapses,
        // even when the exposure is longer than the interval. Now that this
        // shot is captured + saved, start the countdown of the full interval
        // to the next shot. (The GUI shows it on the sequence button as
        // "Shot N done · next in X s"; without this the countdown was skipped
        // whenever the exposure already exceeded the interval.)
        auto nextShot = Clock::now() + std::chrono::duration<double>(intervalS);
        while (!seqStop_)
        {
            auto now = Clock::now();
            if (now >= nextShot) break;
            double rem = std::chrono::duration<double>(nextShot - now).count();
            emit sequenceWait(rem);
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        if (seqStop_) { earlyStop = true; break; }
    }

    if (camLost)
    {
        emit sequenceDone(i, true);
        lost();
        return;
    }

    // Resume the live preview.
    if (ASIStartVideoCapture(cam_) == ASI_SUCCESS) videoActive_ = true;
    nextAllowed_ = Clock::now();
    lastFrameTime_ = Clock::time_point{};

    // `i` is the 1-based index of the last shot processed, i.e. the number of
    // shots captured (a stop mid-exposure may make the last one a failed shot).
    emit sequenceDone(i, earlyStop);
}

void CameraWorker::startRecording(double expS, int fps)
{
    const bool dbg = isDebugRecording();
    (void)expS;
    int bits = cfg_.bitDepth;          // 8, 10 or 14 (--vtest), from the GUI depth
    if (bits != 8 && !hasRaw16_) bits = 8;   // no 2-byte readout -> no deep file
    // .ser for the deep depth always, and for 8-bit when the user picked .ser
    // (serMode). 8-bit H.264 is the only non-.ser video.
    videoIsSer_ = (bits != 8) || cfg_.serMode;
    videoPath_ = "videos/video_" + timestamp().toStdString() + (videoIsSer_ ? ".ser" : ".mp4");

    // Use the measured camera rate if it is below the target (bandwidth),
    // so the file plays back at the correct speed.
    double rate = emaFps_ > 0.5 ? emaFps_ : (double)fps;
    writerFps_ = std::clamp(rate, 1.0, (double)fps);

    // 8-bit H.264 -> GStreamer pipeline (bounded queue, no blocking);
    // 8/deep-bit .ser -> uncompressed (plain file, SerWriter) in ONE plane for
    // a mono body and THREE (R, G, B) for a colour one — the Bayer readout is
    // demosaiced by pushToWriter on the way in, so a stacking tool opening the
    // file sees ordinary RGB, and a mono file is byte-identical to before.
    const CameraCaps cc = caps();
    const int colorId = (isColor_ && videoIsSer_) ? kSerColorRgb : kSerColorMono;
    bool ok = videoIsSer_
            ? serWriter_.open(videoPath_, w_, h_, bits, cc.name.toStdString(), colorId)
            : encoder_.start(videoPath_, w_, h_, writerFps_);
    if (!ok)
    {
        videoPath_ = "";
        videoIsSer_ = false;
        emit recordingStopped(QString(), false);
        return;
    }
    recordingActive_ = true;
    framesWritten_ = 0;
    encoderDropped_ = 0;
    recT0_ = std::chrono::steady_clock::now();
    // Fresh pace anchor: the first frame is written immediately (the 0.5 s
    // resync would cover a stale anchor too, but this avoids the half-period
    // dead time after a long non-recording gap). The EMA restarts so the
    // "Frame rate" settles to the written rate quickly.
    nextAllowed_ = std::chrono::steady_clock::now();
    lastFrameTime_ = std::chrono::steady_clock::time_point{};
    emaFps_ = 0.0;
    if (dbg) std::fprintf(stderr, "[rec] START %s %d-bit colorId=%d writerFps=%.2f emaFps=%.2f path=%s\n",
        videoIsSer_ ? "ser" : "h264", bits, videoIsSer_ ? colorId : 0,
        writerFps_, emaFps_, videoPath_.c_str());
    emit recordingStarted(QString::fromStdString(videoPath_));
}

void CameraWorker::stopRecording()
{
    const bool dbg = isDebugRecording();
    if (dbg) std::fprintf(stderr, "[rec] STOP t=%.3f written=%llu dropped=%llu\n",
        std::chrono::duration<double>(std::chrono::steady_clock::now() - recT0_).count(),
        (unsigned long long)framesWritten_, (unsigned long long)encoderDropped_);
    if (videoIsSer_) serWriter_.close();
    else encoder_.stop();
    videoIsSer_ = false;
    recordingActive_ = false;
    std::string p = videoPath_;
    videoPath_ = "";
    emit recordingStopped(QString::fromStdString(p), true);
}

// Save one captured frame to `path` (the extension selects the container).
//  1 byte/px  -> PNG,  2 bytes/px -> 16-bit TIFF,
// and either single-channel (a mono body, exactly what the sensor read out) or
// 3-channel RGB (a colour body: its Bayer readout is demosaiced first, at the
// FULL depth of the capture — 8-bit PNG or 16-bit TIFF — never downscaled).
// Returns the path on success, or an empty string with `err` filled in.
QString CameraWorker::saveStill(const QString& path, const unsigned char* raw, int w, int h,
                                int bpp, QString& err)
{
    const bool colour = isColor_;
    const int pat = bayer_;
    cv::Mat m;
    if (bpp == 1)
    {
        if (colour)
        {
            capColour_.rgbFull8.resize((size_t)w * h * 3);
            if (!demosaicBayerToBgr(raw, w, h, 1, pat, capColour_.rgbFull8.data()))
            {
                err = QString("cannot convert this frame to RGB (Bayer pattern %1)")
                          .arg(bayerPatternName(pat));
                return QString();
            }
            m = cv::Mat(h, w, CV_8UC3, capColour_.rgbFull8.data());
        }
        else
        {
            m = cv::Mat(h, w, CV_8UC1, const_cast<unsigned char*>(raw));
        }
    }
    else
    {
        if (colour)
        {
            capColour_.rgb16.resize((size_t)w * h * 3 * 2);
            if (!demosaicBayerToBgr(raw, w, h, 2, pat, capColour_.rgb16.data()))
            {
                err = QString("cannot convert this frame to RGB (Bayer pattern %1)")
                          .arg(bayerPatternName(pat));
                return QString();
            }
            m = cv::Mat(h, w, CV_16UC3, capColour_.rgb16.data());
        }
        else
        {
            m = cv::Mat(h, w, CV_16UC1, const_cast<unsigned char*>(raw));
        }
    }
    if (!cv::imwrite(path.toStdString(), m))
    {
        err = "could not write " + path;
        return QString();
    }
    return path;
}

// Publish a still frame into the preview buffer. During a photo/sequence shot
// the camera is dedicated and the preview builder is idle (nothing is
// published), so the capture thread shows what was just saved: the mono frame
// as 8-bit grey, a colour frame as a real RGB thumbnail.
void CameraWorker::publishStillPreview(const unsigned char* raw, int w, int h, int bpp)
{
    if (!isColor_)
    {
        std::lock_guard<std::mutex> l(frameMtx_);
        if (bpp == 1)
            latest_.assign(raw, raw + (size_t)w * h);
        else
        {
            const unsigned short* p = (const unsigned short*)raw;
            latest_.resize((size_t)w * h);
            for (size_t i = 0; i < latest_.size(); ++i)
                latest_[i] = (unsigned char)(p[i] >> 8);
        }
        latestW_ = w;
        latestH_ = h;
        // Grayscale refresh: latestCh_ must follow the data or the display timer
        // builds an RGB888 QImage (3x stride) over a 1-channel buffer and
        // QImage::copy() reads past the end (SIGSEGV — the old photo crash).
        latestCh_ = 1;
        return;
    }

    int dispW, dispH;
    if (w <= kDispMax && h <= kDispMax) { dispW = w; dispH = h; }
    else if (w >= h) { dispW = kDispMax; dispH = (int)((long)h * kDispMax / w); }
    else             { dispH = kDispMax; dispW = (int)((long)w * kDispMax / h); }
    if (dispW < 1) dispW = 1;
    if (dispH < 1) dispH = 1;
    std::vector<unsigned char> rgb((size_t)dispW * dispH * 3, 0);
    std::vector<unsigned char> mask((size_t)dispW * dispH, 0);
    bool anyClip = false;
    colourDisplayFrame(raw, w, h, bpp, true, bayer_, rgb.data(), mask.data(),
                       dispW, dispH, anyClip, capColour_);
    if (anyClip)
    {
        for (size_t i = 0, n = mask.size(); i < n; ++i)
        {
            if (!mask[i]) continue;
            unsigned char* o = rgb.data() + i * 3;
            o[0] = 255; o[1] = 0; o[2] = 0;
        }
    }
    std::lock_guard<std::mutex> l(frameMtx_);
    latest_ = std::move(rgb);
    latestW_ = dispW;
    latestH_ = dispH;
    latestCh_ = 3;                 // RGB with the clipped blocks marked red
}

// Hand one grabbed frame to the active writer. The layout the FILE wants is a
// property of the CAMERA, not of the container:
//
//   mono body  -> .ser: the readout verbatim (1 or 2 bytes/px, single plane);
//                 H.264: grey tripled to BGR (videoconvert has no GRAY input).
//   colour body-> .ser: Bayer -> demosaiced RGB, PER-PIXEL INTERLEAVED R,G,B
//                 (the SER ColorID 100 layout both Siril and Ser-Player read;
//                 pushRgb / pushRgb8 — see ser_writer.h);
//                 H.264: Bayer -> 8-bit BGR (the codec is 8-bit).
// Returns false when the conversion or the write failed (the frame counts as
// dropped).
bool CameraWorker::pushToWriter(const unsigned char* raw, int w, int h, int bpp)
{
    const bool colour = isColor_;
    const int pat = bayer_;

    if (videoIsSer_)
    {
        if (!colour)
            return (bpp == 2) ? serWriter_.push((const uint16_t*)raw)
                              : serWriter_.push8(raw);
        // Demosaic straight to interleaved RGB — one pass, no reorder step:
        // the SER ColorID 100 frame layout IS interleaved RGB, R first.
        if (bpp == 2)
        {
            serRgbBuf_.resize((size_t)w * h * 3 * 2);
            if (!demosaicBayerToRgb(raw, w, h, 2, pat, serRgbBuf_.data())) return false;
            return serWriter_.pushRgb((const uint16_t*)serRgbBuf_.data());
        }
        serRgbBuf_.resize((size_t)w * h * 3);
        if (!demosaicBayerToRgb(raw, w, h, 1, pat, serRgbBuf_.data())) return false;
        return serWriter_.pushRgb8(serRgbBuf_.data());
    }

    // 8-bit H.264.
    bgr8Buf_.resize((size_t)w * h * 3);
    if (colour)
    {
        if (!demosaicBayerToBgr8(raw, w, h, bpp, pat, bgr8Buf_.data(), capColour_)) return false;
    }
    else
    {
        const size_t n = (size_t)w * h;
        unsigned char* o = bgr8Buf_.data();
        if (bpp == 1)
        {
            const unsigned char* p = raw;
            for (size_t i = 0; i < n; ++i) { o[3*i] = p[i]; o[3*i+1] = p[i]; o[3*i+2] = p[i]; }
        }
        else
        {
            const unsigned short* p = (const unsigned short*)raw;
            for (size_t i = 0; i < n; ++i)
            {
                const unsigned char v = (unsigned char)(p[i] >> 8);
                o[3*i] = v; o[3*i+1] = v; o[3*i+2] = v;
            }
        }
    }
    return encoder_.push(bgr8Buf_.data(), w, h);
}

// Stop capture, switch the ROI + pixel format, and (by default) restart.
// Worker thread. With restartVideo=false the camera is left stopped so the
// caller can drive long-exposure snaps directly without a redundant
// start/stop cycle (which intermittently breaks the first long exposure).
// Returns false on failure; if the camera was lost cam_ is reset to -1 so the
// caller can tell a lost camera (handleCameraLost) from a plain failure.
bool CameraWorker::applyRoi(int w, int h, int dbpp, bool restartVideo)
{
    ASI_IMG_TYPE it;
    if (dbpp == 2)
    {
        it = ASI_IMG_RAW16;                     // the deep (2 byte/px) readout
    }
    else
    {
        // The 1-byte readout: RAW8 everywhere it exists; a body that only
        // offers Y8 (rare; mono sensors) still gives usable single-channel data.
        it = hasRaw8_ ? ASI_IMG_RAW8 : ASI_IMG_Y8;
    }
    ASI_ERROR_CODE e;
    if (videoActive_)
    {
        e = ASIStopVideoCapture(cam_);
        videoActive_ = false;
        if (e == ASI_ERROR_CAMERA_REMOVED) { cam_ = -1; return false; }
    }
    e = ASISetROIFormat(cam_, w, h, 1, it);
    if (e == ASI_ERROR_CAMERA_REMOVED) { cam_ = -1; return false; }
    if (e != ASI_SUCCESS)
    {
        emit cameraError(QString("Failed to set ROI to %1x%2 (%3 bytes/px)")
                             .arg(w).arg(h).arg(dbpp));
        return false;
    }
    // Publish the current capture geometry under frameMtx_. The builder uses
    // the geometry stored alongside each completed frame; the raw buffers
    // are already sized to the sensor maximum, so an ROI change needs no resize.
    {
        std::lock_guard<std::mutex> l(frameMtx_);
        w_ = w;
        h_ = h;
        bpp_ = dbpp;
    }
    // ASI_HIGH_SPEED_MODE (the fast ADC readout) only takes effect with RAW8
    // OUTPUT: the RAW16 path ignores it and always reads out at full depth
    // (verified 2025-09-15 — at 3096x2080 RAW16, HSM=0/1 give identical 14-bit
    // data and 27.7 fps, while RAW8 doubles 28.3 -> 56.0 fps; at 480x320 RAW8,
    // 176 -> 351 fps; AGENTS.md §10). Enable it for 1-byte output so 8-bit
    // video/.ser runs on the fast column's readout; keep it off for RAW16.
    // A body without the control (probed: ASI_HIGH_SPEED_MODE absent, as on the
    // ASI174 series) is left alone — setting it there is an error, not a no-op.
    if (hasHsm_)
    {
        const int hsm = (dbpp == 1) ? 1 : 0;
        ASI_ERROR_CODE he = ASISetControlValue(cam_, ASI_HIGH_SPEED_MODE, hsm, ASI_FALSE);
        long hrb = -1; ASI_BOOL ha = ASI_FALSE;
        ASIGetControlValue(cam_, ASI_HIGH_SPEED_MODE, &hrb, &ha);
        std::fprintf(stderr, "[hsm] set=%d err=%d readback=%ld (bpp=%d)\n", hsm, (int)he, hrb, dbpp);
        if (he == ASI_ERROR_INVALID_CONTROL_TYPE)
        {
            hasHsm_ = false;    // the SDK says this body has no such control
            std::fprintf(stderr, "[hsm] not supported here - leaving the control alone\n");
        }
    }
    // The firmware RESETS the camera's exposure and gain controls to their
    // defaults whenever the ROI/pixel format changes (ASISetROIFormat). The
    // "already applied" trackers must be invalidated, or the loop believes the
    // user's exposure/gain are still in effect and never re-applies them —
    // the camera then keeps the (long) default exposure and the next shot
    // comes out completely overexposed. Resetting them here makes the loop
    // re-send the user's values on the very next iteration. White balance is
    // reset by the same firmware behaviour, so its trackers go too (auto mode
    // re-enables, manual mode re-writes the gains).
    lastExpApplied_ = -1;
    lastGainApplied_ = -1;
    lastWbAuto_ = -1;
    lastWbR_ = -1; lastWbB_ = -1;
    wbRetryAfter_ = {};
    if (!restartVideo)
    {
        videoActive_ = false;   // stay stopped; the caller owns the camera now
        return true;
    }
    e = ASIStartVideoCapture(cam_);
    if (e == ASI_ERROR_CAMERA_REMOVED) { cam_ = -1; return false; }
    if (e != ASI_SUCCESS)
    {
        emit cameraError("Failed to restart video capture");
        return false;
    }
    videoActive_ = true;
    // reset pacing / fps measurement after the (re)start
    lastFrameTime_ = std::chrono::steady_clock::now();
    nextAllowed_ = std::chrono::steady_clock::now();
    return true;
}

// Apply the white-balance config (see the header). Worker thread only; every
// SDK call reports removal like the exposure/gain writes next to it.
//
// ZWO semantics: the AUTO flag on the WB controls is the camera's automatic
// white balance — the SDK demo (main_SDK2_video.cpp) enables it via WB_R and
// leaves WB_B manual. Both channels answer IsAutoSupported on the ASI178MC,
// so when the caps say so we set both auto (nothing stays pinned to a stale
// manual gain); where only one does (or none), that one carries the flag and
// the other gets the camera's current value written manually — the demo's own
// pattern. Value arguments with bAuto=TRUE are seeds the camera overwrites
// with its computed gains.
//
// Measured on the ASI178MC with tests/wb_probe.cpp (2026-09-23): the flag is
// camera-GLOBAL — setting it on WB_R alone makes BOTH controls report auto=1
// on read-back — and both patterns converge to the SAME pair (R 93 / B 64 on a
// white daylight target, which then measures R/G 1.02, B/G 0.96). So the two
// ways of asking for AWB are equivalent here, the camera's balance is a good
// one, and this function has nothing to work around. It also showed the
// camera's own AWB can run out of authority (R pinned at its 99 ceiling under
// bluer light) — which is the camera's limit, not this code's.
int CameraWorker::applyWhiteBalanceNow(bool autoWb, int r, int b)
{
    if (!hasWb_ || cam_ < 0) return (int)ASI_SUCCESS;
    const int rC = std::clamp(r, wbRMin_, wbRMax_);
    const int bC = std::clamp(b, wbBMin_, wbBMax_);
    int knownR = rC, knownB = bC;
    ASI_ERROR_CODE e = ASI_SUCCESS;
    if (autoWb)
    {
        long r0 = rC, b0 = bC; ASI_BOOL a = ASI_FALSE;
        if (ASIGetControlValue(cam_, ASI_WB_R, &r0, &a) != ASI_SUCCESS) r0 = rC;
        if (ASIGetControlValue(cam_, ASI_WB_B, &b0, &a) != ASI_SUCCESS) b0 = bC;
        knownR = (int)std::clamp<long>(r0, wbRMin_, wbRMax_);
        knownB = (int)std::clamp<long>(b0, wbBMin_, wbBMax_);
        e = ASISetControlValue(cam_, ASI_WB_R, r0, wbAutoR_ ? ASI_TRUE : ASI_FALSE);
        if (e != ASI_ERROR_CAMERA_REMOVED)
        {
            const ASI_ERROR_CODE e2 =
                ASISetControlValue(cam_, ASI_WB_B, b0, wbAutoB_ ? ASI_TRUE : ASI_FALSE);
            if (e == ASI_SUCCESS) e = e2;
        }
    }
    else
    {
        e = ASISetControlValue(cam_, ASI_WB_R, rC, ASI_FALSE);
        if (e != ASI_ERROR_CAMERA_REMOVED)
        {
            const ASI_ERROR_CODE e2 = ASISetControlValue(cam_, ASI_WB_B, bC, ASI_FALSE);
            if (e == ASI_SUCCESS) e = e2;
        }
    }
    if (e == ASI_ERROR_CAMERA_REMOVED) return (int)e;
    if (e != ASI_SUCCESS)
    {
        std::fprintf(stderr, "[wb] set failed (auto=%d r=%d b=%d) err=%d\n",
                     (int)autoWb, rC, bC, (int)e);
        // Same escape as HighSpeedMode: a body that writes back
        // INVALID_CONTROL_TYPE does not really have the controls — drop WB
        // support (the GUI hides its row on the next cameraReady).
        if (e == ASI_ERROR_INVALID_CONTROL_TYPE)
        {
            hasWb_ = false;
            std::fprintf(stderr, "[wb] not supported here - leaving the controls alone\n");
        }
        return (int)e;
    }
    // Applied state: in auto mode the camera owns the gain values, so the
    // manual trackers are reset and any return to manual re-writes them.
    lastWbAuto_ = autoWb ? 1 : 0;
    lastWbR_ = autoWb ? -1 : rC;
    lastWbB_ = autoWb ? -1 : bC;
    std::lock_guard<std::mutex> tl(teleMtx_);
    tele_.wbR = knownR;
    tele_.wbB = knownB;
    tele_.wbAuto = autoWb;
    tele_.wbValid = true;
    return (int)ASI_SUCCESS;
}

// Probe the ROIs the camera actually accepts. The SDK exposes no enumeration
// API, so we try the candidate list the SENSOR reports (camera_caps.h builds it
// from MaxWidth/MaxHeight: the 1:1 base square first — the entry the selector
// opens on — then full frame, integer bins, aspect-preserving width ladder)
// and keep the ones the camera applies exactly, the square still first. Runs
// once per open (before the capture loop); a reconnect with another body gets
// its own list.
void CameraWorker::probeRois()
{
    const CameraCaps cc = caps();
    // Probe with the readout the camera actually has: the 1-byte one when it
    // exists (RAW8, else the Y8 variant), otherwise the 2-byte one.
    const ASI_IMG_TYPE probeIt = cc.hasRaw8   ? ASI_IMG_RAW8
                               : cc.hasY8     ? ASI_IMG_Y8
                                              : ASI_IMG_RAW16;
    std::vector<std::pair<int,int>> valid;
    for (const RoiSize& s : cc.roiCandidates())
    {
        const int w = s.w, h = s.h;
        if (w > fullW_ || h > fullH_ || w < 64 || h < 48) continue;
        if (ASISetROIFormat(cam_, w, h, 1, probeIt) != ASI_SUCCESS) continue;
        int aw = 0, ah = 0, ab = 0; ASI_IMG_TYPE it = ASI_IMG_END;
        if (ASIGetROIFormat(cam_, &aw, &ah, &ab, &it) != ASI_SUCCESS) continue;
        if (aw != w || ah != h) continue;   // camera clamped it -> not a real size
        valid.push_back({w, h});
    }
    // The 1:1 base square (the selector's first entry — the user-requested
    // default framing) leads the list when this body accepts it; everything
    // else sorts largest-area first behind it.
    const RoiSize base = cc.baseSquare();
    std::vector<std::pair<int,int>> tail;
    for (const auto& p : valid)
        if (!(base.w > 0 && p.first == base.w && p.second == base.h))
            tail.push_back(p);
    std::sort(tail.begin(), tail.end(),
              [](auto& a, auto& b) { return (long)a.first * a.second > (long)b.first * b.second; });
    std::vector<std::pair<int,int>> ordered;
    for (const auto& p : valid)
        if (base.w > 0 && p.first == base.w && p.second == base.h)
            { ordered.push_back(p); break; }
    ordered.insert(ordered.end(), tail.begin(), tail.end());
    if (ordered.empty()) ordered.push_back({fullW_, fullH_});
    validRois_ = ordered;
    if (isDebugRecording())
        for (const auto& p : validRois_)
            std::fprintf(stderr, "[roi] probed %dx%d%s\n", p.first, p.second,
                         (base.w > 0 && p.first == base.w && p.second == base.h)
                             ? " (1:1 base, first)" : "");

    // apply the default (full frame — the highest resolution; 1-byte readout
    // when the camera has one) and start the live capture first, so the
    // default size is live before the probed ROI list is published to the GUI
    // (the GUI selects the same largest-area entry). The 1:1 base square
    // stays the list's first entry but is only captured when the user picks
    // it (or on reconnect, where a user-chosen size — cfg.roiW > 0 — is
    // re-applied by the loop, as always). (applyRoi takes dbpp — 1 or 2
    // bytes/px — not the bit depth: the old "8" here selected RAW16 and left
    // bpp_==8, an invalid value the first loop tick had to fix.)
    applyRoi(fullW_, fullH_, (cc.hasRaw8 || cc.hasY8) ? 1 : 2);

    QStringList list;
    for (auto [w, h] : validRois_)
        list << QString("%1x%2").arg(w).arg(h);
    emit roiListReady(list);
    emit cameraReady(cc.describe());
}

void CameraWorker::updateTelemetry()
{
    if (cam_ < 0) return;   // no live handle: never touch the SDK
    long temp10 = 0;
    ASI_BOOL auto_ = ASI_FALSE;
    if (ASIGetControlValue(cam_, ASI_TEMPERATURE, &temp10, &auto_) == ASI_SUCCESS)
    {
        std::lock_guard<std::mutex> l(teleMtx_);
        tele_.tempC = temp10 / 10.0;
    }
    int dropped = 0;
    if (ASIGetDroppedFrames(cam_, &dropped) == ASI_SUCCESS)
    {
        std::lock_guard<std::mutex> l(teleMtx_);
        tele_.dropped = dropped + droppedPaced_;
    }
    // WB readback is deliberately absent from this 500 ms hot-path tick.
    // Those extra SDK calls are unnecessary for capture and cannot update the
    // GUI's manual settings while automatic balance is running.
    // The pair is sampled at open; successful manual writes update telemetry.
    {
        std::lock_guard<std::mutex> l(teleMtx_);
        tele_.actualFps = emaFps_;
        tele_.frameCount = frameCount_.load();
        tele_.videoFramesWritten = framesWritten_;
        tele_.videoFramesDropped = encoderDropped_;
        tele_.previewBuilds = previewBuilds_.load(std::memory_order_relaxed);
    }
}

// Build a 128-bin histogram of a raw frame and detect clipped pixels
// (reaching the max value). Called by the preview builder thread on the
// published raw frame (never on the live buffer being written). The bin range
// adapts to the capture bit depth: 8-bit -> 0..255 (2 values/bin), 14-bit ->
// 0..65535 (512 values/bin). The 14-bit ADC is delivered in a 16-bit container
// scaled x4 (steps of 4) and saturates at 65528 (not 65535), so the clip
// threshold is that saturation value. When pixels are clipped, the caller
// (previewBuilderLoop) builds the red-marked RGB preview for the GUI.
//
// A COLOUR frame's raw samples are the Bayer mosaic, so every fourth 2x2 block
// is sampled and ALL FOUR of its positions (one R, one B, two G) go into the
// same histogram — the display shows the combined colour distribution instead
// of one channel's, and a clip is reported per BLOCK (any channel of it at max)
// exactly like the red marking in the preview.
//
// `rawPixels` is the frame's real pixel count when `raw` is a subsampled or
// box-filtered VIEW of it: the reported clip count is scaled back to the frame
// so the GUI's percentage means "share of the frame's pixels", not of the view
// it happened to be measured from (the old code reported the box buffer's
// "all four samples saturated" count for a full-res frame — an under-report of
// about 4x against the red marking next to it).
void CameraWorker::computeHistogram(const unsigned char* raw, int w, int h, int bpp,
                                   long rawPixels, bool colour)
{
    std::array<int, kHistBins> bins{};
    long clipCount = 0;
    // Subsample on a 2x2 grid (every other row, every other column): a 128-bin
    // histogram is statistically indistinguishable at 1/4 of the pixels, and
    // this keeps the per-frame cost at ~1/4 of a full scan — at 6.4 MP that is
    // the difference between ~11 ms and ~3 ms, which is what lets the full-res
    // 10-bit HSM stream (56 fps) be drained before the camera's USB buffer
    // overflows. clipCount is the same representative estimate (for the clip
    // indicator), not an exact count.
    const int stride = 2;
    long sampled = 0;
    if (colour)
    {
        // Every fourth 2x2 block, all four of its mosaic samples (see above).
        for (int y = 0; y < h; y += 4)
        {
            for (int sy = 0; sy < 2 && y + sy < h; ++sy)
            {
                const int rowY = y + sy;
                if (bpp == 1)
                {
                    const unsigned char* row = raw + (size_t)rowY * w;
                    for (int x = 0; x < w; x += 4)
                    {
                        for (int sx = 0; sx < 2 && x + sx < w; ++sx)
                        {
                            const unsigned char v = row[x + sx];
                            bins[v >> 1]++;
                            clipCount += (v >= 255);
                            ++sampled;
                        }
                    }
                }
                else
                {
                    const unsigned short* row = (const unsigned short*)raw + (size_t)rowY * w;
                    for (int x = 0; x < w; x += 4)
                    {
                        for (int sx = 0; sx < 2 && x + sx < w; ++sx)
                        {
                            const unsigned short v = row[x + sx];
                            bins[v >> 9]++;
                            clipCount += (v >= 65528);
                            ++sampled;
                        }
                    }
                }
            }
        }
    }
    else if (bpp == 1)
    {
        const unsigned char* p = raw;
        for (int y = 0; y < h; y += stride)
        {
            const unsigned char* row = p + (size_t)y * w;
            for (int x = 0; x < w; x += stride)
            {
                const unsigned char v = row[x];
                bins[v >> 1]++;                    // 256 / 128 = 2
                clipCount += (v >= 255);           // branchless
                ++sampled;
            }
        }
    }
    else
    {
        const unsigned short* p = (const unsigned short*)raw;
        for (int y = 0; y < h; y += stride)
        {
            const unsigned short* row = p + (size_t)y * w;
            for (int x = 0; x < w; x += stride)
            {
                const unsigned short v = row[x];
                bins[v >> 9]++;                    // 65536 / 128 = 512
                clipCount += (v >= 65528);         // 14-bit saturates at 65528
                ++sampled;
            }
        }
    }

    // Scale the sampled estimate up to the frame it was taken from.
    if (rawPixels > 0 && sampled > 0 && rawPixels != sampled)
        clipCount = (long)std::llround((double)clipCount * ((double)rawPixels / (double)sampled));
    if (clipCount > 0 && rawPixels > 0 && clipCount > rawPixels) clipCount = rawPixels;

    std::lock_guard<std::mutex> l(frameMtx_);
    hist_ = bins;
    clipCount_ = (int)clipCount;
}
