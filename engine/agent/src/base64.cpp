#include "base64.h"

#include <cstdint>
#include <fstream>
#include <iterator>

namespace kumo::agent::detail {

std::string base64Encode(std::string_view data) {
    static constexpr char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);

    std::size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        const std::uint32_t chunk =
            (static_cast<std::uint32_t>(static_cast<unsigned char>(data[i])) << 16) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(data[i + 1])) << 8) |
            static_cast<std::uint32_t>(static_cast<unsigned char>(data[i + 2]));
        out.push_back(kTable[(chunk >> 18) & 0x3F]);
        out.push_back(kTable[(chunk >> 12) & 0x3F]);
        out.push_back(kTable[(chunk >> 6) & 0x3F]);
        out.push_back(kTable[chunk & 0x3F]);
    }

    const std::size_t remaining = data.size() - i;
    if (remaining == 1) {
        const std::uint32_t chunk = static_cast<std::uint32_t>(static_cast<unsigned char>(data[i]))
                                    << 16;
        out.push_back(kTable[(chunk >> 18) & 0x3F]);
        out.push_back(kTable[(chunk >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (remaining == 2) {
        const std::uint32_t chunk =
            (static_cast<std::uint32_t>(static_cast<unsigned char>(data[i])) << 16) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(data[i + 1])) << 8);
        out.push_back(kTable[(chunk >> 18) & 0x3F]);
        out.push_back(kTable[(chunk >> 12) & 0x3F]);
        out.push_back(kTable[(chunk >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

std::optional<std::string> base64EncodeFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        return std::nullopt;
    }
    std::string bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    if (stream.bad()) {
        return std::nullopt;
    }
    return base64Encode(bytes);
}

} // namespace kumo::agent::detail
