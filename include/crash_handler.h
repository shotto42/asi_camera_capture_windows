// crash_handler.h
//
// Fatal-crash diagnostics. On SIGSEGV/SIGBUS/SIGFPE/SIGABRT write the signal,
// the faulting address and a native backtrace to stderr (async-signal-safe
// primitives only), then restore the default handler and re-raise so the core
// dump is still produced. This pins the crashing module/offset even on a
// machine without gdb — the backtrace shows whether the fault is inside
// libASICamera2.so, libopencv, libQt, or camera_app itself.
#pragma once

// Must be installed before any Qt/SDK init.
void installCrashHandlers();
