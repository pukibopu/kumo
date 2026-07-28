#include <kumo/agent/scene_tools.h>

#include <kumo/agent/entity_id.h>
#include <kumo/asset/primitives.h>
#include <kumo/core/assert.h>
#include <kumo/math/math.h>
#include <kumo/renderer/forward_renderer.h>
#include <kumo/scene/scene.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kumo::agent {

std::string formatEntityId(scene::EntityId id) {
    return std::format("{}:{}", id.index, id.generation);
}

std::optional<scene::EntityId> parseEntityId(std::string_view text) {
    const std::size_t colon = text.find(':');
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    scene::EntityId id;
    const std::string_view index = text.substr(0, colon);
    const std::string_view generation = text.substr(colon + 1);
    const auto indexResult = std::from_chars(index.begin(), index.end(), id.index);
    const auto generationResult =
        std::from_chars(generation.begin(), generation.end(), id.generation);
    if (indexResult.ec != std::errc{} || indexResult.ptr != index.end() ||
        generationResult.ec != std::errc{} || generationResult.ptr != generation.end()) {
        return std::nullopt;
    }
    return id;
}

namespace {

using nlohmann::json;
using renderer::ForwardRenderer;

std::string okJson() {
    return json{{"status", "ok"}}.dump();
}

// Invalid UTF-8 (e.g. legacy-encoded glTF node names) must degrade to U+FFFD,
// never to a dump() throw that blinds the model to the whole scene.
std::string dumpSafe(const json& value) {
    return value.dump(-1, ' ', false, json::error_handler_t::replace);
}

// Floats round-trip through double with shortest-representation dumping, so
// rounding here keeps scene_list output compact (ADR 0028).
double rounded(float v) {
    return std::round(static_cast<double>(v) * 1000.0) / 1000.0;
}

json numberArray(const float* values, std::size_t count) {
    json out = json::array();
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(rounded(values[i]));
    }
    return out;
}

json numberArray(const math::float3& v) {
    const float values[3] = {v.x, v.y, v.z};
    return numberArray(values, 3);
}

// Readers return false only on a present-but-invalid field, with `error` set; an
// absent key leaves `out` untouched and succeeds, which is what gives every tool
// its partial-update semantics. Values must be finite: 1e39 casting to inf would
// otherwise flow into uniforms as scene state.
bool readNumbers(const json& args, const char* key, float* out, std::size_t count,
                 std::string& error) {
    const auto it = args.find(key);
    if (it == args.end()) {
        return true;
    }
    if (!it->is_array() || it->size() != count) {
        error = std::format("{} must be an array of {} numbers", key, count);
        return false;
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (!(*it)[i].is_number()) {
            error = std::format("{} must be an array of {} numbers", key, count);
            return false;
        }
        const float value = (*it)[i].get<float>();
        if (!std::isfinite(value)) {
            error = std::format("{} must contain finite numbers", key);
            return false;
        }
        out[i] = value;
    }
    return true;
}

bool readFloat3(const json& args, const char* key, math::float3& out, std::string& error) {
    float values[3] = {out.x, out.y, out.z};
    if (!readNumbers(args, key, values, 3, error)) {
        return false;
    }
    out = {values[0], values[1], values[2]};
    return true;
}

bool readNumber(const json& args, const char* key, float& out, std::string& error) {
    const auto it = args.find(key);
    if (it == args.end()) {
        return true;
    }
    if (!it->is_number()) {
        error = std::format("{} must be a number", key);
        return false;
    }
    const float value = it->get<float>();
    if (!std::isfinite(value)) {
        error = std::format("{} must be a finite number", key);
        return false;
    }
    out = value;
    return true;
}

// json::value() throws on present-but-wrong-type keys; raw library exception
// text must never become the tool's reply, so string reads are explicit.
bool readString(const json& args, const char* key, std::string& out, std::string& error) {
    const auto it = args.find(key);
    if (it == args.end()) {
        return true;
    }
    if (!it->is_string()) {
        error = std::format("{} must be a string", key);
        return false;
    }
    out = it->get<std::string>();
    return true;
}

bool readMaterial(const json& args, ForwardRenderer::MaterialParams& params, std::string& error) {
    return readNumbers(args, "base_color", params.baseColor, 4, error) &&
           readNumber(args, "metallic", params.metallic, error) &&
           readNumber(args, "roughness", params.roughness, error) &&
           readNumbers(args, "emissive", params.emissive, 3, error);
}

bool readTransform(const json& args, scene::Transform& transform, std::string& error) {
    if (!readFloat3(args, "position", transform.position, error) ||
        !readFloat3(args, "scale", transform.scale, error)) {
        return false;
    }
    if (args.contains("scale") &&
        (transform.scale.x <= 1e-6f || transform.scale.y <= 1e-6f || transform.scale.z <= 1e-6f)) {
        // A zero component makes the model matrix singular and its inverse (the
        // normal matrix) NaN; the glTF path assumes positive scale throughout.
        error = "scale components must be positive";
        return false;
    }
    math::float3 euler{0.0f, 0.0f, 0.0f};
    if (args.contains("rotation_euler_deg")) {
        if (!readFloat3(args, "rotation_euler_deg", euler, error)) {
            return false;
        }
        transform.rotation = math::quatFromEulerDegrees(euler);
    }
    return true;
}

struct EntityLookup {
    scene::EntityId id;
    scene::Entity* entity = nullptr;
};

std::expected<EntityLookup, std::string> findEntity(scene::Scene& scene, const json& args) {
    const auto it = args.find("entity_id");
    if (it == args.end() || !it->is_string()) {
        return std::unexpected(errorJson("entity_id (string) is required"));
    }
    const std::string text = it->get<std::string>();
    const std::optional<scene::EntityId> id = parseEntityId(text);
    if (!id.has_value()) {
        return std::unexpected(errorJson(std::format("entity_id '{}' is not of the form "
                                                     "'index:generation'",
                                                     text)));
    }
    scene::Entity* entity = scene.entities.get(*id);
    if (entity == nullptr) {
        return std::unexpected(errorJson(std::format("entity not found: {}", text)));
    }
    return EntityLookup{*id, entity};
}

json materialJson(const ForwardRenderer::MaterialParams& params) {
    return {{"base_color", numberArray(params.baseColor, 4)},
            {"metallic", rounded(params.metallic)},
            {"roughness", rounded(params.roughness)},
            {"emissive", numberArray(params.emissive, 3)}};
}

std::string sceneList(const SceneToolContext& context) {
    const scene::Scene& scene = *context.scene;
    json entities = json::array();
    scene.entities.forEach([&](scene::EntityId id, const scene::Entity& entity) {
        json e{{"id", formatEntityId(id)},
               {"name", entity.name},
               {"position", numberArray(entity.transform.position)},
               {"rotation_euler_deg", numberArray(math::eulerDegrees(entity.transform.rotation))},
               {"scale", numberArray(entity.transform.scale)}};
        if (entity.materialIndex >= 0) {
            // Exposed so the model can see when entities share one material.
            e["material_index"] = entity.materialIndex;
        }
        if (!entity.primitive.empty()) {
            e["primitive"] = entity.primitive;
        }
        if (context.renderer != nullptr && entity.meshIndex >= 0) {
            const math::Aabb* local =
                context.renderer->meshLocalAabb(static_cast<std::uint32_t>(entity.meshIndex));
            if (local != nullptr) {
                const math::Aabb world = math::transformAabb(*local, entity.transform.matrix());
                e["aabb_world"] = {{"min", numberArray(world.min)},
                                   {"max", numberArray(world.max)}};
            }
        }
        if (context.renderer != nullptr && entity.materialIndex >= 0) {
            const ForwardRenderer::MaterialParams* params =
                context.renderer->materialParams(static_cast<std::uint32_t>(entity.materialIndex));
            if (params != nullptr) {
                e["material"] = materialJson(*params);
            }
        }
        entities.push_back(std::move(e));
    });

    json lights = json::array();
    for (std::size_t i = 0; i < scene.lights().size(); ++i) {
        const scene::Light& light = scene.lights()[i];
        lights.push_back({{"index", i},
                          {"type", light.type == scene::LightType::Point ? "point" : "directional"},
                          {"color", numberArray(light.color)},
                          {"intensity", rounded(light.intensity)},
                          {"direction", numberArray(light.direction)},
                          {"position", numberArray(light.position)},
                          {"range", rounded(light.range)}});
    }

    const scene::Camera& camera = scene.camera;
    json out{{"entities", std::move(entities)},
             {"camera",
              {{"position", numberArray(camera.position)},
               {"rotation_euler_deg", numberArray(math::eulerDegrees(camera.rotation))},
               {"fov_y_deg", rounded(math::degrees(camera.fovY))},
               {"near", rounded(camera.nearZ)}}},
             {"lights", std::move(lights)}};
    if (context.groups != nullptr && !context.groups->empty()) {
        std::vector<std::string> names;
        names.reserve(context.groups->size());
        for (const auto& [groupName, def] : *context.groups) {
            names.push_back(groupName);
        }
        std::sort(names.begin(), names.end());
        out["groups"] = std::move(names);
    }
    return dumpSafe(out);
}

// The seven names asset::makePrimitive understands; kept alongside it only for
// the human-readable error message (asset::makePrimitive itself is the single
// source of truth for which names actually resolve to geometry).
constexpr std::array<std::string_view, 7> kPrimitiveNames{"sphere", "cube",  "plane",  "cylinder",
                                                          "cone",   "torus", "capsule"};

std::string primitiveNameList() {
    std::string out;
    for (std::size_t i = 0; i < kPrimitiveNames.size(); ++i) {
        if (i > 0) {
            out += ", ";
        }
        out += kPrimitiveNames[i];
    }
    return out;
}

// One parsed-and-validated scene_add_entity(ies) item, not yet uploaded to the
// GPU or inserted into the scene.
struct EntityInput {
    scene::Entity entity;
    ForwardRenderer::MaterialParams material;
};

// Shared by scene_add_entity and scene_add_entities: validates one item's
// primitive name, size, transform and material without touching the scene or
// renderer. The error string is a plain message, not yet wrapped by errorJson,
// so a caller building an indexed batch error can prefix it first.
std::expected<EntityInput, std::string> parseEntityInput(const json& args) {
    std::string error;
    std::string primitive;
    if (!readString(args, "primitive", primitive, error)) {
        return std::unexpected(error);
    }
    if (!asset::makePrimitive(primitive, 1.0f).has_value()) {
        return std::unexpected(std::format("unknown primitive '{}': must be one of: {}", primitive,
                                           primitiveNameList()));
    }
    float size = 1.0f;
    if (!readNumber(args, "size", size, error)) {
        return std::unexpected(error);
    }
    if (args.contains("size") && size <= 0.0f) {
        return std::unexpected(std::string("size must be positive"));
    }

    EntityInput input;
    input.entity.name = primitive;
    if (!readString(args, "name", input.entity.name, error)) {
        return std::unexpected(error);
    }
    if (!readTransform(args, input.entity.transform, error)) {
        return std::unexpected(error);
    }
    input.entity.primitive = primitive;
    input.entity.primitiveSize = size;
    if (args.contains("material")) {
        if (!args["material"].is_object()) {
            return std::unexpected(std::string("material must be an object"));
        }
        if (!readMaterial(args["material"], input.material, error)) {
            return std::unexpected(error);
        }
    }
    return input;
}

// Builds the mesh, uploads mesh+material (when a renderer is attached) and
// inserts the entity. The error string is a plain message, matching
// parseEntityInput, for the same batch-prefixing reason.
std::expected<scene::EntityId, std::string> buildAndInsertEntity(const SceneToolContext& context,
                                                                 EntityInput input) {
    if (context.renderer != nullptr) {
        // parseEntityInput already confirmed the name resolves.
        asset::MeshData mesh =
            *asset::makePrimitive(input.entity.primitive, input.entity.primitiveSize);
        const std::int32_t meshIndex = context.renderer->addMesh(mesh);
        const std::int32_t materialIndex = context.renderer->addMaterial(input.material);
        if (meshIndex < 0 || materialIndex < 0) {
            return std::unexpected(std::string("gpu upload failed"));
        }
        input.entity.meshIndex = meshIndex;
        input.entity.materialIndex = materialIndex;
    }
    return context.scene->entities.insert(input.entity);
}

std::string sceneAddEntity(const SceneToolContext& context, const json& args) {
    std::expected<EntityInput, std::string> input = parseEntityInput(args);
    if (!input.has_value()) {
        return errorJson(input.error());
    }
    std::expected<scene::EntityId, std::string> id =
        buildAndInsertEntity(context, std::move(*input));
    if (!id.has_value()) {
        return errorJson(id.error());
    }
    return json{{"status", "ok"}, {"entity_id", formatEntityId(*id)}}.dump();
}

// Non-destructive: creation only, so it needs none of scene_remove_entity's
// confirmation gating. Validates every item before creating anything; if a GPU
// upload fails partway through, the entities this call already inserted are
// removed again (their GPU resources leak per ADR 0016, same as
// scene_remove_entity) so a failed call never leaves a partial scene behind.
std::string sceneAddEntities(const SceneToolContext& context, const json& args) {
    const auto it = args.find("entities");
    if (it == args.end() || !it->is_array()) {
        return errorJson("entities (array) is required");
    }
    if (it->empty()) {
        return errorJson("entities must not be empty");
    }
    if (it->size() > 128) {
        return errorJson(std::format("entities: at most 128 allowed per call, got {}", it->size()));
    }

    std::vector<EntityInput> parsed;
    parsed.reserve(it->size());
    for (std::size_t i = 0; i < it->size(); ++i) {
        if (!(*it)[i].is_object()) {
            return errorJson(std::format("entities[{}]: must be an object", i));
        }
        std::expected<EntityInput, std::string> input = parseEntityInput((*it)[i]);
        if (!input.has_value()) {
            return errorJson(std::format("entities[{}]: {}", i, input.error()));
        }
        parsed.push_back(std::move(*input));
    }

    std::vector<scene::EntityId> ids;
    ids.reserve(parsed.size());
    for (std::size_t i = 0; i < parsed.size(); ++i) {
        std::expected<scene::EntityId, std::string> id =
            buildAndInsertEntity(context, std::move(parsed[i]));
        if (!id.has_value()) {
            for (scene::EntityId created : ids) {
                context.scene->entities.remove(created);
            }
            return errorJson(std::format("entities[{}]: {}", i, id.error()));
        }
        ids.push_back(*id);
    }

    json idsJson = json::array();
    for (scene::EntityId id : ids) {
        idsJson.push_back(formatEntityId(id));
    }
    return json{{"status", "ok"}, {"entity_ids", std::move(idsJson)}}.dump();
}

GroupMaterialSpec toGroupMaterial(const ForwardRenderer::MaterialParams& params) {
    GroupMaterialSpec out;
    std::copy(std::begin(params.baseColor), std::end(params.baseColor), out.baseColor);
    out.metallic = params.metallic;
    out.roughness = params.roughness;
    std::copy(std::begin(params.emissive), std::end(params.emissive), out.emissive);
    return out;
}

ForwardRenderer::MaterialParams toMaterialParams(const GroupMaterialSpec& spec) {
    ForwardRenderer::MaterialParams out;
    std::copy(std::begin(spec.baseColor), std::end(spec.baseColor), out.baseColor);
    out.metallic = spec.metallic;
    out.roughness = spec.roughness;
    std::copy(std::begin(spec.emissive), std::end(spec.emissive), out.emissive);
    return out;
}

// Reads a scale field that accepts either a single number (uniform) or a
// 3-element array (per-axis); absent leaves `out` untouched, same contract as
// readFloat3/readNumber.
bool readScale(const json& args, const char* key, math::float3& out, std::string& error) {
    const auto it = args.find(key);
    if (it == args.end()) {
        return true;
    }
    if (it->is_number()) {
        const float value = it->get<float>();
        if (!std::isfinite(value)) {
            error = std::format("{} must be finite", key);
            return false;
        }
        out = {value, value, value};
        return true;
    }
    if (it->is_array()) {
        return readFloat3(args, key, out, error);
    }
    error = std::format("{} must be a number or an array of 3 numbers", key);
    return false;
}

// Integer fields arrive as JSON numbers (an LLM may send 5.0 for an integer);
// mirrors light_set's index handling rather than requiring is_number_integer().
bool readInt(const json& args, const char* key, std::int64_t& out, std::string& error) {
    const auto it = args.find(key);
    if (it == args.end()) {
        return true;
    }
    if (!it->is_number()) {
        error = std::format("{} must be an integer", key);
        return false;
    }
    const double value = it->get<double>();
    if (value != std::floor(value)) {
        error = std::format("{} must be an integer", key);
        return false;
    }
    out = static_cast<std::int64_t>(value);
    return true;
}

// Group name validation shared by define and instance: same 1-64 char rule.
std::expected<std::string, std::string> readGroupName(const json& args) {
    const auto it = args.find("name");
    if (it == args.end() || !it->is_string()) {
        return std::unexpected(std::string("name (string) is required"));
    }
    const std::string name = it->get<std::string>();
    if (name.empty() || name.size() > 64) {
        return std::unexpected(std::string("name must be 1-64 characters"));
    }
    return name;
}

// Non-destructive: stores a validated assembly, never touches the scene or
// renderer. Every member is validated with the same parseEntityInput used by
// scene_add_entity(ies), so a bad item is rejected before anything is stored;
// redefining an existing name overwrites it wholesale.
std::string sceneDefineGroup(const SceneToolContext& context, const json& args) {
    std::expected<std::string, std::string> name = readGroupName(args);
    if (!name.has_value()) {
        return errorJson(name.error());
    }

    const auto it = args.find("entities");
    if (it == args.end() || !it->is_array()) {
        return errorJson("entities (array) is required");
    }
    if (it->empty()) {
        return errorJson("entities must not be empty");
    }
    if (it->size() > 32) {
        return errorJson(std::format("entities: at most 32 allowed per group, got {}", it->size()));
    }

    GroupDef def;
    def.members.reserve(it->size());
    for (std::size_t i = 0; i < it->size(); ++i) {
        if (!(*it)[i].is_object()) {
            return errorJson(std::format("entities[{}]: must be an object", i));
        }
        std::expected<EntityInput, std::string> input = parseEntityInput((*it)[i]);
        if (!input.has_value()) {
            return errorJson(std::format("entities[{}]: {}", i, input.error()));
        }
        GroupEntitySpec spec;
        spec.name = input->entity.name;
        spec.primitive = input->entity.primitive;
        spec.size = input->entity.primitiveSize;
        spec.transform = input->entity.transform;
        spec.material = toGroupMaterial(input->material);
        def.members.push_back(std::move(spec));
    }

    const std::size_t members = def.members.size();
    (*context.groups)[*name] = std::move(def);
    return json{{"status", "ok"}, {"group", *name}, {"members", members}}.dump();
}

// world = instanceTRS ∘ memberTRS, no shear: the instance's rotation and
// uniform-or-per-axis scale are applied to the member's local offset before
// translating by the instance position (ADR 0038-adjacent composition, no new
// type needed since scene::Transform already holds TRS).
scene::Transform composeGroupMember(const scene::Transform& instance,
                                    const scene::Transform& member) {
    scene::Transform out;
    out.position = instance.position + instance.rotation * (instance.scale * member.position);
    out.rotation = instance.rotation * member.rotation;
    out.scale = instance.scale * member.scale;
    return out;
}

// Parses one `transforms[i]` item into a full instance transform; position is
// required (unlike scene_add_entity, an instance needs somewhere to be).
std::expected<scene::Transform, std::string> parseInstanceTransform(const json& item) {
    if (!item.is_object()) {
        return std::unexpected(std::string("must be an object"));
    }
    scene::Transform t;
    std::string error;
    if (!item.contains("position")) {
        return std::unexpected(std::string("position is required"));
    }
    if (!readFloat3(item, "position", t.position, error)) {
        return std::unexpected(error);
    }
    math::float3 euler{0.0f, 0.0f, 0.0f};
    if (item.contains("rotation_euler_deg")) {
        if (!readFloat3(item, "rotation_euler_deg", euler, error)) {
            return std::unexpected(error);
        }
        t.rotation = math::quatFromEulerDegrees(euler);
    }
    if (!readScale(item, "scale", t.scale, error)) {
        return std::unexpected(error);
    }
    if (t.scale.x <= 1e-6f || t.scale.y <= 1e-6f || t.scale.z <= 1e-6f) {
        return std::unexpected(std::string("scale components must be positive"));
    }
    return t;
}

// Deterministic scatter: one std::mt19937 seeded from `seed`, drawn in a fixed
// x/z/yaw/scale order per instance so identical seed+args always reproduce the
// same sequence (yaw and scale draws are skipped entirely when their jitter is
// zero, keeping the no-jitter case reproducible without a degenerate [0,0] range).
std::expected<std::vector<scene::Transform>, std::string> parseScatter(const json& scatter) {
    if (!scatter.is_object()) {
        return std::unexpected(std::string("scatter must be an object"));
    }
    std::string error;
    if (!scatter.contains("count")) {
        return std::unexpected(std::string("count is required"));
    }
    std::int64_t count = 0;
    if (!readInt(scatter, "count", count, error)) {
        return std::unexpected(error);
    }
    if (count < 1 || count > 64) {
        return std::unexpected(std::string("count must be between 1 and 64"));
    }

    if (!scatter.contains("area")) {
        return std::unexpected(std::string("area ([width,depth]) is required"));
    }
    float area[2] = {0.0f, 0.0f};
    if (!readNumbers(scatter, "area", area, 2, error)) {
        return std::unexpected(error);
    }
    if (area[0] < 0.0f || area[1] < 0.0f) {
        return std::unexpected(std::string("area components must be non-negative"));
    }

    math::float3 center{0.0f, 0.0f, 0.0f};
    if (!readFloat3(scatter, "position", center, error)) {
        return std::unexpected(error);
    }

    std::int64_t seed = 0;
    if (!readInt(scatter, "seed", seed, error)) {
        return std::unexpected(error);
    }

    float scaleJitter = 0.0f;
    if (!readNumber(scatter, "scale_jitter", scaleJitter, error)) {
        return std::unexpected(error);
    }
    if (scaleJitter < 0.0f || scaleJitter > 0.5f) {
        return std::unexpected(std::string("scale_jitter must be between 0 and 0.5"));
    }

    float rotationJitterDeg = 0.0f;
    if (!readNumber(scatter, "rotation_jitter_deg", rotationJitterDeg, error)) {
        return std::unexpected(error);
    }
    if (rotationJitterDeg < 0.0f || rotationJitterDeg > 180.0f) {
        return std::unexpected(std::string("rotation_jitter_deg must be between 0 and 180"));
    }

    std::mt19937 rng(static_cast<std::uint32_t>(static_cast<std::uint64_t>(seed)));
    std::uniform_real_distribution<float> xDist(-area[0] * 0.5f, area[0] * 0.5f);
    std::uniform_real_distribution<float> zDist(-area[1] * 0.5f, area[1] * 0.5f);
    std::uniform_real_distribution<float> yawDist(-rotationJitterDeg, rotationJitterDeg);
    std::uniform_real_distribution<float> scaleDist(1.0f - scaleJitter, 1.0f + scaleJitter);

    std::vector<scene::Transform> instances;
    instances.reserve(static_cast<std::size_t>(count));
    for (std::int64_t i = 0; i < count; ++i) {
        scene::Transform t;
        t.position = {center.x + xDist(rng), center.y, center.z + zDist(rng)};
        const float yaw = rotationJitterDeg > 0.0f ? yawDist(rng) : 0.0f;
        t.rotation = math::quatFromEulerDegrees({0.0f, yaw, 0.0f});
        const float factor = scaleJitter > 0.0f ? scaleDist(rng) : 1.0f;
        t.scale = {factor, factor, factor};
        instances.push_back(t);
    }
    return instances;
}

// Stamps `group` at every instance transform; batch-atomic like
// scene_add_entities (members are already validated at define time, so only
// the GPU upload can fail here, rolling back this call's inserts).
std::string sceneInstanceGroup(const SceneToolContext& context, const json& args) {
    std::expected<std::string, std::string> name = readGroupName(args);
    if (!name.has_value()) {
        return errorJson(name.error());
    }
    const auto groupIt = context.groups->find(*name);
    if (groupIt == context.groups->end()) {
        return errorJson(std::format("unknown group '{}'", *name));
    }
    const GroupDef& group = groupIt->second;

    const bool hasTransforms = args.contains("transforms");
    const bool hasScatter = args.contains("scatter");
    if (hasTransforms == hasScatter) {
        return errorJson("exactly one of transforms or scatter is required");
    }

    std::vector<scene::Transform> instances;
    if (hasTransforms) {
        const json& transforms = args["transforms"];
        if (!transforms.is_array()) {
            return errorJson("transforms must be an array");
        }
        if (transforms.empty()) {
            return errorJson("transforms must not be empty");
        }
        instances.reserve(transforms.size());
        for (std::size_t i = 0; i < transforms.size(); ++i) {
            std::expected<scene::Transform, std::string> t = parseInstanceTransform(transforms[i]);
            if (!t.has_value()) {
                return errorJson(std::format("transforms[{}]: {}", i, t.error()));
            }
            instances.push_back(*t);
        }
    } else {
        std::expected<std::vector<scene::Transform>, std::string> scattered =
            parseScatter(args["scatter"]);
        if (!scattered.has_value()) {
            return errorJson(std::format("scatter.{}", scattered.error()));
        }
        instances = std::move(*scattered);
    }

    const std::size_t total = instances.size() * group.members.size();
    if (total > 256) {
        return errorJson(
            std::format("instances x group members must be at most 256, got {}", total));
    }

    std::vector<EntityInput> parsed;
    parsed.reserve(total);
    for (std::size_t i = 0; i < instances.size(); ++i) {
        for (const GroupEntitySpec& member : group.members) {
            EntityInput input;
            input.entity.name = std::format("{}_{}_{}", *name, i, member.name);
            input.entity.primitive = member.primitive;
            input.entity.primitiveSize = member.size;
            input.entity.transform = composeGroupMember(instances[i], member.transform);
            input.material = toMaterialParams(member.material);
            parsed.push_back(std::move(input));
        }
    }

    std::vector<scene::EntityId> ids;
    ids.reserve(parsed.size());
    for (std::size_t i = 0; i < parsed.size(); ++i) {
        std::expected<scene::EntityId, std::string> id =
            buildAndInsertEntity(context, std::move(parsed[i]));
        if (!id.has_value()) {
            for (scene::EntityId created : ids) {
                context.scene->entities.remove(created);
            }
            return errorJson(std::format("member {}: {}", i, id.error()));
        }
        ids.push_back(*id);
    }

    json idsJson = json::array();
    for (scene::EntityId id : ids) {
        idsJson.push_back(formatEntityId(id));
    }
    return json{
        {"status", "ok"}, {"instances", instances.size()}, {"entity_ids", std::move(idsJson)}}
        .dump();
}

std::string sceneRemoveEntity(const SceneToolContext& context, const json& args) {
    auto lookup = findEntity(*context.scene, args);
    if (!lookup.has_value()) {
        return lookup.error();
    }
    // Mesh and material GPU resources are intentionally not reclaimed (ADR 0016).
    context.scene->entities.remove(lookup->id);
    return okJson();
}

std::string sceneSetTransform(const SceneToolContext& context, const json& args) {
    auto lookup = findEntity(*context.scene, args);
    if (!lookup.has_value()) {
        return lookup.error();
    }
    // Staged so a rejected field never leaves the entity half-moved: the model
    // must be able to trust that an error means nothing changed.
    scene::Transform staged = lookup->entity->transform;
    std::string error;
    if (!readTransform(args, staged, error)) {
        return errorJson(error);
    }
    lookup->entity->transform = staged;
    return okJson();
}

std::string cameraSet(const SceneToolContext& context, const json& args) {
    scene::Camera staged = context.scene->camera;
    std::string error;
    if (!readFloat3(args, "position", staged.position, error)) {
        return errorJson(error);
    }
    float fovDeg = math::degrees(staged.fovY);
    if (!readNumber(args, "fov_y_deg", fovDeg, error)) {
        return errorJson(error);
    }
    staged.fovY = math::radians(std::clamp(fovDeg, 1.0f, 179.0f));
    if (!readNumber(args, "near", staged.nearZ, error)) {
        return errorJson(error);
    }
    staged.nearZ = std::max(staged.nearZ, 0.001f);
    if (args.contains("look_at")) {
        math::float3 target{0.0f, 0.0f, 0.0f};
        if (!readFloat3(args, "look_at", target, error)) {
            return errorJson(error);
        }
        staged.lookAt(target);
    }
    context.scene->camera = staged;
    return okJson();
}

std::string lightSet(const SceneToolContext& context, const json& args) {
    scene::Scene& scene = *context.scene;
    std::optional<std::size_t> index;
    if (args.contains("index")) {
        const json& raw = args["index"];
        // The schema says integer; a model sending 0.0 still means light 0.
        const double value = raw.is_number() ? raw.get<double>() : -1.0;
        if (value < 0.0 || value != std::floor(value)) {
            return errorJson("index must be a non-negative integer");
        }
        index = static_cast<std::size_t>(value);
        if (scene.light(*index) == nullptr) {
            return errorJson(std::format("light index {} out of range ({} lights)", *index,
                                         scene.lights().size()));
        }
    }

    // Staged so a rejected call neither half-edits a light nor leaves a freshly
    // appended default one behind (lights cannot be removed individually).
    scene::Light staged = index.has_value() ? *scene.light(*index) : scene::Light{};
    if (args.contains("type")) {
        std::string type;
        std::string error;
        if (!readString(args, "type", type, error)) {
            return errorJson(error);
        }
        if (type == "directional") {
            staged.type = scene::LightType::Directional;
        } else if (type == "point") {
            staged.type = scene::LightType::Point;
        } else {
            return errorJson("type must be one of: directional, point");
        }
    }
    std::string error;
    if (!readFloat3(args, "color", staged.color, error) ||
        !readNumber(args, "intensity", staged.intensity, error) ||
        !readFloat3(args, "direction", staged.direction, error) ||
        !readFloat3(args, "position", staged.position, error) ||
        !readNumber(args, "range", staged.range, error)) {
        return errorJson(error);
    }
    const math::float3& d = staged.direction;
    if (args.contains("direction") && d.x * d.x + d.y * d.y + d.z * d.z < 1e-8f) {
        // The renderer normalizes unconditionally; zero would become NaN state.
        return errorJson("direction must be a non-zero vector");
    }

    if (index.has_value()) {
        *scene.light(*index) = staged;
    } else {
        if (!scene.addLight(staged)) {
            return errorJson("light budget exhausted: the scene supports at most 16 lights");
        }
        index = scene.lights().size() - 1;
    }
    return json{{"status", "ok"}, {"index", *index}}.dump();
}

std::string materialSetParam(const SceneToolContext& context, const json& args) {
    if (context.renderer == nullptr) {
        return errorJson("renderer unavailable");
    }
    auto lookup = findEntity(*context.scene, args);
    if (!lookup.has_value()) {
        return lookup.error();
    }
    ForwardRenderer& renderer = *context.renderer;

    ForwardRenderer::MaterialParams params;
    if (lookup->entity->materialIndex >= 0) {
        const ForwardRenderer::MaterialParams* current =
            renderer.materialParams(static_cast<std::uint32_t>(lookup->entity->materialIndex));
        if (current == nullptr) {
            return errorJson("material index out of range");
        }
        params = *current;
    } else if (const ForwardRenderer::MaterialParams* fallback =
                   renderer.materialParams(renderer.defaultMaterialIndex())) {
        params = *fallback;
    }
    std::string error;
    if (!readMaterial(args, params, error)) {
        return errorJson(error);
    }

    if (lookup->entity->materialIndex < 0) {
        // Entities without a material render with the default record; give them
        // their own on first write so they become editable.
        const std::int32_t newIndex = renderer.addMaterial(params);
        if (newIndex < 0) {
            return errorJson("gpu upload failed");
        }
        lookup->entity->materialIndex = newIndex;
        return okJson();
    }

    std::size_t sharers = 0;
    context.scene->entities.forEach([&](scene::EntityId, const scene::Entity& other) {
        if (other.materialIndex == lookup->entity->materialIndex) {
            ++sharers;
        }
    });
    renderer.setMaterialParams(static_cast<std::uint32_t>(lookup->entity->materialIndex), params);
    if (sharers > 1) {
        // Cloning here would strip the shared material's textures (addMaterial is
        // untextured), so shared records are edited in place and the count keeps
        // the model aware of the blast radius; per-entity splitting needs
        // renderer-side material cloning (M6).
        return json{{"status", "ok"}, {"entities_sharing_material", sharers}}.dump();
    }
    return okJson();
}

json parseArgs(std::string_view argsJson) {
    if (argsJson.empty()) {
        return json::object();
    }
    // The registry already rejected non-object payloads (ADR 0028).
    return json::parse(argsJson, nullptr, false);
}

// The object-schema properties for one entity, shared verbatim between
// scene_add_entity's top-level schema and scene_add_entities' array items so
// the two tools can never drift apart.
constexpr const char* kEntityPropertiesSchema =
    R"("name":{"type":"string","description":"Optional display name"},
"primitive":{"type":"string","enum":["sphere","cube","plane","cylinder","cone","torus","capsule"]},
"size":{"type":"number","description":"Overall extent in meters (default 1); use scale for non-uniform sizing"},
"position":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3,"description":"World position [x,y,z]"},
"rotation_euler_deg":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3,"description":"Euler XYZ rotation in degrees"},
"scale":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},
"material":{"type":"object","properties":{
"base_color":{"type":"array","items":{"type":"number"},"minItems":4,"maxItems":4,"description":"Linear RGBA"},
"metallic":{"type":"number"},
"roughness":{"type":"number"},
"emissive":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}}})";

} // namespace

void registerSceneListTool(ToolRegistry& registry, SceneToolContext context) {
    KUMO_ASSERT(context.scene != nullptr);

    registry.add({.name = "scene_list",
                  .description = "List the scene: every entity with id, transform, world-space "
                                 "AABB and material factors, plus the camera and lights. Call "
                                 "this before spatial reasoning or edits.",
                  .parametersSchema = R"({"type":"object","properties":{}})",
                  .destructive = false},
                 [context](std::string_view) { return sceneList(context); });
}

void registerSceneTools(ToolRegistry& registry, SceneToolContext context) {
    KUMO_ASSERT(context.scene != nullptr);
    if (context.groups == nullptr) {
        context.groups = std::make_shared<std::unordered_map<std::string, GroupDef>>();
    }

    registerSceneListTool(registry, context);

    auto add = [&](const char* name, const char* description, const std::string& schema,
                   bool destructive, auto handler) {
        registry.add({.name = name,
                      .description = description,
                      .parametersSchema = schema,
                      .destructive = destructive},
                     [context, handler](std::string_view argsJson) {
                         return handler(context, parseArgs(argsJson));
                     });
    };

    add("scene_add_entity",
        "Add a procedural primitive entity to the scene; returns its entity_id. For multiple "
        "entities prefer scene_add_entities.",
        std::string(R"({"type":"object","properties":{)") + kEntityPropertiesSchema +
            R"(},"required":["primitive"]})",
        false,
        [](const SceneToolContext& ctx, const json& args) { return sceneAddEntity(ctx, args); });

    add("scene_add_entities",
        "Add up to 128 entities in ONE call; prefer this over repeated scene_add_entity when "
        "building scenes. Returns entity_ids in input order.",
        std::string(
            R"({"type":"object","properties":{"entities":{"type":"array","minItems":1,"maxItems":128,"items":{"type":"object","properties":{)") +
            kEntityPropertiesSchema + R"(},"required":["primitive"]}}},"required":["entities"]})",
        false,
        [](const SceneToolContext& ctx, const json& args) { return sceneAddEntities(ctx, args); });

    add("scene_remove_entity", "Remove an entity from the scene permanently.",
        R"({"type":"object","properties":{
"entity_id":{"type":"string","description":"Id from scene_list or scene_add_entity"}},
"required":["entity_id"]})",
        true,
        [](const SceneToolContext& ctx, const json& args) { return sceneRemoveEntity(ctx, args); });

    add("scene_set_transform",
        "Update the position, rotation and/or scale of an entity; omitted fields keep their "
        "current value.",
        R"({"type":"object","properties":{
"entity_id":{"type":"string"},
"position":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},
"rotation_euler_deg":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},
"scale":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}},
"required":["entity_id"]})",
        false,
        [](const SceneToolContext& ctx, const json& args) { return sceneSetTransform(ctx, args); });

    add("camera_set", "Move the camera and/or aim it at a target point.",
        R"({"type":"object","properties":{
"position":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},
"look_at":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3,"description":"Point the camera at this world position"},
"fov_y_deg":{"type":"number"},
"near":{"type":"number"}}})",
        false, [](const SceneToolContext& ctx, const json& args) { return cameraSet(ctx, args); });

    add("light_set",
        "Modify the light at `index`, or append a new light when `index` is omitted (at most 16 "
        "lights).",
        R"({"type":"object","properties":{
"index":{"type":"integer","minimum":0,"description":"Omit to append a new light"},
"type":{"type":"string","enum":["directional","point"]},
"color":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},
"intensity":{"type":"number"},
"direction":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},
"position":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},
"range":{"type":"number"}}})",
        false, [](const SceneToolContext& ctx, const json& args) { return lightSet(ctx, args); });

    add("material_set_param",
        "Update the material factors of an entity; omitted fields keep their current value.",
        R"({"type":"object","properties":{
"entity_id":{"type":"string"},
"base_color":{"type":"array","items":{"type":"number"},"minItems":4,"maxItems":4,"description":"Linear RGBA"},
"metallic":{"type":"number"},
"roughness":{"type":"number"},
"emissive":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}},
"required":["entity_id"]})",
        false,
        [](const SceneToolContext& ctx, const json& args) { return materialSetParam(ctx, args); });

    add("scene_define_group",
        "Define a reusable named assembly of primitives (local transforms relative to the group "
        "origin). Instance it with scene_instance_group.",
        std::string(
            R"({"type":"object","properties":{"name":{"type":"string","description":"Group name, 1-64 characters"},)") +
            R"("entities":{"type":"array","minItems":1,"maxItems":32,"items":{"type":"object","properties":{)" +
            kEntityPropertiesSchema +
            R"(},"required":["primitive"]}}},"required":["name","entities"]})",
        false,
        [](const SceneToolContext& ctx, const json& args) { return sceneDefineGroup(ctx, args); });

    add("scene_instance_group",
        "Stamp N instances of a defined group, either at explicit transforms or scattered "
        "deterministically over an area (count/area/seed/jitter). Prefer this for repeated "
        "structures (forests, crowds, fences).",
        R"({"type":"object","properties":{
"name":{"type":"string","description":"Name from a prior scene_define_group call"},
"transforms":{"type":"array","description":"Explicit instance transforms; mutually exclusive with scatter","items":{"type":"object","properties":{
"position":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},
"rotation_euler_deg":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},
"scale":{"description":"Uniform scale factor or [x,y,z]","oneOf":[{"type":"number"},{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}]}},
"required":["position"]}},
"scatter":{"type":"object","description":"Deterministic random placement; mutually exclusive with transforms","properties":{
"count":{"type":"integer","minimum":1,"maximum":64},
"area":{"type":"array","items":{"type":"number"},"minItems":2,"maxItems":2,"description":"[width,depth] centered on position"},
"position":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3,"description":"Scatter rectangle center, default origin"},
"seed":{"type":"integer","description":"Default 0; identical seed and args reproduce identical output"},
"scale_jitter":{"type":"number","minimum":0,"maximum":0.5,"description":"Per-instance uniform scale factor 1+/-jitter"},
"rotation_jitter_deg":{"type":"number","minimum":0,"maximum":180,"description":"Per-instance yaw jitter"}},
"required":["count","area"]}},
"required":["name"]})",
        false, [](const SceneToolContext& ctx, const json& args) {
            return sceneInstanceGroup(ctx, args);
        });
}

} // namespace kumo::agent
