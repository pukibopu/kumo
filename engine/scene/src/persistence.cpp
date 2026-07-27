#include <kumo/scene/persistence.h>

#include <kumo/math/math.h>

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstddef>
#include <format>
#include <utility>

namespace kumo::scene {

namespace {

using nlohmann::json;

json numberArray(const float* values, std::size_t count) {
    json out = json::array();
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(values[i]);
    }
    return out;
}

json numberArray(const math::float3& v) {
    const float values[3] = {v.x, v.y, v.z};
    return numberArray(values, 3);
}

json quatArray(const math::quat& q) {
    const float values[4] = {q.w, q.x, q.y, q.z};
    return numberArray(values, 4);
}

json materialJson(const SavedMaterial& material) {
    return {{"base_color", numberArray(material.baseColor, 4)},
            {"metallic", material.metallic},
            {"roughness", material.roughness},
            {"emissive", numberArray(material.emissive, 3)}};
}

json lightJson(const Light& light) {
    return {{"type", light.type == LightType::Point ? "point" : "directional"},
            {"color", numberArray(light.color)},
            {"intensity", light.intensity},
            {"position", numberArray(light.position)},
            {"direction", numberArray(light.direction)},
            {"range", light.range}};
}

// Readers return false only on a present-but-invalid field, with `error` set; an
// absent key leaves `out` untouched and succeeds, which is how omitted optional
// fields keep their struct default.
bool readNumbers(const json& obj, const char* key, float* out, std::size_t count,
                 std::string& error) {
    const auto it = obj.find(key);
    if (it == obj.end()) {
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

bool readFloat3(const json& obj, const char* key, math::float3& out, std::string& error) {
    float values[3] = {out.x, out.y, out.z};
    if (!readNumbers(obj, key, values, 3, error)) {
        return false;
    }
    out = {values[0], values[1], values[2]};
    return true;
}

bool readQuat(const json& obj, const char* key, math::quat& out, std::string& error) {
    float values[4] = {out.w, out.x, out.y, out.z};
    if (!readNumbers(obj, key, values, 4, error)) {
        return false;
    }
    out = math::quat(values[0], values[1], values[2], values[3]);
    return true;
}

bool readFloat(const json& obj, const char* key, float& out, std::string& error) {
    const auto it = obj.find(key);
    if (it == obj.end()) {
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

bool readString(const json& obj, const char* key, std::string& out, std::string& error) {
    const auto it = obj.find(key);
    if (it == obj.end()) {
        return true;
    }
    if (!it->is_string()) {
        error = std::format("{} must be a string", key);
        return false;
    }
    out = it->get<std::string>();
    return true;
}

bool readInt(const json& obj, const char* key, std::int32_t& out, std::string& error) {
    const auto it = obj.find(key);
    if (it == obj.end()) {
        return true;
    }
    if (!it->is_number_integer()) {
        error = std::format("{} must be an integer", key);
        return false;
    }
    out = it->get<std::int32_t>();
    return true;
}

bool readMaterial(const json& obj, SavedMaterial& material, std::string& error) {
    return readNumbers(obj, "base_color", material.baseColor, 4, error) &&
           readFloat(obj, "metallic", material.metallic, error) &&
           readFloat(obj, "roughness", material.roughness, error) &&
           readNumbers(obj, "emissive", material.emissive, 3, error);
}

std::expected<Camera, std::string> readCamera(const json& obj) {
    if (!obj.is_object()) {
        return std::unexpected("camera must be an object");
    }
    Camera camera;
    std::string error;
    if (!readFloat3(obj, "position", camera.position, error) ||
        !readQuat(obj, "rotation", camera.rotation, error) ||
        !readFloat(obj, "fov_y", camera.fovY, error) ||
        !readFloat(obj, "near", camera.nearZ, error)) {
        return std::unexpected(error);
    }
    return camera;
}

std::expected<Light, std::string> readLight(const json& obj) {
    if (!obj.is_object()) {
        return std::unexpected("each light must be an object");
    }
    Light light;
    std::string error;
    std::string type;
    if (!readString(obj, "type", type, error)) {
        return std::unexpected(error);
    }
    if (!type.empty()) {
        if (type == "directional") {
            light.type = LightType::Directional;
        } else if (type == "point") {
            light.type = LightType::Point;
        } else {
            return std::unexpected(std::format("unknown light type '{}'", type));
        }
    }
    if (!readFloat3(obj, "color", light.color, error) ||
        !readFloat(obj, "intensity", light.intensity, error) ||
        !readFloat3(obj, "position", light.position, error) ||
        !readFloat3(obj, "direction", light.direction, error) ||
        !readFloat(obj, "range", light.range, error)) {
        return std::unexpected(error);
    }
    return light;
}

std::expected<SavedEntity, std::string> readEntity(const json& obj) {
    if (!obj.is_object()) {
        return std::unexpected("each entity must be an object");
    }
    SavedEntity saved;
    Entity& entity = saved.entity;
    std::string error;
    if (!readString(obj, "name", entity.name, error) ||
        !readFloat3(obj, "position", entity.transform.position, error) ||
        !readQuat(obj, "rotation", entity.transform.rotation, error) ||
        !readFloat3(obj, "scale", entity.transform.scale, error) ||
        !readInt(obj, "mesh_index", entity.meshIndex, error) ||
        !readInt(obj, "material_index", entity.materialIndex, error) ||
        !readString(obj, "primitive", entity.primitive, error) ||
        !readFloat(obj, "primitive_size", entity.primitiveSize, error)) {
        return std::unexpected(error);
    }
    if (obj.contains("material")) {
        if (!obj["material"].is_object()) {
            return std::unexpected("material must be an object");
        }
        SavedMaterial material;
        if (!readMaterial(obj["material"], material, error)) {
            return std::unexpected(error);
        }
        saved.material = material;
    }
    return saved;
}

} // namespace

std::string saveSceneJson(const Scene& scene, std::string_view modelPath,
                          const MaterialLookup& materials) {
    json entities = json::array();
    scene.entities.forEach([&](EntityId, const Entity& entity) {
        json e{{"name", entity.name},
               {"position", numberArray(entity.transform.position)},
               {"rotation", quatArray(entity.transform.rotation)},
               {"scale", numberArray(entity.transform.scale)},
               {"mesh_index", entity.meshIndex},
               {"material_index", entity.materialIndex}};
        if (!entity.primitive.empty()) {
            e["primitive"] = entity.primitive;
            e["primitive_size"] = entity.primitiveSize;
        }
        if (entity.materialIndex >= 0 && materials) {
            if (const std::optional<SavedMaterial> material = materials(entity.materialIndex);
                material.has_value()) {
                e["material"] = materialJson(*material);
            }
        }
        entities.push_back(std::move(e));
    });

    json lights = json::array();
    for (const Light& light : scene.lights()) {
        lights.push_back(lightJson(light));
    }

    const Camera& camera = scene.camera;
    const json out{{"version", kSceneFormatVersion},
                   {"model", std::string(modelPath)},
                   {"camera",
                    {{"position", numberArray(camera.position)},
                     {"rotation", quatArray(camera.rotation)},
                     {"fov_y", camera.fovY},
                     {"near", camera.nearZ}}},
                   {"lights", std::move(lights)},
                   {"entities", std::move(entities)}};
    // Invalid UTF-8 (e.g. legacy-encoded glTF node names carried in entity
    // names) must degrade to U+FFFD, never to a dump() throw.
    return out.dump(2, ' ', false, json::error_handler_t::replace);
}

std::expected<SavedScene, std::string> parseSceneJson(std::string_view text) {
    const json parsed = json::parse(text, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return std::unexpected("scene file is not valid JSON");
    }

    const auto versionIt = parsed.find("version");
    const bool versionOk = versionIt != parsed.end() && versionIt->is_number_integer() &&
                           versionIt->get<int>() == kSceneFormatVersion;
    if (!versionOk) {
        const std::string got = versionIt != parsed.end() && versionIt->is_number_integer()
                                    ? std::to_string(versionIt->get<int>())
                                    : "missing";
        return std::unexpected(std::format("unsupported scene file version {} (expected {})", got,
                                           kSceneFormatVersion));
    }

    SavedScene scene;
    std::string error;
    if (!readString(parsed, "model", scene.modelPath, error)) {
        return std::unexpected(error);
    }
    if (parsed.contains("camera")) {
        auto camera = readCamera(parsed["camera"]);
        if (!camera.has_value()) {
            return std::unexpected(camera.error());
        }
        scene.camera = *camera;
    }
    if (parsed.contains("lights")) {
        if (!parsed["lights"].is_array()) {
            return std::unexpected("lights must be an array");
        }
        for (const json& item : parsed["lights"]) {
            auto light = readLight(item);
            if (!light.has_value()) {
                return std::unexpected(light.error());
            }
            scene.lights.push_back(*light);
        }
    }
    if (parsed.contains("entities")) {
        if (!parsed["entities"].is_array()) {
            return std::unexpected("entities must be an array");
        }
        for (const json& item : parsed["entities"]) {
            auto entity = readEntity(item);
            if (!entity.has_value()) {
                return std::unexpected(entity.error());
            }
            scene.entities.push_back(std::move(*entity));
        }
    }
    return scene;
}

} // namespace kumo::scene
