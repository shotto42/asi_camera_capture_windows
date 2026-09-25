// crash_handler.cpp
//
// Fatal-crash diagnostics (see crash_handler.h).
//
// POSIX: SA_SIGINFO handlers on the fatal signals + backtrace(3) symbols.
// Windows: a process-wide unhandled-exception filter + CaptureStackBackTrace
// (the CRT's signal()/sigaction() path has no backtrace support and SIGBUS
// does not exist); the report goes to stderr before the default handler
// takes over (WerFault dialog / .wer core).

#include "crash_handler.h"

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)

#include <windows.h>
#include <dbghelp.h>   // CaptureStackBackTrace (link: -ldbghelp)

namespace {
// Async-safe enough for a crash path: only WriteFile on fd 2 (stderr) and
// dbghelp's CaptureStackBackTrace (no allocation beyond the frame array).
LONG WINAPI crashHandler(EXCEPTION_POINTERS* ep)
{
    const char* name = "EXCEPTION (unknown)";
    DWORD code = ep ? ep->ExceptionRecord->ExceptionCode : 0;
    switch (code)
    {
        case EXCEPTION_ACCESS_VIOLATION: name = "EXCEPTION_ACCESS_VIOLATION (segmentation fault)"; break;
        case EXCEPTION_IN_PAGE_ERROR:    name = "EXCEPTION_IN_PAGE_ERROR (bus error)"; break;
        case EXCEPTION_ILLEGAL_INSTRUCTION: name = "EXCEPTION_ILLEGAL_INSTRUCTION"; break;
        default: break;   // float exceptions (0xC000008x) & co. keep the generic name
    }
    char head[160];
    std::snprintf(head, sizeof(head),
                  "\n===== CAMERA_APP CRASH REPORT =====\n%s\nexception code: 0x%08lx\n",
                  name, (unsigned long)code);
    std::fwrite(head, 1, std::strlen(head), stderr);
    if (ep && code == EXCEPTION_ACCESS_VIOLATION)
    {
        const void* faultAddr =
            reinterpret_cast<const void*>(ep->ExceptionRecord->ExceptionInformation[1]);
        char line[64];
        std::snprintf(line, sizeof(line), "faulting address: %p\n", faultAddr);
        std::fwrite(line, 1, std::strlen(line), stderr);
    }
    std::fwrite("backtrace:\n", 1, 11, stderr);
    void* frames[40];
    DWORD n = CaptureStackBackTrace(2, 40, frames, nullptr);
    for (DWORD i = 0; i < n; ++i)
    {
        char line[64];
        std::snprintf(line, sizeof(line), "  #%lu %p\n", (unsigned long)i, frames[i]);
        std::fwrite(line, 1, std::strlen(line), stderr);
    }
    std::fwrite("===== END CRASH REPORT =====\n", 1, 28, stderr);
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;   // keep the default handler (WerFault / core)
}
} // namespace

void installCrashHandlers()
{
    SetUnhandledExceptionFilter(crashHandler);
}

#else  // POSIX ----------------------------------------------------------------

#include <execinfo.h>
#include <unistd.h>

namespace {
void crashHandler(int sig, siginfo_t* info, void* /*uctx*/)
{
    int fd = 2;
    auto w = [&fd](const char* s) { ssize_t r = ::write(fd, s, std::strlen(s)); (void)r; };
    const char* name = "SIG?";
    switch (sig)
    {
        case SIGSEGV: name = "SIGSEGV (segmentation fault)"; break;
        case SIGBUS:  name = "SIGBUS (bus error)"; break;
        case SIGFPE:  name = "SIGFPE (floating-point exception)"; break;
        case SIGABRT: name = "SIGABRT (abort)"; break;
        default: break;
    }
    w("\n===== CAMERA_APP CRASH REPORT =====\n");
    w(name);
    w("\n");
    if (info && (sig == SIGSEGV || sig == SIGBUS))
    {
        w("faulting address: 0x");
        char h[18];
        uintptr_t a = (uintptr_t)info->si_addr;
        h[15] = 0; h[0] = '0'; h[1] = 'x';
        for (int i = 14; i >= 2; --i) { h[i] = "0123456789abcdef"[a & 0xF]; a >>= 4; }
        h[15] = '\n';
        ssize_t r = ::write(fd, h, 16); (void)r;
    }
    w("backtrace:\n");
    void* frames[40];
    int n = backtrace(frames, 40);
    backtrace_symbols_fd(frames, n, fd);   // async-signal-safe
    w("===== END CRASH REPORT =====\n");
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}
} // namespace

void installCrashHandlers()
{
    struct sigaction sa = {};
    sa.sa_sigaction = &crashHandler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
}

#endif  // _WIN32
