#include <doctest/doctest.h>

// Private kumo_agent header (engine/agent/src), for the asset_search fixtures.
#include "asset_index.h"

#include <kumo/agent/entity_id.h>
#include <kumo/agent/scene_tools.h>
#include <kumo/agent/tool_registry.h>
#include <kumo/asset/asset.h>
#include <kumo/asset/procedural_sky.h>
#include <kumo/math/math.h>
#include <kumo/scene/scene.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

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

json invokeOn(ToolRegistry& registry, const char* name, const std::string& args = "") {
    const json result = json::parse(registry.invoke(name, args), nullptr, false);
    REQUIRE(result.is_object());
    return result;
}

// True when some finding in `findings` has the given check name.
bool hasFinding(const json& findings, const char* check) {
    for (const auto& finding : findings) {
        if (finding["check"] == check) {
            return true;
        }
    }
    return false;
}

// Removed on destruction; each test gets a fresh, non-colliding directory
// (mirrors test_texture_set.cpp's TempDir, duplicated locally since it is
// private to that translation unit).
struct TempDir {
    std::filesystem::path path;
    explicit TempDir(const char* name) : path(std::filesystem::temp_directory_path() / name) {
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir() { std::filesystem::remove_all(path); }
};

void touch(const std::filesystem::path& path) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path).put('x');
}

// One flat-colored RGBA8 image, same helper as test_texture_set.cpp: real
// pixel data is only needed where loadTextureSet must actually succeed.
std::vector<std::uint8_t> flatPixels(std::uint32_t width, std::uint32_t height,
                                     std::uint8_t value) {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i + 0] = value;
        pixels[i + 1] = value;
        pixels[i + 2] = value;
        pixels[i + 3] = 255;
    }
    return pixels;
}

} // namespace

TEST_CASE("scene tools register all seventeen with object schemas") {
    Fixture f;
    const char* expected[] = {"scene_list",          "asset_list",         "scene_add_entity",
                              "scene_add_entities",  "scene_add_model",    "scene_remove_entity",
                              "scene_set_transform", "camera_set",         "light_set",
                              "light_remove",        "material_set_param", "material_set_texture",
                              "asset_fetch",         "scene_define_group", "scene_instance_group",
                              "environment_set",     "scene_validate"};
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
    CHECK(f.registry.find("light_remove")->destructive);
    CHECK(!f.registry.find("scene_add_entity")->destructive);
    CHECK(!f.registry.find("scene_add_entities")->destructive);
    CHECK(!f.registry.find("scene_add_model")->destructive);
    CHECK(!f.registry.find("scene_define_group")->destructive);
    CHECK(!f.registry.find("scene_instance_group")->destructive);
    CHECK(!f.registry.find("environment_set")->destructive);
    CHECK(!f.registry.find("scene_validate")->destructive);
    CHECK(!f.registry.find("asset_list")->destructive);
    CHECK(!f.registry.find("material_set_texture")->destructive);
    CHECK(!f.registry.find("asset_fetch")->destructive);
}

TEST_CASE("registerSceneListTool registers exactly scene_list") {
    scene::Scene scene;
    ToolRegistry registry;
    registerSceneListTool(registry, {.scene = &scene, .renderer = nullptr});
    REQUIRE(registry.defs().size() == 1);
    CHECK(registry.defs()[0].name == "scene_list");
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
    CHECK(entity->primitive == "sphere");
    CHECK(entity->primitiveSize == doctest::Approx(1.0f));
}

TEST_CASE("scene_add_entity accepts every one of the seven primitive names") {
    Fixture f;
    const char* names[] = {"sphere", "cube", "plane", "cylinder", "cone", "torus", "capsule"};
    for (const char* name : names) {
        const json result =
            f.invoke("scene_add_entity", std::format(R"({{"primitive":"{}"}})", name));
        REQUIRE_MESSAGE(result["status"] == "ok", name);
    }
    REQUIRE(f.scene.entities.size() == 7);
    // "cylinder" is the fourth insert (index 3), provenance recorded verbatim.
    const scene::Entity* cylinder = f.scene.entities.get({3, 0});
    REQUIRE(cylinder != nullptr);
    CHECK(cylinder->primitive == "cylinder");
    CHECK(cylinder->primitiveSize == doctest::Approx(1.0f));
}

TEST_CASE("scene_add_entity validates primitive and field types") {
    Fixture f;
    const json unknown = f.invoke("scene_add_entity", R"({"primitive":"torus_knot"})");
    CHECK(unknown["status"] == "error");
    // The error must name every supported primitive so the model can self-correct.
    for (const char* name : {"sphere", "cube", "plane", "cylinder", "cone", "torus", "capsule"}) {
        CHECK_MESSAGE(unknown["message"].get<std::string>().find(name) != std::string::npos, name);
    }
    CHECK(f.invoke("scene_add_entity", R"({})")["status"] == "error");
    CHECK(f.invoke("scene_add_entity", R"({"primitive":"cube","position":[1,2]})")["status"] ==
          "error");
    CHECK(f.invoke("scene_add_entity", R"({"primitive":"cube","size":"big"})")["status"] ==
          "error");
    CHECK(f.invoke("scene_add_entity", R"({"primitive":"cube","size":0})")["status"] == "error");
    // Wrong-typed strings must yield the tool's own vocabulary, not raw
    // nlohmann exception text.
    const json badName = f.invoke("scene_add_entity", R"({"primitive":"cube","name":42})");
    CHECK(badName["status"] == "error");
    CHECK(badName["message"].get<std::string>().find("json.exception") == std::string::npos);
    CHECK(badName["message"].get<std::string>().find("name") != std::string::npos);
    CHECK(f.scene.entities.empty());
}

TEST_CASE("scene_add_entities inserts every item in order and returns their ids") {
    Fixture f;
    const json result = f.invoke("scene_add_entities",
                                 R"({"entities":[
{"name":"a","primitive":"sphere","position":[1,0,0]},
{"name":"b","primitive":"cylinder","position":[2,0,0]},
{"name":"c","primitive":"capsule","position":[3,0,0]}
]})");
    REQUIRE(result["status"] == "ok");
    REQUIRE(result["entity_ids"].size() == 3);
    CHECK(result["entity_ids"][0] == "0:0");
    CHECK(result["entity_ids"][1] == "1:0");
    CHECK(result["entity_ids"][2] == "2:0");
    REQUIRE(f.scene.entities.size() == 3);

    const scene::Entity* a = f.scene.entities.get({0, 0});
    const scene::Entity* b = f.scene.entities.get({1, 0});
    const scene::Entity* c = f.scene.entities.get({2, 0});
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(c != nullptr);
    CHECK(a->name == "a");
    CHECK(a->primitive == "sphere");
    CHECK(b->name == "b");
    CHECK(b->primitive == "cylinder");
    CHECK(b->transform.position.x == doctest::Approx(2.0f));
    CHECK(c->name == "c");
    CHECK(c->primitive == "capsule");
}

TEST_CASE("scene_add_entities is atomic: one bad item creates nothing") {
    Fixture f;
    const json result = f.invoke("scene_add_entities",
                                 R"({"entities":[
{"primitive":"cube"},
{"primitive":"cube","scale":[1,-1,1]},
{"primitive":"cube"}
]})");
    CHECK(result["status"] == "error");
    CHECK(result["message"].get<std::string>().find("entities[1]") != std::string::npos);
    CHECK(f.scene.entities.empty());
}

TEST_CASE("scene_add_entities rejects an empty array") {
    Fixture f;
    const json result = f.invoke("scene_add_entities", R"({"entities":[]})");
    CHECK(result["status"] == "error");
    CHECK(f.scene.entities.empty());
}

TEST_CASE("scene_add_entities rejects more than 128 entities") {
    Fixture f;
    std::string args = R"({"entities":[)";
    for (int i = 0; i < 129; ++i) {
        if (i > 0) {
            args += ",";
        }
        args += R"({"primitive":"cube"})";
    }
    args += "]}";
    const json result = f.invoke("scene_add_entities", args);
    CHECK(result["status"] == "error");
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

TEST_CASE("scene_set_transform is atomic: an invalid field leaves the entity untouched") {
    Fixture f;
    f.invoke("scene_add_entity", R"({"primitive":"cube","position":[1,1,1]})");
    const json result =
        f.invoke("scene_set_transform", R"({"entity_id":"0:0","position":[5,6,7],"scale":[1,2]})");
    CHECK(result["status"] == "error");
    const scene::Entity* entity = f.scene.entities.get({0, 0});
    REQUIRE(entity != nullptr);
    // The valid position that preceded the bad scale must not have been applied.
    CHECK(entity->transform.position.x == doctest::Approx(1.0f));
}

TEST_CASE("scene_set_transform rejects degenerate and non-finite values") {
    Fixture f;
    f.invoke("scene_add_entity", R"({"primitive":"cube"})");
    // Zero scale would make the model matrix singular (NaN normal matrix).
    CHECK(f.invoke("scene_set_transform", R"({"entity_id":"0:0","scale":[0,0,0]})")["status"] ==
          "error");
    CHECK(f.invoke("scene_set_transform", R"({"entity_id":"0:0","scale":[1,-1,1]})")["status"] ==
          "error");
    // 1e39 exceeds float range and would arrive as inf.
    CHECK(f.invoke("scene_set_transform",
                   R"({"entity_id":"0:0","position":[1e39,0,0]})")["status"] == "error");
    const scene::Entity* entity = f.scene.entities.get({0, 0});
    REQUIRE(entity != nullptr);
    CHECK(entity->transform.scale.x == doctest::Approx(1.0f));
    CHECK(entity->transform.position.x == doctest::Approx(0.0f));
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

TEST_CASE("camera_set is atomic: an invalid field leaves the camera untouched") {
    Fixture f;
    const json result = f.invoke("camera_set", R"({"position":[9,9,9],"fov_y_deg":"wide"})");
    CHECK(result["status"] == "error");
    CHECK(f.scene.camera.position.z == doctest::Approx(3.0f));
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
    // The schema says integer; an integral float still addresses light 0.
    CHECK(f.invoke("light_set", R"({"index":0.0,"intensity":2})")["status"] == "ok");
    CHECK(f.scene.lights()[0].intensity == doctest::Approx(2.0f));
    CHECK(f.invoke("light_set", R"({"index":0.5,"intensity":2})")["status"] == "error");
}

TEST_CASE("light_set is atomic: a rejected call appends nothing and edits nothing") {
    Fixture f;
    // Append-mode rejection must not leave a default light behind.
    CHECK(f.invoke("light_set", R"({"type":"spot"})")["status"] == "error");
    CHECK(f.invoke("light_set", R"({"color":[1,0]})")["status"] == "error");
    CHECK(f.scene.lights().empty());
    // Edit-mode rejection must not half-apply earlier fields.
    f.invoke("light_set", R"({"intensity":1})");
    CHECK(f.invoke("light_set", R"({"index":0,"intensity":9,"color":[1,0]})")["status"] == "error");
    CHECK(f.scene.lights()[0].intensity == doctest::Approx(1.0f));
}

TEST_CASE("light_set rejects a zero direction") {
    Fixture f;
    f.invoke("light_set", R"({"intensity":1})");
    // The renderer normalizes unconditionally; zero would become NaN state.
    const json result = f.invoke("light_set", R"({"index":0,"direction":[0,0,0]})");
    CHECK(result["status"] == "error");
    CHECK(f.scene.lights()[0].direction.y == doctest::Approx(-1.0f));
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
    CHECK(entity["primitive"] == "cube");
    // No renderer, so no AABB or material report.
    CHECK(!entity.contains("aabb_world"));
    CHECK(!entity.contains("material"));
    CHECK(listed["camera"]["fov_y_deg"] == doctest::Approx(60.0));
    REQUIRE(listed["lights"].size() == 1);
    CHECK(listed["lights"][0]["intensity"] == doctest::Approx(3.0));
}

TEST_CASE("scene_define_group validates every member and stores nothing in the scene") {
    Fixture f;
    const json result = f.invoke("scene_define_group",
                                 R"({"name":"pine","entities":[
{"name":"trunk","primitive":"cylinder","position":[0,0.5,0]},
{"name":"top","primitive":"cone","position":[0,1.2,0]}
]})");
    REQUIRE(result["status"] == "ok");
    CHECK(result["group"] == "pine");
    CHECK(result["members"] == 2);
    CHECK(f.scene.entities.empty());
}

TEST_CASE("scene_define_group rejects a bad member, naming it, and stores nothing") {
    Fixture f;
    const json result = f.invoke("scene_define_group",
                                 R"({"name":"bad","entities":[
{"primitive":"cube"},
{"primitive":"cube","scale":[1,-1,1]}
]})");
    CHECK(result["status"] == "error");
    CHECK(result["message"].get<std::string>().find("entities[1]") != std::string::npos);

    // Nothing was stored, so a later instance call reports an unknown group.
    const json instance =
        f.invoke("scene_instance_group", R"({"name":"bad","transforms":[{"position":[0,0,0]}]})");
    CHECK(instance["status"] == "error");
    CHECK(instance["message"].get<std::string>().find("unknown group") != std::string::npos);
}

TEST_CASE("scene_define_group redefinition overwrites the prior definition") {
    Fixture f;
    REQUIRE(f.invoke("scene_define_group",
                     R"({"name":"g","entities":[{"primitive":"cube"}]})")["members"] == 1);
    const json result =
        f.invoke("scene_define_group",
                 R"({"name":"g","entities":[{"primitive":"cube"},{"primitive":"sphere"}]})");
    REQUIRE(result["status"] == "ok");
    CHECK(result["members"] == 2);
}

TEST_CASE("scene_define_group rejects an empty or too-long name") {
    Fixture f;
    CHECK(f.invoke("scene_define_group",
                   R"({"name":"","entities":[{"primitive":"cube"}]})")["status"] == "error");
    const std::string longName(65, 'a');
    CHECK(f.invoke("scene_define_group",
                   std::format(R"({{"name":"{}","entities":[{{"primitive":"cube"}}]}})",
                               longName))["status"] == "error");
}

TEST_CASE("scene_define_group rejects more than 32 entities") {
    Fixture f;
    std::string args = R"({"name":"big","entities":[)";
    for (int i = 0; i < 33; ++i) {
        if (i > 0) {
            args += ",";
        }
        args += R"({"primitive":"cube"})";
    }
    args += "]}";
    CHECK(f.invoke("scene_define_group", args)["status"] == "error");
}

TEST_CASE("scene_instance_group with explicit transforms stamps every member per instance") {
    Fixture f;
    f.invoke("scene_define_group", R"({"name":"pair","entities":[
{"name":"a","primitive":"cube","position":[0,0,0]},
{"name":"b","primitive":"sphere","position":[1,0,0]}
]})");
    const json result = f.invoke("scene_instance_group", R"({"name":"pair","transforms":[
{"position":[10,0,0]},
{"position":[20,0,0]}
]})");
    REQUIRE(result["status"] == "ok");
    CHECK(result["instances"] == 2);
    REQUIRE(result["entity_ids"].size() == 4);
    CHECK(result["entity_ids"][0] == "0:0");
    CHECK(result["entity_ids"][3] == "3:0");
    REQUIRE(f.scene.entities.size() == 4);

    const scene::Entity* e0 = f.scene.entities.get({0, 0});
    const scene::Entity* e1 = f.scene.entities.get({1, 0});
    const scene::Entity* e2 = f.scene.entities.get({2, 0});
    const scene::Entity* e3 = f.scene.entities.get({3, 0});
    REQUIRE(e0 != nullptr);
    REQUIRE(e1 != nullptr);
    REQUIRE(e2 != nullptr);
    REQUIRE(e3 != nullptr);
    CHECK(e0->name == "pair_0_a");
    CHECK(e1->name == "pair_0_b");
    CHECK(e2->name == "pair_1_a");
    CHECK(e3->name == "pair_1_b");
    CHECK(e0->transform.position.x == doctest::Approx(10.0f));
    // Member "b" sits at local (1,0,0); with an identity instance rotation and
    // unit scale the world position is a plain offset.
    CHECK(e1->transform.position.x == doctest::Approx(11.0f));
    CHECK(e2->transform.position.x == doctest::Approx(20.0f));
    CHECK(e3->transform.position.x == doctest::Approx(21.0f));
}

TEST_CASE("scene_instance_group composes instance and member transforms") {
    Fixture f;
    f.invoke("scene_define_group",
             R"({"name":"g","entities":[{"name":"m","primitive":"cube","position":[1,0,0]}]})");
    const json result = f.invoke("scene_instance_group", R"({"name":"g","transforms":[
{"position":[5,2,3],"rotation_euler_deg":[0,90,0],"scale":2}
]})");
    REQUIRE(result["status"] == "ok");
    const scene::Entity* e = f.scene.entities.get({0, 0});
    REQUIRE(e != nullptr);

    // world = instPos + instRot * (instScale * memberLocal); computed directly
    // via quatFromEulerDegrees rather than assuming a rotation convention.
    const math::quat instRot = math::quatFromEulerDegrees({0.0f, 90.0f, 0.0f});
    const math::float3 memberLocal{1.0f, 0.0f, 0.0f};
    const math::float3 instScale{2.0f, 2.0f, 2.0f};
    const math::float3 expected =
        math::float3{5.0f, 2.0f, 3.0f} + instRot * (instScale * memberLocal);

    CHECK(e->transform.position.x == doctest::Approx(expected.x));
    CHECK(e->transform.position.y == doctest::Approx(expected.y));
    CHECK(e->transform.position.z == doctest::Approx(expected.z));
    CHECK(e->transform.scale.x == doctest::Approx(2.0f));
}

TEST_CASE("scene_instance_group composition is exact for rotated members") {
    Fixture f;
    // A member with its own local rotation: uniform instance scale must make
    // the composed TRS match the true matrix product with zero shear.
    f.invoke(
        "scene_define_group",
        R"({"name":"g","entities":[{"name":"m","primitive":"cube","position":[1,0,0],"rotation_euler_deg":[0,0,90],"scale":[1,2,1]}]})");
    const json result = f.invoke("scene_instance_group", R"({"name":"g","transforms":[
{"position":[5,0,0],"rotation_euler_deg":[0,90,0],"scale":2}
]})");
    REQUIRE(result["status"] == "ok");
    const scene::Entity* e = f.scene.entities.get({0, 0});
    REQUIRE(e != nullptr);

    scene::Transform instance;
    instance.position = {5.0f, 0.0f, 0.0f};
    instance.rotation = math::quatFromEulerDegrees({0.0f, 90.0f, 0.0f});
    instance.scale = {2.0f, 2.0f, 2.0f};
    scene::Transform member;
    member.position = {1.0f, 0.0f, 0.0f};
    member.rotation = math::quatFromEulerDegrees({0.0f, 0.0f, 90.0f});
    member.scale = {1.0f, 2.0f, 1.0f};

    const math::float4x4 truth = instance.matrix() * member.matrix();
    const math::float4x4 composed = e->transform.matrix();
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            CHECK(composed[c][r] == doctest::Approx(truth[c][r]).epsilon(0.001));
        }
    }
}

TEST_CASE("scene_instance_group rejects per-axis instance scale") {
    Fixture f;
    f.invoke("scene_define_group", R"({"name":"g","entities":[{"name":"m","primitive":"cube"}]})");
    const json result = f.invoke("scene_instance_group", R"({"name":"g","transforms":[
{"position":[0,0,0],"scale":[1,1,0.5]}
]})");
    CHECK(result["status"] == "error");
    CHECK(result["message"].get<std::string>().find("uniform") != std::string::npos);
    CHECK(f.scene.entities.empty());
}

TEST_CASE("scene_instance_group scatter is deterministic for a fixed seed") {
    auto samplePositions = [](std::int64_t seed) {
        Fixture fx;
        fx.invoke("scene_define_group",
                  R"({"name":"tree","entities":[{"name":"trunk","primitive":"cylinder"}]})");
        const json result = fx.invoke(
            "scene_instance_group",
            std::format(R"({{"name":"tree","scatter":{{"count":5,"area":[10,10],"seed":{}}}}})",
                        seed));
        REQUIRE(result["status"] == "ok");
        std::vector<math::float3> positions;
        fx.scene.entities.forEach([&](scene::EntityId, const scene::Entity& e) {
            positions.push_back(e.transform.position);
        });
        return positions;
    };

    const std::vector<math::float3> a = samplePositions(42);
    const std::vector<math::float3> b = samplePositions(42);
    REQUIRE(a.size() == 5);
    REQUIRE(b.size() == 5);
    for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].x == doctest::Approx(b[i].x));
        CHECK(a[i].z == doctest::Approx(b[i].z));
    }

    const std::vector<math::float3> c = samplePositions(7);
    bool anyDifferent = false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::abs(a[i].x - c[i].x) > 1e-4f || std::abs(a[i].z - c[i].z) > 1e-4f) {
            anyDifferent = true;
        }
    }
    CHECK(anyDifferent);
}

TEST_CASE("scene_instance_group rejects instances x members over 256 and creates nothing") {
    Fixture f;
    f.invoke("scene_define_group", R"({"name":"quad","entities":[
{"primitive":"cube"},{"primitive":"cube"},{"primitive":"cube"},{"primitive":"cube"},{"primitive":"cube"}
]})");
    // 5 members x 60 instances = 300 > 256.
    const json result =
        f.invoke("scene_instance_group",
                 R"({"name":"quad","scatter":{"count":60,"area":[10,10],"seed":1}})");
    CHECK(result["status"] == "error");
    CHECK(f.scene.entities.empty());
}

TEST_CASE("scene_instance_group scatter positions stay within the area bounds") {
    Fixture f;
    f.invoke("scene_define_group", R"({"name":"g","entities":[{"primitive":"cube"}]})");
    const json result = f.invoke(
        "scene_instance_group",
        R"({"name":"g","scatter":{"count":40,"area":[4,6],"position":[100,5,-50],"seed":3}})");
    REQUIRE(result["status"] == "ok");
    constexpr float kEps = 1e-3f;
    f.scene.entities.forEach([&](scene::EntityId, const scene::Entity& e) {
        CHECK(e.transform.position.x >= 98.0f - kEps);
        CHECK(e.transform.position.x <= 102.0f + kEps);
        CHECK(e.transform.position.y == doctest::Approx(5.0f));
        CHECK(e.transform.position.z >= -53.0f - kEps);
        CHECK(e.transform.position.z <= -47.0f + kEps);
    });
}

TEST_CASE("scene_instance_group scatter respects scale_jitter bounds") {
    Fixture f;
    f.invoke("scene_define_group", R"({"name":"g","entities":[{"primitive":"cube"}]})");
    const json result =
        f.invoke("scene_instance_group",
                 R"({"name":"g","scatter":{"count":30,"area":[5,5],"seed":9,"scale_jitter":0.3}})");
    REQUIRE(result["status"] == "ok");
    constexpr float kEps = 1e-4f;
    f.scene.entities.forEach([&](scene::EntityId, const scene::Entity& e) {
        CHECK(e.transform.scale.x >= 0.7f - kEps);
        CHECK(e.transform.scale.x <= 1.3f + kEps);
        CHECK(e.transform.scale.y == doctest::Approx(e.transform.scale.x));
        CHECK(e.transform.scale.z == doctest::Approx(e.transform.scale.x));
    });
}

TEST_CASE("scene_instance_group requires exactly one of transforms or scatter, and a known group") {
    Fixture f;
    f.invoke("scene_define_group", R"({"name":"g","entities":[{"primitive":"cube"}]})");
    CHECK(f.invoke("scene_instance_group", R"({"name":"g"})")["status"] == "error");
    CHECK(f.invoke("scene_instance_group",
                   R"({"name":"g","transforms":[{"position":[0,0,0]}],)"
                   R"("scatter":{"count":1,"area":[1,1]}})")["status"] == "error");
    CHECK(f.invoke("scene_instance_group",
                   R"({"name":"missing","transforms":[{"position":[0,0,0]}]})")["status"] ==
          "error");
}

TEST_CASE("scene_list reports defined group names, sorted, only once any exist") {
    Fixture f;
    const json before = json::parse(f.registry.invoke("scene_list", ""), nullptr, false);
    CHECK(!before.contains("groups"));

    f.invoke("scene_define_group", R"({"name":"zeta","entities":[{"primitive":"cube"}]})");
    f.invoke("scene_define_group", R"({"name":"alpha","entities":[{"primitive":"sphere"}]})");

    const json after = json::parse(f.registry.invoke("scene_list", ""), nullptr, false);
    REQUIRE(after.contains("groups"));
    REQUIRE(after["groups"].size() == 2);
    CHECK(after["groups"][0] == "alpha");
    CHECK(after["groups"][1] == "zeta");
}

TEST_CASE("light_remove removes a light and returns the shifted remaining list") {
    Fixture f;
    f.invoke("light_set", R"({"intensity":1})");
    f.invoke("light_set", R"({"intensity":2})");
    f.invoke("light_set", R"({"intensity":3})");
    const json result = f.invoke("light_remove", R"({"index":1})");
    REQUIRE(result["status"] == "ok");
    REQUIRE(result["lights"].size() == 2);
    CHECK(result["lights"][0]["intensity"] == doctest::Approx(1.0));
    CHECK(result["lights"][0]["index"] == 0);
    CHECK(result["lights"][1]["intensity"] == doctest::Approx(3.0));
    CHECK(result["lights"][1]["index"] == 1);
    REQUIRE(f.scene.lights().size() == 2);
    CHECK(f.scene.lights()[1].intensity == doctest::Approx(3.0f));
}

TEST_CASE("light_remove rejects an out-of-range index, naming the valid range") {
    Fixture f;
    f.invoke("light_set", R"({"intensity":1})");
    const json result = f.invoke("light_remove", R"({"index":5})");
    CHECK(result["status"] == "error");
    CHECK(result["message"].get<std::string>().find("0-0") != std::string::npos);
    REQUIRE(f.scene.lights().size() == 1);
}

TEST_CASE("light_remove on an empty light list reports an error") {
    Fixture f;
    CHECK(f.invoke("light_remove", R"({"index":0})")["status"] == "error");
}

TEST_CASE("readMaterial clamps metallic and roughness to the 0-1 range") {
    scene::Scene scene;
    ToolRegistry registry;
    auto groups = std::make_shared<std::unordered_map<std::string, GroupDef>>();
    registerSceneTools(registry, {.scene = &scene, .renderer = nullptr, .groups = groups});

    const json result = invokeOn(
        registry, "scene_define_group",
        R"({"name":"g","entities":[{"primitive":"cube","material":{"metallic":1.5,"roughness":-0.5}}]})");
    REQUIRE(result["status"] == "ok");
    REQUIRE(groups->count("g") == 1);
    const GroupEntitySpec& member = groups->at("g").members[0];
    CHECK(member.material.metallic == doctest::Approx(1.0f));
    CHECK(member.material.roughness == doctest::Approx(0.0f));
}

TEST_CASE("a group member without a material gets the default non-metal grey") {
    scene::Scene scene;
    ToolRegistry registry;
    auto groups = std::make_shared<std::unordered_map<std::string, GroupDef>>();
    registerSceneTools(registry, {.scene = &scene, .renderer = nullptr, .groups = groups});

    const json result = invokeOn(registry, "scene_define_group",
                                 R"({"name":"g","entities":[{"primitive":"cube"}]})");
    REQUIRE(result["status"] == "ok");
    const GroupEntitySpec& member = groups->at("g").members[0];
    CHECK(member.material.metallic == doctest::Approx(0.0f));
    CHECK(member.material.roughness == doctest::Approx(0.6f));
    CHECK(member.material.baseColor[0] == doctest::Approx(0.8f));
    CHECK(member.material.baseColor[3] == doctest::Approx(1.0f));
}

TEST_CASE("environment_set reports unsupported without an applyEnvironment callback") {
    Fixture f; // default context leaves applyEnvironment null
    const json result = f.invoke("environment_set", R"({"preset":"sunset"})");
    CHECK(result["status"] == "error");
    CHECK(result["message"].get<std::string>().find("not available") != std::string::npos);
}

TEST_CASE("environment_set rejects an unknown preset") {
    scene::Scene scene;
    ToolRegistry registry;
    SceneToolContext ctx{.scene = &scene, .renderer = nullptr};
    ctx.applyEnvironment = [](const asset::ProceduralSkyDesc&) { return true; };
    registerSceneTools(registry, ctx);

    const json result = invokeOn(registry, "environment_set", R"({"preset":"foggy"})");
    CHECK(result["status"] == "error");
    CHECK(result["message"].get<std::string>().find("foggy") != std::string::npos);
}

TEST_CASE("environment_set merges explicit overrides onto the chosen preset") {
    scene::Scene scene;
    ToolRegistry registry;
    std::optional<asset::ProceduralSkyDesc> captured;
    SceneToolContext ctx{.scene = &scene, .renderer = nullptr};
    ctx.applyEnvironment = [&captured](const asset::ProceduralSkyDesc& desc) {
        captured = desc;
        return true;
    };
    registerSceneTools(registry, ctx);

    const json result =
        invokeOn(registry, "environment_set", R"({"preset":"sunset","sun_intensity":99})");
    REQUIRE(result["status"] == "ok");
    CHECK(result["preset"] == "sunset");
    REQUIRE(captured.has_value());
    // The override wins...
    CHECK(captured->sunIntensity == doctest::Approx(99.0f));
    // ...while an untouched field keeps the sunset preset's own value, not
    // clear_day's default (60) or some other preset's.
    CHECK(captured->zenithColor.x == doctest::Approx(0.05f));
    CHECK(captured->sunDirection.x == doctest::Approx(-0.8f));
}

TEST_CASE("environment_set rejects a zero sun_direction override") {
    scene::Scene scene;
    ToolRegistry registry;
    SceneToolContext ctx{.scene = &scene, .renderer = nullptr};
    ctx.applyEnvironment = [](const asset::ProceduralSkyDesc&) { return true; };
    registerSceneTools(registry, ctx);

    const json result = invokeOn(registry, "environment_set", R"({"sun_direction":[0,0,0]})");
    CHECK(result["status"] == "error");
}

TEST_CASE("environment_set bounds sun_intensity and exposure") {
    scene::Scene scene;
    ToolRegistry registry;
    bool applied = false;
    SceneToolContext ctx{.scene = &scene, .renderer = nullptr};
    ctx.applyEnvironment = [&applied](const asset::ProceduralSkyDesc&) {
        applied = true;
        return true;
    };
    registerSceneTools(registry, ctx);

    CHECK(invokeOn(registry, "environment_set", R"({"sun_intensity":1e30})")["status"] == "error");
    CHECK(invokeOn(registry, "environment_set", R"({"exposure":1000})")["status"] == "error");
    CHECK(!applied);
    CHECK(invokeOn(registry, "environment_set", R"({"sun_intensity":10000})")["status"] == "ok");
    CHECK(applied);
}

TEST_CASE("scene_validate flags a floating entity but not one resting on the ground") {
    Fixture f;
    f.invoke("scene_add_entity", R"({"primitive":"cube","position":[0,5,0]})");
    f.invoke("scene_add_entity", R"({"primitive":"cube","position":[10,0.5,0]})");

    const json result = f.invoke("scene_validate", "");
    REQUIRE(result["status"] == "ok");
    bool floatingFlaggedForA = false;
    bool floatingFlaggedForB = false;
    for (const auto& finding : result["findings"]) {
        if (finding["check"] != "floating") {
            continue;
        }
        if (finding["entity_id"] == "0:0") {
            floatingFlaggedForA = true;
        }
        if (finding["entity_id"] == "1:0") {
            floatingFlaggedForB = true;
        }
    }
    CHECK(floatingFlaggedForA);
    CHECK(!floatingFlaggedForB);
}

TEST_CASE("scene_validate flags an overlapping pair of entities") {
    Fixture f;
    f.invoke("scene_add_entity", R"({"primitive":"cube","position":[0,0.5,0]})");
    f.invoke("scene_add_entity", R"({"primitive":"cube","position":[0.2,0.5,0]})");

    const json result = f.invoke("scene_validate", "");
    REQUIRE(result["status"] == "ok");
    CHECK(hasFinding(result["findings"], "overlap"));
}

TEST_CASE("scene_validate flags the camera being inside an entity's bounding box") {
    Fixture f;
    // Default camera sits at (0, 0, 3); a size-4 cube there fully contains it.
    f.invoke("scene_add_entity", R"({"primitive":"cube","size":4,"position":[0,0,3]})");

    const json result = f.invoke("scene_validate", "");
    REQUIRE(result["status"] == "ok");
    CHECK(hasFinding(result["findings"], "camera_inside"));
}

TEST_CASE("scene_validate flags a zero-light scene") {
    Fixture f;
    const json result = f.invoke("scene_validate", "");
    REQUIRE(result["status"] == "ok");
    CHECK(hasFinding(result["findings"], "lighting"));
}

TEST_CASE("scene_validate flags an entity behind the camera as out of frustum, using a fixed "
          "viewport aspect") {
    scene::Scene scene;
    ToolRegistry registry;
    SceneToolContext ctx{.scene = &scene, .renderer = nullptr};
    ctx.viewportSize = [] { return std::make_pair(std::uint32_t(1920), std::uint32_t(1080)); };
    registerSceneTools(registry, ctx);

    invokeOn(registry, "scene_add_entity", R"({"primitive":"cube","position":[0,0,10]})");
    const json result = invokeOn(registry, "scene_validate");
    REQUIRE(result["status"] == "ok");

    bool flagged = false;
    for (const auto& finding : result["findings"]) {
        if (finding["check"] == "out_of_frustum") {
            flagged = true;
            // A real viewport was supplied, so the message must not claim an
            // assumed aspect ratio.
            CHECK(finding["message"].get<std::string>().find("assumed") == std::string::npos);
        }
    }
    CHECK(flagged);
}

// --- asset_list -------------------------------------------------------

TEST_CASE("asset_list reports unset assetDir as unsupported") {
    Fixture f; // default context leaves assetDir empty
    const json result = f.invoke("asset_list", "");
    CHECK(result["status"] == "error");
    CHECK(result["message"].get<std::string>().find("not configured") != std::string::npos);
}

TEST_CASE("asset_list scans textures, models (including categorized ones) and env, sorted "
          "deterministically") {
    TempDir dir("kumo_scene_tools_asset_list");
    touch(dir.path / "textures" / "sand" / "albedo.png");
    touch(dir.path / "textures" / "sand" / "normal.png");
    touch(dir.path / "textures" / "empty" / "readme.txt"); // no recognized maps, excluded
    touch(dir.path / "models" / "Tree.glb");
    // A categorized model (models/<category>/<name>.glb): the pre-F1 fallback
    // (a flat directory_iterator over "models" for *.glb) never saw these;
    // asset::listModelIds does.
    touch(dir.path / "models" / "nature" / "bridge_stone.glb");
    touch(dir.path / "env" / "day.hdr");

    scene::Scene scene;
    ToolRegistry registry;
    SceneToolContext ctx{.scene = &scene, .renderer = nullptr};
    ctx.assetDir = dir.path;
    registerSceneTools(registry, ctx);

    const json result = invokeOn(registry, "asset_list");
    REQUIRE(result["status"] == "ok");
    REQUIRE(result["textures"].size() == 1);
    CHECK(result["textures"][0]["name"] == "sand");
    REQUIRE(result["textures"][0]["maps"].size() == 2);
    CHECK(result["textures"][0]["maps"][0] == "albedo");
    CHECK(result["textures"][0]["maps"][1] == "normal");
    REQUIRE(result["models"].size() == 2);
    // "Tree" < "nature/bridge_stone": uppercase sorts before lowercase in the
    // ordinary byte-wise ordering asset::listModelIds/std::sort both use.
    CHECK(result["models"][0]["name"] == "Tree");
    CHECK(result["models"][1]["name"] == "nature/bridge_stone");
    REQUIRE(result["env"].size() == 1);
    CHECK(result["env"][0]["name"] == "day.hdr");
    CHECK(!result.contains("note"));
}

TEST_CASE("asset_list on a configured but empty library reports empty arrays with a note") {
    TempDir dir("kumo_scene_tools_asset_list_empty");
    scene::Scene scene;
    ToolRegistry registry;
    SceneToolContext ctx{.scene = &scene, .renderer = nullptr};
    ctx.assetDir = dir.path;
    registerSceneTools(registry, ctx);

    const json result = invokeOn(registry, "asset_list");
    REQUIRE(result["status"] == "ok");
    CHECK(result["textures"].empty());
    CHECK(result["models"].empty());
    CHECK(result["env"].empty());
    CHECK(result.contains("note"));
}

TEST_CASE("asset_list v2 merges index.json metadata onto the disk scan (F1): a stale index entry "
          "for an asset no longer on disk is dropped, and a disk asset absent from the index "
          "still appears with no extra metadata") {
    TempDir dir("kumo_scene_tools_asset_list_index");
    touch(dir.path / "textures" / "sand" / "albedo.png");
    touch(dir.path / "textures" / "sand" / "normal.png");
    touch(dir.path / "textures" / "moss" / "albedo.png"); // on disk, absent from the index
    touch(dir.path / "models" / "nature" / "bridge_stone.glb");
    touch(dir.path / "models" / "props" / "crate.glb"); // on disk, absent from the index
    touch(dir.path / "env" / "day.hdr");

    const char* kIndexJson = R"({"version":1,"generated":"t","entries":[
{"id":"sand","kind":"texture","name":"Sand","style":"realistic","resolution":2048},
{"id":"nature/bridge_stone","kind":"model","category":"nature","style":"stylized",
 "dimensions":[1.2,0.4,2.5],"triangles":240,"instancing_ok":true},
{"id":"day","kind":"env","style":"realistic"},
{"id":"ghost_model","kind":"model","category":"nature"}
]})";
    {
        std::ofstream out(dir.path / "index.json");
        out << kIndexJson;
    }

    scene::Scene scene;
    ToolRegistry registry;
    SceneToolContext ctx{.scene = &scene, .renderer = nullptr};
    ctx.assetDir = dir.path;
    registerSceneTools(registry, ctx);

    const json result = invokeOn(registry, "asset_list");
    REQUIRE(result["status"] == "ok");
    CHECK(!result.contains("note"));

    // textures: "moss" (index-absent) still appears; "sand" is decorated,
    // and F3's identity rule holds -- "name" stays the disk id, the index's
    // human-readable name surfaces separately as "display_name".
    REQUIRE(result["textures"].size() == 2);
    CHECK(result["textures"][0]["name"] == "moss");
    CHECK(!result["textures"][0].contains("display_name"));
    CHECK(!result["textures"][0].contains("style"));
    CHECK(result["textures"][1]["name"] == "sand");
    CHECK(result["textures"][1]["display_name"] == "Sand");
    CHECK(result["textures"][1]["style"] == "realistic");
    CHECK(result["textures"][1]["resolution"] == 2048);
    REQUIRE(result["textures"][1]["maps"].size() == 2); // disk-scanned, not index-derived

    // models: "ghost_model" (index-only, no matching disk asset) never
    // appears -- disk is the source of truth for existence.
    REQUIRE(result["models"].size() == 2);
    CHECK(result["models"][0]["name"] == "nature/bridge_stone");
    CHECK(result["models"][0]["category"] == "nature");
    CHECK(result["models"][0]["style"] == "stylized");
    CHECK(result["models"][0]["triangles"] == 240);
    CHECK(result["models"][0]["instancing_ok"] == true);
    REQUIRE(result["models"][0]["dimensions"].size() == 3);
    CHECK(result["models"][0]["dimensions"][0] == doctest::Approx(1.2));
    CHECK(result["models"][1]["name"] == "props/crate");
    CHECK(!result["models"][1].contains("category"));

    REQUIRE(result["env"].size() == 1);
    CHECK(result["env"][0]["name"] == "day.hdr");
    CHECK(result["env"][0]["style"] == "realistic");

    // No caption/embedding leakage into asset_list's output (that's search's
    // job later, per the milestone spec).
    for (const auto& kind : {"textures", "models", "env"}) {
        for (const auto& entry : result[kind]) {
            CHECK(!entry.contains("caption"));
            CHECK(!entry.contains("embedding_offset"));
            CHECK(!entry.contains("thumbnail"));
        }
    }
}

TEST_CASE("asset_list reports an empty-with-note summary when both disk and index.json are "
          "empty") {
    TempDir dir("kumo_scene_tools_asset_list_index_empty");
    {
        std::ofstream out(dir.path / "index.json");
        out << R"({"version":1,"generated":"t","entries":[]})";
    }

    scene::Scene scene;
    ToolRegistry registry;
    SceneToolContext ctx{.scene = &scene, .renderer = nullptr};
    ctx.assetDir = dir.path;
    registerSceneTools(registry, ctx);

    const json result = invokeOn(registry, "asset_list");
    REQUIRE(result["status"] == "ok");
    CHECK(result["textures"].empty());
    CHECK(result["models"].empty());
    CHECK(result["env"].empty());
    CHECK(result.contains("note"));
}

// --- material_set_texture ----------------------------------------------

TEST_CASE("material_set_texture requires the entity to already have a material") {
    Fixture f;
    // No renderer, so scene_add_entity leaves materialIndex at -1.
    f.invoke("scene_add_entity", R"({"primitive":"cube"})");
    const json result = f.invoke("material_set_texture", R"({"entity_id":"0:0","texture":"sand"})");
    CHECK(result["status"] == "error");
    CHECK(result["message"].get<std::string>().find("material") != std::string::npos);
}

TEST_CASE("material_set_texture reports an unknown set and lists available names") {
    TempDir dir("kumo_scene_tools_material_texture_unknown");
    touch(dir.path / "textures" / "sand" / "albedo.png");

    scene::Scene scene;
    ToolRegistry registry;
    SceneToolContext ctx{.scene = &scene, .renderer = nullptr};
    ctx.assetDir = dir.path;
    registerSceneTools(registry, ctx);

    invokeOn(registry, "scene_add_entity", R"({"primitive":"cube"})");
    scene::Entity* entity = scene.entities.get({0, 0});
    REQUIRE(entity != nullptr);
    entity->materialIndex = 0; // only "has a material" matters for this check

    const json result =
        invokeOn(registry, "material_set_texture", R"({"entity_id":"0:0","texture":"bogus"})");
    CHECK(result["status"] == "error");
    const std::string message = result["message"].get<std::string>();
    CHECK(message.find("unknown texture set") != std::string::npos);
    CHECK(message.find("sand") != std::string::npos);
}

TEST_CASE("material_set_texture without a renderer reports a structured error") {
    TempDir dir("kumo_scene_tools_material_texture_no_renderer");
    std::filesystem::create_directories(dir.path / "textures" / "sand");
    const std::vector<std::uint8_t> albedo = flatPixels(2, 2, 200);
    REQUIRE(asset::writePng(dir.path / "textures" / "sand" / "albedo.png", 2, 2, albedo.data()));

    scene::Scene scene;
    ToolRegistry registry;
    SceneToolContext ctx{.scene = &scene, .renderer = nullptr};
    ctx.assetDir = dir.path;
    registerSceneTools(registry, ctx);

    invokeOn(registry, "scene_add_entity", R"({"primitive":"cube"})");
    scene::Entity* entity = scene.entities.get({0, 0});
    REQUIRE(entity != nullptr);
    entity->materialIndex = 0;

    const json result =
        invokeOn(registry, "material_set_texture", R"({"entity_id":"0:0","texture":"sand"})");
    CHECK(result["status"] == "error");
    CHECK(result["message"].get<std::string>().find("renderer") != std::string::npos);
}

TEST_CASE("material_set_texture rejects traversal/absolute/hidden texture names") {
    Fixture f;
    f.invoke("scene_add_entity", R"({"primitive":"cube"})");
    scene::Entity* entity = f.scene.entities.get({0, 0});
    REQUIRE(entity != nullptr);
    entity->materialIndex = 0;

    for (const char* bad : {"../x", "a/b", "/tmp/x", ".hidden"}) {
        const json result = f.invoke("material_set_texture",
                                     std::format(R"({{"entity_id":"0:0","texture":"{}"}})", bad));
        CHECK(result["status"] == "error");
        CHECK(result["message"] == "asset names must be plain names from asset_list, not paths");
    }
}

TEST_CASE("material_set_texture rejects an invalid tiling value") {
    Fixture f;
    f.invoke("scene_add_entity", R"({"primitive":"cube"})");
    scene::Entity* entity = f.scene.entities.get({0, 0});
    REQUIRE(entity != nullptr);
    entity->materialIndex = 0;

    const json result =
        f.invoke("material_set_texture", R"({"entity_id":"0:0","texture":"sand","tiling":-1})");
    CHECK(result["status"] == "error");
    CHECK(result["message"].get<std::string>().find("tiling") != std::string::npos);
}

// --- asset_fetch -----------------------------------------------------------

TEST_CASE("asset_fetch reports unsupported without a fetchAsset callback") {
    Fixture f; // default context leaves fetchAsset null
    const json result = f.invoke("asset_fetch", R"({"kind":"texture","query":"asphalt"})");
    CHECK(result["status"] == "error");
    CHECK(result["message"].get<std::string>().find("not available") != std::string::npos);
}

TEST_CASE("asset_fetch validates kind and query before ever calling the callback") {
    scene::Scene scene;
    ToolRegistry registry;
    bool called = false;
    SceneToolContext ctx{.scene = &scene, .renderer = nullptr};
    ctx.fetchAsset = [&called](std::string_view,
                               std::string_view) -> std::expected<FetchedAsset, std::string> {
        called = true;
        return FetchedAsset{};
    };
    registerSceneTools(registry, ctx);

    json result = invokeOn(registry, "asset_fetch", R"({"kind":"bogus","query":"asphalt"})");
    CHECK(result["status"] == "error");
    CHECK(result["message"].get<std::string>().find("kind") != std::string::npos);

    result = invokeOn(registry, "asset_fetch", R"({"kind":"texture"})");
    CHECK(result["status"] == "error");
    CHECK(result["message"].get<std::string>().find("query") != std::string::npos);

    result = invokeOn(registry, "asset_fetch", R"({"kind":"texture","query":""})");
    CHECK(result["status"] == "error");

    result = invokeOn(registry, "asset_fetch",
                      std::format(R"({{"kind":"texture","query":"{}"}})", std::string(65, 'a')));
    CHECK(result["status"] == "error");
    CHECK(result["message"].get<std::string>().find("64") != std::string::npos);

    CHECK(!called);
}

TEST_CASE("asset_fetch passes kind/query through and reports the callback's success shape") {
    scene::Scene scene;
    ToolRegistry registry;
    std::string capturedKind;
    std::string capturedQuery;
    SceneToolContext ctx{.scene = &scene, .renderer = nullptr};
    ctx.fetchAsset = [&](std::string_view kind,
                         std::string_view query) -> std::expected<FetchedAsset, std::string> {
        capturedKind = kind;
        capturedQuery = query;
        return FetchedAsset{.name = "asphalt_02",
                            .maps = {"albedo", "normal"},
                            .alreadyPresent = false,
                            .alternatives = {"asphalt_01", "aerial_asphalt_01"}};
    };
    registerSceneTools(registry, ctx);

    const json result =
        invokeOn(registry, "asset_fetch", R"({"kind":"texture","query":"asphalt"})");
    REQUIRE(result["status"] == "ok");
    CHECK(result["kind"] == "texture");
    CHECK(result["name"] == "asphalt_02");
    CHECK(result["already_present"] == false);
    REQUIRE(result["maps"].size() == 2);
    CHECK(result["maps"][0] == "albedo");
    REQUIRE(result["alternatives"].size() == 2);
    CHECK(result["alternatives"][0] == "asphalt_01");
    CHECK(capturedKind == "texture");
    CHECK(capturedQuery == "asphalt");
}

TEST_CASE("asset_fetch reports model kind's files count instead of a maps array") {
    scene::Scene scene;
    ToolRegistry registry;
    std::string capturedKind;
    SceneToolContext ctx{.scene = &scene, .renderer = nullptr};
    ctx.fetchAsset = [&](std::string_view kind,
                         std::string_view) -> std::expected<FetchedAsset, std::string> {
        capturedKind = kind;
        return FetchedAsset{.name = "Barrel_01",
                            .maps = {"scene.gltf", "Barrel_01.bin", "textures/diff.jpg"},
                            .alreadyPresent = false,
                            .alternatives = {}};
    };
    registerSceneTools(registry, ctx);

    const json result = invokeOn(registry, "asset_fetch", R"({"kind":"model","query":"barrel"})");
    REQUIRE(result["status"] == "ok");
    CHECK(capturedKind == "model");
    CHECK(result["kind"] == "model");
    CHECK(result["name"] == "Barrel_01");
    CHECK(result["files"] == 3);
    CHECK(!result.contains("maps"));
    CHECK(result["already_present"] == false);
}

TEST_CASE("asset_fetch passes the callback's error through unchanged, and omits alternatives "
          "when empty") {
    scene::Scene scene;
    ToolRegistry registry;
    SceneToolContext ctx{.scene = &scene, .renderer = nullptr};
    ctx.fetchAsset = [](std::string_view,
                        std::string_view) -> std::expected<FetchedAsset, std::string> {
        return std::unexpected(std::string("no asset matches 'zzz'"));
    };
    registerSceneTools(registry, ctx);

    const json result = invokeOn(registry, "asset_fetch", R"({"kind":"env","query":"zzz"})");
    CHECK(result["status"] == "error");
    CHECK(result["message"] == "no asset matches 'zzz'");
}

TEST_CASE("asset_fetch omits the alternatives key when the callback returns none") {
    scene::Scene scene;
    ToolRegistry registry;
    SceneToolContext ctx{.scene = &scene, .renderer = nullptr};
    ctx.fetchAsset = [](std::string_view,
                        std::string_view) -> std::expected<FetchedAsset, std::string> {
        return FetchedAsset{.name = "sand", .maps = {"albedo"}, .alreadyPresent = true};
    };
    registerSceneTools(registry, ctx);

    const json result = invokeOn(registry, "asset_fetch", R"({"kind":"texture","query":"sand"})");
    REQUIRE(result["status"] == "ok");
    CHECK(result["already_present"] == true);
    CHECK(!result.contains("alternatives"));
}

// --- scene_add_model -----------------------------------------------------

TEST_CASE("scene_add_model reports unsupported without an instantiateModel callback") {
    Fixture f; // default context leaves instantiateModel null
    const json result = f.invoke("scene_add_model", R"({"model":"Avocado","position":[0,0,0]})");
    CHECK(result["status"] == "error");
    CHECK(result["message"].get<std::string>().find("not available") != std::string::npos);
}

TEST_CASE("scene_add_model rejects a non-uniform scale") {
    scene::Scene scene;
    ToolRegistry registry;
    SceneToolContext ctx{.scene = &scene, .renderer = nullptr};
    ctx.instantiateModel =
        [](const scene::Transform&, std::string_view,
           const ModelPlacementRequest&) -> std::expected<ModelPlacementResult, std::string> {
        return ModelPlacementResult{.entityIds = {"0:0"}};
    };
    registerSceneTools(registry, ctx);

    const json result = invokeOn(registry, "scene_add_model",
                                 R"({"model":"Avocado","position":[0,0,0],"scale":[1,1,0.5]})");
    CHECK(result["status"] == "error");
    CHECK(result["message"].get<std::string>().find("uniform") != std::string::npos);
}

TEST_CASE("scene_add_model rejects traversal/absolute/hidden model names without invoking the "
          "callback") {
    scene::Scene scene;
    ToolRegistry registry;
    bool called = false;
    SceneToolContext ctx{.scene = &scene, .renderer = nullptr};
    ctx.instantiateModel =
        [&called](
            const scene::Transform&, std::string_view,
            const ModelPlacementRequest&) -> std::expected<ModelPlacementResult, std::string> {
        called = true;
        return ModelPlacementResult{.entityIds = {"0:0"}};
    };
    registerSceneTools(registry, ctx);

    // "a/b" is deliberately absent here: scene_add_model allows one category
    // component (unlike environment_set's file names below), see the
    // acceptance test right after this one. Backslash rejection is already
    // covered directly on isPlainAssetPath in test_core_file.cpp -- round-
    // tripping a literal backslash through this JSON-argument interface would
    // require careful escaping (a bare "\\" is the JSON backspace escape, not
    // a literal backslash), which is not what this test is about.
    for (const char* bad : {"../x", "/tmp/x", ".hidden", "a/b/c", "a/../b", "props/../crate",
                            "props/.crate", ".props/crate"}) {
        const json result = invokeOn(registry, "scene_add_model",
                                     std::format(R"({{"model":"{}","position":[0,0,0]}})", bad));
        CHECK(result["status"] == "error");
        CHECK(result["message"].get<std::string>().find("category") != std::string::npos);
    }
    CHECK(!called);
}

TEST_CASE("scene_add_model accepts a one-level category prefix and passes it through unchanged") {
    scene::Scene scene;
    ToolRegistry registry;
    std::optional<std::string> capturedModel;
    SceneToolContext ctx{.scene = &scene, .renderer = nullptr};
    ctx.instantiateModel =
        [&](const scene::Transform&, std::string_view model,
            const ModelPlacementRequest&) -> std::expected<ModelPlacementResult, std::string> {
        capturedModel = std::string(model);
        return ModelPlacementResult{.entityIds = {"0:0"}};
    };
    registerSceneTools(registry, ctx);

    const json result = invokeOn(registry, "scene_add_model",
                                 R"({"model":"nature/bridge_stone","position":[0,0,0]})");
    REQUIRE(result["status"] == "ok");
    REQUIRE(capturedModel.has_value());
    CHECK(*capturedModel == "nature/bridge_stone");
}

TEST_CASE("scene_add_model returns entity_ids from the callback and renames on request") {
    scene::Scene scene;
    scene::Entity node;
    node.name = "Avocado_mesh0";
    const scene::EntityId nodeId = scene.entities.insert(node);

    ToolRegistry registry;
    std::optional<scene::Transform> capturedRoot;
    std::optional<std::string> capturedModel;
    SceneToolContext ctx{.scene = &scene, .renderer = nullptr};
    ctx.instantiateModel =
        [&](const scene::Transform& root, std::string_view model,
            const ModelPlacementRequest&) -> std::expected<ModelPlacementResult, std::string> {
        capturedRoot = root;
        capturedModel = std::string(model);
        return ModelPlacementResult{.entityIds = {formatEntityId(nodeId)}};
    };
    registerSceneTools(registry, ctx);

    const json result =
        invokeOn(registry, "scene_add_model",
                 R"({"model":"Avocado","name":"fruit","position":[1,2,3],"scale":2})");
    REQUIRE(result["status"] == "ok");
    CHECK(result["model"] == "Avocado");
    REQUIRE(result["entities"].size() == 1);
    CHECK(result["entities"][0] == formatEntityId(nodeId));

    REQUIRE(capturedRoot.has_value());
    CHECK(capturedRoot->position.x == doctest::Approx(1.0f));
    CHECK(capturedRoot->scale.x == doctest::Approx(2.0f));
    REQUIRE(capturedModel.has_value());
    CHECK(*capturedModel == "Avocado");

    // The entity the fake callback "created" is renamed from the model's own
    // node name to the caller's chosen prefix.
    const scene::Entity* renamed = scene.entities.get(nodeId);
    REQUIRE(renamed != nullptr);
    CHECK(renamed->name == "fruit_mesh0");
}

// --- placement constraints (MP) ------------------------------------------

TEST_CASE("scene_add_entity snap_to_ground rests a centered-pivot primitive on the ground") {
    Fixture f;
    // A unit cube requested at y=5 must land with its bottom at the default
    // 0.01 clearance, i.e. center y = 0.51; x/z are the intended location.
    const json result = f.invoke(
        "scene_add_entity", R"({"primitive":"cube","position":[2,5,3],"snap_to_ground":true})");
    REQUIRE(result["status"] == "ok");
    REQUIRE(result.contains("position"));
    CHECK(result["position"][0].get<float>() == doctest::Approx(2.0f));
    CHECK(result["position"][1].get<float>() == doctest::Approx(0.51f));
    CHECK(result["position"][2].get<float>() == doctest::Approx(3.0f));
    REQUIRE(result.contains("aabb_world"));
    CHECK(result["aabb_world"]["min"][1].get<float>() == doctest::Approx(0.01f));
    CHECK(result["aabb_world"]["max"][1].get<float>() == doctest::Approx(1.01f));

    const scene::Entity* entity = f.scene.entities.get({0, 0});
    REQUIRE(entity != nullptr);
    CHECK(entity->transform.position.y == doctest::Approx(0.51f));
}

TEST_CASE("scene_add_entity without placement options keeps its previous behavior") {
    Fixture f;
    f.invoke("scene_add_entity", R"({"primitive":"cube","position":[0,0.5,0]})");
    // Overlapping placement still succeeds without avoid_overlap, no snapping
    // happens without snap_to_ground, and the new aabb_world field is additive.
    const json result =
        f.invoke("scene_add_entity", R"({"primitive":"cube","position":[0.2,5,0]})");
    REQUIRE(result["status"] == "ok");
    CHECK(!result.contains("position"));
    CHECK(result.contains("aabb_world"));
    const scene::Entity* entity = f.scene.entities.get({1, 0});
    REQUIRE(entity != nullptr);
    CHECK(entity->transform.position.y == doctest::Approx(5.0f));
    CHECK(f.scene.entities.size() == 2);
}

TEST_CASE("scene_add_entity validates the placement option types") {
    Fixture f;
    CHECK(f.invoke("scene_add_entity",
                   R"({"primitive":"cube","snap_to_ground":"yes"})")["status"] == "error");
    CHECK(f.invoke("scene_add_entity", R"({"primitive":"cube","clearance":-0.5})")["status"] ==
          "error");
    CHECK(f.invoke("scene_add_entity", R"({"primitive":"cube","avoid_overlap":1})")["status"] ==
          "error");
    CHECK(f.scene.entities.empty());
}

TEST_CASE("scene_add_entity avoid_overlap rejects before mutation and names the conflict") {
    Fixture f;
    f.invoke("scene_add_entity", R"({"primitive":"cube","position":[0,0.5,0]})");

    const json rejected = f.invoke(
        "scene_add_entity", R"({"primitive":"cube","position":[0.2,0.5,0],"avoid_overlap":true})");
    CHECK(rejected["status"] == "error");
    REQUIRE(rejected.contains("conflicting_entity_ids"));
    REQUIRE(rejected["conflicting_entity_ids"].size() == 1);
    CHECK(rejected["conflicting_entity_ids"][0] == "0:0");
    REQUIRE(rejected.contains("overlap_depth"));
    CHECK(rejected["overlap_depth"][0].get<float>() == doctest::Approx(0.8f));
    CHECK(rejected["overlap_depth"][1].get<float>() == doctest::Approx(1.0f));
    CHECK(rejected["requested_position"][0].get<float>() == doctest::Approx(0.2f));
    // The rejection left the scene untouched.
    CHECK(f.scene.entities.size() == 1);
}

TEST_CASE("scene_add_entity avoid_overlap suggests a deterministic free position that works") {
    Fixture f;
    f.invoke("scene_add_entity", R"({"primitive":"cube","position":[0,0.5,0]})");

    const char* call = R"({"primitive":"cube","position":[0.2,0.5,0],"avoid_overlap":true})";
    const json first = f.invoke("scene_add_entity", call);
    const json second = f.invoke("scene_add_entity", call);
    REQUIRE(first["status"] == "error");
    REQUIRE(first.contains("suggested_position"));
    REQUIRE(second.contains("suggested_position"));
    for (int axis = 0; axis < 3; ++axis) {
        CHECK(first["suggested_position"][axis].get<float>() ==
              second["suggested_position"][axis].get<float>());
    }
    // Y is carried over from the request, and the suggestion actually fits.
    CHECK(first["suggested_position"][1].get<float>() == doctest::Approx(0.5f));
    const json placed =
        f.invoke("scene_add_entity",
                 std::format(R"({{"primitive":"cube","position":[{},{},{}],"avoid_overlap":true}})",
                             first["suggested_position"][0].get<float>(),
                             first["suggested_position"][1].get<float>(),
                             first["suggested_position"][2].get<float>()));
    CHECK(placed["status"] == "ok");
}

TEST_CASE("scene_add_model forwards placement options and returns the callback's bounds") {
    scene::Scene scene;
    ToolRegistry registry;
    std::optional<ModelPlacementRequest> captured;
    SceneToolContext ctx{.scene = &scene, .renderer = nullptr};
    ctx.instantiateModel = [&](const scene::Transform&, std::string_view,
                               const ModelPlacementRequest& request)
        -> std::expected<ModelPlacementResult, std::string> {
        captured = request;
        // A snapped multi-node model: the facade returns the union AABB and the
        // corrected root position.
        return ModelPlacementResult{.entityIds = {"0:0", "1:0"},
                                    .aabb = {{-1.0f, 0.02f, -1.0f}, {1.0f, 2.02f, 1.0f}},
                                    .finalPosition = {4.0f, 0.32f, -2.0f}};
    };
    registerSceneTools(registry, ctx);

    const json result =
        invokeOn(registry, "scene_add_model",
                 R"({"model":"barn","position":[4,0,-2],"snap_to_ground":true,"clearance":0.02})");
    REQUIRE(result["status"] == "ok");
    CHECK(result["entities"].size() == 2);
    REQUIRE(result.contains("aabb_world"));
    CHECK(result["aabb_world"]["min"][1].get<float>() == doctest::Approx(0.02f));
    CHECK(result["aabb_world"]["max"][1].get<float>() == doctest::Approx(2.02f));
    REQUIRE(result.contains("position"));
    CHECK(result["position"][1].get<float>() == doctest::Approx(0.32f));

    REQUIRE(captured.has_value());
    CHECK(captured->snapToGround);
    CHECK(captured->clearance == doctest::Approx(0.02f));
    CHECK(!captured->avoidOverlap);
}

TEST_CASE("scene_add_model surfaces a placement conflict as the structured rejection") {
    scene::Scene scene;
    ToolRegistry registry;
    SceneToolContext ctx{.scene = &scene, .renderer = nullptr};
    ctx.instantiateModel =
        [](const scene::Transform&, std::string_view,
           const ModelPlacementRequest&) -> std::expected<ModelPlacementResult, std::string> {
        ModelPlacementResult rejected;
        rejected.conflict = ModelPlacementConflict{.conflictingIds = {"3:0"},
                                                   .depth = {0.4f, 1.0f, 0.6f},
                                                   .suggested = math::float3{6.0f, 0.5f, 0.0f}};
        return rejected;
    };
    registerSceneTools(registry, ctx);

    const json result = invokeOn(registry, "scene_add_model",
                                 R"({"model":"barn","position":[5,0.5,0],"avoid_overlap":true})");
    CHECK(result["status"] == "error");
    REQUIRE(result.contains("conflicting_entity_ids"));
    CHECK(result["conflicting_entity_ids"][0] == "3:0");
    CHECK(result["overlap_depth"][0].get<float>() == doctest::Approx(0.4f));
    CHECK(result["requested_position"][0].get<float>() == doctest::Approx(5.0f));
    CHECK(result["suggested_position"][0].get<float>() == doctest::Approx(6.0f));
    CHECK(scene.entities.empty());
}

TEST_CASE("scene_instance_group scatter respects min_spacing between instances") {
    Fixture f;
    f.invoke("scene_define_group", R"({"name":"g","entities":[{"primitive":"cube"}]})");
    const json result =
        f.invoke("scene_instance_group",
                 R"({"name":"g","scatter":{"count":8,"area":[30,30],"seed":5,"min_spacing":1.0}})");
    REQUIRE(result["status"] == "ok");

    std::vector<math::float3> positions;
    f.scene.entities.forEach([&](scene::EntityId, const scene::Entity& e) {
        positions.push_back(e.transform.position);
    });
    REQUIRE(positions.size() == 8);
    for (std::size_t i = 0; i < positions.size(); ++i) {
        for (std::size_t j = i + 1; j < positions.size(); ++j) {
            // Unit cubes: XZ edge-to-edge separation must reach min_spacing on
            // at least one axis.
            const float gapX = std::abs(positions[i].x - positions[j].x) - 1.0f;
            const float gapZ = std::abs(positions[i].z - positions[j].z) - 1.0f;
            CHECK(std::max(gapX, gapZ) >= 1.0f - 1e-4f);
        }
    }
}

TEST_CASE("scene_instance_group scatter avoid_existing keeps instances off scene entities") {
    Fixture f;
    // A 6x1x6 obstacle centered at the origin.
    f.invoke("scene_add_entity",
             R"({"primitive":"cube","size":6,"position":[0,0.5,0],"scale":[1,0.1667,1]})");
    f.invoke("scene_define_group", R"({"name":"g","entities":[{"primitive":"cube"}]})");
    const json result = f.invoke(
        "scene_instance_group",
        R"({"name":"g","scatter":{"count":10,"area":[20,20],"position":[0,0.5,0],"seed":11,"avoid_existing":true}})");
    REQUIRE(result["status"] == "ok");

    const scene::Entity* obstacle = f.scene.entities.get({0, 0});
    REQUIRE(obstacle != nullptr);
    f.scene.entities.forEach([&](scene::EntityId id, const scene::Entity& e) {
        if (id.index == 0) {
            return;
        }
        // Scattered unit cubes may touch the obstacle within the 2 cm support
        // margin but never meaningfully interpenetrate its XZ footprint.
        const float gapX = std::abs(e.transform.position.x) - 0.5f - 3.0f;
        const float gapZ = std::abs(e.transform.position.z) - 0.5f - 3.0f;
        CHECK(std::max(gapX, gapZ) >= -0.02f - 1e-4f);
    });
}

TEST_CASE("scene_instance_group scatter with placement options is deterministic per seed") {
    auto samplePositions = [] {
        Fixture fx;
        fx.invoke("scene_add_entity", R"({"primitive":"cube","position":[0,0.5,0]})");
        fx.invoke("scene_define_group", R"({"name":"g","entities":[{"primitive":"cube"}]})");
        const json result = fx.invoke(
            "scene_instance_group",
            R"({"name":"g","scatter":{"count":6,"area":[15,15],"position":[0,0.5,0],"seed":42,"min_spacing":0.5,"avoid_existing":true}})");
        REQUIRE(result["status"] == "ok");
        std::vector<math::float3> positions;
        fx.scene.entities.forEach([&](scene::EntityId, const scene::Entity& e) {
            positions.push_back(e.transform.position);
        });
        return positions;
    };
    const std::vector<math::float3> a = samplePositions();
    const std::vector<math::float3> b = samplePositions();
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].x == b[i].x);
        CHECK(a[i].z == b[i].z);
    }
}

TEST_CASE("scene_instance_group scatter fails atomically with a useful error when impossible") {
    Fixture f;
    f.invoke("scene_define_group", R"({"name":"g","entities":[{"primitive":"cube"}]})");
    const json result =
        f.invoke("scene_instance_group",
                 R"({"name":"g","scatter":{"count":10,"area":[1,1],"seed":1,"min_spacing":5}})");
    CHECK(result["status"] == "error");
    CHECK(result["requested"] == 10);
    CHECK(result["accepted"].get<int>() < 10);
    CHECK(result["area"][0].get<float>() == doctest::Approx(1.0f));
    CHECK(result["min_spacing"].get<float>() == doctest::Approx(5.0f));
    CHECK(result["message"].get<std::string>().find("attempts") != std::string::npos);
    CHECK(f.scene.entities.empty());
}

TEST_CASE("scene_validate keeps shallow support contact below warning severity") {
    Fixture f;
    // A crate resting on a cube, sunk 1 cm: intended support contact.
    f.invoke("scene_add_entity", R"({"primitive":"cube","position":[0,0.5,0]})");
    f.invoke("scene_add_entity", R"({"primitive":"cube","position":[0,1.49,0]})");

    const json result = f.invoke("scene_validate", "");
    REQUIRE(result["status"] == "ok");
    for (const auto& finding : result["findings"]) {
        if (finding["check"] == "overlap") {
            CHECK(finding["severity"] != "warning");
        }
    }
}

TEST_CASE("scene_validate reports deep overlap once with both ids, depth and ratio") {
    Fixture f;
    f.invoke("scene_add_entity", R"({"primitive":"cube","position":[0,0.5,0]})");
    f.invoke("scene_add_entity", R"({"primitive":"cube","position":[0.2,0.5,0]})");

    const json result = f.invoke("scene_validate", "");
    REQUIRE(result["status"] == "ok");
    int overlapFindings = 0;
    for (const auto& finding : result["findings"]) {
        if (finding["check"] != "overlap") {
            continue;
        }
        ++overlapFindings;
        CHECK(finding["severity"] == "warning");
        CHECK(finding["entity_id"] == "0:0");
        CHECK(finding["other_entity_id"] == "1:0");
        REQUIRE(finding.contains("overlap_depth"));
        CHECK(finding["overlap_depth"][0].get<float>() == doctest::Approx(0.8f));
        CHECK(finding["overlap_depth"][1].get<float>() == doctest::Approx(1.0f));
        REQUIRE(finding.contains("overlap_ratio"));
        CHECK(finding["overlap_ratio"].get<float>() == doctest::Approx(0.8f));
    }
    // A-vs-B only, never the mirrored B-vs-A duplicate.
    CHECK(overlapFindings == 1);
}

// --- environment_set file support ----------------------------------------

TEST_CASE("environment_set rejects file combined with a procedural field") {
    scene::Scene scene;
    ToolRegistry registry;
    SceneToolContext ctx{.scene = &scene, .renderer = nullptr};
    ctx.applyEnvironment = [](const asset::ProceduralSkyDesc&) { return true; };
    ctx.applyEnvironmentFile = [](const std::string&) { return true; };
    registerSceneTools(registry, ctx);

    const json result =
        invokeOn(registry, "environment_set", R"({"file":"day.hdr","preset":"sunset"})");
    CHECK(result["status"] == "error");
    CHECK(result["message"].get<std::string>().find("mutually exclusive") != std::string::npos);
}

TEST_CASE("environment_set rejects traversal/absolute/hidden file names without invoking the "
          "callback") {
    scene::Scene scene;
    ToolRegistry registry;
    bool called = false;
    SceneToolContext ctx{.scene = &scene, .renderer = nullptr};
    ctx.applyEnvironmentFile = [&called](const std::string&) {
        called = true;
        return true;
    };
    registerSceneTools(registry, ctx);

    for (const char* bad : {"../x", "a/b", "/tmp/x", ".hidden"}) {
        const json result =
            invokeOn(registry, "environment_set", std::format(R"({{"file":"{}"}})", bad));
        CHECK(result["status"] == "error");
        CHECK(result["message"] == "asset names must be plain names from asset_list, not paths");
    }
    CHECK(!called);
}

TEST_CASE("environment_set routes file to the applyEnvironmentFile callback") {
    scene::Scene scene;
    ToolRegistry registry;
    std::optional<std::string> captured;
    SceneToolContext ctx{.scene = &scene, .renderer = nullptr};
    ctx.applyEnvironmentFile = [&captured](const std::string& file) {
        captured = file;
        return true;
    };
    registerSceneTools(registry, ctx);

    const json result = invokeOn(registry, "environment_set", R"({"file":"day.hdr"})");
    REQUIRE(result["status"] == "ok");
    CHECK(result["file"] == "day.hdr");
    REQUIRE(captured.has_value());
    CHECK(*captured == "day.hdr");
}

TEST_CASE("environment_set reports unsupported for file without an applyEnvironmentFile "
          "callback") {
    Fixture f; // default context leaves applyEnvironmentFile null
    const json result = f.invoke("environment_set", R"({"file":"day.hdr"})");
    CHECK(result["status"] == "error");
    CHECK(result["message"].get<std::string>().find("not available") != std::string::npos);
}

TEST_CASE("scene_validate flags thin geometry buried in another entity as a warning") {
    Fixture f;
    f.invoke("scene_add_entity", R"({"primitive":"cube","position":[0,0.5,0]})");
    // A 1 cm wall crossing the cube: its X depth is tiny, but this is not
    // support contact and must not slip under the margin.
    f.invoke("scene_add_entity", R"({"primitive":"cube","position":[0,0.5,0],"scale":[0.01,2,2]})");

    const json result = f.invoke("scene_validate", "");
    REQUIRE(result["status"] == "ok");
    bool warned = false;
    for (const auto& finding : result["findings"]) {
        if (finding["check"] == "overlap" && finding["severity"] == "warning") {
            warned = true;
        }
    }
    CHECK(warned);
}

TEST_CASE("scene_validate skips overlaps inside one assembly but reports across assemblies") {
    Fixture f;
    // Two members occupying the same spot: intended interpenetration inside
    // the assembly, like a lamp head meeting its pole.
    f.invoke(
        "scene_define_group",
        R"({"name":"lamp","entities":[{"name":"a","primitive":"cube"},{"name":"b","primitive":"cube"}]})");
    f.invoke("scene_instance_group", R"({"name":"lamp","transforms":[{"position":[0,0.5,0]}]})");

    const json alone = f.invoke("scene_validate", "");
    REQUIRE(alone["status"] == "ok");
    CHECK(!hasFinding(alone["findings"], "overlap"));

    // A second instance stamped into the first: overlaps BETWEEN the two
    // assemblies are real and must still be reported.
    f.invoke("scene_instance_group", R"({"name":"lamp","transforms":[{"position":[0.3,0.5,0]}]})");
    const json crossed = f.invoke("scene_validate", "");
    REQUIRE(crossed["status"] == "ok");
    bool crossWarning = false;
    for (const auto& finding : crossed["findings"]) {
        if (finding["check"] == "overlap" && finding["severity"] == "warning") {
            crossWarning = true;
        }
    }
    CHECK(crossWarning);
}

TEST_CASE("scene_add_entity avoid_overlap ignores clearance as a collision tolerance") {
    Fixture f;
    f.invoke("scene_add_entity", R"({"primitive":"cube","position":[0,0.5,0]})");
    // A huge clearance must not reclassify full interpenetration as support.
    const json rejected =
        f.invoke("scene_add_entity",
                 R"({"primitive":"cube","position":[0,0.5,0],"avoid_overlap":true,"clearance":1})");
    CHECK(rejected["status"] == "error");
    REQUIRE(rejected.contains("conflicting_entity_ids"));
    CHECK(f.scene.entities.size() == 1);
}

// --- asset_search (MR) ------------------------------------------------

namespace {

// One searchable library: two textures, one model, one env, plus recipe/spec
// entries that must stay invisible to asset_search.
AssetIndex searchableIndex() {
    AssetIndex index;
    AssetIndexEntry asphalt;
    asphalt.id = "asphalt";
    asphalt.kind = AssetIndexKind::Texture;
    asphalt.tags = {"road", "urban"};
    asphalt.caption = "wet asphalt road surface";
    asphalt.thumbnail = ".thumbnails/textures/asphalt.png";
    index.entries.push_back(asphalt);
    AssetIndexEntry grass;
    grass.id = "grass";
    grass.kind = AssetIndexKind::Texture;
    grass.tags = {"ground", "nature"};
    index.entries.push_back(grass);
    AssetIndexEntry crate;
    crate.id = "props/crate";
    crate.kind = AssetIndexKind::Model;
    crate.category = "props";
    crate.tags = {"wooden", "box"};
    crate.dimensions = math::float3{0.5f, 0.5f, 0.5f};
    index.entries.push_back(crate);
    AssetIndexEntry night;
    night.id = "city_night";
    night.kind = AssetIndexKind::Environment;
    night.tags = {"night", "urban"};
    index.entries.push_back(night);
    AssetIndexEntry recipe;
    recipe.id = "wood_grain";
    recipe.kind = AssetIndexKind::Recipe;
    recipe.tags = {"wood"};
    index.entries.push_back(recipe);
    AssetIndexEntry spec;
    spec.id = "product_studio";
    spec.kind = AssetIndexKind::Spec;
    spec.tags = {"studio"};
    index.entries.push_back(spec);
    return index;
}

ToolRegistry makeSearchRegistry(scene::Scene& scene, const std::filesystem::path& assetDir,
                                SceneToolContext overrides = {}) {
    ToolRegistry registry;
    overrides.scene = &scene;
    overrides.renderer = nullptr;
    overrides.assetDir = assetDir;
    registerSceneTools(registry, overrides);
    return registry;
}

} // namespace

TEST_CASE("asset_search reports unset assetDir as unsupported") {
    Fixture f;
    const json result = f.invoke("asset_search", R"({"query":"asphalt"})");
    CHECK(result["status"] == "error");
    CHECK(result["message"].get<std::string>().find("not configured") != std::string::npos);
}

TEST_CASE("asset_search without an index points at viewer --index") {
    TempDir dir("kumo_scene_tools_search_noindex");
    scene::Scene scene;
    ToolRegistry registry = makeSearchRegistry(scene, dir.path);
    const json result = invokeOn(registry, "asset_search", R"({"query":"asphalt"})");
    CHECK(result["status"] == "error");
    CHECK(result["message"].get<std::string>().find("--index") != std::string::npos);
}

TEST_CASE("asset_search validates its arguments") {
    TempDir dir("kumo_scene_tools_search_args");
    scene::Scene scene;
    ToolRegistry registry = makeSearchRegistry(scene, dir.path);
    CHECK(invokeOn(registry, "asset_search", "{}")["status"] == "error");
    CHECK(invokeOn(registry, "asset_search",
                   json{{"query", std::string(65, 'x')}}.dump())["status"] == "error");
    CHECK(invokeOn(registry, "asset_search", R"({"query":"a","kind":"recipe"})")["status"] ==
          "error");
    CHECK(invokeOn(registry, "asset_search", R"({"query":"a","limit":0})")["status"] == "error");
    CHECK(invokeOn(registry, "asset_search", R"({"query":"a","limit":6})")["status"] == "error");
    CHECK(invokeOn(registry, "asset_search", R"({"query":"a","max_dimension":-1})")["status"] ==
          "error");
}

TEST_CASE("asset_search ranks FTS hits, excludes recipe/spec entries and attaches existing "
          "thumbnails") {
    TempDir dir("kumo_scene_tools_search_fts");
    REQUIRE(saveAssetIndex(dir.path, searchableIndex()));
    touch(dir.path / ".thumbnails" / "textures" / "asphalt.png");
    scene::Scene scene;
    ToolRegistry registry = makeSearchRegistry(scene, dir.path);

    const json result = invokeOn(registry, "asset_search", R"({"query":"wet asphalt"})");
    REQUIRE(result["status"] == "ok");
    CHECK(result["vector_search"] == false);
    REQUIRE(result["results"].size() == 1);
    CHECK(result["results"][0]["id"] == "asphalt");
    CHECK(result["results"][0]["kind"] == "texture");
    CHECK(result["results"][0]["thumbnail_attached"] == true);
    REQUIRE(result["image_paths"].size() == 1);
    CHECK(result["image_paths"][0].get<std::string>().find("asphalt.png") != std::string::npos);
    CHECK(result["image_detail"] == "low");

    // "wood" matches the wood_grain recipe and the crate's tag; only the
    // model may surface here.
    const json wood = invokeOn(registry, "asset_search", R"({"query":"wooden"})");
    REQUIRE(wood["status"] == "ok");
    REQUIRE(wood["results"].size() == 1);
    CHECK(wood["results"][0]["id"] == "props/crate");

    const json none = invokeOn(registry, "asset_search", R"({"query":"volcano"})");
    REQUIRE(none["status"] == "ok");
    CHECK(none["results"].empty());
    CHECK(none.contains("note"));
}

TEST_CASE("asset_search applies kind, category and max_dimension filters") {
    TempDir dir("kumo_scene_tools_search_filters");
    REQUIRE(saveAssetIndex(dir.path, searchableIndex()));
    scene::Scene scene;
    ToolRegistry registry = makeSearchRegistry(scene, dir.path);

    const json urbanEnv = invokeOn(registry, "asset_search", R"({"query":"urban","kind":"env"})");
    REQUIRE(urbanEnv["status"] == "ok");
    REQUIRE(urbanEnv["results"].size() == 1);
    CHECK(urbanEnv["results"][0]["id"] == "city_night");

    const json fits =
        invokeOn(registry, "asset_search", R"({"query":"box","kind":"model","max_dimension":1.0})");
    REQUIRE(fits["results"].size() == 1);
    const json tooBig =
        invokeOn(registry, "asset_search", R"({"query":"box","kind":"model","max_dimension":0.2})");
    CHECK(tooBig["results"].empty());
}

TEST_CASE("asset_search fuses the embedded query when sidecar and effector exist, and degrades "
          "without them") {
    TempDir dir("kumo_scene_tools_search_vec");
    AssetIndex index = searchableIndex();
    index.embedding = AssetIndexEmbedding{.model = "test", .dim = 2, .file = "rows.bin"};
    index.entries[0].embeddingOffset = 0; // asphalt -> [1, 0]
    index.entries[1].embeddingOffset = 1; // grass   -> [0, 1]
    REQUIRE(saveAssetIndex(dir.path, index));
    REQUIRE(
        saveEmbeddingRows(dir.path, *index.embedding, std::vector<float>{1.0f, 0.0f, 0.0f, 1.0f}));
    scene::Scene scene;

    SceneToolContext withEmbed;
    std::string capturedQuery;
    withEmbed.embedQuery = [&capturedQuery](std::string_view query) {
        capturedQuery = std::string(query);
        return std::optional<std::vector<float>>{{1.0f, 0.0f}};
    };
    ToolRegistry registry = makeSearchRegistry(scene, dir.path, withEmbed);
    // "pavement" shares no token with any entry: only the vector can find it.
    const json result = invokeOn(registry, "asset_search", R"({"query":"pavement"})");
    REQUIRE(result["status"] == "ok");
    CHECK(result["vector_search"] == true);
    CHECK(capturedQuery == "pavement");
    REQUIRE(result["results"].size() == 2);
    CHECK(result["results"][0]["id"] == "asphalt");

    // A wrong-dimension embedding degrades to FTS silently.
    SceneToolContext badEmbed;
    badEmbed.embedQuery = [](std::string_view) {
        return std::optional<std::vector<float>>{{1.0f, 0.0f, 0.0f}};
    };
    ToolRegistry degraded = makeSearchRegistry(scene, dir.path, badEmbed);
    const json fallback = invokeOn(degraded, "asset_search", R"({"query":"pavement"})");
    REQUIRE(fallback["status"] == "ok");
    CHECK(fallback["vector_search"] == false);
    CHECK(fallback["results"].empty());
}

// --- primitive_heavy (MS) ---------------------------------------------

namespace {

void insertShaped(scene::Scene& scene, const char* primitive, const char* model, int count) {
    for (int i = 0; i < count; ++i) {
        scene::Entity entity;
        entity.primitive = primitive;
        entity.model = model;
        scene.entities.insert(entity);
    }
}

bool primitiveHeavyFlagged(Fixture& f) {
    const json result = f.invoke("scene_validate", "");
    REQUIRE(result["status"] == "ok");
    return hasFinding(result["findings"], "primitive_heavy");
}

} // namespace

TEST_CASE("scene_validate flags a primitive-heavy scene (MS model-first policy)") {
    Fixture f;
    insertShaped(f.scene, "cube", "", 6);
    CHECK(primitiveHeavyFlagged(f));
}

TEST_CASE("scene_validate primitive_heavy stays quiet at or under the count threshold") {
    Fixture f;
    insertShaped(f.scene, "cube", "", 5); // needs MORE than 5
    CHECK(!primitiveHeavyFlagged(f));
}

TEST_CASE("scene_validate primitive_heavy exempts planes and model-majority scenes") {
    Fixture planes;
    insertShaped(planes.scene, "plane", "", 10); // architecture, exempt
    CHECK(!primitiveHeavyFlagged(planes));

    Fixture mixed;
    insertShaped(mixed.scene, "cube", "", 6);
    insertShaped(mixed.scene, "", "furniture/bedDouble", 7); // models outnumber
    CHECK(!primitiveHeavyFlagged(mixed));

    Fixture tipped;
    insertShaped(tipped.scene, "cube", "", 8);
    insertShaped(tipped.scene, "", "furniture/bedDouble", 3);
    CHECK(primitiveHeavyFlagged(tipped));
}
