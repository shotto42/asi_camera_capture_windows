# Photo capture and interval sequences (AGENTS.md §6.6, §6.7)

### 6.6 Photo capture (`doPhoto`)

Stops video, sets the exposure, runs a single `ASIStartExposure` +
`ASIGetDataAfterExp` (polling `ASIGetExpStatus` up to `expS + 5 s`), then saves:

Both depths go through one helper, `saveStill(path, raw, w, h, bpp, err)`, which
is where the mono/colour rule lives for stills (shared by `doPhoto` and
`doSequence`, so the two cannot drift apart):

- **mono body:** 8-bit → `CV_8UC1`, deep → `CV_16UC1` — exactly the buffers this
  code wrote before colour support existed.
- **colour body:** the mosaic is demosaiced first, so 8-bit → `CV_8UC3`, deep →
  `CV_16UC3`. OpenCV round-trips 16-bit 3-channel TIFF and PNG losslessly
  (checked by `--colourtest` reading its own files back, and by `--smoke`
  `cv::imread`-ing every file it wrote and asserting **channels == 3 on a colour
  body, == 1 on a mono body**, plus the depth and the pixel size).
- Output names/containers are unchanged: **8-bit → PNG, deep → 16-bit TIFF**,
  `photos/photo_<ts>.png|.tif`.

On success it refreshes the preview via `publishStillPreview` — full-frame
grayscale (`latestCh_ = 1`) for mono, an RGB thumbnail with clipped blocks marked
red (`latestCh_ = 3`) for colour — and resumes video capture. (`latestCh_` must
follow the buffer it just filled; see the invariant in §6.2.)

### 6.7 Interval sequence (`doSequence`)

A back-to-back long-exposure sequence, driven by `requestSequence(expS,
intervalS, count)` (GUI: the INTERVAL mode's START button, which passes the
**main exposure slider's** value — there is no separate exposure control) and
stopped by `stopSequence()` (a `seqStop_` atomic flag). The exposure is clamped
to `[1 ms, kExpSdkMaxS]` (2000 s, the SDK max) and the interval to a **1 s
minimum**; the next shot starts exactly `intervalS` after the previous one
**finished** — the interval is the time between images and always elapses,
independent of the exposure length (start-to-start spacing = exposure +
interval + per-shot overhead). `count=0` runs continuously until stopped.

Per shot it uses the same long-exposure snap pattern as `doPhoto`:
`ASIStartExposure` → poll `ASIGetExpStatus` (50 ms steps, deadline `expS + 10 s`,
checking `seqStop_` each pass) → `ASIGetDataAfterExp`. On a stop mid-exposure it
cancels the in-flight exposure with `ASIStopExposure`. Each frame saves to
`sequences/seq_<ts>/NNNN.<ext>` — **8-bit → PNG**, **14-bit → 16-bit TIFF**,
both through the same `saveStill()` as `doPhoto`, so a colour body's sequence is
RGB and a mono body's is single-channel (`NNNN` is a zero-padded 1-based shot
index) — and refreshes the live preview with `publishStillPreview` (§6.6). It emits `sequenceExposing(i, count)` when a shot's exposure starts
(the GUI shows “Exposing shot N/M…” — keeps the button line current through
long exposures instead of freezing on the last countdown value),
`sequenceShot(i, count, path)` per shot (empty `path` = failed shot),
`sequenceWait(remainingS)` every 250 ms during the interval between shots (the
full interval counts down after every image — even after an exposure that is
longer than the interval), and `sequenceDone(total, earlyStop)` at the end (no
trailing wait after the final shot).

While running it **pauses the live preview** (stops video capture) and
**exclusively owns the camera** — any active recording is stopped first. After
the last shot (or a stop) it resumes video capture and resets the fps pacing.
If the sequence's ROI/bit-depth differs from the current capture format, it
applies it via `applyRoi(..., restartVideo=false)` so there is no redundant
video start/stop before the first long exposure.

