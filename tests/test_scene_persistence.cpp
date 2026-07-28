#include <doctest/doctest.h>

#include <kumo/math/math.h>
#include <kumo/scene/persistence.h>

#include <optional>
#include <string>

using namespace kumo;

namespace {

scene::SavedMaterial makeMaterial() {
    scene::SavedMaterial material;
    material.baseColor[0] = 0.2f;
    material.baseColor[1] = 0.4f;
    material.baseColor[2] = 0.6f;
    material.baseColor[3] = 1.0f;
    material.metallic = 0.8f;
    material.roughness = 0.3f;
    material.emissive[0] = 0.15f;
    material.emissive[1] = 0.05f;
    material.emissive[2] = 0.0f;
    return material;
}

} // namespace

TEST_CASE("save/parse round trip preserves entities, camera and lights") {
    scene::Scene scene;

    scene::Entity ball;
    ball.name = "ball";
    ball.transform.position = {1.0f, 2.0f, 3.0f};
    ball.transform.rotation = math::quatFromEulerDegrees({10.0f, 20.0f, 30.0f});
    ball.transform.scale = {1.5f, 1.5f, 1.5f};
    ball.meshIndex = 0;
    ball.materialIndex = 0;
    ball.primitive = "sphere";
    ball.primitiveSize = 2.0f;
    scene.entities.insert(ball);

    scene::Entity gltfNode;
    gltfNode.name = "node_mesh";
    gltfNode.transform.position = {-1.0f, 0.0f, 0.5f};
    gltfNode.meshIndex = 3;
    gltfNode.materialIndex = -1;
    scene.entities.insert(gltfNode);

    scene::Entity capsule;
    capsule.name = "capsule";
    capsule.transform.position = {2.0f, 0.0f, -1.0f};
    capsule.meshIndex = 1;
    capsule.materialIndex = -1;
    capsule.primitive = "capsule";
    capsule.primitiveSize = 1.5f;
    scene.entities.insert(capsule);

    scene::Entity cylinder;
    cylinder.name = "cylinder";
    cylinder.transform.position = {0.0f, 0.0f, 2.0f};
    cylinder.meshIndex = 2;
    cylinder.materialIndex = -1;
    cylinder.primitive = "cylinder";
    cylinder.primitiveSize = 0.8f;
    scene.entities.insert(cylinder);

    CHECK(scene.addLight({.type = scene::LightType::Directional, .intensity = 2.0f}));
    CHECK(scene.addLight({.type = scene::LightType::Point,
                          .color = {1.0f, 0.0f, 0.0f},
                          .intensity = 5.0f,
                          .position = {0.0f, 3.0f, 0.0f},
                          .range = 10.0f}));

    scene.camera.position = {0.0f, 1.0f, 5.0f};
    scene.camera.rotation = math::quatFromEulerDegrees({5.0f, -15.0f, 40.0f});
    scene.camera.fovY = math::radians(50.0f);
    scene.camera.nearZ = 0.25f;

    const scene::MaterialLookup materials =
        [](std::int32_t index) -> std::optional<scene::SavedMaterial> {
        return index == 0 ? std::make_optional(makeMaterial()) : std::nullopt;
    };

    const std::string json = scene::saveSceneJson(scene, "assets/model.glb", materials);
    const auto parsed = scene::parseSceneJson(json);
    REQUIRE(parsed.has_value());
    const scene::SavedScene& saved = *parsed;

    CHECK(saved.modelPath == "assets/model.glb");
    REQUIRE(saved.entities.size() == 4);

    const scene::SavedEntity& savedBall = saved.entities[0];
    CHECK(savedBall.entity.name == "ball");
    CHECK(savedBall.entity.primitive == "sphere");
    CHECK(savedBall.entity.primitiveSize == 2.0f);
    CHECK(savedBall.entity.transform.position.x == 1.0f);
    CHECK(savedBall.entity.transform.position.y == 2.0f);
    CHECK(savedBall.entity.transform.position.z == 3.0f);
    CHECK(savedBall.entity.transform.rotation.w == ball.transform.rotation.w);
    CHECK(savedBall.entity.transform.rotation.x == ball.transform.rotation.x);
    CHECK(savedBall.entity.transform.rotation.y == ball.transform.rotation.y);
    CHECK(savedBall.entity.transform.rotation.z == ball.transform.rotation.z);
    CHECK(savedBall.entity.transform.scale.x == 1.5f);
    REQUIRE(savedBall.material.has_value());
    CHECK(savedBall.material->baseColor[2] == 0.6f);
    CHECK(savedBall.material->metallic == 0.8f);
    CHECK(savedBall.material->roughness == 0.3f);
    CHECK(savedBall.material->emissive[0] == 0.15f);

    const scene::SavedEntity& savedNode = saved.entities[1];
    CHECK(savedNode.entity.name == "node_mesh");
    CHECK(savedNode.entity.primitive.empty());
    CHECK(savedNode.entity.meshIndex == 3);
    CHECK(savedNode.entity.materialIndex == -1);
    CHECK(!savedNode.material.has_value());

    const scene::SavedEntity& savedCapsule = saved.entities[2];
    CHECK(savedCapsule.entity.name == "capsule");
    CHECK(savedCapsule.entity.primitive == "capsule");
    CHECK(savedCapsule.entity.primitiveSize == 1.5f);
    CHECK(savedCapsule.entity.transform.position.x == 2.0f);
    CHECK(savedCapsule.entity.transform.position.z == -1.0f);
    CHECK(!savedCapsule.material.has_value());

    const scene::SavedEntity& savedCylinder = saved.entities[3];
    CHECK(savedCylinder.entity.name == "cylinder");
    CHECK(savedCylinder.entity.primitive == "cylinder");
    CHECK(savedCylinder.entity.primitiveSize == 0.8f);
    CHECK(savedCylinder.entity.transform.position.z == 2.0f);
    CHECK(!savedCylinder.material.has_value());

    CHECK(saved.camera.position.y == 1.0f);
    CHECK(saved.camera.rotation.w == scene.camera.rotation.w);
    CHECK(saved.camera.rotation.x == scene.camera.rotation.x);
    CHECK(saved.camera.rotation.y == scene.camera.rotation.y);
    CHECK(saved.camera.rotation.z == scene.camera.rotation.z);
    CHECK(saved.camera.fovY == scene.camera.fovY);
    CHECK(saved.camera.nearZ == 0.25f);

    REQUIRE(saved.lights.size() == 2);
    CHECK(saved.lights[0].type == scene::LightType::Directional);
    CHECK(saved.lights[0].intensity == 2.0f);
    CHECK(saved.lights[1].type == scene::LightType::Point);
    CHECK(saved.lights[1].color.x == 1.0f);
    CHECK(saved.lights[1].intensity == 5.0f);
    CHECK(saved.lights[1].position.y == 3.0f);
    CHECK(saved.lights[1].range == 10.0f);
}

TEST_CASE("parseSceneJson rejects a different format version") {
    const auto parsed =
        scene::parseSceneJson(R"({"version":1,"model":"","lights":[],"entities":[]})");
    REQUIRE(!parsed.has_value());
    CHECK(parsed.error().find("version") != std::string::npos);
}

TEST_CASE("parseSceneJson rejects malformed json") {
    const auto parsed = scene::parseSceneJson("{ not json at all");
    CHECK(!parsed.has_value());
}

TEST_CASE("parseSceneJson names the offending field on a type mismatch") {
    const auto parsed =
        scene::parseSceneJson(R"({"version":0,"model":42,"lights":[],"entities":[]})");
    REQUIRE(!parsed.has_value());
    CHECK(parsed.error().find("model") != std::string::npos);
}

TEST_CASE("parseSceneJson rejects an unknown light type") {
    const auto parsed =
        scene::parseSceneJson(R"({"version":0,"lights":[{"type":"spot"}],"entities":[]})");
    REQUIRE(!parsed.has_value());
    CHECK(parsed.error().find("spot") != std::string::npos);
}

TEST_CASE("parseSceneJson entities without primitive parse with empty provenance") {
    const auto parsed = scene::parseSceneJson(
        R"({"version":0,"lights":[],"entities":[{"name":"n","mesh_index":2,"material_index":-1}]})");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->entities.size() == 1);
    CHECK(parsed->entities[0].entity.name == "n");
    CHECK(parsed->entities[0].entity.primitive.empty());
    CHECK(parsed->entities[0].entity.meshIndex == 2);
    CHECK(!parsed->entities[0].material.has_value());
}
