#include <kumo/agent/scene_tools.h>

#include <kumo/agent/entity_id.h>
#include <kumo/asset/primitives.h>
#include <kumo/core/assert.h>
#include <kumo/math/math.h>
#include <kumo/renderer/forward_renderer.h>
#include <kumo/scene/scene.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

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
    const json out{{"entities", std::move(entities)},
                   {"camera",
                    {{"position", numberArray(camera.position)},
                     {"rotation_euler_deg", numberArray(math::eulerDegrees(camera.rotation))},
                     {"fov_y_deg", rounded(math::degrees(camera.fovY))},
                     {"near", rounded(camera.nearZ)}}},
                   {"lights", std::move(lights)}};
    return dumpSafe(out);
}

std::string sceneAddEntity(const SceneToolContext& context, const json& args) {
    std::string error;
    std::string primitive;
    if (!readString(args, "primitive", primitive, error)) {
        return errorJson(error);
    }
    if (primitive != "sphere" && primitive != "cube" && primitive != "plane") {
        return errorJson("primitive must be one of: sphere, cube, plane");
    }
    float size = 1.0f;
    if (!readNumber(args, "size", size, error)) {
        return errorJson(error);
    }
    if (args.contains("size") && size <= 0.0f) {
        return errorJson("size must be positive");
    }

    scene::Entity entity;
    entity.name = primitive;
    if (!readString(args, "name", entity.name, error)) {
        return errorJson(error);
    }
    if (!readTransform(args, entity.transform, error)) {
        return errorJson(error);
    }
    entity.primitive = primitive;
    entity.primitiveSize = size;
    ForwardRenderer::MaterialParams material;
    if (args.contains("material")) {
        if (!args["material"].is_object()) {
            return errorJson("material must be an object");
        }
        if (!readMaterial(args["material"], material, error)) {
            return errorJson(error);
        }
    }

    if (context.renderer != nullptr) {
        asset::MeshData mesh;
        if (primitive == "sphere") {
            mesh = asset::makeSphere(size * 0.5f);
        } else if (primitive == "cube") {
            mesh = asset::makeCube(size * 0.5f);
        } else {
            mesh = asset::makePlane(size * 0.5f);
        }
        const std::int32_t meshIndex = context.renderer->addMesh(mesh);
        const std::int32_t materialIndex = context.renderer->addMaterial(material);
        if (meshIndex < 0 || materialIndex < 0) {
            return errorJson("gpu upload failed");
        }
        entity.meshIndex = meshIndex;
        entity.materialIndex = materialIndex;
    }

    const scene::EntityId id = context.scene->entities.insert(entity);
    return json{{"status", "ok"}, {"entity_id", formatEntityId(id)}}.dump();
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

    registerSceneListTool(registry, context);

    auto add = [&](const char* name, const char* description, const char* schema, bool destructive,
                   auto handler) {
        registry.add({.name = name,
                      .description = description,
                      .parametersSchema = schema,
                      .destructive = destructive},
                     [context, handler](std::string_view argsJson) {
                         return handler(context, parseArgs(argsJson));
                     });
    };

    add("scene_add_entity",
        "Add a procedural primitive entity to the scene; returns its entity_id.",
        R"({"type":"object","properties":{
"name":{"type":"string","description":"Optional display name"},
"primitive":{"type":"string","enum":["sphere","cube","plane"]},
"size":{"type":"number","description":"Overall extent in meters (default 1); use scale for non-uniform sizing"},
"position":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3,"description":"World position [x,y,z]"},
"rotation_euler_deg":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3,"description":"Euler XYZ rotation in degrees"},
"scale":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},
"material":{"type":"object","properties":{
"base_color":{"type":"array","items":{"type":"number"},"minItems":4,"maxItems":4,"description":"Linear RGBA"},
"metallic":{"type":"number"},
"roughness":{"type":"number"},
"emissive":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}}}},
"required":["primitive"]})",
        false,
        [](const SceneToolContext& ctx, const json& args) { return sceneAddEntity(ctx, args); });

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
}

} // namespace kumo::agent
