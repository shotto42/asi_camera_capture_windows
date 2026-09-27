// main.cpp
//
// ASI camera application entry point (any ZWO ASI body: mono or colour).
//
//   * Live frame display on the left, all controls in a side panel on the right
//   * What the panel OFFERS comes from the connected camera: its name, its
//     resolution candidates, the bit depths it can read out, its exposure
//     limits and its frame-rate ceiling (see camera_caps.h). A colour body
//     (IsColorCam) is captured as a Bayer mosaic and saved as RGB; a mono body
//     is saved single-channel. Nothing here is ASI178-specific any more — that
//     model is just what the tables were measured on.
//   * Photo mode: logarithmic exposure slider with a range switch —
//     32 µs .. 1 s (switch off) or 1 .. 60 s (switch on) — plus a circular
//     shutter button (red circle with a white ring) that saves a single frame.
//     When the exposure exceeds one frame period the live preview runs
//     at the full exposure on the video stream: the camera stretches its frame
//     period to match the exposure, so the preview updates once per exposure at
//     the real exposure (the old single-shot path — one standalone
//     ASIStartExposure per frame — added a fixed ~250 ms camera-side cost per
//     frame and is no longer used for the preview; doPhoto()/doSequence() still
//     use standalone exposures for their saved shots).
//   * Video mode: frame-rate slider (1 .. max fps for the current ROI),
//     exposure slider limited to the frame period (exposure <= 1/fps), and a
//     record button that turns red while recording. Bit depth picks the output:
//     8-bit H.264 -> MP4 (GStreamer/x264enc); 8/14-bit .ser -> an
//     uncompressed .ser (LUCAM-RECORDER v3) file (plain C++ stdio; the
//     8-bit .ser stores 1 byte/px, 14-bit 2 bytes/px). A colour body writes
//     3-plane RGB .ser and RGB MP4; a mono body writes MONO, as before.
//   * Interval mode: back-to-back long-exposure sequence. The sequence takes
//     its exposure from the main exposure slider (there is no separate exposure
//     control); in this mode the slider offers the SAME range switch as photo
//     mode — 32 µs .. 1 s (switch off) or 1 .. 60 s (switch on, linear, whole-
//     second steps). The next shot starts `interval` (min 1 s) after the
//     previous one FINISHED — the interval is the time between images and
//     always elapses, independent of the exposure length.
//     Images save to sequences/seq_<ts>/NNNN.<ext> (0 images = continuous).
//
// Build:  make camera_app
// Run:    ./camera_app            (GUI)
//         ./camera_app --smoke    (headless self-test, exits 0 on success)
//         ./camera_app --uishot <file> [--mode 0|1|2]
//                                  (offscreen: saves a PNG screenshot of the
//                                  window + shutter button to <file> /
//                                  <file>.busy.png; --mode switches the mode
//                                  first — 0 photo, 1 interval, 2 video)
//         ./camera_app --sertest  (headless SerWriter self-test: writes and
//                                  validates the .ser ramp samples in samples/
//                                  (sample08/08hsm/14.ser mono + colourtest RGB,
//                                  incl. the push8 RAW8/HSM path); no camera)
//         ./camera_app --frametest (headless FrameView regression test)
//         ./camera_app --capstest  (headless capability-model test: the
//                                  resolution/depth/fps model any ASI body is
//                                  driven from — no camera needed)
//         ./camera_app --colourtest (headless colour test: Bayer->RGB mapping
//                                  for all four patterns at 8 and 16 bits, the
//                                  RGB .ser frame order (interleaved R,G,B,
//                                  pinned byte-for-byte + end to end), the mono output
//                                  regression, the colour thumbnail + clip mask)
//         ./camera_app --wbtest   (headless white-balance model test: the
//                                  Planckian-locus anchors, the monotonic
//                                  temperature signature, the neutral anchor
//                                  at the camera's default WB gains, the
//                                  slider directions, exact (K,tint)<->gain
//                                  round trips, cap clamping, AWB-follow
//                                  quantization; no camera needed)
//         ./camera_app --vtestser 0   with --vtest: record 8-bit H.264 (MP4)
//                                  instead of the default uncompressed .ser
//                                  (diagnostic override of the Bayer pattern a
//                                  colour camera reports, for the rare body
//                                  whose reported pattern does not match its
//                                  data; ignored by mono cameras)
//         ./camera_app --seqtest [--seqexp S --seqinterval S --seqcount N]
//                                  (hidden diagnostic: timed interval
//                                  sequence; prints per-shot completion times)
//         ./camera_app --vtest [--vtestroi WxH --vtestbits 8|10|14
//                        --vtestfps N --vtestdur S]
//                                  (hidden diagnostic: record a .ser clip at
//                                  the given ROI/depth/fps and print the
//                                  achieved rate — frames written, measured +
//                                  EMA fps, camera-side + app-side drops)
//         ./camera_app --prevtest [--prevroi WxH --prevbits 8|14
//                        --prevexp S --prevdur S --prevexp2 S]
//                                  (hidden diagnostic: drive the real GUI into
//                                  the photo-mode slow preview and measure the
//                                  update rate (expected 1/exp); --prevexp2
//                                  switches the exposure at the midpoint)
//         ./camera_app --fpstest
//                                  (hidden regression test: the video-mode fps
//                                  slider keeps its value across a
//                                  video->photo->video round trip, and a real
//                                  format change still re-snaps to the spec max)
//         ./camera_app --camera N
//                                  (which body to open when more than one ASI
//                                  camera is connected: N is a CameraID of a
//                                  connected camera, or its index in the
//                                  enumeration (0-based, the number the
//                                  selector dialog shows). Without it the
//                                  selector dialog is shown BEFORE the main
//                                  window on every launch (Cancel/Esc = exit
//                                  without opening anything; clicking a row
//                                  confirms that camera); with a single
//                                  connected camera it is opened as before)

#include <QApplication>
#include <QFileInfo>
#include <QThread>

#include <gst/gst.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <camera_caps.h>
#include <camera_selector.h>
#include <crash_handler.h>
#include <main_window.h>
#include <shutter_button.h>
#include <style.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "test_suites.h"

int main(int argc, char** argv)
{
    // Crash diagnostics: print the backtrace to stderr before dying (and
    // still produce the core dump). Must be installed before any Qt/SDK init.
    installCrashHandlers();
    setvbuf(stderr, nullptr, _IONBF, 0);   // never lose a diagnostic line on a crash

    bool smoke = false, seqtest = false, sertest = false, uishot = false, frametest = false, vtest = false, prevtest = false, fpstest = false;
    bool capstest = false, colourtest = false, wbtest = false;
    int  bayerOverride = -1;      // --bayer rggb|bggr|grbg|gbrg (colour bodies only)
    QString camArg;               // --camera <CameraID|index> (multi-camera selection)
    QString uishotFile;
    double stExp = 0.5, stInterval = 5.0;
    int stCount = 3, uiMode = -1;   // uiMode: --mode 0|1|2 (photo/interval/video), uishot only
    int vtW = 480, vtH = 320, vtBits = 8, vtFps = 404, vtSer = 1;
    double vtDur = 4.0;
    int pvW = 480, pvH = 320, pvBits = 14; double pvExp = 0.1, pvDur = 6.0, pvExp2 = 0.0;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--smoke") == 0)        smoke = true;
        else if (std::strcmp(argv[i], "--seqtest") == 0) seqtest = true;
        else if (std::strcmp(argv[i], "--sertest") == 0) sertest = true;
        else if (std::strcmp(argv[i], "--frametest") == 0) frametest = true;
        else if (std::strcmp(argv[i], "--capstest") == 0) capstest = true;
        else if (std::strcmp(argv[i], "--colourtest") == 0) colourtest = true;
        else if (std::strcmp(argv[i], "--wbtest") == 0)     wbtest = true;
        else if (std::strcmp(argv[i], "--bayer") == 0 && i + 1 < argc)
        {
            // Escape hatch for a colour body whose reported BayerPattern does
            // not match its data: force the demosaic order. Mono cameras ignore
            // it (the worker only consults it when IsColorCam is set).
            bayerOverride = parseBayerPattern(QString::fromLocal8Bit(argv[++i]));
            if (bayerOverride < 0)
                std::fprintf(stderr, "[bayer] unknown pattern '%s' (rggb|bggr|grbg|gbrg)\n",
                             argv[i]);
        }
        else if (std::strcmp(argv[i], "--vtest") == 0)   vtest = true;
        else if (std::strcmp(argv[i], "--prevtest") == 0) prevtest = true;
        else if (std::strcmp(argv[i], "--fpstest") == 0) fpstest = true;
        else if (std::strcmp(argv[i], "--vtestroi") == 0 && i + 1 < argc)
        {
            // "WxH"
            char* s = argv[++i];
            if (char* x = std::strchr(s, 'x')) { vtW = std::atoi(s); vtH = std::atoi(x + 1); }
        }
        else if (std::strcmp(argv[i], "--vtestbits") == 0 && i + 1 < argc) vtBits = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--vtestfps") == 0 && i + 1 < argc)  vtFps  = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--vtestser") == 0 && i + 1 < argc)  vtSer  = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--vtestdur") == 0 && i + 1 < argc)  vtDur  = std::atof(argv[++i]);
        else if (std::strcmp(argv[i], "--prevroi") == 0 && i + 1 < argc)
        {
            char* s = argv[++i];
            if (char* x = std::strchr(s, 'x')) { pvW = std::atoi(s); pvH = std::atoi(x + 1); }
        }
        else if (std::strcmp(argv[i], "--prevbits") == 0 && i + 1 < argc) pvBits = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--prevexp") == 0 && i + 1 < argc)  pvExp  = std::atof(argv[++i]);
        else if (std::strcmp(argv[i], "--prevdur") == 0 && i + 1 < argc)  pvDur  = std::atof(argv[++i]);
        else if (std::strcmp(argv[i], "--prevexp2") == 0 && i + 1 < argc) pvExp2 = std::atof(argv[++i]);
        else if (std::strcmp(argv[i], "--uishot") == 0 && i + 1 < argc)
        {
            uishot = true;
            uishotFile = QString::fromLocal8Bit(argv[++i]);
        }
        else if (std::strcmp(argv[i], "--camera") == 0 && i + 1 < argc)
        {
            // Which body to open when more than one is connected (CameraID of
            // a connected camera, or its 0-based enumeration index).
            camArg = QString::fromLocal8Bit(argv[++i]);
        }
        else if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc)        uiMode     = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--seqexp") == 0 && i + 1 < argc)      stExp      = std::atof(argv[++i]);
        else if (std::strcmp(argv[i], "--seqinterval") == 0 && i + 1 < argc) stInterval = std::atof(argv[++i]);
        else if (std::strcmp(argv[i], "--seqcount") == 0 && i + 1 < argc)    stCount    = std::atoi(argv[++i]);
    }

    // --sertest / --capstest / --colourtest / --wbtest need neither the camera
    // nor Qt/GStreamer: run them before any of that initialises and exit with
    // their status.
    if (sertest)
        return runSerWriterSelfTest() ? 0 : 1;

    if (capstest)
        return runCameraCapsSelfTest() ? 0 : 1;

    if (colourtest)
        return runColourSelfTest() ? 0 : 1;

    // --wbtest is pure math — no camera, no Qt, no GStreamer.
    if (wbtest)
        return runWhiteBalanceSelfTest() ? 0 : 1;

    // --frametest needs Qt (offscreen) but not the camera or GStreamer.
    if (frametest)
        return runFrameViewSelfTest() ? 0 : 1;

    if (smoke || seqtest || uishot || vtest || prevtest || fpstest)
        qputenv("QT_QPA_PLATFORM", "offscreen"); // never pop a window during self-test
    qputenv("OPENCV_LOG_LEVEL", "ERROR");       // keep console clean

    // Bundled GStreamer plugins: a self-contained package ships the GStreamer
    // runtime + the encoder plugins (appsrc/videoconvert/x264enc/mp4mux) next
    // to the executable. Point GStreamer at a local plugin directory before
    // gst_init() so its registry scan finds them (no system GStreamer needed).
    {
        QString exedir;
#ifdef _WIN32
        wchar_t modpath[MAX_PATH];
        DWORD n = GetModuleFileNameW(nullptr, modpath, MAX_PATH);
        if (n > 0 && n < MAX_PATH)
            exedir = QFileInfo(QString::fromWCharArray(modpath, n)).absolutePath();
#else
        exedir = QFileInfo(QString::fromLocal8Bit(argv[0])).absolutePath();
#endif
        const QString plugins = exedir + "/gstreamer-1.0";
        if (QFileInfo(plugins).isDir())
            qputenv("GST_PLUGIN_PATH", plugins.toUtf8());
    }

    gst_init(nullptr, nullptr);

    QApplication app(argc, argv);
    app.setStyleSheet(kStyleSheet);

    // ---- which camera to open (see camera_selector.h) ---------------------
    // Decided BEFORE the main window appears. The enumeration only asks the
    // SDK (no camera is opened), so it is safe to do here. With two or more
    // connected cameras an interactive launch ALWAYS asks: the selector
    // dialog is shown before the main window on every launch (Cancel/Esc
    // exits without opening anything; clicking a row confirms that camera).
    // The choice is deliberately NOT remembered — the user decides on every
    // launch. Headless self-tests never show the dialog (no user is there):
    // they honour --camera and otherwise open the first connected camera,
    // exactly as before.
    const bool headless = smoke || seqtest || uishot || vtest || prevtest || fpstest;
    int cameraId = -1;                          // -1 = auto: first connected camera
    const auto cameras = enumerateCameras();
    if (!camArg.isEmpty())
    {
        cameraId = resolveCameraValue(cameras, camArg);
        if (cameraId < 0)
        {
            std::fprintf(stderr, "[camera] --camera %s: %s\n",
                         camArg.toLocal8Bit().constData(),
                         cameras.empty() ? "no cameras connected yet"
                                         : "matches none of the connected cameras:");
            for (const auto& c : cameras)
                std::fprintf(stderr, "    %d: %s   (%s, id %d)\n", c.index,
                             c.name.toLocal8Bit().constData(),
                             c.detail.toLocal8Bit().constData(), c.id);
            return 1;
        }
        for (const auto& c : cameras)
            if (c.id == cameraId)
                std::fprintf(stderr, "[camera] --camera %s -> %s (id %d, index %d)\n",
                             camArg.toLocal8Bit().constData(),
                             c.name.toLocal8Bit().constData(), c.id, c.index);
    }
    else if (!headless)
    {
        if (cameras.size() > 1)
        {
            // More than one body connected: ask on every launch (see the
            // note above).
            cameraId = showCameraSelector(cameras);
            if (cameraId < 0)
            {
                std::fprintf(stderr, "[camera] no camera selected - exiting\n");
                return 0;
            }
            for (const auto& c : cameras)
                if (c.id == cameraId)
                    std::fprintf(stderr, "[camera] selected %s (id %d, index %d)\n",
                                 c.name.toLocal8Bit().constData(), c.id, c.index);
        }
        else if (cameras.size() == 1)
        {
            cameraId = cameras[0].id;           // the only camera: no dialog, as before
        }
        // none connected yet: proceed and let the worker's open loop wait for
        // a camera to appear (the usual "waiting for camera" state).
    }

    MainWindow w(smoke, seqtest, stExp, stInterval, stCount, vtest, vtW, vtH, vtBits, vtFps, vtDur,
                 prevtest, pvW, pvH, pvBits, pvExp, pvDur, pvExp2, fpstest, bayerOverride,
                 vtSer != 0, cameraId);
    w.show();

    if (uishot)
    {
        // UI self-check: let the first layout pass and the "waiting for
        // camera" state settle, then save a screenshot of the window and of
        // the shutter button (idle + busy/dimmed state) to PNG files.
        // --mode N switches the mode before the shot (0 photo, 1 interval, 2 video).
        if (uiMode >= 0) w.setUiMode(uiMode);
        for (int i = 0; i < 40; ++i) { app.processEvents(); QThread::msleep(50); }
        // Explicit PNG: save() with a format-less filename (no extension)
        // fails on the Windows Qt build, where the format is only inferred
        // from the extension.
        const bool winOk  = w.grab().save(uishotFile, "PNG");
        bool btnOk = false;
        if (auto* btn = w.findChild<ShutterButton*>("photoBtn"))
        {
            const QString busyFile = uishotFile + ".busy.png";
            btn->setBusy(true);
            app.processEvents();
            btnOk = btn->grab().save(busyFile);
        }
        std::printf("UISHOT %s win=%d shutter=%d\n",
                    uishotFile.toLocal8Bit().constData(), (int)winOk, (int)btnOk);
        return (winOk && btnOk) ? 0 : 1;
    }

    return app.exec();
}
