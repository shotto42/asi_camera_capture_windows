// util.h
//
// Small shared helpers.
#pragma once

#include <QString>

// "yyyyMMdd_HHmmss" timestamp for output file names.
QString timestamp();

// Temporary debug logging for the recording/capture paths (set CAMDBG=1).
// Read once; the environment cannot change while the app runs.
bool isDebugRecording();
