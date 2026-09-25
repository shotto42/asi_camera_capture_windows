# Preview, histogram, colour (AGENTS.md §6.3, §6.4)

### 6.3 Bit depth, histogram, and clipping

- **8-bit** capture: `ASI_IMG_RAW8`, 1 byte/pixel, values 0–255. Used for
  8-bit H.264 video, 8-bit stills, **and 8-bit `.ser`** — with
  `ASI_HIGH_SPEED_MODE=1` set by the app, RAW8 output runs the sensor's
  **10-bit "high speed" readout** (the 10-bit datasheet column: 404 fps at
  480×320, 60 at full-res; §9).
- **14-bit** capture: the SDK has **no RAW14 type** — 14-bit is delivered as
  `ASI_IMG_RAW16` (2 bytes/pixel). The sensor's `BitDepth` field reads 14 and the
  data is scaled into the 16-bit container; **it saturates at 65528, not 65535**
  (values come in steps of 4; a saturated frame shows a hard spike of ~200k
  pixels at exactly 65528). The RAW16 output path **ignores HighSpeedMode** and
  always reads out 14-bit (§9).
- **10-bit `.ser` (dropped 2026-09-15):** it was not a native
  readout — there is no 10-bit pixel format in the SDK and HSM's 10-bit
  readout is only delivered as RAW8. It used to be captured as `ASI_IMG_RAW16`
  (14-bit readout, HSM=0) with the `.ser` output keeping the top 10 bits
  **left-justified** (`(raw16 >> 6) << 6`, 2 bytes/px) (§6.5). In the live
  preview, histogram, and clipping it was indistinguishable from 14-bit (same
  RAW16 data, `bpp_==2`) — i.e. it offered the same readout, FPS, and file
  size as 14-bit `.ser` while silently discarding 4 bits, so the UI no longer
  offers it. The worker/`SerWriter` still accept depth 10 (reachable via
  `--vtest --vtestbits 10`).
- **Histogram** (`computeHistogram`): 128 bins, computed by the preview builder
  on the published raw frame (or the 2×2 box buffer when the box path ran),
  **stride-sampled on a 2×2 grid** (every other row/column — a 128-bin
  histogram is statistically indistinguishable at ¼ of the pixels; this is what
  keeps the 6.4 MP per-frame cost to ~3 ms). Bin range adapts to
  bytes-per-pixel: 8-bit (`bpp_==1`) → `value >> 1` (2 values/bin);
  14-bit (`bpp_==2`) → `value >> 9` (512 values/bin). Stored in `hist_`
  under `frameMtx_`.
- **Clipping detection** (same pass): a pixel is "clipped" when it reaches the
  max value. Threshold is bpp-aware: **8-bit (RAW8) → `value >= 255`**,
  **14-bit (RAW16) → `value >= 65528`** (the 14-bit saturation value). The count (also stride-sampled, so a representative
  estimate, not exact) is stored in `clipCount_`.
- **Red-marked preview:** the clipped-pixel mask comes from the thumbnail pass
  (`boxDownsample2x`'s per-box clip mask on the box path, else
  `buildDisplayFrame`'s block-corner flags — not the histogram); the preview
  builder emits masked pixels as pure red in an RGB buffer, stored in `latest_`
  with `latestCh_=3`; otherwise `latest_` is grayscale (`latestCh_=1`).
- **GUI side:** the 33 ms timer shows `latest_` — `Format_RGB888` (red-marked)
  when `latestCh_==3`, `Format_Grayscale8` otherwise. The clip amount is shown
  by the **histogram itself**: when `clipCount_ > 0`,
  `HistogramWidget::setClipPct` makes it paint `⚠ CLIPPED X.xx%` (percent of
  raw-frame pixels at max value, capped at 100%) in red at its top-right
  corner; nothing is drawn otherwise. (The old separate `clipWarn_` banner
  label is gone.)

### 6.4 Frame display

`FrameView::setFrame(data, dataSize, w, h, channels)` builds a `QImage` —
`Format_Grayscale8`
when `channels==1`, `Format_RGB888` when `channels==3` (the red-marked preview) —
then `update()`. `paintEvent` scales it to fit the widget (aspect-preserving) on
a black background. `dataSize` is the actual byte count of `data`: a
(data, channels) combination that would build an out-of-bounds image is refused
or degraded to grayscale (see the display-buffer invariant in §6.2).

**BYTE ORDER — the third leg of the display contract (`Format_RGB888` reads
byte 0 as R).** Every 3-channel writer into `latest_` must emit **R,G,B** byte
order: `colourDisplayFrame()` does (its quad path writes R,G,B directly; its
fallback demosaics through OpenCV's BGR and then swaps the interleaved B/R —
both pinned by `--colourtest` for all four Bayer patterns), and `paintClipRed()`
paints its flag `o[0]=255` in the same order. The PNG/TIFF/MP4 file paths
deliberately stay **BGR** (`demosaicBayerToBgr` → `imwrite`, the H.264
appsrc); the `.ser` colour path demosaices straight to interleaved **RGB**
(`demosaicBayerToRgb` → `SerWriter` — the SER ColorID 100 order). A BGR-ordered
preview buffer was exactly this project's preview bug:
an R/B-exchanged live image while every recorded file looked perfect — the
files never see the order, the preview always does.

