#include "kumo/core/log.h"

#include <atomic>
#include <cstdio>
#include <mutex>

namespace kumo {

namespace {

std::mutex logMutex;
std::atomic<bool> logAllToStderr{false};

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
    std::FILE* out =
        level >= LogLevel::Warn || logAllToStderr.load(std::memory_order_relaxed) ? stderr : stdout;
    std::lock_guard lock(logMutex);
    std::fprintf(out, "[%s] %.*s\n", levelTag(level), static_cast<int>(message.size()),
                 message.data());
}

void setLogAllToStderr(bool enabled) {
    logAllToStderr.store(enabled, std::memory_order_relaxed);
}

} // namespace kumo
