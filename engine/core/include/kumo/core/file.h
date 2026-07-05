#pragma once

#include <expected>
#include <filesystem>
#include <string>

namespace kumo {
// Reads an entire file as bytes-as-text; error is a human-readable message.
std::expected<std::string, std::string> readTextFile(const std::filesystem::path& path);
} // namespace kumo
