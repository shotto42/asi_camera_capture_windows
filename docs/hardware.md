# Hardware / environment (AGENTS.md §4)

## 4. Hardware / environment

- **Cameras:** an ASI178MM (mono) and, attached to the development machine now,
  an **ASI178MC** (colour): `IsColorCam=1`, `BayerPattern=0` (RGGB),
  `BitDepth=14`, formats {RAW8, RGB24, Y8, RAW16}, bins 1–4, gain 0–510
  (default 210), exposure 32 µs…2000 s, BandWidth 40–100 (persists 60; the app
  raises it to 100 on open), HighSpeedMode present, and — measured 2026-09-23 with `camera_probe` —
  `WB_R` **1..99 (factory default 70)** and `WB_B` **1..99 (factory default 90)**,
  both writable and **both auto-capable** (the camera's own AWB): the app exposes
  them as the colour-only white-balance controls, **AWB on by default** (§1, §6.9,
  §7). Meaningless on a mono body (no Bayer colour to balance), and the app never
  touches them there.
  On the MC, `ASIGetControlCaps` **works** (it fails on the MM — §6.8), and ROI
  widths must be divisible by 8 (1548/774/618/516 are rejected; 1032×692,
  1920×1080, 1280×720, 2080×2080 are accepted) — which is why
  `CameraCaps::roiCandidates()` aligns widths to 8 and heights to 4 (§6.2).
   The USB bus/device numbers change on re-plug, so never hardcode them - the
   app only uses the SDK's own enumeration, which does not.
- **Several cameras at once:** the app opens ONE body at a time. With two or
   more connected, a plain interactive launch shows the pre-GUI selector
   dialog (or `--camera N` selects without it — §2, §7); the worker remembers
   the opened CameraID for every reconnect (a USB re-enumeration keeps the ID;
   first-connected camera as fallback, §6.2). `camera_probe` dumps every
   connected body.
- **"App waits for camera" = the ZWO driver was never installed.** The SDK
   on Windows needs a bound driver to do *anything*: a plugged-in camera shows
  `USB\VID_03C3&PID_178A` with **Problem 28 (CM_PROB_FAILED_INSTALL)**, class
  "Unbekannt/Unknown", no driver bound — and then
  `ASIGetNumOfConnectedCameras()` returns 0. SharpCap hits the same wall (it
  bundles `ASICamera2.dll` but no driver). ZWO states it outright: Windows
  users must install the native driver.
  - **One-time fix:** the official signed installer (SUZHOU ZWO CO., LTD.):
    `https://dl.zwoastro.com/software?app=AsiCameraDriver&platform=windows64&region=Overseas`
    (v3.28.0.0, NSIS; the `platform=windows86` variant is the 32-bit-host
    build). Run it elevated (`/S` for silent). It installs
    `oem178.inf` (`asicamusb3.inf`, "ZWO ASI178MC Camera", class Image).
    Afterwards the device reads *started/Gestartet* and the SDK enumerates
    it. Verified 2026-09-25: after installing, a small probe printing
    `ASIGetNumOfConnectedCameras` + `ASIGetCameraProperty` reported
    `1 / ZWO ASI178MC 3096×2080 colour USB3`, and the app's live preview
    (RGB demosaic, red clipping marks, histogram, status line) rendered
    correctly.
  - **Check without the app:** a ~20-line probe linked against
    `ASICamera2.lib` calling `ASIGetNumOfConnectedCameras()` — if it prints
    `0`, it is a driver/USB problem, not an app problem (the GUI launches
    fine without a camera and just shows "Waiting for camera…").
- **Only one process can hold the camera at a time.** Stop the app (close the
  window, or `taskkill /IM camera_app.exe`) before running any other binary
  that opens the camera.
- **Camera bad-state after a kill:** occasionally, right after killing the app,
  the camera enters a state where `ASIOpenCamera`/`ASIInitCamera` return error
  `2` (INVALID_ID) in a *fresh* process. Relaunching the app (or waiting a few
  seconds) recovers it. This is a USB/SDK quirk, not a code bug.
- **Dependencies**: Qt6 (Widgets), OpenCV 4.x, GStreamer 1.x, MSVC 2022. All
  are **vendored** in `third_party/` / `ASI_SDK/` — see
  "Build system notes" below (§5).

