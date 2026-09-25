# Architecture: modules and threading (AGENTS.md §6 intro, §6.1)

## 6. Architecture

The app is one module per file (`include/<name>.h` + `src/<name>.cpp`):

1. **`GstVideoEncoder`** (`gst_video_encoder`) — H.264 → MP4 (8-bit) via a custom GStreamer pipeline (§6.5).
2. **`SerWriter`** (`ser_writer`) — uncompressed `.ser` (LUCAM-RECORDER v3) writer for
   8/14-bit video: a 178-byte header + 1-byte (8-bit) or 2-byte (9–16-bit)
   LE frames, plain `stdio` (no GStreamer) (§6.5). It writes **MONO (1 plane)
   or RGB (3 channels, PER-PIXEL INTERLEAVED R,G,B — the ColorID 100 layout
   Siril/Ser-Player decode, *not* full planes)** depending on the `colorId`
   passed to `open()`:
   `push()/push8()` take one channel and are rejected on an RGB file,
   `pushRgb()/pushRgb8()` take the 3 interleaved channels and are rejected on
   a mono one
   (the wrong call cannot silently produce a corrupt file — it fails loudly). The writer is generic over
   the depth (9–16 all store 2 B/px left-justified; depth 10 is no longer
   offered by the UI but still reachable via `--vtest`). `validateSerFile()`
   lives here too.
3. **`CameraWorker : QThread`** (`camera_worker`) — owns the camera and does all capture/encoding
   on its own thread (§6.2).
4. **`FrameView`** (`frame_view`) — paints the latest frame, scaled to fit (§6.4).
5. **`HistogramWidget`** (`histogram_widget`) — 128-bar histogram (§6.3).
6. **`ModeToggle`** (`mode_toggle`) — the 3-way PHOTO / INTERVAL / VIDEO switch.
7. **`ShutterButton` / `RecordButton` / `SequenceButton`** (`shutter_button`,
   `record_button`, `sequence_button`) — the painted action buttons.
8. **`MainWindow : QMainWindow`** (`main_window`) — the UI, wiring, and the 33 ms display timer;
   its constructor is split into `setupUi()` (layout + widgets),
   `setupConnections()` (signal/slot wiring) and `setupInitialState()`
   (timer + initial exposure/mode), called in that order, then `worker_.start()`.
9. **`CameraCaps`** (`camera_caps`) — what one connected camera can do, probed
   once per open: name, sensor size, `isColor` + Bayer pattern, native depth,
   which of RAW8/RAW16/RGB24/Y8 it streams, gain/exposure limits, whether it has
   HighSpeedMode — plus everything the UI needs derived from that:
   `depths()`/`deepDepth()`/`supportsDepth()`, `roiCandidates()` (the resolution
   list — the 1:1 base square first, then full frame + crops largest-first) and
   `baseSquare()` (that first entry), `maxFps()`, `describe()`/`kindText()`
   (§6.2, §7).
10. **`colour`** (`colour`) — the Bayer→RGB path: `bayerToBgrCode()` (the SDK
    pattern ↔ OpenCV code cross-map), `demosaicBayerToBgr[8]()` for the saved
    files, `colourDisplayFrame()` (quad-demosaic RGB thumbnail + clip mask) for
    the preview (**RGB byte order, byte 0 = R** — the order `Format_RGB888`
    and `paintClipRed()` expect; the file paths stay BGR, and mixing the two
    shows an R/B-swapped preview with correct files — see `preview-colour.md`
    §6.4), `bayerToRgbCode()`/`demosaicBayerToRgb()` for the interleaved-RGB
    `.ser`, and `paintClipRed()`
    (shared with the mono path) (§6.3, §6.5–§6.7).
11. Supporting modules: `main` (entry point, flag dispatch, uishot),
   `display_frame` (worker-side thumbnail downscale `buildDisplayFrame`),
   `exposure` (slider↔seconds mapping + formatting), `fps_spec` (ZWO fps table),
   `depth_code` (combo item-data encoding), `constants`, `util`
   (`timestamp()`, `isDebugRecording()`/CAMDBG), `crash_handler`, `style`
   (stylesheet). Tests live in `tests/` (`ser_writer_test`, `frame_view_test`).

### 6.1 Threading model

- `CameraWorker` runs the capture loop on a worker `QThread`. The GUI thread
  only *reads* the latest frame / histogram / telemetry and *writes* config.
- **Config** is a `Config` struct guarded by `cfgMtx_`. GUI-thread setters
  (`setMode`, `setExposure`, `setFps`, `setRoi`, `setBitDepth`, `setSerMode`,
  `setRecording`, `requestPhoto`, `requestSequence(expS, intervalS, count)`,
  `requestQuit`) just update it; the worker copies it each loop
  iteration (`stopSequence()` instead just flips the `seqStop_` atomic).
- **Frame data** (`latest_` = display thumbnail, `latestCh_`, `hist_`,
  `clipCount_`) is guarded by `frameMtx_`. **Telemetry** is guarded by
  `teleMtx_`.
- **Preview builder thread** (`CameraWorker::previewBuilderLoop`, a plain
  `std::thread` owned by the worker, §6.2): owns the app's **30 Hz preview
  clock**. It builds the thumbnail + histogram from the drain loop's published
  frame at most every 33 ms — in every mode, independent of the record fps,
  and never faster than 30 fps. It runs *off* the drain loop on purpose: the
  thumbnail build (up to ~8 ms at 6.4 MP) must never stall the frame grab, or
  the camera's USB buffer overflows. The worker's destructor joins
  it (started only after a successful camera open; `previewQuit_` gates both
  the start and the stop).
- The GUI reads via getters (`getLatestFrame(buf,w,h,channels)`, `getHistogram`,
  `getClipCount`, `getTelemetry`). Results cross threads via Qt **signals**
  (`photoResult`, `recordingStarted`, `recordingStopped`, `cameraError`,
  `cameraReconnected`, `roiListReady`, `cameraReady(description)` (fired after
  EVERY successful open, so a reconnect with a different body re-runs the GUI's
  `applyCameraCaps()`), `gainReady(min, max, current)`, and the
  interval-sequence signals
  `sequenceStarted`, `sequenceExposing(index, total)`,
  `sequenceShot(index, total, path)`, `sequenceWait(remainingS)`, and
  `sequenceDone(total, earlyStop)`).

