// util.cpp
//
// Small shared helpers (see util.h).

#include "util.h"

#include <QDateTime>

#include <cstdlib>

QString timestamp()
{
    return QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
}

bool isDebugRecording()
{
    static const bool enabled = [] {
        const char* e = std::getenv("CAMDBG");
        return e && e[0] == '1';
    }();
    return enabled;
}
