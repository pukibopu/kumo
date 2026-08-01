#include <doctest/doctest.h>

#include <kumo/agent/scene_tools.h>
#include <kumo/agent/tool_registry.h>
#include <kumo/math/math.h>
#include <kumo/scene/scene.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <format>
#include <string>
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

} // namespace

TEST_CASE("scene tools register all ten with object schemas") {
    Fixture f;
    const char* expected[] = {"scene_list",          "scene_add_entity",    "scene_add_entities",
                              "scene_remove_entity", "scene_set_transform", "camera_set",
                              "light_set",           "material_set_param",  "scene_define_group",
                              "scene_instance_group"};
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
    CHECK(!f.registry.find("scene_add_entities")->destructive);
    CHECK(!f.registry.find("scene_define_group")->destructive);
    CHECK(!f.registry.find("scene_instance_group")->destructive);
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
