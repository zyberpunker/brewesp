#pragma once

#include <Arduino.h>

namespace Logger {
void setDebugEnabled(bool enabled);
bool isDebugEnabled();
void infof(const char* format, ...);
void warnf(const char* format, ...);
void debugf(const char* format, ...);
void info(const char* message);
void warn(const char* message);
void debug(const char* message);
}

#define LOG_INFO(...) ::Logger::infof(__VA_ARGS__)
#define LOG_WARN(...) ::Logger::warnf(__VA_ARGS__)
#define LOG_DEBUG(...) ::Logger::debugf(__VA_ARGS__)
#define LOG_INFO_MSG(message) ::Logger::info(message)
#define LOG_WARN_MSG(message) ::Logger::warn(message)
#define LOG_DEBUG_MSG(message) ::Logger::debug(message)
