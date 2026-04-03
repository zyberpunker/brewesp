#include "support/Logger.h"

#include <stdarg.h>

namespace {
bool gDebugEnabled = false;

void vlogf(bool enabled, const char* format, va_list args) {
    if (!enabled) {
        return;
    }

    char buffer[384];
    vsnprintf(buffer, sizeof(buffer), format, args);
    Serial.print(buffer);
}
}

void Logger::setDebugEnabled(bool enabled) {
    gDebugEnabled = enabled;
}

bool Logger::isDebugEnabled() {
    return gDebugEnabled;
}

void Logger::infof(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vlogf(true, format, args);
    va_end(args);
}

void Logger::warnf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vlogf(true, format, args);
    va_end(args);
}

void Logger::debugf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vlogf(gDebugEnabled, format, args);
    va_end(args);
}

void Logger::info(const char* message) {
    if (message == nullptr) {
        return;
    }
    Serial.println(message);
}

void Logger::warn(const char* message) {
    if (message == nullptr) {
        return;
    }
    Serial.println(message);
}

void Logger::debug(const char* message) {
    if (!gDebugEnabled || message == nullptr) {
        return;
    }
    Serial.println(message);
}
