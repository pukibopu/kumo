#include <kumo/core/asset_name.h>

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

} // namespace kumo
