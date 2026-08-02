// Headless batch mode (MA milestone): `viewer --thumbnails` renders a preview
// PNG per model/texture-set/env under the asset library and (re)writes
// index.json, without opening a window (mirrors tests/test_gpu_contract.cpp's
// windowless gpu::createDevice() pattern) or going through EngineRuntime (this
// tool needs to swap scenes far more often than EngineRuntime's one-scene-per-
// process contract allows).

#include "asset_index.h" // private kumo_agent header (engine/agent/src)

#include <kumo/asset/asset.h>
#include <kumo/asset/model_resolver.h>
#include <kumo/asset/procedural_sky.h>
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
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
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

} // namespace

int runThumbnails(const std::filesystem::path& assetDir, bool force) {
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

    agent::AssetIndex index;
    index.generated = "viewer --thumbnails";

    Tally modelTally;
    processModels(*device, renderer, environment, assetDir, force, index, modelTally);
    Tally textureTally;
    processTextureSets(assetDir, force, index, textureTally);
    Tally envTally;
    processEnvironments(assetDir, force, index, envTally);

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
    logInfo("thumbnails: wrote {} with {} entries", (assetDir / "index.json").string(),
            index.entries.size());

    return (modelTally.failed > 0 || textureTally.failed > 0 || envTally.failed > 0) ? 1 : 0;
}
