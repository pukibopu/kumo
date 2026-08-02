#include <kumo/core/asset_name.h>

#include <cstddef>

namespace kumo {

bool isPlainAssetName(std::string_view name) {
    if (name.empty() || name == "." || name == "..") {
        return false;
    }
    if (name.front() == '.') {
        return false;
    }
    return name.find('/') == std::string_view::npos && name.find('\\') == std::string_view::npos;
}

bool isPlainAssetPath(std::string_view name, int maxComponents) {
    if (name.empty() || maxComponents < 1 || name.find('\\') != std::string_view::npos) {
        return false;
    }
    int components = 1;
    std::size_t start = 0;
    for (;;) {
        const std::size_t slash = name.find('/', start);
        const std::string_view part = slash == std::string_view::npos
                                          ? name.substr(start)
                                          : name.substr(start, slash - start);
        if (!isPlainAssetName(part)) {
            return false;
        }
        if (slash == std::string_view::npos) {
            return true;
        }
        if (++components > maxComponents) {
            return false;
        }
        start = slash + 1;
    }
}

} // namespace kumo
