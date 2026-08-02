#include <kumo/agent/scene_tools.h>

#include "asset_index.h"
#include "placement.h"

#include <kumo/agent/entity_id.h>
#include <kumo/asset/model_resolver.h>
#include <kumo/asset/primitives.h>
#include <kumo/asset/procedural_sky.h>
#include <kumo/asset/texture_set.h>
#include <kumo/core/assert.h>
#include <kumo/core/asset_name.h>
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
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
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

bool readBool(const json& args, const char* key, bool& out, std::string& error) {
    const auto it = args.find(key);
    if (it == args.end()) {
        return true;
    }
    if (!it->is_boolean()) {
        error = std::format("{} must be a boolean", key);
        return false;
    }
    out = it->get<bool>();
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
    if (!readNumbers(args, "base_color", params.baseColor, 4, error) ||
        !readNumber(args, "metallic", params.metallic, error) ||
        !readNumber(args, "roughness", params.roughness, error) ||
        !readNumbers(args, "emissive", params.emissive, 3, error)) {
        return false;
    }
    // PBR factors outside [0,1] are undefined for the shading model; clamped
    // rather than rejected so a slightly-off value from the model still lands
    // somewhere sensible.
    params.metallic = std::clamp(params.metallic, 0.0f, 1.0f);
    params.roughness = std::clamp(params.roughness, 0.0f, 1.0f);
    return true;
}

// Entities without an explicit material get a plausible default instead of
// the renderer's untextured-white/fully-metallic record (which reads as a
// mirror ball, a bad default for freshly added geometry).
ForwardRenderer::MaterialParams defaultEntityMaterial() {
    return {.baseColor = {0.8f, 0.8f, 0.8f, 1.0f}, .metallic = 0.0f, .roughness = 0.6f};
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

// Shared by scene_list and light_remove so the wire shape of a light entry
// never drifts between the two.
json lightJson(std::size_t index, const scene::Light& light) {
    return {{"index", index},
            {"type", light.type == scene::LightType::Point ? "point" : "directional"},
            {"color", numberArray(light.color)},
            {"intensity", rounded(light.intensity)},
            {"direction", numberArray(light.direction)},
            {"position", numberArray(light.position)},
            {"range", rounded(light.range)}};
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
        lights.push_back(lightJson(i, scene.lights()[i]));
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
    } else {
        input.material = defaultEntityMaterial();
    }
    return input;
}

// Defined next to scene_validate below; declared here because placement
// preflight (snap/avoid_overlap) needs every existing entity's world AABB
// before it mutates anything.
struct AabbEntity {
    std::string id;
    math::Aabb box;
    std::int32_t assemblyId = 0; // shared positive id = same model/group instance
};
std::vector<AabbEntity> collectAabbs(const SceneToolContext& context);

json aabbJson(const math::Aabb& box) {
    return {{"min", numberArray(box.min)}, {"max", numberArray(box.max)}};
}

// Optional placement controls shared by scene_add_entity and scene_add_model
// (MP milestone). Defaults keep pre-MP calls byte-identical in behavior.
struct PlacementArgs {
    bool snapToGround = false;
    float clearance = 0.01f;
    bool avoidOverlap = false;
};

std::expected<PlacementArgs, std::string> parsePlacementArgs(const json& args) {
    PlacementArgs out;
    std::string error;
    if (!readBool(args, "snap_to_ground", out.snapToGround, error) ||
        !readNumber(args, "clearance", out.clearance, error) ||
        !readBool(args, "avoid_overlap", out.avoidOverlap, error)) {
        return std::unexpected(error);
    }
    if (args.contains("clearance") && out.clearance < 0.0f) {
        return std::unexpected(std::string("clearance must be non-negative"));
    }
    return out;
}

// The structured avoid_overlap rejection both placement tools return: every
// conflicting id, the deepest conflict's per-axis penetration, the caller's
// requested position and (when the ring search finds one) a deterministic
// alternative.
json placementConflictJson(json conflictingIds, const math::float3& depth,
                           const math::float3& requested,
                           const std::optional<math::float3>& suggested) {
    json out{{"status", "error"},
             {"message", "placement rejected: the candidate bounds overlap existing entities"},
             {"conflicting_entity_ids", std::move(conflictingIds)},
             {"overlap_depth", numberArray(depth)},
             {"requested_position", numberArray(requested)}};
    if (suggested.has_value()) {
        out["suggested_position"] = numberArray(*suggested);
    }
    return out;
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
    const std::expected<PlacementArgs, std::string> options = parsePlacementArgs(args);
    if (!options.has_value()) {
        return errorJson(options.error());
    }

    // Candidate world bounds from the primitive's CPU-side local AABB —
    // available before any scene or renderer mutation, so snapping and the
    // collision preflight run entirely on the candidate.
    const math::Aabb local =
        asset::makePrimitive(input->entity.primitive, input->entity.primitiveSize)->localAabb;
    math::Aabb candidate = math::transformAabb(local, input->entity.transform.matrix());
    const bool snapped = options->snapToGround;
    if (snapped) {
        const float deltaY = placement::snapDeltaY(candidate, options->clearance);
        input->entity.transform.position.y += deltaY;
        candidate.min.y += deltaY;
        candidate.max.y += deltaY;
    }

    if (options->avoidOverlap) {
        const std::vector<AabbEntity> existing = collectAabbs(context);
        std::vector<math::Aabb> boxes;
        boxes.reserve(existing.size());
        for (const AabbEntity& entry : existing) {
            boxes.push_back(entry.box);
        }
        const std::vector<placement::Conflict> conflicts =
            placement::findConflicts(candidate, boxes, placement::kSupportTolerance);
        if (!conflicts.empty()) {
            json ids = json::array();
            for (const placement::Conflict& conflict : conflicts) {
                ids.push_back(existing[conflict.index].id);
            }
            const std::optional<math::float3> suggested =
                placement::suggestPosition(candidate, input->entity.transform.position, boxes,
                                           placement::kSupportTolerance, 8);
            return placementConflictJson(std::move(ids),
                                         placement::deepestConflict(conflicts).depth,
                                         input->entity.transform.position, suggested)
                .dump();
        }
    }

    std::expected<scene::EntityId, std::string> id =
        buildAndInsertEntity(context, std::move(*input));
    if (!id.has_value()) {
        return errorJson(id.error());
    }
    json result{
        {"status", "ok"}, {"entity_id", formatEntityId(*id)}, {"aabb_world", aabbJson(candidate)}};
    if (snapped) {
        const scene::Entity* inserted = context.scene->entities.get(*id);
        if (inserted != nullptr) {
            result["position"] = numberArray(inserted->transform.position);
        }
    }
    return result.dump();
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
    std::copy(std::begin(params.uvTiling), std::end(params.uvTiling), out.uvTiling);
    return out;
}

ForwardRenderer::MaterialParams toMaterialParams(const GroupMaterialSpec& spec) {
    ForwardRenderer::MaterialParams out;
    std::copy(std::begin(spec.baseColor), std::end(spec.baseColor), out.baseColor);
    out.metallic = spec.metallic;
    out.roughness = spec.roughness;
    std::copy(std::begin(spec.emissive), std::end(spec.emissive), out.emissive);
    std::copy(std::begin(spec.uvTiling), std::end(spec.uvTiling), out.uvTiling);
    return out;
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

// world = instanceTRS ∘ memberTRS. Exact only because instance scale is
// uniform (enforced at parse time): uniform scale commutes with the member
// rotation, so the product stays a shear-free TRS.
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
    if (item.contains("scale")) {
        const json& s = item["scale"];
        // A per-axis instance scale over a rotated member needs shear, which
        // TRS cannot represent; instances scale uniformly (members keep their
        // own per-axis scale inside the group definition).
        if (!s.is_number()) {
            return std::unexpected(
                std::string("scale must be a single uniform number for instances"));
        }
        const float value = s.get<float>();
        if (!std::isfinite(value) || value <= 1e-6f) {
            return std::unexpected(std::string("scale must be a positive finite number"));
        }
        t.scale = {value, value, value};
    }
    return t;
}

// Deterministic scatter: one std::mt19937 seeded from `seed`, drawn in a fixed
// x/z/yaw/scale order per instance so identical seed+args always reproduce the
// same sequence (yaw and scale draws are skipped entirely when their jitter is
// zero, keeping the no-jitter case reproducible without a degenerate [0,0] range).
// Parse-and-validate only; sampling happens in placement::sampleScatter so the
// rejection logic stays a pure, directly-testable function.
std::expected<placement::ScatterParams, std::string> parseScatter(const json& scatter) {
    if (!scatter.is_object()) {
        return std::unexpected(std::string("scatter must be an object"));
    }
    std::string error;
    if (!scatter.contains("count")) {
        return std::unexpected(std::string("count is required"));
    }
    placement::ScatterParams params;
    if (!readInt(scatter, "count", params.count, error)) {
        return std::unexpected(error);
    }
    if (params.count < 1 || params.count > 64) {
        return std::unexpected(std::string("count must be between 1 and 64"));
    }

    if (!scatter.contains("area")) {
        return std::unexpected(std::string("area ([width,depth]) is required"));
    }
    if (!readNumbers(scatter, "area", params.area, 2, error)) {
        return std::unexpected(error);
    }
    if (params.area[0] < 0.0f || params.area[1] < 0.0f) {
        return std::unexpected(std::string("area components must be non-negative"));
    }

    if (!readFloat3(scatter, "position", params.center, error)) {
        return std::unexpected(error);
    }
    if (!readInt(scatter, "seed", params.seed, error)) {
        return std::unexpected(error);
    }

    if (!readNumber(scatter, "scale_jitter", params.scaleJitter, error)) {
        return std::unexpected(error);
    }
    if (params.scaleJitter < 0.0f || params.scaleJitter > 0.5f) {
        return std::unexpected(std::string("scale_jitter must be between 0 and 0.5"));
    }

    if (!readNumber(scatter, "rotation_jitter_deg", params.rotationJitterDeg, error)) {
        return std::unexpected(error);
    }
    if (params.rotationJitterDeg < 0.0f || params.rotationJitterDeg > 180.0f) {
        return std::unexpected(std::string("rotation_jitter_deg must be between 0 and 180"));
    }

    if (!readNumber(scatter, "min_spacing", params.minSpacing, error)) {
        return std::unexpected(error);
    }
    if (params.minSpacing < 0.0f) {
        return std::unexpected(std::string("min_spacing must be non-negative"));
    }
    if (!readBool(scatter, "avoid_existing", params.avoidExisting, error)) {
        return std::unexpected(error);
    }
    if (!readInt(scatter, "max_attempts", params.maxAttempts, error)) {
        return std::unexpected(error);
    }
    if (scatter.contains("max_attempts") && (params.maxAttempts < 1 || params.maxAttempts > 4096)) {
        return std::unexpected(std::string("max_attempts must be between 1 and 4096"));
    }
    return params;
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
        std::expected<placement::ScatterParams, std::string> params = parseScatter(args["scatter"]);
        if (!params.has_value()) {
            return errorJson(std::format("scatter.{}", params.error()));
        }

        // The aggregate footprint of one group instance at identity, from the
        // members' CPU-side primitive bounds (define-time validation already
        // proved every member's primitive resolves).
        std::vector<math::Aabb> memberBoxes;
        memberBoxes.reserve(group.members.size());
        for (const GroupEntitySpec& member : group.members) {
            memberBoxes.push_back(
                math::transformAabb(asset::makePrimitive(member.primitive, member.size)->localAabb,
                                    member.transform.matrix()));
        }
        const math::Aabb groupLocal = *placement::aggregateAabb(memberBoxes);

        std::vector<math::Aabb> existingBoxes;
        if (params->avoidExisting) {
            const std::vector<AabbEntity> existing = collectAabbs(context);
            existingBoxes.reserve(existing.size());
            for (const AabbEntity& entry : existing) {
                existingBoxes.push_back(entry.box);
            }
        }

        // Shallow support contact with the ground (or any surface the scatter
        // sits on) must not reject candidates; 2cm mirrors scene_validate.
        std::expected<std::vector<scene::Transform>, placement::ScatterFailure> sampled =
            placement::sampleScatter(*params, groupLocal, existingBoxes,
                                     placement::kSupportTolerance);
        if (!sampled.has_value()) {
            return json{
                {"status", "error"},
                {"message", std::format("scatter could not place {} instances within {} attempts "
                                        "({} accepted); grow the area, lower min_spacing or count",
                                        sampled.error().requested, sampled.error().attempts,
                                        sampled.error().accepted)},
                {"requested", sampled.error().requested},
                {"accepted", sampled.error().accepted},
                {"area", json::array({params->area[0], params->area[1]})},
                {"min_spacing", params->minSpacing}}
                .dump();
        }
        instances = std::move(*sampled);
    }

    const std::size_t total = instances.size() * group.members.size();
    if (total > 256) {
        return errorJson(
            std::format("instances x group members must be at most 256, got {}", total));
    }

    std::vector<EntityInput> parsed;
    parsed.reserve(total);
    for (std::size_t i = 0; i < instances.size(); ++i) {
        // One assembly per stamped instance (MP): members of one instance
        // overlap by design (a lamp head meets its pole), so scene_validate
        // skips those pairs while different instances still check against
        // each other. The counter only ever advances, so ids burned by a
        // failed call are simply skipped.
        const std::int32_t assemblyId = context.scene->nextAssemblyId++;
        for (const GroupEntitySpec& member : group.members) {
            EntityInput input;
            input.entity.name = std::format("{}_{}_{}", *name, i, member.name);
            input.entity.primitive = member.primitive;
            input.entity.primitiveSize = member.size;
            input.entity.transform = composeGroupMember(instances[i], member.transform);
            input.entity.assemblyId = assemblyId;
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

// Destructive: indices are positional, so every light after `index` shifts
// down by one; the response echoes the full remaining list so the model never
// has to guess the new indices.
std::string lightRemove(const SceneToolContext& context, const json& args) {
    scene::Scene& scene = *context.scene;
    const auto it = args.find("index");
    if (it == args.end() || !it->is_number()) {
        return errorJson("index (integer) is required");
    }
    const double value = it->get<double>();
    if (value < 0.0 || value != std::floor(value)) {
        return errorJson("index must be a non-negative integer");
    }
    const auto index = static_cast<std::size_t>(value);
    const std::size_t count = scene.lights().size();
    if (index >= count) {
        return errorJson(count == 0 ? std::string("light index out of range: the scene has no "
                                                  "lights")
                                    : std::format("light index {} out of range: valid range is "
                                                  "0-{}",
                                                  index, count - 1));
    }
    scene.removeLight(index);

    json lights = json::array();
    for (std::size_t i = 0; i < scene.lights().size(); ++i) {
        lights.push_back(lightJson(i, scene.lights()[i]));
    }
    return json{{"status", "ok"}, {"lights", std::move(lights)}}.dump();
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

// Asset library tools (M6.98 PR-2): read real textures/models/HDRIs from
// SceneToolContext::assetDir, the layout tools/fetch_assets.sh populates
// (<assetDir>/{textures/<name>/{albedo,normal,roughness,metalness,ao}.{png,jpg},
// models/<name>.glb, env/<name>.hdr}).

// The five stems loadTextureSet probes, in the order assets/README.md
// documents them; only the ones actually present are reported.
constexpr std::array<std::string_view, 5> kTextureMapStems{"albedo", "normal", "roughness",
                                                           "metalness", "ao"};

bool mapFileExists(const std::filesystem::path& dir, std::string_view stem) {
    std::error_code ec;
    for (std::string_view ext : {".png", ".jpg"}) {
        if (std::filesystem::exists(dir / (std::string(stem) + std::string(ext)), ec)) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> presentMaps(const std::filesystem::path& dir) {
    std::vector<std::string> maps;
    for (std::string_view stem : kTextureMapStems) {
        if (mapFileExists(dir, stem)) {
            maps.emplace_back(stem);
        }
    }
    return maps;
}

struct TextureSetEntry {
    std::string name;
    std::vector<std::string> maps;
};

// Shared by asset_list (full listing) and material_set_texture's "unknown
// set" error (available names). A subdirectory with no recognized map files
// is not a texture set and is skipped.
std::vector<TextureSetEntry> collectTextureSets(const std::filesystem::path& assetDir) {
    std::vector<TextureSetEntry> out;
    std::error_code ec;
    const std::filesystem::path dir = assetDir / "textures";
    if (!std::filesystem::is_directory(dir, ec)) {
        return out;
    }
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_directory()) {
            continue;
        }
        std::vector<std::string> maps = presentMaps(entry.path());
        if (!maps.empty()) {
            out.push_back({entry.path().filename().string(), std::move(maps)});
        }
    }
    std::sort(out.begin(), out.end(),
              [](const TextureSetEntry& a, const TextureSetEntry& b) { return a.name < b.name; });
    return out;
}

std::vector<std::string> collectStems(const std::filesystem::path& dir, const char* extension) {
    std::vector<std::string> out;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return out;
    }
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.is_regular_file() && entry.path().extension() == extension) {
            out.push_back(entry.path().stem().string());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string textureSetNameList(const std::filesystem::path& assetDir) {
    const std::vector<TextureSetEntry> sets = collectTextureSets(assetDir);
    if (sets.empty()) {
        return "(none found; run tools/fetch_assets.sh)";
    }
    std::string out;
    for (std::size_t i = 0; i < sets.size(); ++i) {
        if (i > 0) {
            out += ", ";
        }
        out += sets[i].name;
    }
    return out;
}

// asset_list v2 (MA milestone/F1 rework): the disk scan below is the source
// of truth for what assets EXIST (index.json can go stale the moment
// asset_fetch or a manual copy adds something after the last `viewer
// --thumbnails` run); index.json, when it loads, is merged in purely as an
// optional metadata overlay keyed by id, per kind. An index entry with no
// matching disk id is silently dropped (it no longer exists); a disk entry
// with no matching index id just carries no extra metadata. `maps` is
// deliberately NOT taken from the index even for textures -- collectTextureSets
// above is already the authoritative, freshly-scanned map list.
std::unordered_map<std::string, const AssetIndexEntry*> indexById(const AssetIndex& index,
                                                                  AssetIndexKind kind) {
    std::unordered_map<std::string, const AssetIndexEntry*> out;
    for (const AssetIndexEntry& entry : index.entries) {
        if (entry.kind == kind) {
            out.emplace(entry.id, &entry);
        }
    }
    return out;
}

// Only non-empty/present fields are added, so the output stays compact (no
// captions or embeddings here -- that's search's job later, per MR). `name`
// is deliberately NOT copied here: it is the index's human-readable display
// name, exposed as `display_name` alongside the disk-derived, tool-callable
// `name` this function's caller already set (F3) rather than replacing it.
void mergeIndexMetadata(json& j, const AssetIndexEntry* entry) {
    if (entry == nullptr) {
        return;
    }
    if (!entry->name.empty()) {
        j["display_name"] = entry->name;
    }
    if (!entry->category.empty()) {
        j["category"] = entry->category;
    }
    if (!entry->style.empty()) {
        j["style"] = entry->style;
    }
    if (!entry->license.empty()) {
        j["license"] = entry->license;
    }
    if (!entry->source.empty()) {
        j["source"] = entry->source;
    }
    if (entry->resolution.has_value()) {
        j["resolution"] = *entry->resolution;
    }
    if (entry->dimensions.has_value()) {
        j["dimensions"] = numberArray(*entry->dimensions);
    }
    if (entry->triangles.has_value()) {
        j["triangles"] = *entry->triangles;
    }
    if (entry->instancingOk.has_value()) {
        j["instancing_ok"] = *entry->instancingOk;
    }
}

// Read-only: lists what's under assetDir so the model can pick real assets
// over primitives. context.assetDir empty means no asset library is
// configured at all (mirrors environment_set's unsupported style); a
// configured but empty/missing library reports empty arrays plus a note
// instead, since that is a normal pre-fetch_assets.sh state, not a
// misconfiguration.
std::string assetList(const SceneToolContext& context) {
    if (context.assetDir.empty()) {
        return errorJson("asset directory not configured");
    }

    const std::optional<AssetIndex> index = loadAssetIndex(context.assetDir);
    const std::unordered_map<std::string, const AssetIndexEntry*> textureIndex =
        index ? indexById(*index, AssetIndexKind::Texture)
              : std::unordered_map<std::string, const AssetIndexEntry*>{};
    const std::unordered_map<std::string, const AssetIndexEntry*> modelIndex =
        index ? indexById(*index, AssetIndexKind::Model)
              : std::unordered_map<std::string, const AssetIndexEntry*>{};
    const std::unordered_map<std::string, const AssetIndexEntry*> envIndex =
        index ? indexById(*index, AssetIndexKind::Environment)
              : std::unordered_map<std::string, const AssetIndexEntry*>{};

    const std::vector<TextureSetEntry> textureSets = collectTextureSets(context.assetDir);
    json textures = json::array();
    for (const TextureSetEntry& entry : textureSets) {
        json j{{"name", entry.name}, {"maps", entry.maps}};
        const auto it = textureIndex.find(entry.name);
        mergeIndexMetadata(j, it != textureIndex.end() ? it->second : nullptr);
        textures.push_back(std::move(j));
    }

    const std::vector<std::string> modelIds = asset::listModelIds(context.assetDir / "models");
    json models = json::array();
    for (const std::string& id : modelIds) {
        json j{{"name", id}};
        const auto it = modelIndex.find(id);
        mergeIndexMetadata(j, it != modelIndex.end() ? it->second : nullptr);
        models.push_back(std::move(j));
    }

    const std::vector<std::string> envIds = collectStems(context.assetDir / "env", ".hdr");
    json envs = json::array();
    for (const std::string& id : envIds) {
        json j{{"name", id + ".hdr"}};
        const auto it = envIndex.find(id);
        mergeIndexMetadata(j, it != envIndex.end() ? it->second : nullptr);
        envs.push_back(std::move(j));
    }

    json out{{"status", "ok"},
             {"textures", std::move(textures)},
             {"models", std::move(models)},
             {"env", std::move(envs)}};
    if (textureSets.empty() && modelIds.empty() && envIds.empty()) {
        out["note"] =
            "no assets found under " + context.assetDir.string() + "; run tools/fetch_assets.sh";
    }
    return dumpSafe(out);
}

// Applies (or, with no `tiling` argument, keeps) a texture set on an entity's
// material. Errors: no such entity, entity has no material, bad tiling, asset
// library not configured, unknown texture set (lists available names), or
// renderer unavailable. Input is validated (entity, material, texture name,
// tiling, and that the named set actually exists on disk) before the
// renderer is required, so those failures are visible even with no GPU
// available; only the actual upload/bind needs a renderer. Uploads are
// cached per set name in context.textureSets so twenty entities sharing
// "sand" upload it once.
std::string materialSetTexture(const SceneToolContext& context, const json& args) {
    auto lookup = findEntity(*context.scene, args);
    if (!lookup.has_value()) {
        return lookup.error();
    }
    if (lookup->entity->materialIndex < 0) {
        return errorJson("entity has no material; call material_set_param first, or give it a "
                         "material when creating it");
    }

    const auto textureIt = args.find("texture");
    if (textureIt == args.end() || !textureIt->is_string() ||
        textureIt->get<std::string>().empty()) {
        return errorJson("texture (string) is required");
    }
    const std::string textureName = textureIt->get<std::string>();
    if (!isPlainAssetName(textureName)) {
        return errorJson("asset names must be plain names from asset_list, not paths");
    }

    // Staged like every other tool here: tiling is validated before any GPU
    // mutation, so a bad tiling value never leaves the texture bound but the
    // tiling stale, or vice versa.
    bool hasTiling = false;
    float tiling[2] = {1.0f, 1.0f};
    if (args.contains("tiling")) {
        hasTiling = true;
        const json& t = args["tiling"];
        if (t.is_number()) {
            tiling[0] = tiling[1] = t.get<float>();
        } else if (t.is_array() && t.size() == 2 && t[0].is_number() && t[1].is_number()) {
            tiling[0] = t[0].get<float>();
            tiling[1] = t[1].get<float>();
        } else {
            return errorJson("tiling must be a number or an array of 2 numbers");
        }
        for (float v : tiling) {
            if (!std::isfinite(v) || v <= 0.0f || v > 1000.0f) {
                return errorJson("tiling must be positive and at most 1000");
            }
        }
    }

    if (context.assetDir.empty()) {
        return errorJson("asset directory not configured");
    }

    TextureSetIndices indices;
    const auto cacheIt = context.textureSets->find(textureName);
    if (cacheIt != context.textureSets->end()) {
        indices = cacheIt->second;
    } else {
        const std::filesystem::path dir = context.assetDir / "textures" / textureName;
        std::expected<asset::TextureSetData, std::string> textureSet = asset::loadTextureSet(dir);
        if (!textureSet.has_value()) {
            return errorJson(std::format("unknown texture set '{}': must be one of: {}",
                                         textureName, textureSetNameList(context.assetDir)));
        }
        if (context.renderer == nullptr) {
            return errorJson("renderer unavailable");
        }
        ForwardRenderer& uploadRenderer = *context.renderer;
        if (textureSet->baseColor.has_value()) {
            indices.baseColor = uploadRenderer.addTexture(*textureSet->baseColor);
            if (indices.baseColor < 0) {
                return errorJson(
                    std::format("gpu upload failed for texture set '{}'", textureName));
            }
        }
        if (textureSet->metallicRoughness.has_value()) {
            indices.metallicRoughness = uploadRenderer.addTexture(*textureSet->metallicRoughness);
            if (indices.metallicRoughness < 0) {
                return errorJson(
                    std::format("gpu upload failed for texture set '{}'", textureName));
            }
        }
        if (textureSet->normal.has_value()) {
            indices.normal = uploadRenderer.addTexture(*textureSet->normal);
            if (indices.normal < 0) {
                return errorJson(
                    std::format("gpu upload failed for texture set '{}'", textureName));
            }
        }
        if (textureSet->occlusion.has_value()) {
            indices.occlusion = uploadRenderer.addTexture(*textureSet->occlusion);
            if (indices.occlusion < 0) {
                return errorJson(
                    std::format("gpu upload failed for texture set '{}'", textureName));
            }
        }
        (*context.textureSets)[textureName] = indices;
    }

    if (context.renderer == nullptr) {
        return errorJson("renderer unavailable");
    }
    ForwardRenderer& renderer = *context.renderer;
    const auto materialIndex = static_cast<std::uint32_t>(lookup->entity->materialIndex);
    const ForwardRenderer::MaterialTextureIndices toBind{
        .baseColor = indices.baseColor,
        .metallicRoughness = indices.metallicRoughness,
        .normal = indices.normal,
        .occlusion = indices.occlusion,
        .emissive = indices.emissive,
    };
    if (!renderer.setMaterialTextures(materialIndex, toBind)) {
        return errorJson("failed to bind textures to material");
    }
    if (hasTiling) {
        ForwardRenderer::MaterialParams params;
        if (const ForwardRenderer::MaterialParams* current =
                renderer.materialParams(materialIndex)) {
            params = *current;
        }
        params.uvTiling[0] = tiling[0];
        params.uvTiling[1] = tiling[1];
        renderer.setMaterialParams(materialIndex, params);
    }

    // Save/load provenance (M6.99): every entity sharing this material gets
    // the texture-set name recorded too, not just the one the call named,
    // since setMaterialTextures above already applies to all of them (they
    // share one GPU material record). Without this, reload would rebuild
    // those other entities' materials untextured (EngineRuntime::loadScene
    // only knows procedural-primitive/model provenance, not texture sets).
    std::size_t sharers = 0;
    context.scene->entities.forEach([&](scene::EntityId, scene::Entity& other) {
        if (other.materialIndex == lookup->entity->materialIndex) {
            other.textureSet = textureName;
            ++sharers;
        }
    });
    if (sharers > 1) {
        // Same in-place-edit semantics as material_set_param: no per-entity
        // material cloning yet, so a shared material's texture rebind is
        // visible on every entity that shares it.
        return json{{"status", "ok"}, {"entities_sharing_material", sharers}}.dump();
    }
    return okJson();
}

// Downloads a CC0 asset from Poly Haven into the asset library on demand
// (M6.99), for when asset_list shows nothing that fits. Mutates only the
// on-disk library, never the scene, so it needs no undo checkpoint (see
// kReadOnlyTools in engine_runtime.cpp) and no confirmation gate.
std::string assetFetch(const SceneToolContext& context, const json& args) {
    if (!context.fetchAsset) {
        return errorJson("asset fetching is not available");
    }
    std::string kind;
    std::string error;
    if (!readString(args, "kind", kind, error)) {
        return errorJson(error);
    }
    if (kind != "texture" && kind != "env" && kind != "model") {
        return errorJson("kind must be one of: texture, env, model");
    }
    const auto queryIt = args.find("query");
    if (queryIt == args.end() || !queryIt->is_string()) {
        return errorJson("query (string) is required");
    }
    const std::string query = queryIt->get<std::string>();
    if (query.empty() || query.size() > 64) {
        return errorJson("query must be 1-64 characters");
    }

    std::expected<FetchedAsset, std::string> result = context.fetchAsset(kind, query);
    if (!result.has_value()) {
        return errorJson(result.error());
    }
    json out{{"status", "ok"}, {"kind", kind}, {"name", result->name}};
    // model kind's FetchedAsset::maps holds the downloaded files' relative
    // paths (scene.gltf, textures/..., ...); the tool reports just the count,
    // per this milestone's result shape, rather than the full path list.
    if (kind == "model") {
        out["files"] = result->maps.size();
    } else {
        out["maps"] = result->maps;
    }
    out["already_present"] = result->alreadyPresent;
    if (!result->alternatives.empty()) {
        out["alternatives"] = result->alternatives;
    }
    return dumpSafe(out);
}

// Places a real glTF model from the asset library (mirrors scene_add_entity's
// creation contract, but the mesh/material come from instantiateModel instead
// of a procedural primitive). Non-destructive.
std::string sceneAddModel(const SceneToolContext& context, const json& args) {
    if (!context.instantiateModel) {
        return errorJson("model instancing is not available");
    }
    const auto modelIt = args.find("model");
    if (modelIt == args.end() || !modelIt->is_string() || modelIt->get<std::string>().empty()) {
        return errorJson("model (string) is required");
    }
    const std::string model = modelIt->get<std::string>();
    if (!isPlainAssetPath(model)) {
        return errorJson("model names must be plain names from asset_list, optionally with one "
                         "category prefix (e.g. props/crate), not arbitrary paths");
    }

    if (!args.contains("position")) {
        return errorJson("position is required");
    }
    scene::Transform root;
    std::string error;
    if (!readFloat3(args, "position", root.position, error)) {
        return errorJson(error);
    }
    if (args.contains("rotation_euler_deg")) {
        math::float3 euler{0.0f, 0.0f, 0.0f};
        if (!readFloat3(args, "rotation_euler_deg", euler, error)) {
            return errorJson(error);
        }
        root.rotation = math::quatFromEulerDegrees(euler);
    }
    if (args.contains("scale")) {
        const json& s = args["scale"];
        // Mirrors parseInstanceTransform's rule: a non-uniform root scale
        // composed onto a rotated child node would shear it, which TRS
        // cannot represent.
        if (!s.is_number()) {
            return errorJson("scale must be a single uniform number for instances");
        }
        const float value = s.get<float>();
        if (!std::isfinite(value) || value <= 1e-6f) {
            return errorJson("scale must be a positive finite number");
        }
        root.scale = {value, value, value};
    }

    const std::expected<PlacementArgs, std::string> options = parsePlacementArgs(args);
    if (!options.has_value()) {
        return errorJson(options.error());
    }
    const ModelPlacementRequest request{.snapToGround = options->snapToGround,
                                        .clearance = options->clearance,
                                        .avoidOverlap = options->avoidOverlap};

    std::expected<ModelPlacementResult, std::string> placed =
        context.instantiateModel(root, model, request);
    if (!placed.has_value()) {
        return errorJson(placed.error());
    }
    if (placed->conflict.has_value()) {
        json ids = json::array();
        for (const std::string& id : placed->conflict->conflictingIds) {
            ids.push_back(id);
        }
        return placementConflictJson(std::move(ids), placed->conflict->depth, root.position,
                                     placed->conflict->suggested)
            .dump();
    }
    const std::vector<std::string>* ids = &placed->entityIds;

    if (args.contains("name")) {
        std::string prefix;
        if (!readString(args, "name", prefix, error)) {
            return errorJson(error);
        }
        // Renames each instantiated entity from "<model>_<node>" to
        // "<prefix>_<node>" so the caller's chosen name shows up in scene_list
        // instead of the raw glTF node names.
        if (!prefix.empty()) {
            const std::string modelPrefix = model + "_";
            for (const std::string& idText : *ids) {
                const std::optional<scene::EntityId> id = parseEntityId(idText);
                if (!id.has_value()) {
                    continue;
                }
                scene::Entity* entity = context.scene->entities.get(*id);
                if (entity == nullptr) {
                    continue;
                }
                entity->name = entity->name.starts_with(modelPrefix)
                                   ? prefix + "_" + entity->name.substr(modelPrefix.size())
                                   : prefix;
            }
        }
    }

    json idsJson = json::array();
    for (const std::string& idText : *ids) {
        idsJson.push_back(idText);
    }
    json result{{"status", "ok"},
                {"entities", std::move(idsJson)},
                {"model", model},
                {"aabb_world", aabbJson(placed->aabb)}};
    if (request.snapToGround) {
        result["position"] = numberArray(placed->finalPosition);
    }
    return result.dump();
}

// Starting point for environment_set's preset resolution; explicit fields in
// the call then override individual members. Values below are tuning
// constants, not derived from anything physical.
std::optional<asset::ProceduralSkyDesc> presetDesc(const std::string& preset) {
    asset::ProceduralSkyDesc desc; // clear_day: the type's own defaults
    if (preset.empty() || preset == "clear_day") {
        return desc;
    }
    if (preset == "sunset") {
        desc.zenithColor = {0.05f, 0.08f, 0.25f};
        desc.horizonColor = {0.95f, 0.45f, 0.15f};
        desc.groundColor = {0.15f, 0.10f, 0.08f};
        desc.sunDirection = {-0.8f, -0.25f, -0.4f};
        desc.sunColor = {1.0f, 0.55f, 0.25f};
        desc.sunIntensity = 25.0f;
        desc.sunAngularRadiusDeg = 2.0f;
        return desc;
    }
    if (preset == "overcast") {
        desc.zenithColor = {0.55f, 0.56f, 0.58f};
        desc.horizonColor = {0.65f, 0.65f, 0.66f};
        desc.groundColor = {0.20f, 0.20f, 0.20f};
        desc.sunDirection = {-0.3f, -0.9f, -0.3f};
        desc.sunColor = {0.9f, 0.9f, 0.9f};
        desc.sunIntensity = 0.0f; // flat grey light, no visible disc
        return desc;
    }
    if (preset == "night") {
        desc.zenithColor = {0.02f, 0.03f, 0.08f};
        desc.horizonColor = {0.05f, 0.06f, 0.12f};
        desc.groundColor = {0.01f, 0.01f, 0.015f};
        desc.sunDirection = {-0.3f, -0.6f, -0.4f}; // moon direction
        desc.sunColor = {0.6f, 0.65f, 0.8f};
        desc.sunIntensity = 0.8f;
        desc.sunAngularRadiusDeg = 0.6f;
        return desc;
    }
    if (preset == "studio") {
        desc.zenithColor = {0.85f, 0.85f, 0.85f};
        desc.horizonColor = {0.6f, 0.6f, 0.6f};
        desc.groundColor = {0.25f, 0.25f, 0.25f};
        desc.sunColor = {1.0f, 1.0f, 1.0f};
        desc.sunIntensity = 0.0f; // neutral gradient only, no sun disc
        return desc;
    }
    return std::nullopt;
}

constexpr std::array<std::string_view, 5> kEnvironmentPresets{"clear_day", "sunset", "overcast",
                                                              "night", "studio"};

std::string environmentPresetList() {
    std::string out;
    for (std::size_t i = 0; i < kEnvironmentPresets.size(); ++i) {
        if (i > 0) {
            out += ", ";
        }
        out += kEnvironmentPresets[i];
    }
    return out;
}

// The procedural sky fields, checked against `file` for mutual exclusion.
constexpr std::array<const char*, 8> kProceduralSkyFields{
    "preset",       "sun_direction", "sun_intensity", "sun_color",
    "zenith_color", "horizon_color", "ground_color",  "exposure"};

// Bakes and swaps in a new IBL environment. No preset means clear_day; explicit
// fields override the preset's values one at a time (same partial-update
// semantics as every other tool here). `file` (an HDR from asset_list's env
// list) replaces the whole procedural path instead of overriding it: the two
// are mutually exclusive, since a baked-from-disk HDR has no preset fields to
// override.
std::string environmentSet(const SceneToolContext& context, const json& args) {
    std::string error;
    std::string file;
    if (!readString(args, "file", file, error)) {
        return errorJson(error);
    }
    if (!file.empty()) {
        if (!isPlainAssetName(file)) {
            return errorJson("asset names must be plain names from asset_list, not paths");
        }
        for (const char* field : kProceduralSkyFields) {
            if (args.contains(field)) {
                return errorJson("file and procedural sky parameters are mutually exclusive");
            }
        }
        if (!context.applyEnvironmentFile) {
            return errorJson("environment control is not available");
        }
        if (!context.applyEnvironmentFile(file)) {
            return errorJson(std::format("failed to load environment file '{}'", file));
        }
        return json{{"status", "ok"}, {"file", file}}.dump();
    }

    if (!context.applyEnvironment) {
        return errorJson("environment control is not available");
    }
    std::string presetName;
    if (!readString(args, "preset", presetName, error)) {
        return errorJson(error);
    }
    const std::optional<asset::ProceduralSkyDesc> resolved = presetDesc(presetName);
    if (!resolved.has_value()) {
        return errorJson(std::format("unknown preset '{}': must be one of: {}", presetName,
                                     environmentPresetList()));
    }
    asset::ProceduralSkyDesc desc = *resolved;
    if (!readFloat3(args, "sun_direction", desc.sunDirection, error) ||
        !readNumber(args, "sun_intensity", desc.sunIntensity, error) ||
        !readFloat3(args, "sun_color", desc.sunColor, error) ||
        !readFloat3(args, "zenith_color", desc.zenithColor, error) ||
        !readFloat3(args, "horizon_color", desc.horizonColor, error) ||
        !readFloat3(args, "ground_color", desc.groundColor, error) ||
        !readNumber(args, "exposure", desc.exposure, error)) {
        return errorJson(error);
    }
    // Upper bounds keep the baked sky inside half-float range with margin;
    // proceduralSky clamps as well, this layer exists so the model sees why.
    if (args.contains("sun_intensity") &&
        (desc.sunIntensity < 0.0f || desc.sunIntensity > 10000.0f)) {
        return errorJson("sun_intensity must be between 0 and 10000");
    }
    if (args.contains("exposure") && (desc.exposure <= 0.0f || desc.exposure > 100.0f)) {
        return errorJson("exposure must be positive and at most 100");
    }
    const math::float3& d = desc.sunDirection;
    if (d.x * d.x + d.y * d.y + d.z * d.z < 1e-8f) {
        // proceduralSky falls back silently on a zero direction; the tool
        // rejects it instead so the model notices its own mistake.
        return errorJson("sun_direction must be a non-zero vector");
    }
    if (!context.applyEnvironment(desc)) {
        return errorJson("environment control is not available");
    }
    return json{{"status", "ok"},
                {"preset", presetName.empty() ? std::string("clear_day") : presetName},
                {"sun_direction", numberArray(desc.sunDirection)}}
        .dump();
}

// Per-entity world AABBs, reusing exactly how scene_list computes aabb_world;
// empty when there is no renderer to ask (struct declared above with the
// placement helpers).
std::vector<AabbEntity> collectAabbs(const SceneToolContext& context) {
    std::vector<AabbEntity> out;
    context.scene->entities.forEach([&](scene::EntityId id, const scene::Entity& entity) {
        std::optional<math::Aabb> local;
        if (context.renderer != nullptr && entity.meshIndex >= 0) {
            if (const math::Aabb* found =
                    context.renderer->meshLocalAabb(static_cast<std::uint32_t>(entity.meshIndex))) {
                local = *found;
            }
        }
        if (!local.has_value() && !entity.primitive.empty()) {
            // CPU-only fallback: procedural primitives know their local AABB
            // without a GPU upload, so scene_validate stays useful for them
            // even with no renderer attached (renderer-backed tests already
            // cover the glTF/uploaded-mesh path via meshLocalAabb above).
            if (const auto mesh = asset::makePrimitive(entity.primitive, entity.primitiveSize)) {
                local = mesh->localAabb;
            }
        }
        if (!local.has_value()) {
            return;
        }
        out.push_back({formatEntityId(id), math::transformAabb(*local, entity.transform.matrix()),
                       entity.assemblyId});
    });
    return out;
}

struct Finding {
    const char* check;
    const char* severity;
    std::string entityId; // empty when the finding names no single entity
    std::string message;
    // Overlap findings only (MP): the second party plus measured penetration.
    // Explicit default member initializers keep the many aggregate-init sites
    // for the other checks free of missing-field warnings.
    std::string otherEntityId = {};
    std::optional<math::float3> overlapDepth = {};
    std::optional<float> overlapRatio = {};
};

json findingJson(const Finding& finding) {
    json out{
        {"check", finding.check}, {"severity", finding.severity}, {"message", finding.message}};
    if (!finding.entityId.empty()) {
        out["entity_id"] = finding.entityId;
    }
    if (!finding.otherEntityId.empty()) {
        out["other_entity_id"] = finding.otherEntityId;
    }
    if (finding.overlapDepth.has_value()) {
        out["overlap_depth"] = numberArray(*finding.overlapDepth);
    }
    if (finding.overlapRatio.has_value()) {
        out["overlap_ratio"] = *finding.overlapRatio;
    }
    return out;
}

bool xzOverlaps(const math::Aabb& a, const math::Aabb& b) {
    return a.min.x < b.max.x && a.max.x > b.min.x && a.min.z < b.max.z && a.max.z > b.min.z;
}

// support = the highest surface directly beneath the entity's XZ footprint
// (ground when nothing is), so a box sitting flush on another one is not
// flagged even though its own min.y is above the world ground plane.
void checkFloating(const std::vector<AabbEntity>& aabbs, std::vector<Finding>& findings) {
    constexpr float kEps = 0.02f;
    for (const AabbEntity& e : aabbs) {
        if (e.box.min.y <= kEps) {
            continue;
        }
        float support = 0.0f;
        for (const AabbEntity& other : aabbs) {
            if (&other == &e) {
                continue;
            }
            if (other.box.max.y <= e.box.min.y + 1e-4f && xzOverlaps(other.box, e.box)) {
                support = std::max(support, other.box.max.y);
            }
        }
        const float gap = e.box.min.y - support;
        if (gap > kEps) {
            findings.push_back({"floating", "warning", e.id,
                                std::format("entity is floating {:.3f}m above its support", gap)});
        }
    }
}

// One finding per unordered pair (i<j, never both A-vs-B and B-vs-A): minimal
// per-axis penetration beyond 2cm is real interpenetration (warning); between
// 1mm and 2cm it is support contact worth at most a note (info); below that,
// noise.
void checkOverlap(const std::vector<AabbEntity>& aabbs, std::vector<Finding>& findings) {
    if (aabbs.size() > 500) {
        findings.push_back({"overlap", "info", "",
                            std::format("overlap check skipped: {} entities exceeds the 500 limit",
                                        aabbs.size())});
        return;
    }
    const auto volume = [](const math::Aabb& box) {
        return (box.max.x - box.min.x) * (box.max.y - box.min.y) * (box.max.z - box.min.z);
    };
    for (std::size_t i = 0; i < aabbs.size(); ++i) {
        for (std::size_t j = i + 1; j < aabbs.size(); ++j) {
            // Entities placed together as one assembly (a model's own nodes, a
            // group instance's members) overlap by construction; only overlaps
            // between separate placements are actionable.
            if (aabbs[i].assemblyId > 0 && aabbs[i].assemblyId == aabbs[j].assemblyId) {
                continue;
            }
            const math::Aabb& a = aabbs[i].box;
            const math::Aabb& b = aabbs[j].box;
            const std::optional<math::float3> depth = placement::overlapDepth(a, b);
            if (!depth.has_value()) {
                continue;
            }
            const float minAxis = std::min({depth->x, depth->y, depth->z});
            if (minAxis <= placement::kNoiseEps) {
                continue;
            }
            // Support contact stays info; anything else — including thin
            // geometry buried sideways, whose smallest axis depth is tiny —
            // is a real interpenetration.
            const bool deep =
                !placement::supportContact(a, b, *depth, placement::kSupportTolerance);
            const float ratio = std::clamp((depth->x * depth->y * depth->z) /
                                               std::max(std::min(volume(a), volume(b)), 1e-9f),
                                           0.0f, 1.0f);
            Finding finding{
                "overlap", deep ? "warning" : "info", aabbs[i].id,
                deep ? std::format("entity {} interpenetrates entity {}", aabbs[i].id, aabbs[j].id)
                     : std::format("entity {} rests in shallow contact with entity {}", aabbs[i].id,
                                   aabbs[j].id)};
            finding.otherEntityId = aabbs[j].id;
            finding.overlapDepth = *depth;
            finding.overlapRatio = ratio;
            findings.push_back(std::move(finding));
        }
    }
}

void checkCameraInside(const scene::Camera& camera, const std::vector<AabbEntity>& aabbs,
                       std::vector<Finding>& findings) {
    const math::float3& p = camera.position;
    for (const AabbEntity& e : aabbs) {
        if (p.x >= e.box.min.x && p.x <= e.box.max.x && p.y >= e.box.min.y && p.y <= e.box.max.y &&
            p.z >= e.box.min.z && p.z <= e.box.max.z) {
            findings.push_back({"camera_inside", "warning", e.id,
                                "the camera is inside this entity's bounding box"});
        }
    }
}

struct Plane {
    math::float3 point;
    math::float3 normal; // need not be unit length; only the dot product's sign is used
};

// Six view-frustum half-spaces built directly from the camera basis rather
// than by extracting rows of the projection matrix: the renderer's reversed-Z
// [0,1] depth range makes the usual near/far row extraction awkward, and the
// side planes depend only on fovY/aspect regardless of depth convention.
std::array<Plane, 6> frustumPlanes(const scene::Camera& camera, float aspect, float farZ) {
    const math::float3 forward = camera.rotation * math::float3(0.0f, 0.0f, -1.0f);
    const math::float3 up = camera.rotation * math::float3(0.0f, 1.0f, 0.0f);
    const math::float3 right = camera.rotation * math::float3(1.0f, 0.0f, 0.0f);
    const math::float3& p = camera.position;
    const float halfV = farZ * std::tan(camera.fovY * 0.5f);
    const float halfH = halfV * aspect;

    return {Plane{p + forward * camera.nearZ, forward}, Plane{p + forward * farZ, -forward},
            Plane{p, halfH * forward - farZ * right},   Plane{p, farZ * right + halfH * forward},
            Plane{p, halfV * forward - farZ * up},      Plane{p, halfV * forward + farZ * up}};
}

bool aabbOutsidePlane(const math::Aabb& box, const Plane& plane) {
    // The corner furthest along the plane normal; if even that one is behind
    // the plane, the whole box is.
    const math::float3 positive{plane.normal.x >= 0.0f ? box.max.x : box.min.x,
                                plane.normal.y >= 0.0f ? box.max.y : box.min.y,
                                plane.normal.z >= 0.0f ? box.max.z : box.min.z};
    return math::dot(plane.normal, positive - plane.point) < 0.0f;
}

void checkFrustum(const SceneToolContext& context, const std::vector<AabbEntity>& aabbs,
                  std::vector<Finding>& findings) {
    float aspect = 16.0f / 9.0f;
    bool assumedAspect = true;
    if (context.viewportSize) {
        const auto [w, h] = context.viewportSize();
        if (w > 0 && h > 0) {
            aspect = static_cast<float>(w) / static_cast<float>(h);
            assumedAspect = false;
        }
    }
    constexpr float kFar = 1000.0f;
    const std::array<Plane, 6> planes = frustumPlanes(context.scene->camera, aspect, kFar);
    for (const AabbEntity& e : aabbs) {
        bool outside = false;
        for (const Plane& plane : planes) {
            if (aabbOutsidePlane(e.box, plane)) {
                outside = true;
                break;
            }
        }
        if (!outside) {
            continue;
        }
        findings.push_back(
            {"out_of_frustum", "info", e.id,
             assumedAspect ? std::format("entity {} is outside the camera frustum (assumed 16:9 "
                                         "aspect ratio)",
                                         e.id)
                           : std::format("entity {} is outside the camera frustum", e.id)});
    }
}

void checkLights(const scene::Scene& scene, std::vector<Finding>& findings) {
    const auto lights = scene.lights();
    bool anyEffective = false;
    for (const scene::Light& light : lights) {
        if (light.intensity > 0.0f) {
            anyEffective = true;
        }
    }
    if (lights.empty() || !anyEffective) {
        findings.push_back({"lighting", "warning", "", "scene has no effective lighting"});
    }
    for (std::size_t i = 0; i < lights.size(); ++i) {
        if (lights[i].intensity > 50.0f) {
            findings.push_back({"lighting", "info", "",
                                std::format("light {} intensity {:.1f} is unusually high", i,
                                            lights[i].intensity)});
        }
    }
}

// Read-only: pure math over the current scene/renderer state, never mutates
// anything and never auto-fixes what it finds.
std::string sceneValidate(const SceneToolContext& context) {
    std::vector<Finding> findings;
    const std::vector<AabbEntity> aabbs = collectAabbs(context);
    checkFloating(aabbs, findings);
    checkOverlap(aabbs, findings);
    checkCameraInside(context.scene->camera, aabbs, findings);
    checkFrustum(context, aabbs, findings);
    checkLights(*context.scene, findings);

    json out{{"status", "ok"}, {"findings", json::array()}};
    for (const Finding& finding : findings) {
        out["findings"].push_back(findingJson(finding));
    }
    return dumpSafe(out);
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
"material":{"type":"object","description":"Omit for a default non-metal grey material","properties":{
"base_color":{"type":"array","items":{"type":"number"},"minItems":4,"maxItems":4,"description":"Linear RGBA"},
"metallic":{"type":"number","description":"Clamped to the 0-1 range"},
"roughness":{"type":"number","description":"Clamped to the 0-1 range"},
"emissive":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}}})";

// Placement options (MP) shared by scene_add_entity and scene_add_model only —
// deliberately NOT part of kEntityPropertiesSchema: scene_add_entities'
// per-item placement would need a batch-atomic preflight, a later milestone.
constexpr const char* kPlacementPropertiesSchema =
    R"("snap_to_ground":{"type":"boolean","description":"Rest the object on the ground: translate Y so its bounds sit clearance above y=0. Default false"},
"clearance":{"type":"number","minimum":0,"description":"Meters above ground for snap_to_ground. Default 0.01. Collision tolerance is fixed at 0.02 and not affected by this"},
"avoid_overlap":{"type":"boolean","description":"Reject the call with conflicting_entity_ids and a suggested_position when the bounds would interpenetrate an existing entity; nothing is created. Default false"})";

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
    if (context.textureSets == nullptr) {
        context.textureSets =
            std::make_shared<std::unordered_map<std::string, TextureSetIndices>>();
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

    registry.add({.name = "asset_list",
                  .description = "List the asset library: real texture sets, glTF models and HDR "
                                 "environments available under the configured asset directory. "
                                 "Call once per conversation before building.",
                  .parametersSchema = R"({"type":"object","properties":{}})",
                  .destructive = false},
                 [context](std::string_view) { return assetList(context); });

    add("scene_add_entity",
        "Add a procedural primitive entity to the scene; returns its entity_id and world-space "
        "AABB. For multiple entities prefer scene_add_entities.",
        std::string(R"({"type":"object","properties":{)") + kEntityPropertiesSchema + ",\n" +
            kPlacementPropertiesSchema + R"(},"required":["primitive"]})",
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

    add("scene_add_model",
        "Place a real glTF model from the asset library (name from asset_list) at a position; "
        "returns its entity_ids, one per mesh node, and the aggregate world-space AABB. Prefer "
        "this over primitives for organic or detailed things (trees, props, characters) when a "
        "fitting model exists.",
        std::string(R"({"type":"object","properties":{
"model":{"type":"string","description":"Model name from asset_list, optionally with one category prefix, e.g. props/crate"},
"name":{"type":"string","description":"Optional entity name prefix; default keeps the model's own node names"},
"position":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3,"description":"World position [x,y,z]"},
"rotation_euler_deg":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3,"description":"Euler XYZ rotation in degrees"},
"scale":{"type":"number","description":"Uniform scale factor; default 1"},
)") + kPlacementPropertiesSchema +
            R"(},
"required":["model","position"]})",
        false,
        [](const SceneToolContext& ctx, const json& args) { return sceneAddModel(ctx, args); });

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

    add("light_remove",
        "Remove the light at `index` permanently; every later light's index shifts down by one, "
        "so re-check scene_list or this call's own response before addressing another light.",
        R"({"type":"object","properties":{
"index":{"type":"integer","minimum":0}},
"required":["index"]})",
        true, [](const SceneToolContext& ctx, const json& args) { return lightRemove(ctx, args); });

    add("material_set_param",
        "Update the material factors of an entity; omitted fields keep their current value.",
        R"({"type":"object","properties":{
"entity_id":{"type":"string"},
"base_color":{"type":"array","items":{"type":"number"},"minItems":4,"maxItems":4,"description":"Linear RGBA"},
"metallic":{"type":"number","description":"Clamped to the 0-1 range"},
"roughness":{"type":"number","description":"Clamped to the 0-1 range"},
"emissive":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}},
"required":["entity_id"]})",
        false,
        [](const SceneToolContext& ctx, const json& args) { return materialSetParam(ctx, args); });

    add("material_set_texture",
        "Apply a real texture set (from asset_list) to an entity's material: base color, normal, "
        "roughness/metalness and occlusion maps, replacing flat material factors. Prefer this over "
        "material_set_param's flat colors for ground, walls and other large or detailed surfaces.",
        R"({"type":"object","properties":{
"entity_id":{"type":"string"},
"texture":{"type":"string","description":"Texture-set name from asset_list, e.g. sand or planks"},
"tiling":{"description":"UV tiling, a single uniform number or a [u,v] pair; keeps the current value when omitted; positive, at most 1000"}},
"required":["entity_id","texture"]})",
        false, [](const SceneToolContext& ctx, const json& args) {
            return materialSetTexture(ctx, args);
        });

    add("asset_fetch",
        "Download a CC0 texture set, HDR environment or glTF model from Poly Haven into the asset "
        "library when asset_list has nothing that fits; then use the returned name like any "
        "library asset (material_set_texture / environment_set / scene_add_model). Runs "
        "synchronously and can take several seconds, longer for env and model. On a near-miss the "
        "error lists close alternative names to retry with.",
        R"({"type":"object","properties":{
"kind":{"type":"string","enum":["texture","env","model"]},
"query":{"type":"string","minLength":1,"maxLength":64,"description":"Short English search term, e.g. asphalt, snow, night city, wooden barrel"}},
"required":["kind","query"]})",
        false, [](const SceneToolContext& ctx, const json& args) { return assetFetch(ctx, args); });

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
"scale":{"type":"number","description":"Uniform scale factor; per-axis scale on instances would shear rotated members"}},
"required":["position"]}},
"scatter":{"type":"object","description":"Deterministic random placement; mutually exclusive with transforms","properties":{
"count":{"type":"integer","minimum":1,"maximum":64},
"area":{"type":"array","items":{"type":"number"},"minItems":2,"maxItems":2,"description":"[width,depth] centered on position"},
"position":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3,"description":"Scatter rectangle center, default origin"},
"seed":{"type":"integer","description":"Default 0; identical seed and args reproduce identical output"},
"scale_jitter":{"type":"number","minimum":0,"maximum":0.5,"description":"Per-instance uniform scale factor 1+/-jitter"},
"rotation_jitter_deg":{"type":"number","minimum":0,"maximum":180,"description":"Per-instance yaw jitter"},
"min_spacing":{"type":"number","minimum":0,"description":"Minimum XZ edge-to-edge distance between instances in meters; default 0 allows contact"},
"avoid_existing":{"type":"boolean","description":"Also reject samples that would interpenetrate existing scene entities; default false"},
"max_attempts":{"type":"integer","minimum":1,"maximum":4096,"description":"Sampling budget; default max of 10*count and 64. The call fails atomically, placing nothing, when count cannot fit"}},
"required":["count","area"]}},
"required":["name"]})",
        false, [](const SceneToolContext& ctx, const json& args) {
            return sceneInstanceGroup(ctx, args);
        });

    add("environment_set",
        "Set the sky environment: either a real HDR file (name from asset_list's env list) or a "
        "procedural sky, starting from a preset (clear_day, sunset, overcast, night, studio) and "
        "optionally overriding individual fields such as sun_direction or exposure. file and the "
        "procedural fields are mutually exclusive. Rebakes the IBL lighting; for a procedural sky, "
        "align the scene's key light with the returned sun_direction for consistent shading.",
        R"({"type":"object","properties":{
"file":{"type":"string","description":"HDR file name from asset_list's env list; mutually exclusive with the fields below"},
"preset":{"type":"string","enum":["clear_day","sunset","overcast","night","studio"],"description":"Default clear_day"},
"sun_direction":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3,"description":"Direction the sunlight travels, must be non-zero"},
"sun_intensity":{"type":"number","minimum":0,"maximum":10000},
"sun_color":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},
"zenith_color":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},
"horizon_color":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},
"ground_color":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},
"exposure":{"type":"number","description":"Positive, at most 100"}}})",
        false,
        [](const SceneToolContext& ctx, const json& args) { return environmentSet(ctx, args); });

    add("scene_validate",
        "Check the scene for common authoring mistakes: floating or overlapping entities, the "
        "camera stuck inside geometry, entities outside the view frustum, and missing or "
        "extreme-intensity lighting. Read-only; never modifies the scene.",
        R"({"type":"object","properties":{}})", false,
        [](const SceneToolContext& ctx, const json&) { return sceneValidate(ctx); });
}

} // namespace kumo::agent
