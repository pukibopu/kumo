#pragma once

#include <string_view>

namespace kumo {

// True when `name` is safe to join onto an asset root with
// std::filesystem::operator/: non-empty, no '/' or '\\' (also rejects a
// POSIX absolute path, which operator/ would otherwise let replace the whole
// left-hand side), and not "." or ".." or starting with '.'. Shared by the
// agent tool layer (scene_tools.cpp) and the facade layer (EngineRuntime) so
// asset-name path traversal is rejected the same way in both places.
bool isPlainAssetName(std::string_view name);

} // namespace kumo
