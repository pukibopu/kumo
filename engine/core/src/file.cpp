#include "kumo/core/file.h"

#include <format>
#include <fstream>
#include <sstream>
#include <utility>

namespace kumo {

std::expected<std::string, std::string> readTextFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return std::unexpected(std::format("failed to open file: {}", path.string()));
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    if (stream.bad()) {
        return std::unexpected(std::format("failed to read file: {}", path.string()));
    }
    return std::move(buffer).str();
}

} // namespace kumo
