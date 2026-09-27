// camera_selector.h
//
// Choosing which camera to open when more than one ASI body is connected.
//
// The ZWO SDK enumerates the connected cameras (ASIGetNumOfConnectedCameras +
// ASIGetCameraProperty) WITHOUT opening any of them, so it works even while
// another process holds a camera. The app opens exactly one body; the choice
// happens BEFORE the main window appears (see main.cpp):
//
//   * no --camera flag, one camera connected  -> it is used silently (as before)
//   * no --camera flag, two or more connected -> a small selector dialog is
//     shown BEFORE the main window on EVERY launch (the choice is
//     deliberately not remembered): clicking a row (or Enter) confirms the
//     selected camera, Cancel/Esc exit without opening anything (there is no
//     Ok button - clicking already confirms)
//   * --camera N                              -> N is resolved against the
//     enumeration (a CameraID of a connected camera first, then the list
//     index) and used without a dialog
//
// The chosen CameraID is what the worker keeps as its preference: if the
// selected body re-enumerates and comes back, it is found by ID; if it is gone
// entirely, the first connected camera is opened instead (the app never
// dead-ends on a lost body while another one answers — the GUI is told which
// body actually opened by cameraReady and re-labels itself).
//
// Clicking a row in the dialog (or Enter) confirms that camera and the app
// opens — one gesture, not selection plus a second button.

#pragma once

#include <QString>
#include <vector>

// One connected camera as the SDK enumerates it (without opening it).
struct CameraOption
{
    int index = 0;     // enumeration index (what the dialog shows; --camera accepts it)
    int id = -1;       // the SDK CameraID — the stable identity the worker opens
    QString name;      // SDK Name (e.g. "ASI178MM")
    QString detail;    // one identifying line: "colour · 3096×2080"
};

// Enumerate the connected cameras without opening them. Empty when none is
// connected. A camera the SDK counts but cannot describe (ZWO driver missing
// or not bound, see docs/hardware.md) is skipped with a note on stderr.
std::vector<CameraOption> enumerateCameras();

// Resolve a --camera value against a connected enumeration: an exact
// CameraID of one of them first, then a list index (0-based).
// Returns the CameraID to open, or -1 when nothing matches.
int resolveCameraValue(const std::vector<CameraOption>& cams, const QString& value);

// Interactive pre-GUI selection: the small dark dialog listing every
// connected camera. Call only with two or more entries (headless runs never
// call it). Returns the chosen CameraID, or -1 when the user cancels.
// Must run on the GUI thread (it blocks until the dialog is closed).
// Clicking a row (or Enter) confirms that camera; Cancel/Esc cancel.
int showCameraSelector(const std::vector<CameraOption>& cams);
