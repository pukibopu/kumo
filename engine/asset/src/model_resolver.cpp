#include <kumo/asset/model_resolver.h>

#include <algorithm>
#include <system_error>

namespace kumo::asset {

namespace {

namespace fs = std::filesystem;

bool isRegularFile(const fs::path& path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec);
}

bool isDirectory(const fs::path& path) {
    std::error_code ec;
    return fs::is_directory(path, ec);
}

// A directory is itself a (self-named or Poly-Haven-style) model when it
// holds either "<dirName>.gltf" or "scene.gltf" directly.
bool isModelDirectory(const fs::path& dir) {
    return isRegularFile(dir / (dir.filename().string() + ".gltf")) ||
           isRegularFile(dir / "scene.gltf");
}

} // namespace

std::filesystem::path resolveModelPath(const std::filesystem::path& modelsDir,
                                       std::string_view name) {
    // Split category/leaf BEFORE building candidates: naive string appends
    // would both produce a garbage self-named path (<cat>/<leaf>/<cat>/
    // <leaf>.gltf) and check scene.gltf ahead of the documented self-named
    // priority. More than one '/' is not a model name at all.
    const std::string n(name);
    const std::size_t slash = n.find('/');
    if (slash != std::string::npos && n.find('/', slash + 1) != std::string::npos) {
        return {};
    }
    const fs::path base = slash == std::string::npos ? modelsDir : modelsDir / n.substr(0, slash);
    const std::string leaf = slash == std::string::npos ? n : n.substr(slash + 1);

    const fs::path candidates[] = {base / (leaf + ".glb"), base / leaf / (leaf + ".gltf"),
                                   base / leaf / "scene.gltf"};
    for (const fs::path& candidate : candidates) {
        if (isRegularFile(candidate)) {
            return candidate;
        }
    }
    return {};
}

std::vector<std::string> listModelIds(const std::filesystem::path& modelsDir) {
    std::vector<std::string> ids;
    std::error_code ec;
    if (!isDirectory(modelsDir)) {
        return ids;
    }
    for (const auto& entry : fs::directory_iterator(modelsDir, ec)) {
        // Dot-prefixed entries are never a model id (e.g. PolyHavenClient::
        // fetchModel's ".partial_<id>" temp directory, left behind only if a
        // fetch is interrupted mid-download rather than failing cleanly).
        if (entry.path().filename().string().starts_with('.')) {
            continue;
        }
        if (entry.is_regular_file(ec)) {
            if (entry.path().extension() == ".glb") {
                ids.push_back(entry.path().stem().string());
            }
            continue;
        }
        if (!entry.is_directory(ec)) {
            continue;
        }
        const fs::path dir = entry.path();
        if (isModelDirectory(dir)) {
            ids.push_back(dir.filename().string());
            continue;
        }
        // Not a model itself: treat as a category directory and look one
        // level deeper for its members.
        const std::string category = dir.filename().string();
        std::error_code subEc;
        for (const auto& sub : fs::directory_iterator(dir, subEc)) {
            if (sub.is_regular_file(subEc)) {
                if (sub.path().extension() == ".glb") {
                    ids.push_back(category + "/" + sub.path().stem().string());
                }
                continue;
            }
            if (sub.is_directory(subEc) && isModelDirectory(sub.path())) {
                ids.push_back(category + "/" + sub.path().filename().string());
            }
        }
    }
    std::sort(ids.begin(), ids.end());
    // One id can exist in several supported layouts at once (Avocado.glb plus
    // Avocado/scene.gltf); callers want each id exactly once.
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

} // namespace kumo::asset
