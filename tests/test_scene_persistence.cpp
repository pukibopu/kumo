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

TEST_CASE("save/parse round trip preserves the environment when present") {
    scene::Scene scene;
    const scene::MaterialLookup noMaterials = [](std::int32_t) { return std::nullopt; };

    scene::SavedEnvironment env;
    env.zenithColor[0] = 0.1f;
    env.zenithColor[1] = 0.2f;
    env.zenithColor[2] = 0.3f;
    env.horizonColor[0] = 0.4f;
    env.groundColor[2] = 0.05f;
    env.sunDirection[0] = -0.6f;
    env.sunDirection[1] = -0.5f;
    env.sunDirection[2] = -0.2f;
    env.sunColor[1] = 0.8f;
    env.sunIntensity = 12.5f;
    env.sunAngularRadiusDeg = 2.5f;
    env.exposure = 1.4f;

    const std::string json = scene::saveSceneJson(scene, "assets/model.glb", noMaterials, env);
    const auto parsed = scene::parseSceneJson(json);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->environment.has_value());
    const scene::SavedEnvironment& roundTripped = *parsed->environment;
    CHECK(roundTripped.zenithColor[0] == 0.1f);
    CHECK(roundTripped.zenithColor[1] == 0.2f);
    CHECK(roundTripped.zenithColor[2] == 0.3f);
    CHECK(roundTripped.horizonColor[0] == 0.4f);
    CHECK(roundTripped.groundColor[2] == 0.05f);
    CHECK(roundTripped.sunDirection[0] == -0.6f);
    CHECK(roundTripped.sunDirection[1] == -0.5f);
    CHECK(roundTripped.sunDirection[2] == -0.2f);
    CHECK(roundTripped.sunColor[1] == 0.8f);
    CHECK(roundTripped.sunIntensity == 12.5f);
    CHECK(roundTripped.sunAngularRadiusDeg == 2.5f);
    CHECK(roundTripped.exposure == 1.4f);
    CHECK(!roundTripped.file.has_value()); // procedural: no file key was written
}

TEST_CASE("save/parse round trip preserves an environment file reference") {
    scene::Scene scene;
    const scene::MaterialLookup noMaterials = [](std::int32_t) { return std::nullopt; };

    scene::SavedEnvironment env;
    env.file = "sunset_2k.hdr";

    const std::string json = scene::saveSceneJson(scene, "assets/model.glb", noMaterials, env);
    CHECK(json.find("\"file\": \"sunset_2k.hdr\"") != std::string::npos);
    const auto parsed = scene::parseSceneJson(json);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->environment.has_value());
    REQUIRE(parsed->environment->file.has_value());
    CHECK(*parsed->environment->file == "sunset_2k.hdr");
}

TEST_CASE("parseSceneJson defaults environment.file to absent when the key is missing") {
    const auto parsed = scene::parseSceneJson(
        R"({"version":0,"model":"","lights":[],"entities":[],)"
        R"("environment":{"zenith_color":[0,0,0],"horizon_color":[0,0,0],)"
        R"("ground_color":[0,0,0],"sun_direction":[0,-1,0],"sun_color":[1,1,1],)"
        R"("sun_intensity":1,"sun_angular_radius_deg":1,"exposure":1}})");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->environment.has_value());
    CHECK(!parsed->environment->file.has_value());
}

TEST_CASE("save/parse round trip preserves an entity's model provenance") {
    scene::Scene scene;
    scene::Entity entity;
    entity.name = "tree_0";
    entity.meshIndex = 2;
    entity.materialIndex = 1;
    entity.model = "tree";
    entity.modelMesh = 3;
    scene.entities.insert(entity);

    const scene::MaterialLookup noMaterials = [](std::int32_t) { return std::nullopt; };
    const std::string json = scene::saveSceneJson(scene, "assets/model.glb", noMaterials);
    const auto parsed = scene::parseSceneJson(json);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->entities.size() == 1);
    CHECK(parsed->entities[0].entity.model == "tree");
    CHECK(parsed->entities[0].entity.modelMesh == 3);
}

TEST_CASE("parseSceneJson defaults an entity's model provenance to empty/-1 when absent") {
    const auto parsed =
        scene::parseSceneJson(R"({"version":0,"model":"","lights":[],"entities":[{"name":"e"}]})");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->entities.size() == 1);
    CHECK(parsed->entities[0].entity.model.empty());
    CHECK(parsed->entities[0].entity.modelMesh == -1);
}

TEST_CASE("save/parse round trip preserves a material's custom shader source") {
    scene::Scene scene;

    scene::Entity shaded;
    shaded.name = "shaded";
    shaded.meshIndex = 0;
    shaded.materialIndex = 0;
    scene.entities.insert(shaded);

    scene::Entity plain;
    plain.name = "plain";
    plain.meshIndex = 1;
    plain.materialIndex = 1;
    scene.entities.insert(plain);

    const scene::MaterialLookup noMaterials = [](std::int32_t) { return std::nullopt; };
    const std::string customSource = "// custom fragment shader\nvoid main() {}\n";
    const scene::ShaderLookup shaders = [&](std::int32_t index) -> std::optional<std::string> {
        return index == 0 ? std::make_optional(customSource) : std::nullopt;
    };

    const std::string json =
        scene::saveSceneJson(scene, "assets/model.glb", noMaterials, std::nullopt, shaders);
    const auto parsed = scene::parseSceneJson(json);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->entities.size() == 2);

    REQUIRE(parsed->entities[0].shaderSource.has_value());
    CHECK(*parsed->entities[0].shaderSource == customSource);
    CHECK(!parsed->entities[1].shaderSource.has_value());
}

TEST_CASE("save/parse round trip omits shader_source when the lookup returns nullopt") {
    scene::Scene scene;
    scene::Entity entity;
    entity.name = "plain";
    entity.materialIndex = 0;
    scene.entities.insert(entity);

    const scene::MaterialLookup noMaterials = [](std::int32_t) { return std::nullopt; };
    const scene::ShaderLookup noShaders = [](std::int32_t) { return std::nullopt; };
    const std::string json =
        scene::saveSceneJson(scene, "assets/model.glb", noMaterials, std::nullopt, noShaders);
    CHECK(json.find("\"shader_source\"") == std::string::npos);
    const auto parsed = scene::parseSceneJson(json);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->entities.size() == 1);
    CHECK(!parsed->entities[0].shaderSource.has_value());
}

TEST_CASE("save/parse round trip omits the environment key when absent") {
    scene::Scene scene;
    const scene::MaterialLookup noMaterials = [](std::int32_t) { return std::nullopt; };
    const std::string json = scene::saveSceneJson(scene, "assets/model.glb", noMaterials);
    CHECK(json.find("\"environment\"") == std::string::npos);
    const auto parsed = scene::parseSceneJson(json);
    REQUIRE(parsed.has_value());
    CHECK(!parsed->environment.has_value());
}

TEST_CASE("save/parse round trip preserves a material's uv_tiling") {
    scene::Scene scene;
    scene::Entity entity;
    entity.name = "tiled";
    entity.materialIndex = 0;
    scene.entities.insert(entity);

    scene::SavedMaterial material = makeMaterial();
    material.uvTiling[0] = 3.0f;
    material.uvTiling[1] = 4.5f;
    const scene::MaterialLookup materials =
        [&](std::int32_t index) -> std::optional<scene::SavedMaterial> {
        return index == 0 ? std::make_optional(material) : std::nullopt;
    };

    const std::string json = scene::saveSceneJson(scene, "assets/model.glb", materials);
    const auto parsed = scene::parseSceneJson(json);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->entities.size() == 1);
    REQUIRE(parsed->entities[0].material.has_value());
    CHECK(parsed->entities[0].material->uvTiling[0] == 3.0f);
    CHECK(parsed->entities[0].material->uvTiling[1] == 4.5f);
}

TEST_CASE("parseSceneJson defaults uv_tiling to identity when the key is absent") {
    const auto parsed = scene::parseSceneJson(
        R"({"version":0,"model":"","lights":[],"entities":[{"name":"e","material":)"
        R"({"base_color":[1,1,1,1],"metallic":1,"roughness":1,"emissive":[0,0,0]}}]})");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->entities.size() == 1);
    REQUIRE(parsed->entities[0].material.has_value());
    CHECK(parsed->entities[0].material->uvTiling[0] == 1.0f);
    CHECK(parsed->entities[0].material->uvTiling[1] == 1.0f);
}

// --- textureSet (M6.99) ------------------------------------------------

TEST_CASE("save/parse round trip preserves an entity's texture-set provenance") {
    scene::Scene scene;
    scene::Entity entity;
    entity.name = "wall";
    entity.primitive = "cube";
    entity.primitiveSize = 2.0f;
    entity.materialIndex = 0;
    entity.textureSet = "bark";
    scene.entities.insert(entity);

    const scene::MaterialLookup noMaterials = [](std::int32_t) { return std::nullopt; };
    const std::string json = scene::saveSceneJson(scene, "assets/model.glb", noMaterials);
    const auto parsed = scene::parseSceneJson(json);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->entities.size() == 1);
    CHECK(parsed->entities[0].entity.textureSet == "bark");
}

TEST_CASE("parseSceneJson defaults an entity's texture-set provenance to empty when absent (pre-"
          "M6.99 saves)") {
    const auto parsed =
        scene::parseSceneJson(R"({"version":0,"model":"","lights":[],"entities":[{"name":"e"}]})");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->entities.size() == 1);
    CHECK(parsed->entities[0].entity.textureSet.empty());
}

TEST_CASE("save/parse round trip omits texture_set when empty") {
    scene::Scene scene;
    scene::Entity entity;
    entity.name = "plain";
    scene.entities.insert(entity);

    const scene::MaterialLookup noMaterials = [](std::int32_t) { return std::nullopt; };
    const std::string json = scene::saveSceneJson(scene, "assets/model.glb", noMaterials);
    CHECK(json.find("\"texture_set\"") == std::string::npos);
}

TEST_CASE("save/parse round trip preserves a texture-set entity combined with uv_tiling") {
    // Mirrors the bark-wall scenario: a primitive entity textured via
    // material_set_texture with non-default tiling must reload both textured
    // (textureSet non-empty) AND tiled (material.uvTiling preserved).
    scene::Scene scene;
    scene::Entity entity;
    entity.name = "bark_wall";
    entity.primitive = "cube";
    entity.materialIndex = 0;
    entity.textureSet = "bark";
    scene.entities.insert(entity);

    scene::SavedMaterial material = makeMaterial();
    material.uvTiling[0] = 3.0f;
    material.uvTiling[1] = 3.0f;
    const scene::MaterialLookup materials =
        [&](std::int32_t index) -> std::optional<scene::SavedMaterial> {
        return index == 0 ? std::make_optional(material) : std::nullopt;
    };

    const std::string json = scene::saveSceneJson(scene, "assets/model.glb", materials);
    const auto parsed = scene::parseSceneJson(json);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->entities.size() == 1);
    CHECK(parsed->entities[0].entity.textureSet == "bark");
    REQUIRE(parsed->entities[0].material.has_value());
    CHECK(parsed->entities[0].material->uvTiling[0] == 3.0f);
    CHECK(parsed->entities[0].material->uvTiling[1] == 3.0f);
}

// --- assemblyId (MP) ----------------------------------------------------

TEST_CASE("save/parse round trip preserves assembly ids and omits zero") {
    scene::Scene scene;
    scene::Entity member;
    member.name = "lamp_pole";
    member.assemblyId = 7;
    scene.entities.insert(member);
    scene::Entity solo;
    solo.name = "rock";
    scene.entities.insert(solo);

    const scene::MaterialLookup noMaterials = [](std::int32_t) { return std::nullopt; };
    const std::string json = scene::saveSceneJson(scene, "assets/model.glb", noMaterials);
    const auto parsed = scene::parseSceneJson(json);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->entities.size() == 2);
    CHECK(parsed->entities[0].entity.assemblyId == 7);
    CHECK(parsed->entities[1].entity.assemblyId == 0);
    // Independent entities carry no assembly_id key at all (pre-MP saves stay
    // byte-identical), so the key appears exactly once: on the assembly member.
    const std::size_t first = json.find("\"assembly_id\"");
    REQUIRE(first != std::string::npos);
    CHECK(json.rfind("\"assembly_id\"") == first);
}

// --- surface params (MD) ------------------------------------------------

TEST_CASE("save/parse round trip preserves surface params by name, type and value") {
    scene::Scene scene;
    scene::Entity entity;
    entity.name = "crate";
    entity.materialIndex = 2;
    scene.entities.insert(entity);

    const scene::MaterialLookup noMaterials = [](std::int32_t) { return std::nullopt; };
    const scene::SurfaceLookup surfaces =
        [](std::int32_t materialIndex) -> std::vector<scene::SavedSurfaceParam> {
        if (materialIndex != 2) {
            return {};
        }
        scene::SavedSurfaceParam scale;
        scale.name = "grainScale";
        scale.value[0] = 8.0f;
        scene::SavedSurfaceParam tint;
        tint.name = "tint";
        tint.isVec4 = true;
        tint.value[0] = 0.1f;
        tint.value[3] = 1.0f;
        return {scale, tint};
    };
    const std::string json =
        scene::saveSceneJson(scene, "assets/model.glb", noMaterials, std::nullopt, {}, surfaces);
    const auto parsed = scene::parseSceneJson(json);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->entities.size() == 1);
    const auto& params = parsed->entities[0].surfaceParams;
    REQUIRE(params.size() == 2);
    CHECK(params[0].name == "grainScale");
    CHECK(!params[0].isVec4);
    CHECK(params[0].value[0] == doctest::Approx(8.0f));
    CHECK(params[1].name == "tint");
    CHECK(params[1].isVec4);
    CHECK(params[1].value[3] == doctest::Approx(1.0f));
}

TEST_CASE("parseSceneJson rejects malformed surface params and defaults to none when absent") {
    const auto missing =
        scene::parseSceneJson(R"({"version":0,"model":"","lights":[],"entities":[{"name":"e"}]})");
    REQUIRE(missing.has_value());
    CHECK(missing->entities[0].surfaceParams.empty());

    const auto badArity = scene::parseSceneJson(
        R"({"version":0,"model":"","lights":[],"entities":[{"name":"e",
"surface_params":[{"name":"x","type":"vec4","value":[1,2]}]}]})");
    CHECK(!badArity.has_value());
}
