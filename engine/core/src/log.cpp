#include "kumo/core/log.h"

#include <cstdio>
#include <mutex>

namespace kumo {

namespace {

std::mutex logMutex;

const char* levelTag(LogLevel level) {
    switch (level) {
    case LogLevel::Debug:
        return "debug";
    case LogLevel::Info:
        return "info ";
    case LogLevel::Warn:
        return "warn ";
    case LogLevel::Error:
        return "error";
    }
    return "?";
}

} // namespace

void logMessage(LogLevel level, std::string_view message) {
    std::FILE* out = level >= LogLevel::Warn ? stderr : stdout;
    std::lock_guard lock(logMutex);
    std::fprintf(out, "[%s] %.*s\n", levelTag(level), static_cast<int>(message.size()),
                 message.data());
}

} // namespace kumo
