# Video recording (AGENTS.md §6.5)

### 6.5 Video recording

Two back-ends, chosen in `startRecording` by the bit depth **and** the `.ser`
flag (`videoIsSer_ = (bitDepth != 8) || cfg_.serMode`; 14-bit is always
`.ser`, and the 8-bit `.ser` entry sets `serMode`). A third axis, the camera,
picks the **channel count**: `startRecording` opens the writer with
`colorId = isColor ? kSerColorRgb(100) : kSerColorMono(0)` and logs
`[rec] START … colorId=… planes=…`. The frame *rate* and readout are unaffected
by colour: colour costs no extra readout time (§9), it costs 3× the file bytes
and the host demosaic on the writer side (§6.2 step 11). `--smoke` validates the
ColorID the camera implies, so a colour body recording a MONO `.ser` fails the
test rather than shipping.

**8-bit H.264 → MP4 (`GstVideoEncoder`)**
- Pipeline: `appsrc (BGR, is-live, block=false, bounded) ! videoconvert !
  x264enc (speed-preset=ultrafast, tune=zerolatency, bitrate=10000,
  profile=baseline) ! mp4mux ! filesink`. `bitrate` is the **ABR target in
  kbit/s** (GStreamer 1.26 property: default 2048, range 1–2048000), so
  `bitrate=10000` = a **10 Mbit/s** target — a ceiling, not a floor: x264 ABR
  does not pad, so easy-to-compress planetary footage lands far below it
  (measured ~0.17 Mbit/s on a mostly-static 1280×960@30 test scene through the
  same pipeline).
- **x264enc baseline profile is 8-bit only**, so the 16-bit readout is converted
  to 8-bit (`>> 8`) before encoding — the MP4 is always 8-bit.
- The pipeline is fed **BGR**: a mono body's frames are grey-tripled, a colour
  body's are the demosaiced RGB (`pushToWriter`, §6.2 step 11) — so its MP4 is a
  colour video. Verified on the ASI178MC: `--vtest --vtestser 0` at 1280×720
  gives a 1280×720 H.264 (Constrained Baseline) 30 fps MP4
  (`gst-discoverer-1.0` reports Depth 24).
- The bounded `appsrc` queue with `block=false` means `push()` **never blocks**:
  if the encoder falls behind, frames are dropped (and counted) instead of
  stalling the capture loop, and stop is always prompt.
- GStreamer 1.26 API notes that were already handled: `gst_app_src_push_buffer`
  takes ownership of the buffer (do **not** unref after push); the bus is read
  with `gst_bus_timed_pop_filtered` and a bounded deadline in `stop()`.

**8/14-bit → uncompressed `.ser` (`SerWriter`): MONO or interleaved RGB**
- The **LUCAM-RECORDER** SER format v3: a **178-byte header** followed by the
  raw frames (and an optional timestamp trailer we omit by leaving the
  DateTime fields 0). It is a *plain container* (not AVI/FFV1) — written with
  plain `stdio`, no GStreamer, so no new package plugins. A frame is **MONO**
  (`ColorID`=0, W×H samples) for a mono body and **RGB** (`ColorID`=100,
  three channels **per-pixel interleaved R,G,B** — `RGBRGB…`, *not* full
  planes `RRR…GGG…BBB`) for a colour body — `serPlanesForColorId()` is the one
  place that says how many channels a ColorID means (100/101 → 3, everything
  else → 1), and it is what the frame-size arithmetic, `validateSerFile` and
  the close-time log line all agree with.
- **Why interleaved is the only correct layout (verified against the readers,
  2026-09-23):** Siril's `src/io/ser.c` de-interleaves a ColorID-100 frame
  with `for (i = 0; j < rx*ry; i += 3, j++)` triples, its crop helper says
  "reorder the RGBRGB to RRGGBB and crop", and Siril's own writer emits
  interleaved (`dest = plane; dest += number_of_planes`); Ser-Player
  (cgarry/ser-player, PIPP `pipp_ser.cpp`) reads `r=*p++; g=*p++; b=*p++`
  per pixel. An earlier revision of this writer stored full R,G,B planes and
  blamed the players for showing a "3×3 grid of tiles" (2026-09-23) — that
  was WRONG: the 3×3 mosaic (each channel plane of the scene decoded
  compressed ~3×, tiled 3 wide × 3 bands) is exactly what a planar file
  renders under both readers; reproduced byte-for-byte. Byte COUNTS are
  identical for either order, so only a sample-order check can tell them
  apart — `--colourtest` pins the on-disk interleaved order and the R,G,B
  channel mapping end-to-end (`interleaved RGBRGB order in file bytes`,
  `.ser end-to-end` for all four patterns). Do not re-flip to planar.
- **Header** (little-endian): offset 0 `FileID` = the 14 bytes
  `"LUCAM-RECORDER"` (all 14 — do **not** null-terminate it); 14 `LuID`=0;
  18 `ColorID`=0 (MONO) **or 100 (RGB)**; 22 `LittleEndian`=**0** for our little-endian data —
  see the endianness note below; 26/30 `Width`/`Height`; 34
  `PixelDepthPerPlane` = **8** for 1-byte files, **16** for 2-byte files (the
  *container* width — never the effective depth; see the data-alignment note
  below);
  **38 `FrameCount`** (written 0 on open, **patched via `fseek(38)` on close**);
  42/82/122 Observer/Instrument/Telescope (40-byte, 0-padded ASCII); 162/170
  DateTime/DateTime_UTC = 0 (→ no timestamp trailer).
- **Frame data** from offset 178: `W*H*bpp*channels` bytes/frame (**top-down,
  no padding**; for RGB the channel samples are interleaved per pixel, R
  first), `bpp` = 1 for 8-bit, 2 for 9–16-bit. Verified byte-wise
  (`--colourtest` writes RGB `.ser` files, re-reads the interleaved order,
  and runs the Bayer→file end-to-end check; `--smoke` checks size
  `178 + count*planes*W*H*bpp` for both depths). The sensor reads out 14-bit
  **left-justified** in the 16-bit container (0–65528 = raw14<<2). `SerWriter`
  keeps the target depth's bits **left-justified (MSB-aligned)** in the container:
  `shift = 16 - bitDepth` → **14-bit: store `raw16` as-is** (top 14 bits, low 2
  bits 0; 2 B/px), **10-bit: `(raw16 >> 6) << 6`** (top 10 bits, low 6 bits 0;
  2 B/px), **8-bit: `raw16 >> 8`** (top 8 bits, 1 B/px). The app's live 8-bit
  `.ser` path feeds the camera's RAW8 (HSM) values straight in through
  `push8()` — no host shift; `raw16 >> 8` is `push()`'s generic 8-bit path.
- **Why the header declares 16, not the effective depth (verified against the
  spec and reader sources):** the Wilkens/Hahn SER v3 spec *text* prescribes LSB
  alignment for 9–16-bit (value in the low bits), but readers disagree on how to
  undo a declared sub-16 depth: Siril's `ser.c` takes the 16-bit word **as-is**
  (no shift) and its own writer *always* labels 2-byte files 16; SER.Lib reads
  the word as-is and normalises by `2^PixelDepthPerPlane - 1`. With the
  effective depth (14) declared, a left-justified value clips to a **white image**
  under such readers
  (65528/16383), and a spec-LSB value renders at 1/4…1/64 brightness under
  as-is readers. Declaring **16** with the value left-justified is the one
  combination every reader model (as-is, shift-by-`16-depth` = zero shift,
  depth-normalise) decodes to the full-range image — it is also exactly what
  Siril's own writer emits. The effective depth stays implicit in the data
  (14-bit tops out at 65528, in steps of 4).
- **The offset-22 "LittleEndian" field is written 0, not 1 (verified against
  Siril's source, `src/io/ser.h` + `ser.c`).** The Wilkens/Hahn spec *text*
  says the field is a `LittleEndian` boolean (1 = LE data), but the first SER
  programs misread it as a *BigEndian* flag (0 = LE data, 1 = BE data), and —
  as Siril's own comment documents — "later programs, like Siril and GoQat,
  have also decided to implement this header in opposite meaning to the
  specification" (`SER_LITTLE_ENDIAN = 0`). De-facto writers (SharpCap/
  FireCapture/PIPP/AutoStakkert, the SerCapture NINA plugin) emit LE data with
  field **0**. Siril's reader: field==1 → `be16_to_cpu()` (byte-swap every
  word), field==0 → `le16_to_cpu()`. Our original files wrote LE data with
  field=1 → every reader following the de-facto convention swapped the words:
  a 14-bit ramp became `0, 32768, 1, 32769, …` — a high-frequency checkerboard,
  i.e. the "noise" the user reported (8-bit ignores the field, which is why
  only it worked). Fix: `put32(22, 0)`; `validateSerFile` expects 0. 8-bit
  data is 1 byte/px and is unaffected by the field either way.
- `.ser` is **uncompressed**: files are large and the disk write is the
  bottleneck (full-res 14-bit ≈ 12.6 MB/frame; 8-bit ≈ 6.3 MB/frame). `push()` writes to a 4 MiB static buffer (`setvbuf`) so the
  capture loop is not stalled by disk latency; a failed `fwrite` is counted as
  a dropped frame.
- **Writer fps:** `startRecording` uses the *measured* camera rate (EMA) if it's
  below the target (post-BandWidth-fix, the sensor's RAW16 readout caps full-res
  at ~27.7 fps and 1920×1280 at ~46 fps — §9), clamped to `[1, target]`, so the
  file plays back at the correct speed. (For `.ser`, the frame *rate* in the
  file is simply the number of frames; there is no codec timestamp.)

