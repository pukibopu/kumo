#include <kumo/agent/scene_tools.h>

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

namespace {

using nlohmann::json;
using renderer::ForwardRenderer;

std::string errorJson(std::string_view message) {
    return json{{"status", "error"}, {"message", message}}.dump();
}

std::string okJson() {
    return json{{"status", "ok"}}.dump();
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

std::string formatId(scene::EntityId id) {
    return std::format("{}:{}", id.index, id.generation);
}

std::optional<scene::EntityId> parseId(std::string_view text) {
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

// Readers return false only on a present-but-invalid field, with `error` set; an
// absent key leaves `out` untouched and succeeds, which is what gives every tool
// its partial-update semantics.
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
        out[i] = (*it)[i].get<float>();
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
    out = it->get<float>();
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
    const std::optional<scene::EntityId> id = parseId(text);
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
        json e{{"id", formatId(id)},
               {"name", entity.name},
               {"position", numberArray(entity.transform.position)},
               {"rotation_euler_deg", numberArray(math::eulerDegrees(entity.transform.rotation))},
               {"scale", numberArray(entity.transform.scale)}};
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
    return out.dump();
}

std::string sceneAddEntity(const SceneToolContext& context, const json& args) {
    const std::string primitive = args.value("primitive", "");
    if (primitive != "sphere" && primitive != "cube" && primitive != "plane") {
        return errorJson("primitive must be one of: sphere, cube, plane");
    }
    std::string error;
    float size = 1.0f;
    if (!readNumber(args, "size", size, error)) {
        return errorJson(error);
    }

    scene::Entity entity;
    entity.name = args.value("name", primitive);
    if (!readTransform(args, entity.transform, error)) {
        return errorJson(error);
    }
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
    return json{{"status", "ok"}, {"entity_id", formatId(id)}}.dump();
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
    std::string error;
    if (!readTransform(args, lookup->entity->transform, error)) {
        return errorJson(error);
    }
    return okJson();
}

std::string cameraSet(const SceneToolContext& context, const json& args) {
    scene::Camera& camera = context.scene->camera;
    std::string error;
    if (!readFloat3(args, "position", camera.position, error)) {
        return errorJson(error);
    }
    float fovDeg = math::degrees(camera.fovY);
    if (!readNumber(args, "fov_y_deg", fovDeg, error)) {
        return errorJson(error);
    }
    camera.fovY = math::radians(std::clamp(fovDeg, 1.0f, 179.0f));
    if (!readNumber(args, "near", camera.nearZ, error)) {
        return errorJson(error);
    }
    camera.nearZ = std::max(camera.nearZ, 0.001f);
    if (args.contains("look_at")) {
        math::float3 target{0.0f, 0.0f, 0.0f};
        if (!readFloat3(args, "look_at", target, error)) {
            return errorJson(error);
        }
        camera.lookAt(target);
    }
    return okJson();
}

std::string lightSet(const SceneToolContext& context, const json& args) {
    scene::Scene& scene = *context.scene;
    std::size_t index = 0;
    if (args.contains("index")) {
        if (!args["index"].is_number_unsigned()) {
            return errorJson("index must be a non-negative integer");
        }
        index = args["index"].get<std::size_t>();
        if (scene.light(index) == nullptr) {
            return errorJson(std::format("light index {} out of range ({} lights)", index,
                                         scene.lights().size()));
        }
    } else {
        if (!scene.addLight({})) {
            return errorJson("light budget exhausted: the scene supports at most 16 lights");
        }
        index = scene.lights().size() - 1;
    }
    scene::Light& light = *scene.light(index);

    if (args.contains("type")) {
        const std::string type = args.value("type", "");
        if (type == "directional") {
            light.type = scene::LightType::Directional;
        } else if (type == "point") {
            light.type = scene::LightType::Point;
        } else {
            return errorJson("type must be one of: directional, point");
        }
    }
    std::string error;
    if (!readFloat3(args, "color", light.color, error) ||
        !readNumber(args, "intensity", light.intensity, error) ||
        !readFloat3(args, "direction", light.direction, error) ||
        !readFloat3(args, "position", light.position, error) ||
        !readNumber(args, "range", light.range, error)) {
        return errorJson(error);
    }
    return json{{"status", "ok"}, {"index", index}}.dump();
}

std::string materialSetParam(const SceneToolContext& context, const json& args) {
    if (context.renderer == nullptr) {
        return errorJson("renderer unavailable");
    }
    auto lookup = findEntity(*context.scene, args);
    if (!lookup.has_value()) {
        return lookup.error();
    }
    if (lookup->entity->materialIndex < 0) {
        return errorJson("entity has no material");
    }
    const auto materialIndex = static_cast<std::uint32_t>(lookup->entity->materialIndex);
    const ForwardRenderer::MaterialParams* current =
        context.renderer->materialParams(materialIndex);
    if (current == nullptr) {
        return errorJson("material index out of range");
    }
    ForwardRenderer::MaterialParams params = *current;
    std::string error;
    if (!readMaterial(args, params, error)) {
        return errorJson(error);
    }
    context.renderer->setMaterialParams(materialIndex, params);
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

void registerSceneTools(ToolRegistry& registry, SceneToolContext context) {
    KUMO_ASSERT(context.scene != nullptr);

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

    registry.add({.name = "scene_list",
                  .description = "List the scene: every entity with id, transform, world-space "
                                 "AABB and material factors, plus the camera and lights. Call "
                                 "this before spatial reasoning or edits.",
                  .parametersSchema = R"({"type":"object","properties":{}})",
                  .destructive = false},
                 [context](std::string_view) { return sceneList(context); });

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
