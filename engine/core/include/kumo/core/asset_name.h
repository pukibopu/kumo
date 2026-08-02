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

// True when `name` is one or more '/'-separated plain components (each
// passing isPlainAssetName on its own), at most `maxComponents` of them: e.g.
// "props/crate" with the default maxComponents=2. Rejects '\\' up front (a
// component-wise isPlainAssetName check alone would not catch a backslash
// hiding inside what looks like one component). Models may live under a
// category subdirectory (MA milestone); textures and environments stay
// single-component and keep using isPlainAssetName directly.
bool isPlainAssetPath(std::string_view name, int maxComponents = 2);

} // namespace kumo
