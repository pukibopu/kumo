#pragma once

#include <format>
#include <string_view>
#include <utility>

namespace kumo {

enum class LogLevel { Debug, Info, Warn, Error };

void logMessage(LogLevel level, std::string_view message);

// Routes every level to stderr; stdout then belongs to a wire protocol (MCP).
void setLogAllToStderr(bool enabled);

template <typename... Args> void logDebug(std::format_string<Args...> fmt, Args&&... args) {
    logMessage(LogLevel::Debug, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args> void logInfo(std::format_string<Args...> fmt, Args&&... args) {
    logMessage(LogLevel::Info, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args> void logWarn(std::format_string<Args...> fmt, Args&&... args) {
    logMessage(LogLevel::Warn, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args> void logError(std::format_string<Args...> fmt, Args&&... args) {
    logMessage(LogLevel::Error, std::format(fmt, std::forward<Args>(args)...));
}

} // namespace kumo
