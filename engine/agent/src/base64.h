#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace kumo::agent::detail {

std::string base64Encode(std::string_view data);

// nullopt on any I/O failure; the caller degrades to text-only content.
std::optional<std::string> base64EncodeFile(const std::filesystem::path& path);

} // namespace kumo::agent::detail
