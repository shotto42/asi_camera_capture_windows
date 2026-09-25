# The GUI layout (AGENTS.md §7)

## 7. The GUI layout

`MainWindow` is a 1440×900 window: `FrameView` on the left (expanding), a fixed
~420 px panel on the right. The **window title names the camera**
("ASI178MC Camera"): it starts as `"<caps name> Camera"` from the caps the GUI
probes for itself, and `cameraReady` re-applies it — so a swapped body renames
the window and rebuilds the panel in place (§7 `applyCameraCaps`).
Panel top-to-bottom:

1. **ModeToggle** (3-way: PHOTO / INTERVAL / VIDEO) — a compact segmented
   control (min-height 60 px, 15 pt labels).
2. **Image size** row → `roiCombo_` (populated from `probeRois`, i.e. **the
   sizes this camera reported**, not a fixed list — §6.2). The probed list
   opens on the **1:1 base square** (side = the sensor's y-resolution, `2080×2080`
   on an ASI178) — the user-requested first entry — then the full frame, then
   the rest largest-first. The **highest resolution (full frame) is selected
   on open** and is the worker's live default — the 1:1 base square is only
   the list's first entry and is captured when the user picks it.
3. **Bit depth** row → `depthCombo_`. The items are **mode-aware** *and*
   **camera-aware**, rebuilt by `setDepthComboForMode(mode)` (called from
   `applyMode` **and** `applyCameraCaps`) from `effectiveCaps().depths()` — the
   depths this body can actually deliver. Labels come from `depthLabel(bits,
   container)`: mono "8-bit  (PNG)", "14-bit  (16-bit TIFF)", "8-bit  (H.264)",
   "14-bit  (.ser)"; colour inserts RGB: "8-bit RGB (PNG)", "14-bit RGB (16-bit
   TIFF)", "8-bit RGB (H.264)", "14-bit RGB (.ser)". `effectiveCaps()` is the
   ASI178-shaped stand-in **until the camera answers `cameraReady`** (so the
   panel is never empty while the camera is unplugged), then the real probe. Each item's data encodes the bit depth
   (low byte) **plus** a `.ser` flag (bit 0x100) — the flag distinguishes the
   two 8-bit video entries (H.264 vs `.ser`; 14-bit always sets it). The
   selection is preserved across mode switches (the `.ser` flag is dropped on
   stills); the
   worker gets `setBitDepth` + `setSerMode` explicitly since the item changes
   are signal-blocked. `currentDepth()` strips the flag for status displays. The
   row is shown in **every** mode (video now needs the selector); it sits in the
   same layout row as `roiCombo_` (no wrapper widget).
   `roiCombo_` and `depthCombo_` are given fixed widths by `syncRoiDepthWidths()`
   (called from the `roiListReady` handler and on every mode switch), each sized
   to **its own** longest label — the ROI selector to its probed items, the depth
   selector to the union of **both** modes' labels (so the row doesn't reflow when
   a mode switch swaps the items) — so the longest label isn't clipped. The text
   width plus Qt's non-text chrome (left pad + drop-down) is measured once on a
   throwaway, polished combo (the live combos can't be the reference: unpolished
   sizeHints, and sizeHints cached across item rebuilds, both yield bogus
   padding). If the two widths plus the row's 14 px spacing exceed the panel's
   388 px content width, the ROI selector yields first (floored to text + minimal
   chrome). Don't revert this — the ROI combo's own `sizeHint` under-sizes it and
   its text gets clipped.
4. **Exposure** row + slider (range switch: 32 µs…1 s log / 1…60 s linear; capped at 1/fps in video) + value label.
5. **Gain** row + slider (0.1 dB steps) + dB value label (`gainValue_`, amber).
   The range is **the camera's** — `gainReady(min,max,cur)` from the probe
   (ASI178MC 0–510 = 51.0 dB, ASI178MM 0–400 = 40.0 dB — §6.8); the slider is
   rebuilt on `cameraReady`, so a swapped body re-ranges it. The gain applies in
   all modes.
6. **White balance row — colour bodies only.** Hidden entirely until a
   *colour* camera reports both WB controls (`setWbVisible`, driven by
   `caps_.isColor && caps_.wbControls()` in `applyCameraCaps`): an **AWB**
   checkbox (`awbCheck_`, checked by default) + two value labels, with
   **Temperature** (2500–10000 K, 50 K steps) and **Tint** (−100 green …
   +100 magenta) sliders below. While AWB is checked the sliders are disabled
   and retain the last manual values. AWB never sets either slider. Unchecking
   AWB immediately applies those retained values. A drag updates its label and
   image as it moves: the first pair goes to the worker immediately, subsequent
   pairs are paced at 80 ms, and release sends the final pair. Re-checking
   re-engages the SDK auto flag. Neither slider has a tooltip. The K/Tint ↔ gains mapping is the
   Planckian-locus model in `white_balance.h`, normalized at the body's
   **measured neutral pair** (`wbMeasuredNeutral`) and never at the factory
   default gains — those are not neutral, and anchoring on them made an earlier
   build display 8300 K under 6500 K light with the 10000 K slider end sitting
   exactly on neutral (§6.9, §9). **Colour temperature and Tint are independent— in the sliders AND in the values they show** (2026-09-23, two user-reported couplings): the Tint keeps its full −100..+100 range at every temperature,
   the Temperature never clamps, moves, or disables it, and `updateWbLabels`
   shows EXACTLY the set manual balance (the sliders ARE the setting — no read-back,
   no asterisk). Outside the deliverable window
   (`wbTintRangeForK`: measured on the MC −100..+15 at 6500 K, −100..−32 at 10000 K,
   **nothing at all below ~3500 K**) one WB channel is pinned at its cap and the other carries the
   balance — `wbToGains` clamps per channel, so the temperature axis stays on
   blue↔yellow-orange in the image (a third user report that day: the old
   ratio-preserving pair-scale dragged the free channel below neutral and a 3000 K
   light rendered GREEN; per-channel it stays in the yellow family) — the
   headroom never leaks into the displayed numbers. History: the window used to BE the slider's range — that fixed the dead
   travel outside it (tint and the out-of-cap pair-fit act along one and the same
   factor, so ~130 units of travel delivered ONE gain pair), but coupled the controls
   (moving the Temperature clamped and moved the user's Tint), and the read-back
   labels then showed a `*`-flagged DELIVERED tint that was a function of the
   temperature alone where a channel is capped — both reported, both removed, and
   the window removed from the slider's range (§6.9, §9). A body whose WB caps report no auto support gets
   the checkbox disabled and starts manual;
   a mono body never sees the row at all.
7. **QStackedWidget** — photo page: "Take photo" button. Video page: "Frame rate"
   row → `fpsSlider_` (1 → the mode-aware max fps for the current ROI + format;
   `fpsValue_` shows the value) + the circular `RecordButton` (96 px — both circular action buttons are 96 px now, 20% down from 120 on user request; same painted circle style as the photo shutter: red when idle, dark circle with a red REC dot while recording; state set by `setRecording` from the recording signals — the old text and `#recBtn` CSS state rules are gone). The slider maximum is
   set by `updateFpsMax(w,h)` (called from the ROI-combo handler **and** the
   depth-combo handler, and seeded for the initial full ROI) to
   `fpsCeilingFor(w,h,bits,ser)` — 8-bit H.264 capped at 60 fps, 8-bit
   `.ser` the 10-bit spec column (RAW8 + HSM=1 readout, §9), 14-bit `.ser`
   the 14-bit column (RAW16 readout; HSM is ignored on that path), linearly
   interpolated by pixel count, floored, clamped to ≥ 1; see the fps table below.
   The table is scaled **from the camera's own full-frame rate** (the ASI178
   anchor point 6 439 680 px) so a different sensor gets sensible numbers, then
   capped by a measured USB-payload constant (both §6.2 `--capstest`, §9).
   For `.ser` modes the ceiling is additionally **raised to whatever this
   machine is actually delivering** (`fpsMeasuredMax_`, tracked in `updateStatus`
   with a 5% headroom): the live rate is a better bound than the datasheet, and
   a body that over-performs its column is not artificially limited. Stills/H.264
   keep the datasheet number (H.264 is intentionally capped at 60, and a still's
   rate is not a UI limit at all). Interval page: the sequence
   uses the **main** exposure slider **and its range switch**, exactly like
   photo mode (there is no separate exposure control; the old on-page hint
   label was removed),
   **Interval** (`seqIntervalSpin_`, 1–1200 s, label "min 1 s", default 5), **Images**
   (`seqCountSpin_`, 0 = continuous, default 10) and the `SequenceButton`
   (`seqBtn_`, 84 px, painted in code): title line "▶ START SEQUENCE" / red
   "■ STOP" while running, **plus the status line painted inside the button**
   (bottom band, bold: "Ready" → "Starting · …" → "Capturing…" →
   "Exposing shot N / M…" → "Shot N / M saved" → "Shot N done · next in X s" →
   "Done/Stopped: N image(s) in T s")
   — the old separate `seqStatus_` label below the button is gone; the main
   status widget reads the same text via `seqBtn_->statusLine()`.
8. **HistogramWidget** (128 bins). The clip readout is painted **inside it**:
   `⚠ CLIPPED x.xx%` (percentage of raw-frame pixels at max value, capped at
   100%) in red at the top-right corner via `setClipPct()` — the old
   `clipWarn_` banner label (which made the panel reflow) is gone. On a colour
   body the histogram is the raw-mosaic sample (§6.3) and the clip marking is the
   RGB thumbnail's red blocks; nothing about the widget itself changed.

9. **status_** label (exposure/mode/bit-depth — with ` RGB` appended on a colour
   body, e.g. "8-bit RGB" — + fps/gain/dropped/temp while
   the camera is up, plus the **live** lines only: `◷ SEQ …` while a sequence
   runs and `● REC  (m:ss)  [N frames]` while recording — no filename, no
   dropped count). The former "Last photo / Last video /
   Last sequence" lines are gone — they persisted forever and made the label
   (and therefore the panel) grow permanently after the first capture. The
   label is **fixed-height for 4 lines** (2 steady + the 2 live lines;
   `4 × QFontMetrics(14 px).height() + 20` padding, text top-aligned) so the
   window never resizes when live lines appear/disappear.

A 33 ms `QTimer` drives the preview + histogram + clipping update; the status
text refreshes every 8th tick (~4 Hz).

**fps slider maximum (`maxFpsForMode`):** the slider's upper bound is the ZWO
USB3.0 spec rate for the current ROI **and** the selected video format, from the
datasheet's two ADC columns (pixels → fps):

| pixels    | 10-bit fps | 14-bit fps |
|-----------|-----------|-----------|
| 76 800    | 479.7     | 239.8     |
| 307 200   | 253.1     | 126.5     |
| 480 000   | 204.7     | 102.3     |
| 1 228 800 | 130.0     | 65.0      |
| 2 211 840 | 116.0     | 58.0      |
| 5 242 880 | 62.0      | 31.0      |
| 6 439 680 | 60.0      | 30.0      |

`maxFpsForMode(w,h,bitDepth,serMode)` is the **pure table** (ASI178 numbers, no
scaling — what `--fpstest` asserts). The GUI ceiling goes through
`maxFpsEstimate(w,h,bitDepth,serMode)` (`fps_spec.cpp`), which takes that table
value, scales it when the ROI has more pixels than the anchor
(`topPx/px`, since past the datasheet's end the limit is a constant pixel
rate), and finally caps the result by `kUsbStreamBytesPerSec = 550.0e6` — the
measured payload ceiling (§9) — so a hypothetical monster ROI cannot offer an
unrecordable rate. It is **calibrated to leave the ASI178's own datasheet pair
unclipped**: full-res RAW8+HSM (56.7 measured vs 60 spec) and full-res RAW16
(28.0 vs 30 spec) both stream 386–387 MB/s, so the constant sits above them and
below the point where the datasheet's *own* 60 fps full-res entry would be
wrongly cut (an earlier 360 MB/s value clipped it to 55/27 and `--capstest`
caught it). picks the column by format: **8-bit H.264
(MP4) is hard-capped at 60 fps**; **8-bit `.ser`** uses the 10-bit column (it
captures RAW8 with `ASI_HIGH_SPEED_MODE=1` — the 10-bit "high speed" ADC
readout, §9); **14-bit `.ser`** uses the 14-bit column (RAW16 readout; HSM
is ignored on the RAW16 output path). The value is linearly
interpolated by pixel count (clamped to the endpoints) and floored to an integer
≥ 1, applied via `worker_.setFps` (clamped to 1–1000), and re-seeded whenever the
**ROI or the bit-depth/format changes** (not just the ROI). On a **bit-depth/format
change** the slider snaps up to the new format's spec rate
(`updateFpsMax(..., snapToMax=true)`; it always clamps *down* if the value
exceeds the new bound); a plain **video → photo → video round trip with the
format unchanged keeps the user's chosen fps** (entering video mode used to
snap to the max unconditionally and silently reset the choice).
These are the spec ceilings; on a healthy USB3 link the unit delivers
~80–95% of the column
(2025-09-15: 351 fps at 480×320 RAW8-HSM vs 404 spec; 176–185 at 480×320 RAW16
vs 202; 56 at full-res RAW8-HSM vs 60), and a USB-bandwidth-limited connection
delivers less (§9); the app
raises the camera's `BandWidth` governor to 100 on open (the persisted 60
throttled 480×320 to ~111 fps — §9).

**Exposure slider (log + range switch):** slider range is 0–1000 mapped
exponentially to `kExpMinS=32e-6` (32 µs) … a **mode-dependent max**: photo
**and** interval follow the range switch — `kExpShortMaxS=1.0` s (switch off,
log) or `kExpLongMinS`…`kExpLongMaxS` = 1…60 s (switch on: the exception —
**linear** in whole-second steps via `sliderToSecondsLinear`, handle snaps to the
step); video is `1/fps` (exposure must fit in one frame). The 32 µs floor and
2000 s ceiling (`kExpSdkMaxS`) are the camera's documented min/max (query them
live with `ASIGetControlCaps` on the `Exposure` control — `MinValue=32` µs,
`MaxValue=2000000000` µs); the GUI slider no longer reaches 2000 s (photo/interval
top out at 60 s), so `kExpSdkMaxS` remains only as the worker-side safety clamp
in `doSequence`. The value label (`fmtExposure`) shows sub-millisecond
exposures in µs (e.g. "32 µs") and ≥ 1 h in minutes (e.g. "33.3 min").
