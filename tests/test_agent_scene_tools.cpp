#include <doctest/doctest.h>

#include <kumo/agent/scene_tools.h>
#include <kumo/agent/tool_registry.h>
#include <kumo/math/math.h>
#include <kumo/scene/scene.h>

#include <nlohmann/json.hpp>

#include <cmath>
#include <format>
#include <string>

using namespace kumo;
using namespace kumo::agent;
using nlohmann::json;

namespace {

struct Fixture {
    scene::Scene scene;
    ToolRegistry registry;

    Fixture() { registerSceneTools(registry, {.scene = &scene, .renderer = nullptr}); }

    json invoke(const char* name, const std::string& args) {
        const json result = json::parse(registry.invoke(name, args), nullptr, false);
        REQUIRE(result.is_object());
        return result;
    }
};

} // namespace

TEST_CASE("scene tools register all seven with object schemas") {
    Fixture f;
    const char* expected[] = {"scene_list",          "scene_add_entity", "scene_remove_entity",
                              "scene_set_transform", "camera_set",       "light_set",
                              "material_set_param"};
    for (const char* name : expected) {
        const ToolDef* def = f.registry.find(name);
        REQUIRE_MESSAGE(def != nullptr, name);
        CHECK(!def->description.empty());
        // Every schema must parse as a JSON object: the MCP export precondition.
        const json schema = json::parse(def->parametersSchema, nullptr, false);
        CHECK_MESSAGE(schema.is_object(), name);
        CHECK(schema["type"] == "object");
    }
    CHECK(f.registry.find("scene_remove_entity")->destructive);
    CHECK(!f.registry.find("scene_add_entity")->destructive);
}

TEST_CASE("scene_add_entity inserts an entity and returns its id") {
    Fixture f;
    const json result = f.invoke(
        "scene_add_entity",
        R"({"name":"ball","primitive":"sphere","position":[1,2,3],"rotation_euler_deg":[0,45,0],"scale":[2,2,2]})");
    REQUIRE(result["status"] == "ok");
    CHECK(result["entity_id"] == "0:0");
    REQUIRE(f.scene.entities.size() == 1);

    const scene::Entity* entity = f.scene.entities.get({0, 0});
    REQUIRE(entity != nullptr);
    CHECK(entity->name == "ball");
    CHECK(entity->transform.position.x == doctest::Approx(1.0f));
    CHECK(entity->transform.position.z == doctest::Approx(3.0f));
    CHECK(entity->transform.scale.x == doctest::Approx(2.0f));
    const math::float3 euler = math::eulerDegrees(entity->transform.rotation);
    CHECK(euler.y == doctest::Approx(45.0f).epsilon(0.01));
    // Without a renderer there is nothing to upload.
    CHECK(entity->meshIndex == -1);
}

TEST_CASE("scene_add_entity validates primitive and field types") {
    Fixture f;
    CHECK(f.invoke("scene_add_entity", R"({"primitive":"torus"})")["status"] == "error");
    CHECK(f.invoke("scene_add_entity", R"({})")["status"] == "error");
    CHECK(f.invoke("scene_add_entity", R"({"primitive":"cube","position":[1,2]})")["status"] ==
          "error");
    CHECK(f.invoke("scene_add_entity", R"({"primitive":"cube","size":"big"})")["status"] ==
          "error");
    CHECK(f.scene.entities.empty());
}

TEST_CASE("scene_set_transform updates only the provided fields") {
    Fixture f;
    f.invoke("scene_add_entity", R"({"primitive":"cube","position":[1,1,1]})");
    const json result =
        f.invoke("scene_set_transform", R"({"entity_id":"0:0","position":[5,6,7]})");
    CHECK(result["status"] == "ok");
    const scene::Entity* entity = f.scene.entities.get({0, 0});
    REQUIRE(entity != nullptr);
    CHECK(entity->transform.position.y == doctest::Approx(6.0f));
    CHECK(entity->transform.scale.x == doctest::Approx(1.0f));
}

TEST_CASE("entity id parsing rejects garbage and stale ids") {
    Fixture f;
    f.invoke("scene_add_entity", R"({"primitive":"cube"})");
    CHECK(f.invoke("scene_set_transform", R"({"entity_id":"abc","position":[0,0,0]})")["status"] ==
          "error");
    CHECK(f.invoke("scene_set_transform", R"({"position":[0,0,0]})")["status"] == "error");

    REQUIRE(f.invoke("scene_remove_entity", R"({"entity_id":"0:0"})")["status"] == "ok");
    CHECK(f.scene.entities.empty());
    // The slot is reused with a bumped generation; the stale id must miss.
    f.invoke("scene_add_entity", R"({"primitive":"cube"})");
    const json stale = f.invoke("scene_set_transform", R"({"entity_id":"0:0","scale":[9,9,9]})");
    CHECK(stale["status"] == "error");
    CHECK(stale["message"].get<std::string>().find("not found") != std::string::npos);
}

TEST_CASE("scene_remove_entity on a missing id reports not found") {
    Fixture f;
    const json result = f.invoke("scene_remove_entity", R"({"entity_id":"4:2"})");
    CHECK(result["status"] == "error");
}

TEST_CASE("camera_set applies position, look_at and fov") {
    Fixture f;
    const json result = f.invoke(
        "camera_set", R"({"position":[0,2,5],"look_at":[0,0,0],"fov_y_deg":45,"near":0.5})");
    CHECK(result["status"] == "ok");
    CHECK(f.scene.camera.position.y == doctest::Approx(2.0f));
    CHECK(f.scene.camera.fovY == doctest::Approx(math::radians(45.0f)));
    CHECK(f.scene.camera.nearZ == doctest::Approx(0.5f));
    // Looking at the origin from +Z keeps forward pointing down -Z.
    const math::float3 euler = math::eulerDegrees(f.scene.camera.rotation);
    CHECK(euler.y == doctest::Approx(0.0f).epsilon(0.01));
}

TEST_CASE("light_set appends when index is omitted and enforces the 16 cap") {
    Fixture f;
    const json first = f.invoke(
        "light_set", R"({"type":"point","color":[1,0,0],"intensity":2,"position":[0,3,0]})");
    REQUIRE(first["status"] == "ok");
    CHECK(first["index"] == 0);
    REQUIRE(f.scene.lights().size() == 1);
    CHECK(f.scene.lights()[0].type == scene::LightType::Point);
    CHECK(f.scene.lights()[0].intensity == doctest::Approx(2.0f));

    for (std::size_t i = 1; i < scene::Scene::kMaxLights; ++i) {
        REQUIRE(f.invoke("light_set", R"({"intensity":1})")["status"] == "ok");
    }
    const json overflow = f.invoke("light_set", R"({"intensity":1})");
    CHECK(overflow["status"] == "error");
    CHECK(overflow["message"].get<std::string>().find("16") != std::string::npos);
}

TEST_CASE("light_set modifies an existing light and rejects bad indices") {
    Fixture f;
    f.invoke("light_set", R"({"intensity":1})");
    const json result = f.invoke("light_set", R"({"index":0,"intensity":7,"color":[0,1,0]})");
    CHECK(result["status"] == "ok");
    CHECK(f.scene.lights()[0].intensity == doctest::Approx(7.0f));
    CHECK(f.invoke("light_set", R"({"index":5,"intensity":1})")["status"] == "error");
    CHECK(f.invoke("light_set", R"({"index":0,"type":"spot"})")["status"] == "error");
}

TEST_CASE("material_set_param without a renderer reports a structured error") {
    Fixture f;
    f.invoke("scene_add_entity", R"({"primitive":"sphere"})");
    const json result = f.invoke("material_set_param", R"({"entity_id":"0:0","metallic":0.5})");
    CHECK(result["status"] == "error");
    CHECK(result["message"].get<std::string>().find("renderer") != std::string::npos);
}

TEST_CASE("scene_list reports entities, camera and lights as compact JSON") {
    Fixture f;
    f.invoke("scene_add_entity", R"({"name":"a","primitive":"cube","position":[1,0,-2]})");
    f.invoke("light_set", R"({"intensity":3})");

    const json listed = json::parse(f.registry.invoke("scene_list", ""), nullptr, false);
    REQUIRE(listed.is_object());
    REQUIRE(listed["entities"].size() == 1);
    const json& entity = listed["entities"][0];
    CHECK(entity["id"] == "0:0");
    CHECK(entity["name"] == "a");
    CHECK(entity["position"][2] == doctest::Approx(-2.0));
    // No renderer, so no AABB or material report.
    CHECK(!entity.contains("aabb_world"));
    CHECK(!entity.contains("material"));
    CHECK(listed["camera"]["fov_y_deg"] == doctest::Approx(60.0));
    REQUIRE(listed["lights"].size() == 1);
    CHECK(listed["lights"][0]["intensity"] == doctest::Approx(3.0));
}
