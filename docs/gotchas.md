# Known issues / gotchas (AGENTS.md §9)

## 9. Known issues / gotchas

- **Camera bad-state after a kill** (error `2` in a fresh process) — relaunch
  the app or wait a few seconds (§4).
- **Only one process holds the camera** — stop the GUI (and any probe) before
  running other camera binaries; two copies of the app at once means the second
  waits at "Waiting for camera…" forever.
- **14-bit ≠ 65535** — the 14-bit data saturates at 65528; never compare 14-bit
  values to 65535 (§6.3).
- **MP4 is 8-bit; 8/14-bit `.ser` is uncompressed** — 8-bit *H.264* video is
  H.264 (x264enc baseline, 8-bit only) → MP4. The 8/14-bit `.ser` options are
  written uncompressed to `.ser` (§6.5), so the full bit depth is preserved there
  (8-bit `.ser` stores 1 byte/px; 14-bit 2 bytes/px).
- **`.ser` channel count follows the camera** — `ColorID`=0 (MONO, 1 plane) for
  a mono body, **100 (RGB, 3 channels interleaved R,G,B per pixel)** for a
  colour body; verify with the header (offset 18) before handing the file to a
  stacking tool, and remember the frame is `W*H*bpp*planes`
  (`--smoke`/`--colourtest` do). An RGB `.ser` is
  **3× the bytes** of the mono one: full-res 14-bit RGB ≈ 37 MB/frame, which this
  machine's disk cannot sustain at 30 fps (measured 177 frames with camera-side
  drops at target 58); 8-bit RGB records cleanly (1473 frames, 0 drops at
  480×320/120 fps). Expect to use a smaller ROI or faster storage for 2-byte RGB
  video — the format is correct, the throughput is physics.
- **RGB `.ser` frames are PER-PIXEL INTERLEAVED R,G,B — do NOT write full
  planes (all R, then all G, then all B)** — on 2026-09-23 this project went
  the other way around: it pinned the planar layout and blamed the players
  for showing a "3×3 grid of tiles". That verdict was WRONG. Both readers in
  the ecosystem de-interleave: Siril `src/io/ser.c` (`for (i += 3)` triples
  in `ser_read_frame`, "reorder the RGBRGB to RRGGBB and crop" in its crop
  path, and its own writer interleaves); Ser-Player (`pipp_ser.cpp`) reads
  `r=*p++; g=*p++; b=*p++` per pixel. A planar file under either renders
  exactly as the reported 3×3 mosaic of the same (compressed, fringed) scene
  — reproduced byte-for-byte. Both layouts have identical byte counts, so a
  silent flip is undetectable by size — `--colourtest` pins the on-disk
  interleaved order ("interleaved RGBRGB order in file bytes") and the
  Bayer→file channel mapping end-to-end for all four patterns. §6.5.
- **The Bayer pattern mapping is read down the FIRST COLUMN, not the row** — the
  datasheet's "RGGB" describes the mosaic read *column-first* (`R G / R G` ⇒
  row0 = RG), which is what OpenCV's `COLOR_BayerXX` names also encode, so the
  SDK name and the OpenCV name **look** like they should match — and for GRBG and
  GBRG they do **not**: SDK GRBG → `COLOR_BayerGB`, SDK GBRG → `COLOR_BayerGR`
  (matching the row instead swaps R and B on those two patterns; caught by
  `--colourtest` on 16-bit synthetic frames, R=50/B=200 where a swap is obvious).
  `bayerToBgrCode()` carries the empirical table + the derivation — do not
  "simplify" it. `--bayer <pat>` re-derives the mapping on a live camera if a
  body's R/B ever come out transposed.
- **Never capture RGB24 on a colour body** — the camera *offers* it, and it is a
  trap: it runs on the **slow** readout at 3 bytes/px (measured 28.7 fps at full
  res, the same 554 MB/s that RAW8-HSM spends on 1 byte/px at 56.7 fps), so
  demosaicing RAW8/HSM on the host is strictly faster *and* keeps the raw data.
  `--bayer`/RGB24 support exists only as a diagnostic path (§2).
- **White balance: user-controllable on colour bodies, AWB on by default — and
  mono bodies are never touched.** A colour body exposes `WB_R`/`WB_B`
  (measured ASI178MC: both 1..99, factory defaults R 70 / B 90, both writable
  and both auto-capable). The app applies them (§6.9, §7): with **AWB checked
  (the default on colour bodies)** the controls are driven through the SDK auto
  flag; the disabled Temperature/Tint sliders show the last sampled balance.
  Unchecking AWB reads the current gains once, freezes that pair, and enables
  the sliders. A drag applies one manual pair on release, so SDK writes do not
  repeatedly interrupt the video stream.
  This replaces the pre-2026-09-23 rule "WB is never touched, data stays
  neutral" — the user explicitly asked for AWB-on-colour plus manual sliders
  (§1, §7). What still holds: a saved file carries the balance **baked in**
  (it happens in the camera before the mosaic readout), so an astro pipeline
  that wants an untouched frame unchecks AWB and leaves the sliders at the
  anchor; and on a **mono** body the WB controls are never written and the UI
  row is not shown at all (`hasWb_ = isColor && caps.wbControls()`).
- **The camera's factory WB defaults are NOT its daylight-neutral pair — never
  re-anchor the Kelvin scale on them.** The first version of `white_balance.h`
  assumed "the defaults are 6500 K daylight", and on the ASI178MC that was wrong
  by a factor ~1.8 in R/B ratio. Two user-visible bugs came out of it: the
  camera's *correct* AWB gains displayed as 8300…10000 K under 6500 K light, and
  the top of the Temperature slider (10000 K) landed exactly on neutral — so the
  whole cool half of the slider produced no red at all. Measured with
  `./wb_probe --neutral` (2026-09-23, white target filling the frame in window
  daylight): the neutral pair is **R 93 / B 65** (the camera's AWB independently
  converged to 93/64 with the target measuring R/G 1.02, B/G 0.96), while the
  factory defaults (70/90) leave that same target visibly blue (R/G 0.76,
  B/G 1.36). The anchor now lives in `wbMeasuredNeutral()` per measured body and
  its caps; only an unmeasured body falls back to its defaults (the GUI has no
  tooltips, so it does not say so on screen either).
- **The Kelvin/Tint numbers are an estimate sitting on top of that measured
  anchor.** `white_balance.h` maps (K, tint) through the Planckian locus
  normalized at the body's neutral pair; it is monotone, exactly invertible at
  the anchor, and round-trips within one slider step. Two honest caveats. (a)
  The locus model assumes sRGB-matching primaries — every camera's WB UI makes
  that assumption — so the Kelvin numbers are estimates, while the anchor they
  hang on is a measurement. (b) The body runs out of WB authority: measured on
  the MC, tint 0 holds only across roughly **4500..7300 K**, because its neutral
  pair sits at 93 of a 99 `WB_R` ceiling and 65 of a 99 `WB_B` one. Outside that
  `wbToGains` clamps PER CHANNEL: the overflowing channel sits at its cap and the
  free one keeps its temperature-correct value, so the temperature axis stays on
  blue↔yellow-orange in the image (the old ratio-preserving pair-scale dragged the
  free channel below neutral and a 3000 K light rendered GREEN — user-reported,
  fixed the same day; on this body the best 3000 K light can do is a slightly warm
  white, because the B gain tops out at 1.52x its neutral). The GUI labels never
  read back: they show exactly the balance you set.
- **The manual Temperature and Tint sliders are INDEPENDENT ON PURPOSE — do not
  re-couple them.** Making the Tint range per-temperature (`wbTintRangeForK` as
  the slider's range, 2026-09-23) fixed the dead travel outside the window but
  coupled the controls: moving the Temperature clamped and MOVED the user's
  Tint (and disabled it below ~3500 K). The user then reported that the
  Temperature should be completely independent, so the coupling was removed:
  the Tint keeps its full −100..+100 range at every temperature, and the
  Temperature never clamps, moves, or disables it (`MainWindow::updateWbControls`
  only sets enabled states). What still holds from
  the physics (unchanged): outside the deliverable window one WB channel is pinned
  at its cap and only the other is free (MC: −100..+15 at 6500 K, −100..−32 at 10000 K,
  none below ~3500 K). The labels show exactly the set balance — no read-back, no `*`
  (the window is documentation-only information; user reports that day removed the coupled
  controls, the coupled read-back values and the asterisk). `--smoke` asserts the
  temperature move leaves the tint slider alone AND the labels show the set values
  without a `*`; `--wbtest` pins the windows and the per-channel clamp at the slider
  ends; `wb_probe.exe --tint` measures the capped behaviour on real frames.
- **`ASIGetControlCaps` is not portable across bodies** — it works on the MC and
  fails on the MM; anything that needs a range must have a documented fallback
  (§6.8), and `--capstest` covers the "caps unreadable" branch.
- **`.ser` 2-byte (14-bit) output needs all three header/data choices:
  left-justified values, `PixelDepthPerPlane = 16`, and the offset-22
  endianness field = 0.** The SER spec text prescribes LSB alignment for
  9–16-bit and a `LittleEndian` boolean flag (1 = LE), but readers don't
  agree on how to undo a declared sub-16 depth (Siril reads the word as-is;
  SER.Lib normalises by 2^depth−1), so with the effective depth declared
  *neither* alignment renders correctly in every tool (MSB+14 → white image;
  LSB+14 → 1/4…1/64 brightness), and the de-facto tools (Siril/GoQat/
  first SER programs/FireCapture/PIPP) treat the flag as inverted
  (0 = LE data): with field=1 our LE data had every word byte-swapped →
  checkerboard "noise". Left-justified + declared 16 + field 0 is the one
  combination all reader models decode to the full-range image (§6.5).
  8-bit (1-byte) files declare 8, ignore the field, and are unaffected.
- **HighSpeedMode works — but ONLY with RAW8 output; the app uses it for every
  8-bit output.** `ASI_HIGH_SPEED_MODE` (the 10-bit "high speed" ADC mode)
  switches the sensor to the 10-bit fast readout, which the camera delivers as
  `ASI_IMG_RAW8` (1 byte/px). The **RAW16 output path ignores HSM** and always
  reads out 14-bit — so any A/B test that forces RAW16 in both HSM states (as
  the 2025-09-15 probes did) is a false negative. Decisive measurements
  (2025-09-15, `/tmp/hsm_fullres` + `/tmp/hsm_raw8_small`, BW=100, 0 dropped):
  at 3096×2080 (the camera's max video ROI) RAW8 gives **28.3 fps (HSM=0) vs
  56.0 fps (HSM=1)** — the datasheet's 30/60 fps 14-bit/10-bit full-res pair;
  at 480×320 RAW8 **176.0 → 351.3 fps** (spec ~404); at 320×240 RAW8
  **228.3 → 384.7** (spec 479.7) — ~80–93% of the 10-bit column, the same
  efficiency the 14-bit readout shows. With HSM=1 + RAW16 the data is still
  full 14-bit at the 14-bit rate (27.7 fps @3096×2080, identical to HSM=0).
  **App behavior:** `applyRoi` sets HSM=1 whenever the format is RAW8 and 0
  otherwise (log: `[hsm] set=… (bpp=…)`); 8-bit H.264 **and** 8-bit `.ser`
  capture RAW8 on the 10-bit readout (8-bit `.ser` used to capture RAW16 and
  shift `>>8` on the host — same 8-bit data at half the rate); 14-bit `.ser`
  captures RAW16 (HSM=0) and stores the readout as-is (`SerWriter::push`),
  while 8-bit `.ser` copies the RAW8 values straight through
  (`SerWriter::push8`).
  `info.BitDepth` is 14 and `SupportedVideoFormat` is {RAW8, RAW16} — there is
  no 10-bit pixel format in the SDK; the 10-bit data is the HSM readout
  delivered as 8-bit values.
- **Build artifacts** — all generated files (`moc_*.cpp` from `build_win.bat`,
  CMake + nmake outputs) live in `build_win/`; nmake's `.d` dependency files
  pick up header edits automatically, and `build_win.bat clean` wipes
  `build_win\` and `package_win\` (it does NOT touch `photos/`, `videos/`,
  `sequences/` or `samples/`). There is no stale-moc step (the moc files are
  regenerated on every configure).
- **Long-exposure first shot after a format switch** — if a sequence changes the
  ROI/bit-depth, the old `applyRoi` restarted video capture and `doSequence`
  immediately stopped it again; that redundant start/stop intermittently made the
  *first* long exposure fail (~50% on 6 s exposures). Fixed by
  `applyRoi(..., restartVideo=false)`, which leaves the camera stopped so the
  sequence drives the snaps directly (§6.2, §6.7).
- **14-bit preview rendered pure white (fixed).** The old worker-side box-average
  downscale averaged the 16-bit source at full depth and clamped the average at
  255 WITHOUT the `>> 8` 16→8-bit scale, so every 14-bit scene with values above
  256 (0.4% of full scale) displayed as 255 — the preview was white except for
  the true clips, which still showed red (clip flagging is independent). The
  8-bit path and the small-ROI path were unaffected. The downscale now maps
  16-bit to 8-bit per pixel (`>> 8`) **first** and averages in the 8-bit domain —
  `buildDisplayFrame()`'s INTER_AREA path in `src/display_frame.cpp` for small
  frames, and the preview builder's `boxDownsample2x` pass for frames ≥ 2× the
  thumbnail — and
  `./camera_app --frametest` covers it: 16-bit mid-gray must come out 128, not
  255.
- **Full-resolution colour preview stalled the camera stream (fixed,
  2026-09-24).** On this host OpenCV's default parallel thumbnail resize made
  the ASI178MC capture loop lose about 650 ms every few seconds, even though a
  typical resize itself took only ~10 ms. `cv::setNumThreads(1)` before the
  preview builder starts kept a 30.5 s full-frame RAW8 run at 59.90 fps, the
  same ~60 fps the raw SDK benchmark delivered. Keep three raw frame buffers:
  the builder claims its read slot, and the worker excludes that slot from SDK
  writes until the thumbnail and histogram finish. Two alternating buffers
  let the worker overwrite a frame during a slow preview build.
- **Format change resets the camera's exposure AND gain (firmware).**
  `ASISetROIFormat` (bit-depth or ROI switch) makes the firmware drop both
  controls to their defaults; a long default exposure then blew out the next
  shot — seen in photos (a 100%-saturated 8-bit frame 17 s after a normal one,
  after a format toggle). The app therefore invalidates `lastExpApplied_` /
  `lastGainApplied_` inside `applyRoi` on success (the main loop re-applies the
  user's exposure + gain on the very next iteration), and `doPhoto`/`doSequence`
  re-apply the user's gain right after their own "ensure ROI" `applyRoi`
  (same-iteration case: photo/sequence + format change together; they already
  re-apply the exposure).
- **The camera's `BandWidth` governor was silently capping video fps — the
  app now raises it to 100 on open.** The camera persists
  `ASI_BANDWIDTHOVERLOAD` (documented as “占总的带宽的百分比” — the fraction of
  the total bandwidth the camera may use; caps 40..100, default 50, persisted
  **60** on this unit) that throttles video far below what the link carries:
  at 480×320 RAW16 the fps tracks the setting exactly (40→70.0, 50→87.7,
  60→105.3, 100→176.0 fps, all with 0 dropped). **This governor at 60 — not
  the sensor — was the “~112 fps ceiling”** seen in user `.ser` files.
  `CameraWorker::openCamera` (and `tests/usb_bench`) now set it to 100 on
  every open (log: `[bandwidth] set=100 … readback=100`). At 100 the unit
  delivers **~92–95% of the 14-bit datasheet column at every ROI measured**
  (2025-09-15, `/tmp/bw_probe*`): 480×320 → 176 fps raw-SDK / **~185 fps in
  the app** (EMA; a 4 s 14-bit `.ser` vtest matches) vs spec ~202; 320×240 →
  228 vs 239.8; 640×480 → 120 vs 126.5; 800×600 → 97 vs 102.3. The sustained
  byte rate rises with frame size (35 → 54 → 74 → 93.4 MB/s), i.e. a fixed
  per-frame readout/transfer overhead plus a high marginal rate — and the app
  itself is not the bottleneck (`[perf] grab=5ms disp+hist=0ms`; a raw-SDK
  tight drain with no GUI/thumbnail/`.ser` matches the app's rate;
  `ASIGetDroppedFrames`=0). The link is the **USB3** one (sysfs
  `speed=5000` on the camera's bus; 93.4 MB/s sustained ≈ 747 Mbps ≫ a USB2
  ~60 MB/s ceiling), so it is not a USB-2.0-limited connection. Full-res RAW16
  is **27.7 fps** at 3096×2080 (6.44 Mpx readout, 356 MB/s) and
  **46 fps** at 1920×1280 (2025-09-15, BW=100 — supersedes the old
  "~3.7 fps full-res / 15–23 fps @1920×1280" numbers, which predate the
  BandWidth fix), and fps is independent of the exposure as long as it fits
  inside the frame period. The link is clearly
  USB3-class at every size: it sustained **360 MB/s** at full-res 3096×2080
  (both the 27.7 fps RAW16 and the 56 fps HSM-RAW8 phases sit at ~356–361
  MB/s, 0 dropped) — 6.7× a USB2 link's ceiling. **The 10-bit spec column is
  reachable** — via HSM=1 + RAW8 output for every 8-bit output (§9 HSM
  bullet); the 14-bit column is the ceiling for 14-bit `.ser` (RAW16 readout;
  HSM is ignored on that path). The fps slider caps 8-bit `.ser` at the
  10-bit column and 14-bit `.ser` at the 14-bit column.
  If drops ever appear on a marginal USB link, lower the
  governor before capture (`ASISetControlValue(cam, ASI_BANDWIDTHOVERLOAD,
  60, ASI_FALSE)` — no UI yet). Re-measure with `usb_bench.exe W H 16 2475 4`
  (raw path; it sets BandWidth=100 itself) and `CAMDBG=1
  build_win\camera_app.exe --vtest --vtestroi WxH --vtestbits 8 --vtestfps 404
  --vtestdur 4` (full app; prints `[bandwidth]`, `[perf]`, and frames written).
- **GStreamer plugins are not statically linked** — they're loaded from
  `GST_PLUGIN_PATH` at runtime, so a naive "copy the DLLs the exe imports"
  package silently loses the encoder elements. The CMake post-build steps
  bundle the 5 plugins into `gstreamer-1.0/` next to each exe, and `main()`
  points `GST_PLUGIN_PATH` there (§5). If the packaged app's video recording
  errors with a `gst: ...` message or no MP4 appears, that folder (and the
  five plugin DLLs in it) is the place to look.
- **Fixed use-after-free** (was a segfault in `GstVideoEncoder::start()`): the
  old code called `g_error_free(err)` *before* reading `err->message`. It only
  triggered when `gst_parse_launch` failed (i.e., in a broken plugin env), which
  is exactly what the first packaging attempt hit. Now the message is printed
  before freeing.
- **Package is Windows x64 only** — build on the target platform and repackage
  there (§5).
