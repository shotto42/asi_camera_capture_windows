# The capture loop and gain (AGENTS.md §6.2, §6.8)

### 6.2 The capture loop (`CameraWorker::run`)

Before the loop, `openCamera()` finds/opens/inits the camera, creates the output
dirs, calls `probeCameraCaps()` (`camera_caps.cpp`; a camera that streams
nothing this app can read — no RAW8 and no RAW16 — is a **hard error**, since
every later step depends on the answer) and caches the parts the hot loop needs
in atomics (`isColor_`, `bayer_`, `hasRaw8_/hasRaw16_/hasHsm_`), sizes three raw
buffers to the **probed** sensor max, calls `probeRois()` (which emits
`cameraReady(cc.describe())` alongside `roiListReady`), queries the gain caps
(seeding `cfg_.gain` and emitting `gainReady` on the first open — a reconnect
re-applies the user's gain via the loop, and also re-emits `gainReady` so the
slider range follows a swapped body), and resets `lastExpApplied_` /
`lastGainApplied_` / `slowPreview_` so the loop re-applies the current state to
the fresh handle. If the camera is not (yet) reachable, `run()` **retries every
second** (emitting a one-time `cameraError` with the USB hint) instead of
dead-ending — the USB node can appear late or re-enumerate at any time on this
machine.

**Camera-loss handling.** The camera's USB device can re-enumerate mid-session,
which **kills the SDK handle**: every `ASI*` call on the dead handle risks
segfaulting the process (this used to take the app down from `doPhoto`). The
worker therefore treats `ASI_ERROR_CAMERA_REMOVED` as a first-class condition at
*every* SDK boundary (preview start/stop, ROI switch, exposure/gain set, the
slow-preview exposure/status/data sequence, the fast grab, and all of
`doPhoto`/`doSequence` setup + shot loop):

- `handleCameraLost(why)` drops the handle **without calling the SDK on it**
  (`cam_ = -1`, `videoActive_ = false`), finalizes any active recording, marks
  the telemetry, emits `cameraError("Camera lost - reconnecting…")`, and then
  re-tries `openCamera()` every second (via `nap()`, which notices
  `cfg_.quitting` within 50 ms) until it succeeds — then emits
  `cameraReconnected()` (GUI drops the red banner) and the loop resumes with
  fresh handles and re-applied settings.
- `applyRoi` returns `bool`; on a lost camera it also sets `cam_ = -1` so its
  callers (loop / `doPhoto` / `doSequence`) can distinguish a loss (→
  `handleCameraLost`) from a plain failure (→ back off 500 ms and retry).
- Invariant: every worker exit path still emits its terminal signal
  (`photoResult` / `sequenceDone`) *before* handing over to
  `handleCameraLost`, because the GUI flips its buttons optimistically and
  depends on those signals to reset.

**Display-buffer invariant (`latest_` / `latestCh_`).** The 33 ms display timer
reads `(latest_, latestW_, latestH_, latestCh_)` under `frameMtx_` and builds a
`QImage` whose format/stride follow `latestCh_` (1 → `Grayscale8`, stride `w`;
3 → `RGB888`, stride `3w`), then `QImage::copy()` deep-copies it. **Every writer
must keep `latestCh_` consistent with the data it puts in `latest_`**: the
preview downscale writes grayscale or red-marked RGB and sets it accordingly;
`doPhoto`/`doSequence` write 8-bit *grayscale* refreshes and must set
`latestCh_ = 1`. A stale `latestCh_ == 3` over a 1-channel buffer makes
`QImage::copy()` read ~3× past the end of the buffer and segfault the GUI
thread — this was the real cause of the "crash on photo button" (the photo
itself had already saved fine). `FrameView::setFrame` also takes the actual
`dataSize` and refuses/degrades any (data, channels) combination that would
build an out-of-bounds image (logged as `[frame] ...`), so a future invariant
break degrades instead of crashing. `./camera_app --frametest` reproduces the
crash scenario headlessly and guards it. On any fatal signal the app prints a
`CAMERA_APP CRASH REPORT` (signal, faulting address, native backtrace with
module+offset) to stderr before the core dump, so the crashing module is
identifiable without gdb.

Per iteration:

1. Copy `cfg_` (under `cfgMtx_`); break if `quitting`.
2. **Photo requested?** → `doPhoto(expS)` and `continue` (§6.6).
3. **Recording start/stop** → `startRecording` / `stopRecording` (§6.5).
4. **Interval sequence requested?** → `doSequence()` and `continue` (§6.7).
   Handled *after* recording so a pending recording-stop is never deferred
   behind a sequence that pauses the live loop.
5. **Preview mode decision**: `wantSlow = (mode==Photo) && (exposureS > 1/fps)`.
   On a transition to *fast*, restart the video capture cleanly (a photo may
   have left it stopped or at a long exposure). On a transition to *slow* the
   stream **stays running** — the slow preview uses the same video path.
6. **Apply ROI + bit depth** (only when *not* recording — an ROI/format change
   mid-recording would corrupt the file). If the requested `w/h/bpp` differs
   from current, call `applyRoi(restartVideo=true)` — the live preview (fast or
   slow) always runs on the video stream, so a format change leaves it running.
7. **Exposure** → `ASISetControlValue(ASI_EXPOSURE, us)` if changed. Fast
   preview: frame-period-safe (`min(exposureS, 1/fps)`). Slow preview: the
   **FULL** exposure — the camera then stretches the frame period and delivers
   preview frames at 1/exposure.
8. **Grab one frame** into a raw slot that is neither published nor being read
   by the preview builder (`raw_[activeIdx_]`; both
   paths use `ASIGetVideoData`): fast → a **fixed short** wait (10 ms) keeps
   the loop spinning fast so the camera's USB frame buffer is drained
   promptly; a long wait lets the buffer fill and drop frames. The wait is
   deliberately **not tied to `cfg.fps`** (the old `max(10, framePeriodMs/4)`
   was a 250 ms poll at a 1 fps record setting — the preview clock lives on
   the builder thread, below, so nothing in this loop paces to the record
   fps). Slow → a **short** wait
   (`waitMs = min(50, max(10, exposureMs/4))`) keeps the loop responsive
   (it still sees a quit/photo/sequence request between grabs) and the
   polling granularity (≪ exposure) doesn't cap the delivered rate; for a
   long exposure the frame simply isn't ready yet and the grab retries.
9. **Publish to the preview builder**: store `publishedIdx_ = grabSlot` and its
   capture geometry
   under `frameMtx_`, then `frameSeq_.fetch_add(1, release)` LAST (the
   release orders the SDK's buffer write; the builder loads with acquire),
   and choose a free slot for the next grab. The drain loop then
   goes to the next grab as fast as the camera delivers — **independent of
   `cfg.fps`** (the record fps only paces the writer, below).
10. **Pace — gates ONLY the writer**: while recording, a frame that arrives
    earlier than `nextAllowed_` is dropped (`droppedPaced_` — "the frames
    the file didn't get") and not pushed; otherwise `nextAllowed_ += 1/cfg.fps`
    (with a resync after gaps > 0.5 s, e.g. after a long photo exposure).
    The preview builder already saw every frame at 30 Hz no matter the
    record fps. fps EMA = the **accepted (written)** rate while recording,
    and the **live stream delivery** rate otherwise (a pre-pacing "arrival
    rate" EMA would spike to the loop's own speed (~170) in bursts; the
    accepted rate is capped by the slider target and the real sensor rate —
    #38). The status line's "Frame rate" therefore shows the live rate when
    not recording (about 60 at full-res 8-bit on the ASI178MC) and the written rate while
    recording.
11. **Writer push** (only when recording and the pace gate passed) — one call,
    `pushToWriter(raw_[grabSlot], w_, h_, bpp_)`, which knows both axes: mono →
    the readout verbatim (`push`/`push8`); colour + `.ser` → `demosaicBayerToRgb()`
    straight into `serRgbBuf_` (interleaved R,G,B — the SER ColorID 100 layout)
    and `pushRgb`/`pushRgb8`; colour +
    H.264 → `demosaicBayerToBgr8()` into `bgr8Buf_` (mono + H.264 grey-triples
    it) and `encoder_.push`. Demosaicing happens **here, on the writer side only
    when a frame is actually written**, never on the grab path (§6.5, §9 — the
    measured cost is 2.0 ms/frame at 6.4 MP 8-bit, 4.2 ms at 16-bit).
12. Update telemetry every ~0.5 s (includes the builder's `previewBuilds`).
    `CAMDBG=1` prints a per-50-frames timing line
    (`[perf] grab/push/period/emaFps`) and the builder its own `[perf-prev]`
    line (below).

**Preview builder thread (`previewBuilderLoop`).** The app's 30 fps preview
clock, on its own thread (started by `run()` after a successful open;
stopped via `previewQuit_` at the loop's cleanup; joined in the worker's
destructor). Each iteration: load `frameSeq_` (acquire); if unchanged, nap
**2 ms** and re-check (a long nap here skips the 30 Hz slot — measured: a
50 ms nap makes the inter-build gap 66 ms bimodal and the rate ~20 Hz);
read the published geometry under `frameMtx_`; if less than
**33 ms** since the last build (and the geometry didn't change), sleep
**precisely until the next slot** (`lastBuild + 33 ms`) — a 3 ms poll loop
there loses ~15 ms/cycle to wakeup quantization and drifts the rate to
~20 Hz (measured); `lastBuild` is the build START, so the slots stay
anchored at 30 Hz even when a build runs long. Then it claims the latest
published raw slot under `frameMtx_` and reads it without holding that lock.
The worker excludes the claimed slot from SDK writes until the thumbnail and
histogram finish. Three slots guarantee a free write target even if a preview
build spans a camera frame period. A reconnect waits for the claim before
resizing the buffers; the geometry travels with its published frame.

OpenCV is limited to one internal worker via `cv::setNumThreads(1)` before the
preview thread starts. On the ASI178MC host, OpenCV's default parallel resize
caused repeated ~650 ms gaps in the camera drain despite a ~10 ms thumbnail
build. With one OpenCV worker, a 30.5 s full-frame RAW8 preview delivered
1827 frames (59.90 fps) while the GUI continued to build at 30 Hz.

The branch splits by `isColor_`:

- **Colour body → `colourDisplayFrame()`** (RGB thumbnail, never a grey mosaic
  average, which would be a checkerboard of single-channel samples). When the
  frame is at least 2× the thumbnail it takes the **quad path**: every 2×2
  mosaic block *is* one RGB pixel (R, G, G, B), so one pass over the raw buffer
  produces a half-size RGB image plus a per-block clip mask — cheaper than a
  full-resolution demosaic, with no `cv::demosaicing` call; otherwise
  it demosaices fully and resizes. Clipped blocks are then painted red with the
  shared `paintClipRed()` and published with `latestCh_ = 3`.
- **Mono body → the existing path** (unchanged): for even-dimensioned frames ≥ 2×
  the thumbnail (the 6.4 MP corner) a one-pass **2×2 box downsample straight from
  the raw buffer** (`boxDownsample2x`: one 6.4/12.8 MB read, ~5.5–8 ms at 6.4 MP;
  16-bit sources shifted `>> 8` per pixel and averaged in the 8-bit domain,
  with a per-box-pixel clip mask) followed by a small `cv::resize`
  INTER_LINEAR to the exact thumbnail size and a nearest-neighbour mask
  pass; smaller frames use `buildDisplayFrame` (INTER_AREA + corner clip
  flags, §6.4). 1-channel gray, or 3-channel RGB with clipped blocks marked
  red (`latestCh_` tracks it; the `latest_`/`latestCh_` invariant above is
  unchanged). Measured quality vs the old copy+INTER_AREA path: mean abs
  diff 0.04–0.95 grey, identical clip masks on realistic scenes. The old
  path (6.4 MB copy + INTER_AREA + full-frame histogram ≈ 13–14 ms at 6.4 MP)
  measurably stole ~25 fps of write capacity from the drain loop at 6.4 MP
  60 fps; the box path removed that cost (interleaved A/B: builder on ≈ off).
- **histogram**: `computeHistogram` (§6.3, stride-sampled 2×2) on the raw frame
  so bit depth and clip counts retain their source meaning. For a colour body
  it samples every
  4th 2×2 block with all four mosaic positions in it (same cost as before, and
  the four colours are counted like the four channels of a pixel); deriving it
  from the RGB thumbnail instead would report the thumbnail's pixel count, not
  the frame's — which is also why `clipCount_` is scaled back to the frame
  (§6.3, and the bug it fixes: the clip % read ~4× low next to the red marking).

`CAMDBG=1` prints `[perf-prev]` every 50 builds (absolute time,
ROI/geometry, downscale/hist cost, last inter-build gap). During a
photo/sequence the camera is dedicated and nothing is published: the builder
naps (2 ms slices) and the preview holds the last frame; `doPhoto`/
`doSequence` refresh `latest_` themselves after each shot. Physics limits
still apply — the preview can only show what the camera delivered (slow
preview = 1/exposure; full-res 14-bit ≈ 28 fps < 30, so the preview runs at
the delivery rate there).

**`applyRoi(w,h,dbpp,restartVideo=true)`** stops video, calls `ASISetROIFormat`
with `ASI_IMG_RAW16` (the deep readout) or `ASI_IMG_RAW8` (8-bit H.264 **and**
8-bit `.ser` — the 10-bit HSM readout), sets `ASI_HIGH_SPEED_MODE=1` for RAW8 / 0
for RAW16 — but **only if the camera has HighSpeedMode at all** (`hasHsm_` from
the caps; a body that rejects the control with `ASI_ERROR_INVALID_CONTROL_TYPE`
is remembered as HSM-less instead of failing every format switch), and for a
body without RAW8 it falls back to `Y8`/`RAW16` (the byte width the buffer needs
is computed the same way in the loop: `dbpp = (cfg.bitDepth == 8 || !hasRaw16_)
? 1 : 2`), updates `w_/h_/bpp_` (2 bytes/px for 14-bit `.ser`, 1 for 8-bit) under
`frameMtx_`, and restarts video capture. The three raw buffers are sized to
the sensor max (3096×2080×2 on an ASI178); a reconnect with a different body
waits for the preview reader before resizing. A smaller ROI uses the front of
each buffer. With `restartVideo=false` the camera
is left **stopped** so the caller can drive long-exposure snaps directly — a
redundant start/stop cycle before the first long exposure intermittently broke
that first shot (§6.7, §9).

**`probeRois()`** (called on every open) is **generated from the caps, not a
hard-coded list** — `CameraCaps::roiCandidates()` builds it from the sensor the
camera reports: **the 1:1 base square FIRST** (`baseSquare()` — a true square
whose side is the sensor's y-resolution, the largest 1:1 window that fits on a
landscape body; planetary framing, the user-requested default the selector
opens on), then the full frame; 1/b-scale centre crops (b = 2…6, the list's
historical "bins" — the SDK bin control stays at 1); and a width ladder
(3840…480, skipping widths that don't fit) whose **heights are derived from
the sensor's own aspect ratio** — deduplicated, with the full frame + crops
sorted by decreasing area behind the base square. Since
2026-09-23 every entry keeps the sensor shape (worst deviation ~0.5%, from
aligning the height to %4) except that square; the old fixed 16:9/4:3 windows
each sliced away a different wedge of the frame and were dropped. On a 4:3 or
16:9 body the width ladder lands on the usual video windows exactly (a
1280×960 body gets 1024×768, 800×600, 640×480, 480×360). Every candidate is
aligned (width to 8, height to 4) and kept even, because a colour sensor's
Bayer phase must not shift and the ASI178MC outright **rejects** widths that
are not divisible by 8 (1548/774/618/516 are refused — measured, §4), which is
exactly what the old ASI178-only list assumed away. Each candidate is then
verified against the camera (`ASISetROIFormat` + readback in RAW8, Y8 or RAW16
— whichever the body has), so the list can only contain sizes this body
accepts; the base square keeps the first seat when the camera accepts it. It
emits `roiListReady` ("WxH" strings) **and**
`cameraReady(cc.describe())`; the default (**the full frame** — the highest
resolution, the shallow depth) is applied live before the list is published,
and the GUI selects that same largest-area entry (the 1:1 base square stays
the list's first entry; the combo's default selection is the highest
resolution, not index 0).
`--capstest` checks the generator's invariants (bounds, ordering — base
square first, the rest strictly decreasing — no duplicates, even sizes,
**every crop within 1% of the sensor's aspect ratio with the 1:1 base as the
sole exception**) for cameras that are bigger, smaller, mono, colour, or lack
a 2-byte readout.

**Why ROI is not free-form:** the SDK's `ASISetROIFormat(cam, w, h, bin, type)`
takes **no origin** — the crop is always centred on the sensor, only the size
is the user's choice. Widths must be multiples of 8 (measured), heights even
(the app uses %4 for safety on other bodies), and every offered size is
additionally re-verified against the attached camera, so the combo can never
list a size the hardware refuses.


---

### 6.8 Gain control (`ASI_GAIN`)

A **soft control** (no capture restart) that scales the sensor's output. The
value is in **0.1 dB units** and its range is **probed per camera**
(`CameraCaps::gainMin/gainMax`): ASI178MM `0..400` = 0.0..40.0 dB, ASI178MC
`0..510` = 0.0..51.0 dB (defaults differ too: 210 on the MC). Display is
`value/10.0` dB (`fmtGain`, e.g. `120` → "12.0 dB").

- **Set/read** use the `ASI_GAIN` control *type*: `ASISetControlValue(cam,
  ASI_GAIN, v, ASI_FALSE)` / `ASIGetControlValue(cam, ASI_GAIN, &v, &auto)`.
- **Caps, and the two cameras disagree here:** `ASIGetControlCaps` takes the
  control's *list index*, **not** the type (see the SDK doc). On the **ASI178MC**
  it works and reports gain 0..510 (and exposure 32 µs..2000 s, BandWidth
  40..100, HighSpeedMode 0..1) — `probeCameraCaps()` walks the indices and takes
  the range from the entry whose `ControlType` matches. On the **ASI178MM** it
  returns `ASI_ERROR_INVALID_CONTROL_TYPE` for every index, so `capsReadable`
  stays false and `probeCameraCaps()` falls back to the documented ASI178 range
  (0..400) rather than to a made-up one; the exposure limits keep their probed
  values either way (`ASIGetControlValue` on the type always works and is used to
  seed `cfg_.gain`, so startup doesn't jump the gain). `--smoke` drives the
  slider to `min(300, gainMax)` and asserts it came back — on the MC that is
  300, on a hypothetical narrower body whatever that body's ceiling is.
- **Apply:** the run loop keeps `lastGainApplied_` and applies the control only
  on change (`cfg.gain != lastGainApplied_`), then records `tele_.gainApplied`.
  A change takes effect on the **next** frame (a soft control); during an
  in-progress long exposure it applies to the following shot.
- **All modes:** the gain persists on the camera, so it applies to photo, video,
  and interval sequences alike (a sequence uses whatever gain is set when it
  runs).

### 6.9 White balance (`ASI_WB_R` / `ASI_WB_B`)

The other pair of soft controls — **colour bodies only**. `openCamera` sets
`hasWb_ = caps.isColor && caps.wbControls()` and logs the probed pair once
(`[wb] R 1..99 def 70 (now …, auto yes) · B …` — the numbers as measured on
the ASI178MC, 2026-09-23). A mono body gets `hasWb_ = false` and **nothing in
the loop ever touches the controls** — mono data stays exactly what the sensor
sent, and the GUI's WB row is never shown (§7).

- **Two modes share one code path** (`applyWhiteBalanceNow`), using the
  camera's own semantics: with **auto on** the controls are written *through
  the SDK auto flag* (`ASISetControlValue(ASI_WB_R, v, ASI_TRUE)` — the ZWO
  demo pattern; a control whose caps report no auto support keeps its manual
  flag and is written at its current value); with **auto off** both controls
  are written explicitly, clamped to the body's caps. A `WB_*` control that
  answers `INVALID_CONTROL_TYPE` flips `hasWb_ = false` mid-session: the
  controls are left alone from then on and the UI row disappears on the next
  `cameraReady`. Measured with `./wb_probe` (2026-09-23): the auto flag is
  camera-**GLOBAL** — setting it on `WB_R` alone makes *both* controls read back
  `auto=1` — and the ZWO demo pattern (flag on `WB_R`, `WB_B` manual) converges
  to the same pair as flagging both, so the per-control distinction is a caps
  courtesy, not two behaviours.
- **Apply discipline mirrors the gain** (§6.8): the loop keeps
  `lastWbAuto_`/`lastWbR_`/`lastWbB_` and re-applies only on a *mode* change
  (AWB ↔ manual) or a *value* change. Leaving auto mode deliberately resets
  the manual trackers (`=-1`) so re-entering manual always re-writes.
- **ROI/format changes reset the camera's WB state**: `applyRoi` invalidates
  the three WB trackers so the next loop iteration re-asserts the current
  setting, and `doPhoto`/`doSequence` — which switch ROI/format *inside* the
  shot, after their own `applyRoi` — re-apply WB explicitly (the same reason
  they re-assert the gain). Without this a still capture silently resets the
  balance for every preview frame after it — found live: the smoke test's
  AWB auto-flag re-check raced a still's format switch, read `auto=0`, and
  the check became a bounded poll to tolerate the reset window.
- **Telemetry:** the 500 ms `updateTelemetry` tick does not query WB controls;
  those extra SDK calls are unnecessary during capture. The regular temperature
  and dropped-frame telemetry remains active.
  `tele_.wbR/wbB` hold the pair sampled at open or successfully applied
  manually. `tele_.wbValid` is false on mono bodies, WB-less colour bodies,
  and after camera loss. AWB telemetry never moves the manual GUI sliders.
- **First-open seeding:** when the WB controls exist and the app has never
  written them, `openCamera` seeds `cfg_.wbR/wbB` from the live readback, so
  the initial raw config reflects the values the camera actually holds. The
  GUI owns a separate Temperature/Tint pair, initialized to 6500 K / Tint 0.
  Switching AWB off immediately sends that retained manual pair. The first
  slider movement applies immediately; subsequent movements are paced at
  80 ms, and release applies the final value. Failed SDK writes wait one
  second before retrying instead of blocking every frame of the stream.

**What the SDK does NOT offer (measured against v1.41):** there is no
white-balance *mode*, no colour-temperature control and no "what temperature did
AWB estimate" call — `ASI_CONTROL_TYPE` contains exactly `ASI_WB_R` and
`ASI_WB_B` (plus unrelated controls), and the only automatic function in the
whole API is the `isAuto` argument of `ASISetControlValue` (advertised per
control by `IsAutoSupported`), which is what ZWO's own demo drives. Every Kelvin
number in this app is therefore a *model on top of two gain registers*; what the
probe below adds is that the model's origin is measured.

**Measured white-balance facts (ASI178MC, `tests/wb_probe.cpp`, 2026-09-23):**

| fact | measurement |
|---|---|
| gain law | the delivered Bayer data IS white-balanced in the camera, and each control is a **linear multiplier** — `./wb_probe --law` sweeps one channel with the other pinned and gets d ln(R/G)/d ln(WB_R) = **1.016** and d ln(B/G)/d ln(WB_B) = **1.009** (six intervals each, 0.99..1.05), so the model's multiplicative arithmetic holds to ~2% |
| factory defaults | R 70 / B 90 — and a daylight-lit white target sitting on those defaults measures R/G 0.76, B/G 1.36: **not neutral, visibly blue** |
| neutral pair | **R 93 / B 65** (gray-world on the target: 92.0/66.0; the camera's own AWB converged to 93/64 with the target measuring R/G 1.02, B/G 0.96 — two independent estimators, 1.1% apart on R and 3.1% on B) |
| AWB quality | good, and it can saturate: under bluer light the camera pins `WB_R` at its 99 ceiling, which its reported gains make obvious |
| WB authority | tint 0 holds only across roughly **4500..7300 K** (93 of a 99 `WB_R` ceiling, 65 of a 99 `WB_B` one) |
| tint authority | tint scales both modeled gains; `wbToGains` clamps each channel independently at the body's cap. A capped channel stays pinned while the free channel continues to respond to Tint. |
| tint window | values the body can deliver without a capped channel (`wbTintRangeForK`): **−100..+15 at 6500 K, −100..−32 at 10000 K, none below ~3500 K**. This window never changes the GUI slider range or label. |

So `WbCal` carries that **neutral pair**, `wbMeasuredNeutral()` supplies it for
bodies `wb_probe` has measured (and refuses when their caps no longer match), and
an unmeasured body falls back to its factory defaults. The GUI's Kelvin/Tint sliders are
*mappings* of the raw gains (§7, `white_balance.h`); the worker only ever handles
raw control values and the SDK auto flag. The GUI's Kelvin/Tint numbers are the SET balance — **Colour temperature and Tint are independent in the sliders AND in the values shown**
(2026-09-23, two user reports): `updateWbLabels` displays exactly what is set (no read-back, no
asterisk), the Tint keeps its full −100..+100 range, and the Temperature never clamps, moves, or
disables it. Where the body is out of the authority above, the GAINS the worker pushes clamp PER
CHANNEL (`wbToGains`): the overflowing channel sits at its cap and the free one keeps its
temperature-correct value, so the temperature axis stays on blue↔yellow-orange in the image
(the old ratio-preserving pair-scale instead dragged the free channel below neutral and a
3000 K light rendered green — user-reported, fixed the same day). Neither
manual slider has a tooltip; headroom never changes the displayed numbers (§7, §9).
