#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace kumo::asset {

// Resolves a model name to the layout tools/fetch_assets.sh's fetch_pack and
// PolyHavenClient::fetchModel populate under `modelsDir` (MA milestone: glb
// single-file, self-named multi-file glTF, or a category subdirectory of
// either), trying in order:
//   <name>.glb
//   <name>/<name>.gltf
//   <name>/scene.gltf
// and, when `name` is exactly "<category>/<leaf>" (one '/'):
//   <category>/<leaf>.glb
//   <category>/<leaf>/<leaf>.gltf
//   <category>/<leaf>/scene.gltf
// First hit wins; an empty path means none of the six layouts exist. Callers
// must validate `name` (kumo::isPlainAssetPath) before calling this: it does
// no path-traversal checking of its own, and blindly joins `name` onto
// `modelsDir`.
std::filesystem::path resolveModelPath(const std::filesystem::path& modelsDir,
                                       std::string_view name);

// Enumerates every model id resolveModelPath would find under `modelsDir`:
// top-level "<id>.glb" files, top-level "<id>/" directories that are
// themselves a self-named or scene.gltf model, and (one level deeper) the
// same two shapes inside a category subdirectory, reported as
// "<category>/<id>". Sorted; empty when `modelsDir` does not exist. Used by
// `viewer --thumbnails` to discover the library without duplicating the
// resolver's own layout knowledge.
std::vector<std::string> listModelIds(const std::filesystem::path& modelsDir);

} // namespace kumo::asset
