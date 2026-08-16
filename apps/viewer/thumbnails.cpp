// Headless batch mode (MA milestone; MR extends it): `viewer --thumbnails`
// renders a preview PNG per model/texture-set/env under the asset library and
// (re)writes index.json, without opening a window (mirrors
// tests/test_gpu_contract.cpp's windowless gpu::createDevice() pattern) or
// going through EngineRuntime (this tool needs to swap scenes far more often
// than EngineRuntime's one-scene-per-process contract allows). `viewer
// --index` is its superset: also indexes recipes (rendered on a standard
// sphere) and scene-spec templates, captions thumbnails through the
// configured vision model and embeds every entry for asset_search.

// Private kumo_agent headers (engine/agent/src).
#include "asset_index.h"
#include "base64.h"
#include "embedding_client.h"
#include "surface_template.h"

#include <kumo/agent/chat.h>
#include <kumo/agent/config.h>
#include <kumo/agent/http_provider.h>
#include <kumo/asset/asset.h>
#include <kumo/asset/model_resolver.h>
#include <kumo/asset/primitives.h>
#include <kumo/asset/procedural_sky.h>
#include <kumo/core/file.h>
#include <kumo/core/log.h>
#include <kumo/gpu/gpu.h>
#include <kumo/math/math.h>
#include <kumo/renderer/forward_renderer.h>
#include <kumo/renderer/ibl.h>
#include <kumo/scene/scene.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace kumo;
namespace fs = std::filesystem;

namespace {

constexpr std::uint32_t kThumbSize = 256;
constexpr std::uint64_t kInstancingTriangleBudget = 20000;
constexpr gpu::TextureFormat kOutputFormat = gpu::TextureFormat::BGRA8Unorm;

// --- shared helpers ---------------------------------------------------------

// The newest write time among a model's root file and everything in its
// enclosing directory when that directory is model- or pack-scoped: multi-file
// glTFs change through their .bin/textures without touching scene.gltf, and
// pack glbs (Kenney survival) reference a shared sibling Textures/colormap.png.
// A top-level single-file glb keeps file-only granularity — its parent is the
// whole models dir, where "newest sibling" would mean any new model
// invalidating every thumbnail.
fs::file_time_type newestRelevantTime(const fs::path& modelPath, const fs::path& modelsDir) {
    std::error_code ec;
    fs::file_time_type newest = fs::last_write_time(modelPath, ec);
    const fs::path parent = modelPath.parent_path();
    if (parent == modelsDir) {
        return newest;
    }
    for (auto it = fs::recursive_directory_iterator(parent, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code fileEc;
        if (!it->is_regular_file(fileEc)) {
            continue;
        }
        const fs::file_time_type t = fs::last_write_time(it->path(), fileEc);
        if (!fileEc && t > newest) {
            newest = t;
        }
    }
    return newest;
}

bool isFresh(const fs::path& thumbnail, const fs::path& source) {
    std::error_code ec;
    if (!fs::exists(thumbnail, ec)) {
        return false;
    }
    const fs::file_time_type thumbTime = fs::last_write_time(thumbnail, ec);
    if (ec) {
        return false;
    }
    const fs::file_time_type sourceTime = fs::last_write_time(source, ec);
    if (ec) {
        return false;
    }
    return thumbTime >= sourceTime;
}

// Model overload: freshness against the newest dependency, not just the root.
bool isFreshModel(const fs::path& thumbnail, const fs::path& modelPath, const fs::path& modelsDir) {
    std::error_code ec;
    if (!fs::exists(thumbnail, ec)) {
        return false;
    }
    const fs::file_time_type thumbTime = fs::last_write_time(thumbnail, ec);
    if (ec) {
        return false;
    }
    return thumbTime >= newestRelevantTime(modelPath, modelsDir);
}

// assets/.thumbnails/<kind>/<id>.png, id's category structure preserved as
// real subdirectories (a categorized model id like "nature/bridge_stone"
// keeps its "/"); a kind subdirectory (not spelled out verbatim in the
// milestone note) avoids a texture set and an env sharing the same stem
// colliding on one filename. Flattening "/" to "_" instead would collide the
// other way ("nature/bridge_stone" vs. a hypothetical flat "nature_bridge_
// stone"), so callers must create_directories the parent before writing.
fs::path thumbnailPath(const fs::path& assetDir, std::string_view kindDir, const std::string& id) {
    return assetDir / ".thumbnails" / kindDir / (id + ".png");
}

std::string relativeThumbnail(const fs::path& assetDir, const fs::path& path) {
    std::error_code ec;
    const fs::path rel = fs::relative(path, assetDir, ec);
    return ec ? path.string() : rel.generic_string();
}

struct PackInfo {
    std::string category;
    std::string style;
    std::string source;
    std::string license;
};

// <dir>/pack.json, written by tools/fetch_assets.sh's fetch_pack. Absent for
// every source that has no manifest convention (the original single-file
// models/textures/env, ambientCG texture sets, Poly-Haven-fetched models).
std::optional<PackInfo> readPackJson(const fs::path& dir) {
    std::ifstream in(dir / "pack.json", std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const nlohmann::json parsed = nlohmann::json::parse(buffer.str(), nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return std::nullopt;
    }
    auto str = [&](const char* key) -> std::string {
        const auto it = parsed.find(key);
        return it != parsed.end() && it->is_string() ? it->get<std::string>() : std::string();
    };
    return PackInfo{.category = str("category"),
                    .style = str("style"),
                    .source = str("source"),
                    .license = str("license")};
}

// Style/category/source/license: pack.json when the asset has one; otherwise
// style defaults to "realistic" (the pre-MA sources -- Poly Haven, ambientCG,
// Khronos -- are all photoreal), category/source/license stay empty.
void fillPackMetadata(agent::AssetIndexEntry& entry, const fs::path& dir,
                      const std::string& categoryFromPath) {
    entry.style = "realistic";
    entry.category = categoryFromPath;
    if (std::optional<PackInfo> pack = readPackJson(dir)) {
        if (!pack->category.empty()) {
            entry.category = pack->category;
        }
        if (!pack->style.empty()) {
            entry.style = pack->style;
        }
        entry.source = pack->source;
        entry.license = pack->license;
    }
}

struct Tally {
    int rendered = 0;
    int skipped = 0;
    int failed = 0;
    // Ids whose thumbnail was (re)rendered this run: their carried-over
    // caption is stale and must be regenerated.
    std::vector<std::string> rerendered;
};

// What runs beyond the MA thumbnail/metadata baseline; --thumbnails leaves
// everything off, --index turns it all on (minus captions with --no-captions).
struct IndexOptions {
    bool force = false;
    bool recipesAndSpecs = false;
    bool captions = false;
    bool embeddings = false;
};

// --- models ------------------------------------------------------------------

math::Aabb computeWorldAabb(const asset::SceneAsset& sceneAsset) {
    math::Aabb box{math::float3(std::numeric_limits<float>::max()),
                   math::float3(std::numeric_limits<float>::lowest())};
    bool any = false;
    for (const asset::NodeInstance& node : sceneAsset.nodes) {
        if (node.meshIndex < 0 ||
            static_cast<std::size_t>(node.meshIndex) >= sceneAsset.meshes.size()) {
            continue;
        }
        const math::Aabb worldBox = math::transformAabb(
            sceneAsset.meshes[static_cast<std::size_t>(node.meshIndex)].localAabb,
            node.worldTransform);
        box.min = glm::min(box.min, worldBox.min);
        box.max = glm::max(box.max, worldBox.max);
        any = true;
    }
    if (!any) {
        return math::Aabb{{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
    }
    return box;
}

std::uint64_t countTriangles(const asset::SceneAsset& sceneAsset) {
    std::uint64_t total = 0;
    for (const asset::MeshData& mesh : sceneAsset.meshes) {
        total += mesh.indices.size() / 3;
    }
    return total;
}

asset::ProceduralSkyDesc studioSkyDesc() {
    // Mirrors scene_tools.cpp's environment_set "studio" preset: a neutral
    // gradient with no visible sun disc, duplicated here rather than shared
    // since that preset table is private to the agent tool layer.
    asset::ProceduralSkyDesc desc;
    desc.zenithColor = {0.85f, 0.85f, 0.85f};
    desc.horizonColor = {0.6f, 0.6f, 0.6f};
    desc.groundColor = {0.25f, 0.25f, 0.25f};
    desc.sunColor = {1.0f, 1.0f, 1.0f};
    desc.sunIntensity = 0.0f;
    return desc;
}

scene::Light keyLight() {
    constexpr float kAzDeg = 45.0f;
    constexpr float kElDeg = 35.0f;
    const float az = math::radians(kAzDeg);
    const float el = math::radians(kElDeg);
    scene::Light light;
    light.type = scene::LightType::Directional;
    light.color = {1.0f, 1.0f, 1.0f};
    light.intensity = 3.0f;
    light.direction =
        -math::float3{std::cos(el) * std::sin(az), std::sin(el), std::cos(el) * std::cos(az)};
    return light;
}

void fitCamera(scene::Camera& camera, const math::float3& center, float diagonal) {
    constexpr float kAzDeg = 30.0f;
    constexpr float kElDeg = 20.0f;
    const float az = math::radians(kAzDeg);
    const float el = math::radians(kElDeg);
    const float distance = std::max(diagonal, 0.01f) * 1.8f;
    const math::float3 dir{std::cos(el) * std::sin(az), std::sin(el), std::cos(el) * std::cos(az)};
    camera.position = center + dir * distance;
    camera.lookAt(center);
}

bool renderSceneToPng(gpu::Device& device, renderer::ForwardRenderer& renderer,
                      const scene::Scene& world, const fs::path& outputPath) {
    gpu::Ptr<gpu::Texture> output = device.createTexture({
        .size = {kThumbSize, kThumbSize},
        .format = kOutputFormat,
        .usage = gpu::TextureUsage::RenderTarget | gpu::TextureUsage::CopySrc,
    });
    if (!output) {
        return false;
    }
    gpu::Ptr<gpu::CommandEncoder> encoder = device.queue().createCommandEncoder();
    if (!encoder) {
        return false;
    }
    renderer.render(*encoder, world, output.get());
    encoder->finishAndSubmit();
    device.queue().waitIdle();

    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kThumbSize) * kThumbSize * 4);
    if (!device.queue().readTexture(*output, pixels.data(), kThumbSize * 4,
                                    {kThumbSize, kThumbSize})) {
        return false;
    }
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        std::swap(pixels[i], pixels[i + 2]); // BGRA -> RGBA
    }
    std::error_code ec;
    fs::create_directories(outputPath.parent_path(), ec);
    return asset::writePng(outputPath, kThumbSize, kThumbSize, pixels.data());
}

// One entry per model id (as `asset::listModelIds` enumerates them, including
// the one-category-level convention `fetch_pack`/PolyHavenClient::fetchModel
// use). Dimensions/triangle metadata is always recomputed (cheap, CPU-only);
// the thumbnail render itself is skipped when its PNG is already newer than
// the model file, unless `force`.
void processModels(gpu::Device& device, renderer::ForwardRenderer& renderer,
                   const renderer::ibl::Environment& environment, const fs::path& assetDir,
                   bool force, agent::AssetIndex& index, Tally& tally) {
    const fs::path modelsDir = assetDir / "models";
    for (const std::string& id : asset::listModelIds(modelsDir)) {
        const fs::path modelPath = asset::resolveModelPath(modelsDir, id);
        if (modelPath.empty()) {
            logError("thumbnails: could not resolve model '{}'", id);
            ++tally.failed;
            continue;
        }
        std::expected<asset::SceneAsset, std::string> sceneAsset = asset::loadGltf(modelPath);
        if (!sceneAsset.has_value()) {
            logError("thumbnails: failed to load model '{}': {}", id, sceneAsset.error());
            ++tally.failed;
            continue;
        }

        const math::Aabb box = computeWorldAabb(*sceneAsset);
        const math::float3 center = (box.min + box.max) * 0.5f;
        const math::float3 dims = box.max - box.min;
        const float diagonal = math::length(dims);

        agent::AssetIndexEntry entry;
        entry.id = id;
        entry.kind = agent::AssetIndexKind::Model;
        entry.dimensions = dims;
        entry.triangles = countTriangles(*sceneAsset);
        entry.instancingOk = *entry.triangles < kInstancingTriangleBudget;
        const std::size_t slash = id.find('/');
        fillPackMetadata(entry,
                         slash == std::string::npos ? modelsDir : modelsDir / id.substr(0, slash),
                         slash == std::string::npos ? std::string() : id.substr(0, slash));

        const fs::path pngPath = thumbnailPath(assetDir, "models", id);
        if (force || !isFreshModel(pngPath, modelPath, modelsDir)) {
            if (!renderer.loadScene(*sceneAsset, environment)) {
                logError("thumbnails: loadScene failed for model '{}'", id);
                ++tally.failed;
            } else {
                scene::Scene world;
                for (const asset::NodeInstance& node : sceneAsset->nodes) {
                    if (node.meshIndex < 0) {
                        continue;
                    }
                    const math::Trs trs = math::decomposeTrs(node.worldTransform);
                    scene::Entity nodeEntity;
                    nodeEntity.transform = {trs.translation, trs.rotation, trs.scale};
                    nodeEntity.meshIndex = node.meshIndex;
                    nodeEntity.materialIndex =
                        sceneAsset->meshes[static_cast<std::size_t>(node.meshIndex)].materialIndex;
                    world.entities.insert(nodeEntity);
                }
                world.addLight(keyLight());
                fitCamera(world.camera, center, diagonal);
                if (renderSceneToPng(device, renderer, world, pngPath)) {
                    ++tally.rendered;
                    tally.rerendered.push_back(id);
                } else {
                    logError("thumbnails: render failed for model '{}'", id);
                    ++tally.failed;
                }
            }
        } else {
            ++tally.skipped;
        }
        if (fs::exists(pngPath)) {
            entry.thumbnail = relativeThumbnail(assetDir, pngPath);
        }
        index.entries.push_back(std::move(entry));
    }
}

// --- texture sets ------------------------------------------------------------

constexpr std::array<std::string_view, 5> kTextureMapStems{"albedo", "normal", "roughness",
                                                           "metalness", "ao"};

bool findMapFile(const fs::path& dir, std::string_view stem, fs::path& out) {
    for (std::string_view ext : {".png", ".jpg"}) {
        fs::path candidate = dir / (std::string(stem) + std::string(ext));
        std::error_code ec;
        if (fs::exists(candidate, ec)) {
            out = candidate;
            return true;
        }
    }
    return false;
}

void processTextureSets(const fs::path& assetDir, bool force, agent::AssetIndex& index,
                        Tally& tally) {
    const fs::path texturesDir = assetDir / "textures";
    std::error_code ec;
    if (!fs::is_directory(texturesDir, ec)) {
        return;
    }
    std::vector<std::string> ids;
    for (const auto& entry : fs::directory_iterator(texturesDir, ec)) {
        if (entry.is_directory(ec)) {
            ids.push_back(entry.path().filename().string());
        }
    }
    std::sort(ids.begin(), ids.end());

    for (const std::string& id : ids) {
        const fs::path setDir = texturesDir / id;
        fs::path albedoPath;
        if (!findMapFile(setDir, "albedo", albedoPath)) {
            continue; // not a recognized texture set (mirrors collectTextureSets)
        }

        agent::AssetIndexEntry entry;
        entry.id = id;
        entry.kind = agent::AssetIndexKind::Texture;
        fillPackMetadata(entry, setDir, std::string());

        std::uint32_t maxResolution = 0;
        for (std::string_view stem : kTextureMapStems) {
            fs::path mapPath;
            if (!findMapFile(setDir, stem, mapPath)) {
                continue;
            }
            entry.maps.emplace_back(stem);
            // loadImage-then-discard is acceptable at library scale (a few
            // dozen texture sets), simpler than a stb-info-only probe.
            if (std::expected<asset::TextureData, std::string> image = asset::loadImage(mapPath);
                image.has_value()) {
                maxResolution = std::max({maxResolution, image->width, image->height});
            }
        }
        if (maxResolution > 0) {
            entry.resolution = maxResolution;
        }

        const fs::path pngPath = thumbnailPath(assetDir, "textures", id);
        if (force || !isFresh(pngPath, albedoPath)) {
            std::expected<asset::TextureData, std::string> albedo = asset::loadImage(albedoPath);
            if (!albedo.has_value()) {
                logError("thumbnails: failed to load albedo for texture set '{}': {}", id,
                         albedo.error());
                ++tally.failed;
            } else {
                const asset::DownscaledImage down = asset::downscaleRgba(
                    albedo->rgba.data(), albedo->width, albedo->height, kThumbSize);
                std::error_code mkdirEc;
                fs::create_directories(pngPath.parent_path(), mkdirEc);
                if (asset::writePng(pngPath, down.width, down.height, down.rgba.data())) {
                    ++tally.rendered;
                    tally.rerendered.push_back(id);
                } else {
                    logError("thumbnails: failed to write thumbnail for texture set '{}'", id);
                    ++tally.failed;
                }
            }
        } else {
            ++tally.skipped;
        }
        if (fs::exists(pngPath)) {
            entry.thumbnail = relativeThumbnail(assetDir, pngPath);
        }
        index.entries.push_back(std::move(entry));
    }
}

// --- environments -------------------------------------------------------------

std::uint8_t tonemapChannel(float value) {
    const float mapped = value / (value + 1.0f); // crude Reinhard; recognizable, not accurate
    const float gamma = std::pow(std::max(mapped, 0.0f), 1.0f / 2.2f);
    return static_cast<std::uint8_t>(std::clamp(gamma, 0.0f, 1.0f) * 255.0f + 0.5f);
}

bool renderEnvThumbnail(const asset::HdrImage& hdr, const fs::path& outputPath) {
    const std::uint32_t outWidth = kThumbSize;
    const std::uint32_t outHeight =
        std::max<std::uint32_t>(1, kThumbSize * hdr.height / std::max<std::uint32_t>(1, hdr.width));
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(outWidth) * outHeight * 4);
    for (std::uint32_t y = 0; y < outHeight; ++y) {
        const std::uint32_t srcY = std::min(hdr.height - 1, y * hdr.height / outHeight);
        for (std::uint32_t x = 0; x < outWidth; ++x) {
            const std::uint32_t srcX = std::min(hdr.width - 1, x * hdr.width / outWidth);
            const std::size_t srcIdx = (static_cast<std::size_t>(srcY) * hdr.width + srcX) * 4;
            const std::size_t dstIdx = (static_cast<std::size_t>(y) * outWidth + x) * 4;
            rgba[dstIdx + 0] = tonemapChannel(hdr.rgba[srcIdx + 0]);
            rgba[dstIdx + 1] = tonemapChannel(hdr.rgba[srcIdx + 1]);
            rgba[dstIdx + 2] = tonemapChannel(hdr.rgba[srcIdx + 2]);
            rgba[dstIdx + 3] = 255;
        }
    }
    std::error_code ec;
    fs::create_directories(outputPath.parent_path(), ec);
    return asset::writePng(outputPath, outWidth, outHeight, rgba.data());
}

void processEnvironments(const fs::path& assetDir, bool force, agent::AssetIndex& index,
                         Tally& tally) {
    const fs::path envDir = assetDir / "env";
    std::error_code ec;
    if (!fs::is_directory(envDir, ec)) {
        return;
    }
    std::vector<std::string> ids;
    for (const auto& entry : fs::directory_iterator(envDir, ec)) {
        if (entry.is_regular_file(ec) && entry.path().extension() == ".hdr") {
            ids.push_back(entry.path().stem().string());
        }
    }
    std::sort(ids.begin(), ids.end());

    for (const std::string& id : ids) {
        const fs::path hdrPath = envDir / (id + ".hdr");
        agent::AssetIndexEntry entry;
        entry.id = id;
        entry.kind = agent::AssetIndexKind::Environment;
        entry.style = "realistic"; // no per-file pack.json convention for env

        const fs::path pngPath = thumbnailPath(assetDir, "env", id);
        if (force || !isFresh(pngPath, hdrPath)) {
            std::expected<asset::HdrImage, std::string> hdr = asset::loadHdr(hdrPath);
            if (!hdr.has_value()) {
                logError("thumbnails: failed to load env '{}': {}", id, hdr.error());
                ++tally.failed;
            } else if (!renderEnvThumbnail(*hdr, pngPath)) {
                logError("thumbnails: failed to write thumbnail for env '{}'", id);
                ++tally.failed;
            } else {
                ++tally.rendered;
                tally.rerendered.push_back(id);
            }
        } else {
            ++tally.skipped;
        }
        if (fs::exists(pngPath)) {
            entry.thumbnail = relativeThumbnail(assetDir, pngPath);
        }
        index.entries.push_back(std::move(entry));
    }
}

// --- recipes (MR) ------------------------------------------------------------

fs::file_time_type newestOf(std::initializer_list<fs::path> paths) {
    fs::file_time_type newest = fs::file_time_type::min();
    for (const fs::path& path : paths) {
        std::error_code ec;
        const fs::file_time_type t = fs::last_write_time(path, ec);
        if (!ec && t > newest) {
            newest = t;
        }
    }
    return newest;
}

std::string jsonString(const nlohmann::json& obj, const char* key) {
    const auto it = obj.find(key);
    return it != obj.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::vector<std::string> jsonStringArray(const nlohmann::json& obj, const char* key) {
    std::vector<std::string> out;
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_array()) {
        return out;
    }
    for (const auto& item : *it) {
        if (item.is_string()) {
            out.push_back(item.get<std::string>());
        }
    }
    return out;
}

// Default-valued decls from the recipe metadata; entries that do not parse
// are skipped (the recipe still renders, at zeroed params).
std::vector<agent::surface::ParamDecl> recipeParamDecls(const nlohmann::json& meta) {
    std::vector<agent::surface::ParamDecl> decls;
    const auto paramsIt = meta.find("params");
    if (paramsIt == meta.end() || !paramsIt->is_array()) {
        return decls;
    }
    for (const nlohmann::json& param : *paramsIt) {
        if (!param.is_object()) {
            continue;
        }
        agent::surface::ParamDecl decl;
        decl.name = jsonString(param, "name");
        if (decl.name.empty()) {
            continue;
        }
        decl.isVec4 = jsonString(param, "type") == "vec4";
        const auto valueIt = param.find("value");
        if (valueIt != param.end()) {
            if (decl.isVec4 && valueIt->is_array() && valueIt->size() == 4) {
                for (std::size_t c = 0; c < 4; ++c) {
                    if ((*valueIt)[c].is_number()) {
                        decl.value[c] = (*valueIt)[c].get<float>();
                    }
                }
            } else if (!decl.isVec4 && valueIt->is_number()) {
                decl.value[0] = valueIt->get<float>();
            }
        }
        decls.push_back(std::move(decl));
    }
    return decls;
}

// Every recipe rendered on one standard sphere (the MD NaN-testbed setup),
// reusing material slot 0 across recipes. Caption and tags come from the
// recipe's own metadata -- no vision call for recipes.
void processRecipes(gpu::Device& device, renderer::ForwardRenderer& renderer,
                    const renderer::ibl::Environment& environment, const fs::path& assetDir,
                    const fs::path& shaderDir, bool force, agent::AssetIndex& index, Tally& tally) {
    const fs::path recipesDir = shaderDir / "recipes";
    std::error_code ec;
    if (!fs::is_directory(recipesDir, ec)) {
        return;
    }
    std::vector<std::string> ids;
    for (const auto& entry : fs::directory_iterator(recipesDir, ec)) {
        if (entry.path().extension() == ".json") {
            ids.push_back(entry.path().stem().string());
        }
    }
    std::sort(ids.begin(), ids.end());
    if (ids.empty()) {
        return;
    }

    const fs::path templatePath = shaderDir / "pbr_surface_template.frag";
    const auto templateText = readTextFile(templatePath);
    if (!templateText.has_value()) {
        logError("index: cannot read {}", templatePath.string());
        tally.failed += static_cast<int>(ids.size());
        return;
    }

    asset::SceneAsset sphereAsset;
    // Larger than the NaN testbed's sphere: recipes pattern in world space,
    // so a bigger ball shows several noise/ring cycles instead of a sliver.
    std::optional<asset::MeshData> sphere = asset::makePrimitive("sphere", 3.2f);
    if (!sphere.has_value()) {
        tally.failed += static_cast<int>(ids.size());
        return;
    }
    sphere->materialIndex = 0;
    const float diagonal = math::length(sphere->localAabb.max - sphere->localAabb.min);
    sphereAsset.meshes.push_back(std::move(*sphere));
    asset::MaterialData baseMaterial;
    baseMaterial.metallic = 0.0f;
    baseMaterial.roughness = 0.6f;
    sphereAsset.materials.push_back(baseMaterial);
    sphereAsset.nodes.push_back(asset::NodeInstance{.name = "sphere", .meshIndex = 0});
    if (!renderer.loadScene(sphereAsset, environment)) {
        logError("index: recipe preview scene setup failed");
        tally.failed += static_cast<int>(ids.size());
        return;
    }
    scene::Scene world;
    scene::Entity sphereEntity;
    sphereEntity.meshIndex = 0;
    sphereEntity.materialIndex = 0;
    world.entities.insert(sphereEntity);
    world.addLight(keyLight());
    fitCamera(world.camera, {0.0f, 0.0f, 0.0f}, diagonal);

    for (const std::string& id : ids) {
        const fs::path metaPath = recipesDir / (id + ".json");
        const fs::path fragPath = recipesDir / (id + ".frag");
        const auto metaText = readTextFile(metaPath);
        const nlohmann::json meta = metaText.has_value()
                                        ? nlohmann::json::parse(*metaText, nullptr, false)
                                        : nlohmann::json(nlohmann::json::value_t::discarded);
        if (meta.is_discarded() || !meta.is_object()) {
            logError("index: recipe '{}' metadata is corrupt", id);
            ++tally.failed;
            continue;
        }

        agent::AssetIndexEntry entry;
        entry.id = id;
        entry.kind = agent::AssetIndexKind::Recipe;
        entry.caption = jsonString(meta, "description");
        entry.tags = jsonStringArray(meta, "tags");

        const fs::path pngPath = thumbnailPath(assetDir, "recipes", id);
        std::error_code freshEc;
        const bool fresh =
            fs::exists(pngPath, freshEc) &&
            fs::last_write_time(pngPath, freshEc) >= newestOf({fragPath, metaPath, templatePath});
        if (force || !fresh) {
            const auto functionText = readTextFile(fragPath);
            if (!functionText.has_value()) {
                logError("index: cannot read {}", fragPath.string());
                ++tally.failed;
            } else {
                const std::vector<agent::surface::ParamDecl> decls = recipeParamDecls(meta);
                const auto spliced =
                    agent::surface::spliceSurface(*templateText, *functionText, decls);
                bool ok = spliced.has_value();
                if (!ok) {
                    logError("index: recipe '{}': {}", id, spliced.error());
                } else {
                    const auto installed = renderer.setMaterialShader(0, spliced->source);
                    ok = installed.has_value();
                    if (!ok) {
                        logError("index: recipe '{}' failed to compile: {}", id,
                                 installed.error().empty() ? std::string("unknown")
                                                           : installed.error().front().message);
                    } else {
                        std::vector<renderer::ForwardRenderer::SurfaceParam> params;
                        for (std::size_t i = 0; i < spliced->layout.size(); ++i) {
                            renderer::ForwardRenderer::SurfaceParam param{
                                .name = spliced->layout[i].name,
                                .isVec4 = spliced->layout[i].isVec4,
                                .offset = spliced->layout[i].offset};
                            std::copy(std::begin(decls[i].value), std::end(decls[i].value),
                                      param.value);
                            params.push_back(std::move(param));
                        }
                        renderer.setMaterialSurfaceParams(0, std::move(params));
                        ok = renderSceneToPng(device, renderer, world, pngPath);
                        if (!ok) {
                            logError("index: render failed for recipe '{}'", id);
                        }
                    }
                }
                if (ok) {
                    ++tally.rendered;
                    tally.rerendered.push_back(id);
                } else {
                    ++tally.failed;
                }
            }
        } else {
            ++tally.skipped;
        }
        if (fs::exists(pngPath, freshEc)) {
            entry.thumbnail = relativeThumbnail(assetDir, pngPath);
        }
        index.entries.push_back(std::move(entry));
    }
}

// --- scene-spec templates (MR) ----------------------------------------------

// assets/specs/*.json, hand-authored: indexed for retrieval (the director
// pipeline injects the top matches as tone references, MC); caption/tags/
// style come from the file itself, no thumbnail.
void processSpecs(const fs::path& assetDir, agent::AssetIndex& index, Tally& tally) {
    const fs::path specsDir = assetDir / "specs";
    std::error_code ec;
    if (!fs::is_directory(specsDir, ec)) {
        return;
    }
    std::vector<std::string> ids;
    for (const auto& entry : fs::directory_iterator(specsDir, ec)) {
        if (entry.path().extension() == ".json") {
            ids.push_back(entry.path().stem().string());
        }
    }
    std::sort(ids.begin(), ids.end());
    for (const std::string& id : ids) {
        const auto text = readTextFile(specsDir / (id + ".json"));
        const nlohmann::json parsed = text.has_value()
                                          ? nlohmann::json::parse(*text, nullptr, false)
                                          : nlohmann::json(nlohmann::json::value_t::discarded);
        if (parsed.is_discarded() || !parsed.is_object()) {
            logError("index: spec '{}' is not valid JSON", id);
            ++tally.failed;
            continue;
        }
        agent::AssetIndexEntry entry;
        entry.id = id;
        entry.kind = agent::AssetIndexKind::Spec;
        entry.caption = jsonString(parsed, "caption");
        entry.tags = jsonStringArray(parsed, "tags");
        entry.style = jsonString(parsed, "style");
        index.entries.push_back(std::move(entry));
    }
}

// --- captions and embeddings (MR) --------------------------------------------

std::string trimmedCaption(std::string text) {
    std::replace(text.begin(), text.end(), '\n', ' ');
    const std::size_t begin = text.find_first_not_of(" \t\r");
    const std::size_t end = text.find_last_not_of(" \t\r");
    if (begin == std::string::npos) {
        return {};
    }
    text = text.substr(begin, end - begin + 1);
    if (text.size() > 240) {
        text.resize(240);
    }
    return text;
}

// Carries captions forward from the previous index (matched on id+kind),
// invalidates the re-rendered ones, then asks the vision provider for the
// rest. Recipe/spec captions come from their own metadata and are never
// touched here.
void fillCaptions(agent::AssetIndex& index, const std::optional<agent::AssetIndex>& previous,
                  const Tally& rerendered, const fs::path& assetDir, agent::ILLMProvider* vision,
                  const std::string& visionModel) {
    std::unordered_map<std::string, const agent::AssetIndexEntry*> prevById;
    if (previous.has_value()) {
        for (const agent::AssetIndexEntry& entry : previous->entries) {
            prevById.emplace(entry.id, &entry);
        }
    }
    auto wasRerendered = [&](const std::string& id) {
        return std::find(rerendered.rerendered.begin(), rerendered.rerendered.end(), id) !=
               rerendered.rerendered.end();
    };
    int captioned = 0;
    int failed = 0;
    for (agent::AssetIndexEntry& entry : index.entries) {
        if (entry.kind == agent::AssetIndexKind::Recipe ||
            entry.kind == agent::AssetIndexKind::Spec) {
            continue;
        }
        if (entry.caption.empty()) {
            if (const auto it = prevById.find(entry.id);
                it != prevById.end() && it->second->kind == entry.kind) {
                entry.caption = it->second->caption;
            }
        }
        if (wasRerendered(entry.id)) {
            entry.caption.clear();
        }
        if (vision == nullptr || !entry.caption.empty() || entry.thumbnail.empty()) {
            continue;
        }
        const std::optional<std::string> png =
            agent::detail::base64EncodeFile(assetDir / entry.thumbnail);
        if (!png.has_value()) {
            continue;
        }
        agent::ChatMessage message;
        message.role = agent::Role::User;
        message.text = "One short sentence describing this 3D asset preview for a search "
                       "index: subject, style, dominant colors, material feel. Reply with "
                       "only the sentence.";
        message.userImages.push_back(agent::UserImage{.base64 = *png});
        message.userImageDetail = "low";
        agent::ChatRequest request;
        request.model = visionModel;
        request.messages = {std::move(message)};
        // Reasoning off: an OpenAI reasoning model left at its default effort
        // can burn the whole budget on reasoning and return empty text.
        request.reasoningEffort = "none";
        request.maxTokens = 300;
        const agent::CompleteResult result = vision->complete(request);
        if (result.has_value()) {
            entry.caption = trimmedCaption(result->text);
            ++captioned;
        } else {
            ++failed;
            logWarn("index: caption for '{}' failed: {}", entry.id, result.error().message);
        }
    }
    if (captioned > 0 || failed > 0) {
        logInfo("index: {} captions generated, {} failed", captioned, failed);
    }
}

std::string embeddingText(const agent::AssetIndexEntry& entry) {
    std::string text = entry.caption;
    text += ' ';
    text += entry.name.empty() ? entry.id : entry.name;
    for (const std::string& tag : entry.tags) {
        text += ' ';
        text += tag;
    }
    if (!entry.category.empty()) {
        text += ' ';
        text += entry.category;
    }
    return text;
}

// Embeds every entry's caption+name+tags+category, reusing the previous
// sidecar row when the text (and model) is unchanged, so an idempotent re-run
// makes zero requests. A null `client` (--thumbnails, or no openai endpoint)
// runs in reuse-only mode, so a plain thumbnail refresh never wipes the
// sidecar a previous --index built. Any failure leaves the affected entries
// at offset -1; asset_search then covers them with FTS only.
void fillEmbeddings(agent::AssetIndex& index, const std::optional<agent::AssetIndex>& previous,
                    const std::vector<float>& previousRows, const fs::path& assetDir,
                    agent::EmbeddingClient* client) {
    const std::string model =
        client != nullptr
            ? client->model()
            : (previous.has_value() && previous->embedding.has_value() ? previous->embedding->model
                                                                       : std::string());
    if (model.empty()) {
        return;
    }
    std::unordered_map<std::string, const agent::AssetIndexEntry*> prevById;
    if (previous.has_value()) {
        for (const agent::AssetIndexEntry& entry : previous->entries) {
            prevById.emplace(entry.id, &entry);
        }
    }
    const bool canReuse = previous.has_value() && previous->embedding.has_value() &&
                          previous->embedding->model == model && previous->embedding->dim > 0 &&
                          !previousRows.empty();
    if (client == nullptr && !canReuse) {
        return;
    }
    const std::size_t prevDim = canReuse ? previous->embedding->dim : 0;

    std::vector<std::string> texts(index.entries.size());
    std::vector<const float*> reused(index.entries.size(), nullptr);
    std::vector<std::string> toEmbed;
    std::vector<std::size_t> toEmbedIndex;
    for (std::size_t i = 0; i < index.entries.size(); ++i) {
        texts[i] = embeddingText(index.entries[i]);
        const agent::AssetIndexEntry* prev = nullptr;
        if (const auto it = prevById.find(index.entries[i].id); it != prevById.end()) {
            prev = it->second;
        }
        if (canReuse && prev != nullptr && prev->kind == index.entries[i].kind &&
            prev->embeddingOffset >= 0 &&
            (static_cast<std::size_t>(prev->embeddingOffset) + 1) * prevDim <=
                previousRows.size() &&
            embeddingText(*prev) == texts[i]) {
            reused[i] =
                previousRows.data() + static_cast<std::size_t>(prev->embeddingOffset) * prevDim;
        } else {
            toEmbed.push_back(texts[i]);
            toEmbedIndex.push_back(i);
        }
    }

    std::vector<std::vector<float>> fresh;
    if (!toEmbed.empty() && client != nullptr) {
        auto rows = client->embed(toEmbed);
        if (!rows.has_value()) {
            logWarn("index: embeddings skipped: {}", rows.error());
            return;
        }
        fresh = std::move(*rows);
    } else if (!toEmbed.empty()) {
        logInfo("index: {} entries left unembedded (no embeddings endpoint; rerun --index "
                "with an openai-typed provider)",
                toEmbed.size());
    }
    const std::size_t dim = !fresh.empty() ? fresh[0].size() : prevDim;
    if (dim == 0) {
        return;
    }
    if (prevDim != 0 && !fresh.empty() && fresh[0].size() != prevDim) {
        // Same model returning a new dimension: drop reuse, re-embed all.
        auto rows = client->embed(texts);
        if (!rows.has_value()) {
            logWarn("index: embeddings skipped: {}", rows.error());
            return;
        }
        fresh = std::move(*rows);
        toEmbedIndex.resize(index.entries.size());
        for (std::size_t i = 0; i < toEmbedIndex.size(); ++i) {
            toEmbedIndex[i] = i;
        }
        std::fill(reused.begin(), reused.end(), nullptr);
    }

    std::vector<float> rows;
    rows.reserve(index.entries.size() * dim);
    std::size_t freshCursor = 0;
    int embedded = 0;
    for (std::size_t i = 0; i < index.entries.size(); ++i) {
        const float* source = reused[i];
        if (source == nullptr && freshCursor < fresh.size() && toEmbedIndex[freshCursor] == i) {
            source = fresh[freshCursor].data();
            ++freshCursor;
        }
        if (source == nullptr) {
            index.entries[i].embeddingOffset = -1;
            continue;
        }
        index.entries[i].embeddingOffset = static_cast<std::int64_t>(rows.size() / dim);
        rows.insert(rows.end(), source, source + dim);
        ++embedded;
    }

    index.embedding = agent::AssetIndexEmbedding{
        .model = model, .dim = static_cast<std::uint32_t>(dim), .file = "index_embeddings.bin"};
    if (!agent::saveEmbeddingRows(assetDir, *index.embedding, rows)) {
        logError("index: failed to write the embedding sidecar");
        index.embedding.reset();
        for (agent::AssetIndexEntry& entry : index.entries) {
            entry.embeddingOffset = -1;
        }
        return;
    }
    logInfo("index: {} entries embedded ({} rows reused), dim {}", embedded,
            embedded - static_cast<int>(freshCursor), dim);
}

int runIndexing(const fs::path& assetDir, const fs::path& shaderDir, const IndexOptions& options) {
    gpu::Ptr<gpu::Device> device = gpu::createDevice();
    if (!device) {
        logError("thumbnails: failed to create a GPU device");
        return 1;
    }

    renderer::ForwardRenderer renderer;
    if (!renderer.init(*device, kOutputFormat)) {
        logError("thumbnails: renderer init failed");
        return 1;
    }
    renderer.resize({kThumbSize, kThumbSize});

    // Baked once and reused across every model: the IBL environment does not
    // depend on scene content, only on the sky.
    const asset::HdrImage sky = asset::proceduralSky(studioSkyDesc());
    const renderer::ibl::Environment environment = renderer::ibl::bake(*device, sky);
    if (!environment.valid()) {
        logError("thumbnails: IBL bake failed");
        return 1;
    }

    // The previous index feeds caption carry-forward and embedding-row reuse,
    // so a rebuild only pays for what actually changed.
    const std::optional<agent::AssetIndex> previous = agent::loadAssetIndex(assetDir);
    std::vector<float> previousRows;
    if (previous.has_value() && previous->embedding.has_value()) {
        if (std::optional<std::vector<float>> rows =
                agent::loadEmbeddingRows(assetDir, *previous->embedding)) {
            previousRows = std::move(*rows);
        }
    }

    agent::AssetIndex index;
    index.generated = options.recipesAndSpecs ? "viewer --index" : "viewer --thumbnails";

    Tally modelTally;
    processModels(*device, renderer, environment, assetDir, options.force, index, modelTally);
    Tally textureTally;
    processTextureSets(assetDir, options.force, index, textureTally);
    Tally envTally;
    processEnvironments(assetDir, options.force, index, envTally);
    Tally recipeTally;
    if (options.recipesAndSpecs) {
        processRecipes(*device, renderer, environment, assetDir, shaderDir, options.force, index,
                       recipeTally);
        processSpecs(assetDir, index, recipeTally);
    }

    // Config-backed clients for the enrichment passes; either may be absent,
    // each pass degrades on its own.
    std::unique_ptr<agent::ILLMProvider> vision;
    std::string visionModel;
    std::unique_ptr<agent::EmbeddingClient> embedder;
    if (options.captions || options.embeddings) {
        const auto config = agent::loadAgentConfig("kumo.config.json", ".env");
        if (!config.has_value()) {
            logWarn("index: agent config: {}; captions and embeddings skipped", config.error());
        } else {
            if (options.captions) {
                // retrieval.caption_model routes captions to the retrieval
                // endpoint (MS: a local vision model keeps a full re-index
                // free); otherwise the scene/shader agent endpoint serves.
                if (!config->captionModel.empty()) {
                    const std::string base = !config->retrievalBaseUrl.empty()
                                                 ? config->retrievalBaseUrl
                                                 : config->scene.baseUrl;
                    vision = std::make_unique<agent::OpenAiProvider>(
                        base, config->retrievalApiKey, agent::makeUrlSessionTransport());
                    visionModel = config->captionModel;
                } else if (const agent::AgentEndpoint* endpoint =
                               config->scene.available()    ? &config->scene
                               : config->shader.available() ? &config->shader
                                                            : nullptr;
                           endpoint != nullptr) {
                    if (endpoint->type == agent::ProviderType::OpenAi) {
                        vision = std::make_unique<agent::OpenAiProvider>(
                            endpoint->baseUrl, endpoint->apiKey, agent::makeUrlSessionTransport());
                    } else {
                        vision = std::make_unique<agent::ClaudeProvider>(
                            endpoint->baseUrl, endpoint->apiKey, agent::makeUrlSessionTransport());
                    }
                    visionModel = endpoint->model;
                } else {
                    logInfo("index: captions skipped (no configured agent endpoint)");
                }
            }
            if (options.embeddings) {
                std::string base = config->retrievalBaseUrl;
                std::string key = config->retrievalApiKey;
                if (base.empty()) {
                    if (config->scene.type == agent::ProviderType::OpenAi &&
                        config->scene.available()) {
                        base = config->scene.baseUrl;
                        key = config->scene.apiKey;
                    } else if (config->shader.type == agent::ProviderType::OpenAi &&
                               config->shader.available()) {
                        base = config->shader.baseUrl;
                        key = config->shader.apiKey;
                    }
                }
                if (!base.empty()) {
                    embedder = std::make_unique<agent::EmbeddingClient>(
                        agent::makeUrlSessionTransport(), base, key, config->embeddingModel);
                } else {
                    logInfo("index: embeddings skipped (needs an openai-typed endpoint or "
                            "retrieval.base_url)");
                }
            }
        }
    }

    Tally allRerendered;
    for (const Tally* tally : {&modelTally, &textureTally, &envTally, &recipeTally}) {
        allRerendered.rerendered.insert(allRerendered.rerendered.end(), tally->rerendered.begin(),
                                        tally->rerendered.end());
    }
    // Both passes run even caption/embedding-less: carry-forward and reuse-only
    // keep a later --thumbnails run from wiping what --index built.
    fillCaptions(index, previous, allRerendered, assetDir, vision.get(), visionModel);
    fillEmbeddings(index, previous, previousRows, assetDir, embedder.get());

    if (!agent::saveAssetIndex(assetDir, index)) {
        logError("thumbnails: failed to write {}", (assetDir / "index.json").string());
        return 1;
    }

    logInfo("thumbnails: models {} rendered, {} skipped, {} failed", modelTally.rendered,
            modelTally.skipped, modelTally.failed);
    logInfo("thumbnails: textures {} rendered, {} skipped, {} failed", textureTally.rendered,
            textureTally.skipped, textureTally.failed);
    logInfo("thumbnails: env {} rendered, {} skipped, {} failed", envTally.rendered,
            envTally.skipped, envTally.failed);
    if (options.recipesAndSpecs) {
        logInfo("thumbnails: recipes/specs {} rendered, {} skipped, {} failed",
                recipeTally.rendered, recipeTally.skipped, recipeTally.failed);
    }
    logInfo("thumbnails: wrote {} with {} entries", (assetDir / "index.json").string(),
            index.entries.size());

    return (modelTally.failed > 0 || textureTally.failed > 0 || envTally.failed > 0 ||
            recipeTally.failed > 0)
               ? 1
               : 0;
}

} // namespace

int runThumbnails(const std::filesystem::path& assetDir, bool force) {
    return runIndexing(assetDir, {}, IndexOptions{.force = force});
}

int runIndex(const std::filesystem::path& assetDir, const std::filesystem::path& shaderDir,
             bool force, bool captions) {
    return runIndexing(
        assetDir, shaderDir,
        IndexOptions{
            .force = force, .recipesAndSpecs = true, .captions = captions, .embeddings = true});
}
