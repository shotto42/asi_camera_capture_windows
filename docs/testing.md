# Smoke test and development verification (AGENTS.md §8, §10)

## 8. The smoke test

`./camera_app --smoke` runs headless (`QT_QPA_PLATFORM=offscreen`) and exercises
the whole pipeline on a fixed timeline. **It is camera-adaptive**: every size,
depth, gain ceiling and expected channel count comes from the probed camera (so
the same command is the regression test for a mono *and* a colour body), and it
**asserts the channel count of everything it writes**.

- t=1000 ms: wait for the probe (it bails out early if the camera has not
  answered `cameraReady` yet), pick the **deep depth** via
  `selectDepthEntry(deepDepth, false)`, then `pickSmokeRoi()` — the largest
  probed ROI ≤ 2.6 MP (on the ASI178MC: `SMOKE ROI 1920x1080`) — and set the gain
  slider to `min(300, gainMax)`. Driving the depth through the **GUI combo**
  keeps combo and worker in sync, so the deep choice survives the mode switch.
- t=1500 ms: push the gain slider (drives the GUI→worker gain path).
- t=1600–2900 ms (colour bodies only — silently skipped on a mono one): the
  **white-balance round trip**. Uncheck AWB through the real checkbox (t=1600),
  wait for the worker's fresh camera-gain handoff, and move both handles through
  their drag/release path (t=1750). Set Tint to +60 and Temperature to 4000 K;
  at t=2300 the cached applied gains must match the model and both sliders
  must remain enabled and independent (`SMOKE wb handles ... -> ok`,
  `SMOKE wb values independent ... -> ok`). At t=2400 AWB is re-checked, and
  t=2850 asserts the worker successfully applied auto mode, so the saved shots
  below run in the default state. t=2900 sits deliberately **before** the
  stills: a still switches ROI/format inside the shot, which transiently
  resets the camera's WB state until the loop re-applies it (an earlier
  placement raced that window and read `auto=0`).
- t=3000 ms: deep photo; t=4000 ms: switch to 8-bit; t=5000 ms: shallow photo
  (both depths' stills are covered, and every still is recorded as
  `{path, bits, w, h}` with the bits read from the **file's own container**
  (`.tif` → deep, `.png` → 8-bit) and the size from
  **`worker_.frameWidth()/frameHeight()`** — the GUI's combo or requested ROI
  at *delivery* time are both races (a long still's `photoResult` can land
  after the test already switched the depth for the next shot; it did, live —
  printing a false 14-bit expectation against a real 8-bit PNG).
- t=5500 ms: switch to video mode via the GUI toggle (`modeToggle_->setMode(2)`);
  the depth combo is rebuilt for video and keeps the deep entry (→ deep `.ser`).
- t=5500–9000 ms: record the **deep `.ser`** (2 s); t=9500 ms: switch the combo
  to the **8-bit `.ser`** entry — a real format switch (deep readout → RAW8 +
  HSM=1), so `applyRoi` runs here; t=10000–12000 ms: record it.
- t=12500 ms: interval mode via the GUI toggle, main slider to 0.5 s (log short
  range), then the **1-60 s range switch** (clamps + quantizes to 1 s) and a
  1-shot sequence (1 s exposure, 5 s interval).
- t=16500 ms: **the check.** Every recorded `.ser` goes through
  `validateSerFile(path, w, h, depth, colour ? kSerColorRgb : kSerColorMono)` —
  so a colour body's file must say ColorID 100 and a mono body's ColorID 0, and
  the size check uses that file's plane count (`178 + count*planes*W*H*bpp`).
  Every still is re-read with `cv::imread(..., IMREAD_UNCHANGED)` and must have
  `channels() == (colour ? 3 : 1)`, the right `depth()` (CV_8U / CV_16U) and the
  expected cols/rows. Then it prints
  `SMOKE frames=… colour=… stills=… rec=… ser=… seq=… gain=… fps=… temp=…C` and
  exits **0** iff the frames, stills, recordings, `.ser` validation, the sequence,
  and the applied gain all check out. The check sits late in the timeline:
  `doSequence` re-initializes the camera before emitting `sequenceDone`, so an
  earlier check raced the emission.

Latest run on the ASI178MC (exit 0, 2026-09-23, with the WB step live):
`[wb] R 1..99 def 70 (now …, auto yes) · B …` at open, `SMOKE wb: want R=98
B=98 read R=98 B=98 auto=0 -> ok`, `SMOKE wb: auto engaged (read … auto=1) ->
ok`, three stills verified `1920x1288 14-bit 3ch` / `8-bit 3ch`, `ser=1` with
`[ser] OK … 16-bit 3 planes … frames` and `8-bit 3 planes … frames`,
`gain=300 wb=ok fps=60.0 temp=34.3C`.

Run it and check the exit code directly (don't rely on a pipe's exit status):

```bash
./camera_app --smoke > /tmp/smoke.log 2>&1; echo "exit: $?"; grep SMOKE /tmp/smoke.log
```


---

## 10. Verification / ad-hoc tests used during development

### 10.1 Colour support (2026-09-22, on the attached ASI178MC)

All of these ran against the real camera with a clean build of `camera_app`
(no warnings); the app was built for the MC and is expected to be re-run against
the MM whenever it is back on the bench.

- `--capstest`, `--colourtest`, `--sertest`, `--frametest`, `--fpstest` — all
  PASS **without a camera** (the four new/updated suites plus the older ones).
- `--wbtest` (added 2026-09-23 with the WB feature, extended with the measured
  anchor the same day) — PASS **without a camera**: the Planckian locus is
  strictly monotone through the p(T) table, the body's **neutral pair** maps
  exactly to (6500 K, tint 0) and back, the slider round-trips within one 50 K
  step across the full range, the clamp behaviour on a deliberately tiny cap
  window is pinned, and the **measured ASI178MC case** (caps 1..99, anchor
  93/65) is checked at BOTH ends (each pins one channel, keeps the requested
  K ratio within 10%, and follows back with the tint offset that betrays the
  level loss). `testMeasuredAnchor` additionally pins that
  `wbMeasuredNeutral()` answers only for the measured name+caps, that the
  camera's own AWB gains (93/64) display as ~6500 K, that the delivered
  R/B ratio at 10000 K is >1.5× neutral (red) and at 2500 K <0.15× (blue), and
  that the reported tint-0 authority range lands inside 4500..7300 K.
- `camera_probe` — ASI178MC dump: RGGB, gain 0..510 (def 210),
  exposure 32 µs..2e9, BandWidth 40..100 (readback 100 after open),
  HighSpeedMode present, `WB_R`/`WB_B` present, Temperature `type=8 val=352`.
- `--smoke` → **exit 0**: `SMOKE ROI 1920x1080`; three stills verified
  `1920x1080 14-bit 3ch -> ok` and `8-bit 3ch`; `ser=1` with `[ser] OK … 16-bit
  3 planes 60 frames` and `8-bit 3 planes 105 frames`; `gain=300 fps=60.2
  temp=35.0C`.
- `--vtest 1280x720 8-bit --vtestfps 30 --vtestser 0` → MP4; `gst-discoverer-1.0`
  reports 1280×720 H.264 Constrained Baseline, 30/1, 2.6 s (no ffprobe on this
  box, and `h264dec` is **not** installed, so MP4 contents cannot be decoded
  locally — the container/codec probe is the check).
- `--vtest 480x320 8-bit ser` → **1473 RGB frames, camDropped 0 / appDropped 0**
  (colour costs nothing on the 8-bit path). `--vtest 1920x1080 14-bit ser target
  58` → 177 frames with **camDropped 79**: 3 planes = 12.4 MB/frame is more than
  this disk sustains; the writes were correct, the throughput is physics (§9).
- `--prevtest --prevroi 3096x2080 --prevbits 14 --prevexp 0.1` →
  `measured=10.00 fps expected=10.00` (the slow-preview clock is untouched by
  colour).
- `--uishot` on the MC: panel shows `8-bit RGB (PNG)` / `8-bit RGB (H.264)`,
  status `8-bit RGB`, colour preview with red clip marking
  (`⚠ CLIPPED 34.93%`), and `[ui] camera ready: ZWO ASI178MC · colour RGGB ·
  3096x2080 · 14-bit`.
- **R/B orientation settled empirically** (not from the datasheet): a photo of a
  copper ring comes out warm copper, i.e. no R/B swap; the failing cases were
  GRBG/GBRG in `bayerToBgrCode`, found by `--colourtest` and fixed (§9). Plane
  contents were also re-read out of the recorded `.ser` byte-by-byte.
- **Known accepted behaviour:** the camera's white balance (`WB_R`/`WB_B`) is
  deliberately **not** applied to our host-demosaiced RGB (§9). *(Superseded
  2026-09-23: WB became a user feature with AWB on by default for colour
  bodies — see §10.3; mono bodies remain untouched exactly as described.)*

### 10.3 White balance (2026-09-23, on the attached ASI178MC)

The 2026-09-24 repair samples and holds the camera's current gains when AWB is
switched off. Both manual handles enable immediately and send one gain pair
when released; keyboard edits are coalesced. WB registers are no longer polled
on every telemetry tick. The connected ASI178MC passed the GUI-driven smoke
path (`SMOKE wb handles: temp=1 tint=1 -> ok`, independent values at 4000 K /
Tint +60, both `.ser` recordings and all three stills validated). Raw SDK
streaming held 59.8 fps over 20 s. The app originally averaged 40–47 fps with
repeated ~650 ms capture gaps while the full-resolution colour preview ran.
Limiting OpenCV's internal thread count to one removed those gaps: the normal
app, with AWB and temperature/dropped-frame telemetry enabled, delivered 1827
frames over 30.5 s at 59.90 fps. `--colourtest` and `--wbtest` passed after the
thumbnail and buffer changes.

The AWB + Temperature/Tint feature (§1, §6.9, §7) was verified as:

- `camera_probe` dump: `WB_R`/`WB_B` **1..99, factory defaults 70 / 90, both
  writable, both auto-capable** — the numbers the `--wbtest` MC-cases and the
  `--capstest` fixture use.
- `--wbtest` / `--capstest` / `--colourtest` — PASS.
- `--smoke` on the MC → **exit 0** with the new WB step live (see §8): exact
  manual hand-over (`want == read`, `auto=0`), then auto re-engage (`auto=1`),
  `wb=ok` in the summary.
- `--uishot resources/wb_visible.png`: colour camera connected, the WB row is
  visible with AWB checked. At that time the sliders followed live auto gains
  (one capture read `5350 K · Tint −58` from the sensor). The 2026-09-24 repair
  instead samples the camera once when AWB is switched off so the handoff uses
  the current pair without polling the video stream.
- Two lessons the live runs taught, both in §8: the AWB auto-flag check must
  sit **before** the still captures (a still's format switch transiently resets
  camera WB state), and a still's expected depth must be read from its
  **container**, not the GUI combo at `photoResult` delivery time.
- Device note: the session started with the camera not yet enumerated (the §4
  symptom, `ASIGetCameraProperty FAILED`); after the re-enumeration, everything
  above ran as the normal user. The other live failure mode seen all session
  was the **one-process rule** (§4): every probe/test needs the GUI stopped
  first.

### 10.4 The WB anchor re-measured (2026-09-23, later the same day)

The user reported two symptoms of the new white-balance feature: AWB displayed
**8300 K** under light they know is 6500 K, and the top of the Temperature
slider (10000 K) was **not red enough**. Both traced to the single assumption in
`white_balance.h` that had never been tested — "the camera's default WB gains
are the daylight neutral". `tests/wb_probe.cpp` (new; built with CMake
`-DBUILD_PROBES=ON`) measured the body instead:

- **Gain law:** `WB_R`/`WB_B` really are linear multipliers on the delivered
  Bayer data — `./wb_probe --law` (interval slopes over a sweep, one channel
  pinned) gives **1.016** for `WB_R` and **1.009** for `WB_B` over six intervals
  each (0.989..1.051). The model's multiplicative arithmetic is right to ~2%,
  and the balance happens in the camera before the mosaic readout.
- **The defaults are not neutral:** a white sheet filling the frame in window
  daylight measures R/G 0.76, B/G 1.36 at (70, 90) — visibly blue.
- **Neutral is (93, 65):** gray-world on the target gave (92.0, 66.0) and the
  camera's own AWB converged to (93, 64) with the target measuring R/G 1.02,
  B/G 0.96 — two independent estimators, 1.1% apart on R and 3.1% on B. The old anchor was ~1.8×
  away in R/B ratio, which *is* the 8300 K error and *is* why the 10000 K end
  landed on neutral instead of red (measured u/v 0.92 there before the change).
- **After re-anchoring:** 6500 K delivers (93, 65) = neutral, 10000 K delivers
  (99, 42) → R/G 1.065, B/G 0.646 (u/v 1.65, the true Planckian 6500→10000
  shift), and the camera's AWB pair displays as 6550 K instead of 10000 K.
  `wbAuthorityRangeK()` states the consequence: this body holds tint 0 only
  across 4500..7300 K, and outside it the labels show what came back (tint with
  a `*`), never what was asked for.

Verification: `--wbtest` PASS (including the new `testMeasuredAnchor`);
`--capstest`, `--colourtest`, `--frametest`, `--sertest` PASS; `--smoke` on the
MC exit 0 with `wb=ok` (manual hand-over exact, auto re-engages), and its two WB
steps now also print the label text the user is shown, live off the camera:

```
SMOKE wb: want R=82 B=99  read R=82 B=99 auto=0 -> ok
SMOKE wb display (manual): 4000 K  Tint +60
SMOKE wb values independent: temp=4000 K, tint slider still +60, labels show the set values (no *): ok
SMOKE wb: auto engaged (read R=57 B=99 auto=1) -> ok
SMOKE wb display (AWB): 4000 K  Tint -44
```

**The tint-dead-travel defect the user found next, and its guards (2026-09-23).**
The user then reported *"It must be possible to set the tint independent from the
color temperature. now a \* appears at the tint value and it doesn't change
further."* — correct: `wbToGains` fits an over-cap pair by scaling **both** gains,
which is exactly what tint does, so from the boundary outward every tint value
collapsed onto ONE pair (at 10000 K: tint −31..+100 all delivered (99, 42); at
2500 K the whole slider delivered (10, 99)). The honest read-back (`*`) was not a
substitute for a working control, so the Tint range itself became the deliverable
window (`wbTintRangeForK` → `MainWindow::updateWbControls`). The next user
report undid that: moving the Temperature clamped and MOVED the user's Tint —
the coupling — so the two manual sliders became independent (fixed full Tint
range at every temperature). The user then reported that the CALCULATED values were still
coupled and the `*` sat on the tint (outside the window the read-back tint was a function
of the temperature alone), so the labels now show EXACTLY the set balance — no read-back,
no asterisk; the window is documentation-only information, never in the displayed numbers.
The user then reported that under a 3000 K light source, reducing the colour temperature
shifted the image GREEN instead of blue: the ratio-preserving pair-scale dragged the free
channel below neutral where one channel was capped (3000 K: (23, 99) rendered the light
green), so `wbToGains` now clamps per channel — the overflowing channel sits at its cap, the
free one keeps its temperature-correct value (3000 K: (47, 99), the most neutral this body
can render under that light; its B gain tops out at 1.52x its neutral). The guards:

- **`--wbtest`** (no camera) pins the windows −100..+15 at 6500 K, −100..−32 at
  10000 K, dead below ~3500 K; asserts everything inside them is delivered
  unscaled and covers ×1.6 of R-level; asserts that above the window the CAPPED
  channel stays pinned at its cap while the FREE one keeps tracking the tint
  (per-channel clamping), and that a body with caps 1..4096 keeps the whole ±100, so
  the narrowing is provably the body's headroom, not the model's.
- **`--smoke`** drives the real GUI and now asserts the DECOUPLING: it sets Tint
  +60, then moves the Temperature to 4000 K, and asserts the tint slider is
  STILL +60 (the temperature move left it alone) AND the labels show EXACTLY the set
  values (— `4000 K`, `Tint +60`, no `*`, the guard against the coupled calculated
  values) together with the exact-pair camera check — it prints `SMOKE wb values
  independent: temp=4000 K, tint slider still +60, labels show the set values (no *): ok`.
- **`./wb_probe --tint`** measures it on the sensor — inside the window the
  channel means move (10000 K: R/G 1.46 → 1.67 → 1.92 across tint −100/−66/−32),
  and one point past the end they stop dead (`build/wb_tint.log`).

(the manual line is where the per-channel clamp shows: the smoke test asks for
tint +60 at 4000 K — below this body's full-balance headroom — so B is pinned at
99 and R carries the balance (the pair (82, 99)); the label shows the SET values
`4000 K` / `Tint +60`, and `--smoke` asserts exactly that.)
The AWB line is informational, not asserted: it reports the camera's own
balance, which follows the scene (the smoke run above ran without a neutral
target in view, and the camera had pinned both channels at 99). What IS asserted
is the mapping — with the sheet filling the frame the camera's AWB pair
measured (93, 64), which the model reads **6550 K** where the old
default-gains anchor read **10000 K** (`--wbtest` pins that case).
And
`wb_probe.exe --neutral` prints the stored anchor next to a fresh measurement, so
the table in `wbMeasuredNeutral()` stays checkable rather than becoming folklore.
Fixed on the way: **the probe targets did not build at all** — `camera_probe.cpp`
still used `ASIControlCaps`/`nctrl`, dead names from before the
`ASI_CONTROL_CAPS` rename; it compiles and runs again, and `wb_probe` is part
of the probe set (CMake `-DBUILD_PROBES=ON`).

### 10.2 Throwaway probes from the mono era

Probes that lived in `/tmp` (gone now; `/tmp` files do not survive between
shell invocations on this box — put scratch in the workspace) were useful:

- **BandWidth governor + HighSpeedMode probes (2025-09-15).** `/tmp/bw_probe*`
  + `/tmp/hsm_probe*`. The fps "ceiling" investigation first A/B'd
  `ASI_HIGH_SPEED_MODE` at 480×320 RAW16 (identical 105.3 fps HSM=0/1) and
  frame data (full 14-bit, steps of 4, identical min/max spread — a genuine
  10-bit readout would quantise to multiples of 16). Then the
  `ASI_BANDWIDTHOVERLOAD` control turned out to be a **writable governor**
  (40..100, “占总的带宽的百分比”, persisted 60 on this unit) that throttles the
  stream: at 480×320 RAW16 the fps tracked the setting exactly
  (40→70.0, 50→87.7, 60→105.3, 100→176.0, 0 dropped) — so the earlier
  105.3/111 fps numbers were **governor-limited**, and the HSM A/B was
  bandwidth-masked. With the governor at 100: 480×320 RAW16 176.0 fps
  (54.0 MB/s, at the link budget); 320×240 RAW16 228.0 fps (35.0 MB/s, below
  budget → sensor rate directly visible); 640×480 120.3; 800×600 97.3
  (93.4 MB/s sustained ⇒ the link is USB3, sysfs speed=5000 on the camera's
  bus — not USB2-limited). Unmasked HSM re-test at RAW16: 480×320 175.7/175.7
  fps and 320×240 228.0/228.3 fps (HSM=0/1), on-wire bytes/frame identical
  (307,200/153,600, still 2 B/px RAW16), frame data identical (full 14-bit,
  steps of 4) ⇒ **the RAW16 output path ignores HSM** (this looked like "HSM
  is a no-op" — a false negative; the RAW16 path never shows the effect).
  `/tmp/hsm_fullres` at the camera's max video ROI 3096×2080 (BW=100,
  0 dropped) settled it: RAW16 27.7 fps with HSM=0 **and** HSM=1 (still full
  14-bit data, ~356 MB/s), but **RAW8 28.3 → 56.0 fps (HSM=0 → 1)** — exactly
  the datasheet's 30/60 fps 14-bit/10-bit full-res pair, and the link
  sustained 360 MB/s at full-res (6.7× USB2's ceiling). `/tmp/hsm_raw8_small`
  at small ROIs: RAW8 HSM=0→1: 480×320 176.0 → **351.3** fps (spec ~404),
  320×240 228.3 → **384.7** (spec 479.7). Verdict: **HSM works, only via RAW8
  output** (the 10-bit readout is delivered 1 byte/px; there is no 10-bit
  pixel format in the SDK — `BitDepth=14`, `SupportedVideoFormat = {RAW8,
  RAW16}`). `ASIGetNumOfControls`=11 and the full caps scan finds
  `HighSpeedMode` at index 9 (writable 0..1) — an index scan that breaks on
  the first `ASIGetControlCaps` error misses it (early probes reported it
  "absent" — false negative). App + `usb_bench` set `BandWidth=100` on open;
  `applyRoi` sets HSM=1 for RAW8 output / 0 for RAW16; 8-bit outputs run on
  the 10-bit column, 14-bit `.ser` on the 14-bit column.
- A RAW16 readout probe confirmed 14-bit video delivers full 0–65535-range data
  (measured max 65528, ~92.6% of pixels > 255).
- A clipping probe (`/tmp/clip12`) confirmed the 14-bit saturation spike at
  65528 (~209k px at a 30 s exposure) and the value steps of 4.
- `ffprobe` to confirm the 16-bit TIFF is `gray16le` and the MP4 is 8-bit H.264
  (**ffprobe is not installed on this machine any more** — use
  `gst-discoverer-1.0`, and ImageMagick `identify -verbose` for TIFF channels).
- PIL (no numpy, and no `cv2` in the system python) to scan 16-bit TIFF ranges.
- `.ser` validation: the smoke test's `validateSerFile` checks the 178-byte header
  (magic `LUCAM-RECORDER`, MONO, LE, W/H/depth — **16** for 2-byte files —, frame
  count) and that the file size == 178 + count×W×H×bpp (bpp 1 for 8-bit, 2 for
  16-bit). `./camera_app --sertest` (headless, no camera) writes 512×512, 5-frame,
  per-row full-range ramp samples to `samples/sample{08,14}.ser` (8/14-bit, the
  RAW16 readout path) plus `samples/sample08hsm.ser` — the 8-bit sample written
  through `push8` (the camera's RAW8 10-bit HSM readout, copied straight through
  with no host shift) — and round-trip-validates the header (incl. the declared
  depth 8/16), the exact size, and every pixel value of each file. The samples
  are the regression artifacts for the 14-bit convention: open them in a stacking
  tool and each row must show a clean full-range ramp. For the 2025-09 fix the samples
  were additionally decoded with four reader models (word as-is; `word >>
  (16-declared)`; `word << (16-declared)` clipped; normalised by `2^declared−1`) —
  with the declared 16 all four yield the full-range ramp, while the old declared
  10/14 made two of the models clip to white. For the endianness-flag fix, Siril's
  actual decode path was simulated on the samples: old flag=1 files byte-swap
  every word (ramp → `0, 32768, 1, 32769, …` checkerboard = the reported
  "noise"); flag=0 files decode to the clean ramp. Both fixes are user-confirmed
  (the 10/14-bit `.ser` opens correctly in the user's stacking tool).

- **RGB `.ser` layout reversal (2026-09-23, same day as the planar pin)**: the
  planar layout pinned earlier that day (and the "players are broken" verdict)
  was WRONG — Siril **1.2.6**'s own `ser.c` (the version installed on this
  machine, fetched from the gitlab tag) de-interleaves ColorID-100 frames
  (`for (i = 0, j = 0; …; i += 3, j++)` in `ser_read_frame`, "reorder the
  RGBRGB to RRGGBB" in the crop path) and its own writer *interleaves*
  (`dest += number_of_planes`); Ser-Player **1.7.3** (the installed package,
  `cgarry/ser-player`, cloned from source) reads `r=*p++; g=*p++; b=*p++` per
  pixel. A synthetic scene was then written in BOTH layouts and decoded with
  byte-exact simulations of both readers' code: the planar file renders
  exactly as the user-reported **3×3 mosaic of the same (compressed,
  colour-fringed) scene** — nine tiles, three channel bands × three
  horizontal strips — and the interleaved file renders as a single clean
  image. The writer now writes interleaved (`pushRgb`/`pushRgb8` fed directly
  by `demosaicBayerToRgb`), and `--colourtest` pins it: `interleaved RGBRGB
  order in file bytes` (per-pixel varying triples, which a planar file
  cannot fake), `14-bit RGB interleaved samples`, and a Bayer→file
  end-to-end check for all four patterns (`samples/colourtest_rgbE2E.ser`);
  the old planar byte-order check is gone. The regenerated
  `samples/colourtest_rgb8/14/E2E.ser` all re-decode clean under both reader
  simulations. (SIRIL 1.2.6's headless `-s` script mode refused to run in
  this environment — "requires missing"/"Unknown error" regardless of script
  contents, with or without `-d`/writable `HOME` — so the installed binary
  was not driven directly; its shipped source was used instead.)
