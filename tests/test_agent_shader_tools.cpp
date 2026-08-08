#include <doctest/doctest.h>

#include <kumo/agent/entity_id.h>
#include <kumo/agent/shader_tools.h>
#include <kumo/agent/tool_registry.h>
#include <kumo/scene/scene.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

using namespace kumo;
using namespace kumo::agent;
using nlohmann::json;

namespace {

// Records every call and answers success or a fixed compile failure depending
// on `fail`; lets one test flip behavior mid-sequence to exercise the
// attempt-cap reset without needing a second renderer stub.
struct FakeShader {
    bool fail = false;
    std::vector<std::pair<std::uint32_t, std::string>> calls;

    std::expected<void, std::vector<shaderc::CompileError>> operator()(std::uint32_t materialIndex,
                                                                       std::string_view source) {
        calls.emplace_back(materialIndex, std::string(source));
        if (fail) {
            return std::unexpected(
                std::vector<shaderc::CompileError>{{.file = "pbr.frag",
                                                    .line = 42,
                                                    .message = "undeclared identifier 'foo'",
                                                    .secondStage = false}});
        }
        return {};
    }
};

struct Fixture {
    scene::Scene scene;
    ToolRegistry registry;
    std::filesystem::path dir;
    std::filesystem::path templatePath;
    std::filesystem::path generatedDir;

    Fixture() {
        dir = std::filesystem::temp_directory_path() / "kumo_shader_tools_test";
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        templatePath = dir / "pbr.frag";
        generatedDir = dir / "generated";
        std::ofstream(templatePath) << "// shared pbr template\n";
    }

    ~Fixture() { std::filesystem::remove_all(dir); }

    // Fills in scene/templatePath/generatedDir; the caller supplies the hooks.
    void registerTools(ShaderToolContext context) {
        context.scene = &scene;
        context.templatePath = templatePath;
        context.generatedDir = generatedDir;
        registerShaderTools(registry, context);
    }

    scene::EntityId addEntity(std::int32_t materialIndex) {
        scene::Entity entity;
        entity.materialIndex = materialIndex;
        return scene.entities.insert(entity);
    }

    json invoke(const char* name, const std::string& args) {
        const json result = json::parse(registry.invoke(name, args), nullptr, false);
        REQUIRE(result.is_object());
        return result;
    }
};

std::string idOf(scene::EntityId id) {
    return std::format("{}:{}", id.index, id.generation);
}

} // namespace

TEST_CASE(
    "shader tools register shader_read and shader_write with non-destructive object schemas") {
    Fixture f;
    f.registerTools({});
    for (const char* name : {"shader_read", "shader_write"}) {
        const ToolDef* def = f.registry.find(name);
        REQUIRE_MESSAGE(def != nullptr, name);
        CHECK(!def->description.empty());
        CHECK(!def->destructive);
        const json schema = json::parse(def->parametersSchema, nullptr, false);
        CHECK_MESSAGE(schema.is_object(), name);
        CHECK(schema["type"] == "object");
    }
}

TEST_CASE("shader_read falls back to the template when there is no material or hook") {
    Fixture f;
    f.registerTools({});
    const scene::EntityId noMaterial = f.addEntity(-1);

    const json result =
        f.invoke("shader_read", std::format(R"({{"entity_id":"{}"}})", idOf(noMaterial)));
    CHECK(result["status"] == "ok");
    CHECK(result["customized"] == false);
    CHECK(result["material_index"] == -1);
    CHECK(result["source"] == "// shared pbr template\n");
}

TEST_CASE("shader_read falls back to the template when shaderSource hook is null or misses") {
    Fixture f;
    ShaderToolContext ctx;
    ctx.shaderSource = [](std::uint32_t) -> const std::string* { return nullptr; };
    f.registerTools(ctx);
    const scene::EntityId hasMaterial = f.addEntity(2);

    const json result =
        f.invoke("shader_read", std::format(R"({{"entity_id":"{}"}})", idOf(hasMaterial)));
    CHECK(result["status"] == "ok");
    CHECK(result["customized"] == false);
    CHECK(result["material_index"] == 2);
}

TEST_CASE("shader_read returns the customized source when the hook reports one") {
    Fixture f;
    const std::string custom = "// customized fragment\nvoid main() {}\n";
    ShaderToolContext ctx;
    ctx.shaderSource = [&custom](std::uint32_t materialIndex) -> const std::string* {
        return materialIndex == 5 ? &custom : nullptr;
    };
    f.registerTools(ctx);
    const scene::EntityId customized = f.addEntity(5);
    const scene::EntityId uncustomized = f.addEntity(6);

    const json result =
        f.invoke("shader_read", std::format(R"({{"entity_id":"{}"}})", idOf(customized)));
    CHECK(result["status"] == "ok");
    CHECK(result["customized"] == true);
    CHECK(result["material_index"] == 5);
    CHECK(result["source"] == custom);

    const json other =
        f.invoke("shader_read", std::format(R"({{"entity_id":"{}"}})", idOf(uncustomized)));
    CHECK(other["customized"] == false);
}

TEST_CASE("shader_read reports a missing entity as an error") {
    Fixture f;
    f.registerTools({});
    CHECK(f.invoke("shader_read", R"({"entity_id":"4:2"})")["status"] == "error");
}

TEST_CASE("shader_write validates source and entity_id") {
    Fixture f;
    FakeShader fake;
    ShaderToolContext ctx;
    ctx.setShader = [&fake](std::uint32_t idx, std::string_view src) { return fake(idx, src); };
    f.registerTools(ctx);
    const scene::EntityId withMaterial = f.addEntity(0);

    CHECK(f.invoke("shader_write", std::format(R"({{"entity_id":"{}"}})",
                                               idOf(withMaterial)))["status"] == "error");
    CHECK(f.invoke("shader_write", std::format(R"({{"entity_id":"{}","source":""}})",
                                               idOf(withMaterial)))["status"] == "error");
    CHECK(f.invoke("shader_write", R"({"entity_id":"9:9","source":"x"})")["status"] == "error");
    CHECK(fake.calls.empty());
}

TEST_CASE("shader_write rejects entities without a material and a null renderer hook") {
    Fixture f;
    f.registerTools({});
    const scene::EntityId noMaterial = f.addEntity(-1);
    const json noMaterialResult = f.invoke(
        "shader_write", std::format(R"({{"entity_id":"{}","source":"x"}})", idOf(noMaterial)));
    CHECK(noMaterialResult["status"] == "error");
    CHECK(noMaterialResult["message"].get<std::string>().find("material") != std::string::npos);

    const scene::EntityId hasMaterial = f.addEntity(0);
    const json noHookResult = f.invoke(
        "shader_write", std::format(R"({{"entity_id":"{}","source":"x"}})", idOf(hasMaterial)));
    CHECK(noHookResult["status"] == "error");
    CHECK(noHookResult["message"].get<std::string>().find("renderer") != std::string::npos);
}

TEST_CASE("shader_write success path installs the shader, saves it, and reports sharers") {
    Fixture f;
    FakeShader fake;
    ShaderToolContext ctx;
    ctx.setShader = [&fake](std::uint32_t idx, std::string_view src) { return fake(idx, src); };
    f.registerTools(ctx);

    const scene::EntityId shareA = f.addEntity(7);
    f.addEntity(7); // shares materialIndex 7 with shareA
    const scene::EntityId alone = f.addEntity(9);

    // Built via nlohmann::json (not std::format interpolation) since the
    // source text itself contains braces that would otherwise need escaping.
    const std::string source = "void main() { fragColor = vec4(1.0); }\n";
    const json shared =
        f.invoke("shader_write", json{{"entity_id", idOf(shareA)}, {"source", source}}.dump());
    CHECK(shared["status"] == "ok");
    CHECK(shared["material_index"] == 7);
    CHECK(shared["entities_sharing_material"] == 2);
    REQUIRE(fake.calls.size() == 1);
    CHECK(fake.calls[0].first == 7);
    CHECK(fake.calls[0].second == source);

    const auto savedPath = f.generatedDir / "material_7.frag";
    CHECK(std::filesystem::exists(savedPath));
    std::ifstream in(savedPath);
    const std::string written((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    CHECK(written == source);

    const json soloResult =
        f.invoke("shader_write", std::format(R"({{"entity_id":"{}","source":"x"}})", idOf(alone)));
    CHECK(soloResult["status"] == "ok");
    CHECK(!soloResult.contains("entities_sharing_material"));
}

TEST_CASE("shader_write failure path reports structured errors and enforces the attempt cap") {
    Fixture f;
    FakeShader fake;
    fake.fail = true;
    ShaderToolContext ctx;
    ctx.setShader = [&fake](std::uint32_t idx, std::string_view src) { return fake(idx, src); };
    f.registerTools(ctx);
    const scene::EntityId capped = f.addEntity(3);
    const auto args = std::format(R"({{"entity_id":"{}","source":"x"}})", idOf(capped));

    for (int attempt = 1; attempt <= 5; ++attempt) {
        const json result = f.invoke("shader_write", args);
        CHECK(result["status"] == "error");
        CHECK(result["message"] == "shader compile failed");
        CHECK(result["attempts_used"] == attempt);
        REQUIRE(result["errors"].size() == 1);
        const json& error = result["errors"][0];
        CHECK(error["file"] == "pbr.frag");
        CHECK(error["line"] == 42);
        CHECK(error["message"] == "undeclared identifier 'foo'");
        CHECK(error["second_stage"] == false);
    }
    REQUIRE(fake.calls.size() == 5);

    // The 6th attempt is blocked before the hook is ever invoked.
    const json blocked = f.invoke("shader_write", args);
    CHECK(blocked["status"] == "error");
    CHECK(blocked["message"].get<std::string>().find("attempt limit reached (5)") !=
          std::string::npos);
    CHECK(fake.calls.size() == 5);

    // A different material's counter starts fresh, and a success resets it.
    const scene::EntityId other = f.addEntity(5);
    const auto otherArgs = std::format(R"({{"entity_id":"{}","source":"y"}})", idOf(other));
    fake.fail = true;
    CHECK(f.invoke("shader_write", otherArgs)["attempts_used"] == 1);
    CHECK(f.invoke("shader_write", otherArgs)["attempts_used"] == 2);
    fake.fail = false;
    CHECK(f.invoke("shader_write", otherArgs)["status"] == "ok");
    fake.fail = true;
    CHECK(f.invoke("shader_write", otherArgs)["attempts_used"] == 1);
}

// --- surface path (MD) ---------------------------------------------------

namespace {

// Minimal but marker-complete stand-in for pbr_surface_template.frag, so the
// tool tests stay CPU-only and independent of the real shader tree.
constexpr const char* kSurfaceTemplate = "// header\n"
                                         "uniform Factors {\n"
                                         "    vec4 baseColor;\n"
                                         "    //KUMO_SURFACE_PARAMS\n"
                                         "};\n"
                                         "void kumoSurface(inout SurfaceOutputs s, in "
                                         "SurfaceInputs i) {} //KUMO_SURFACE_FUNCTION\n"
                                         "// tail\n";

struct SurfaceFixture : Fixture {
    FakeShader shader;
    std::vector<SurfaceParamSpec> installed;
    std::vector<std::pair<std::string, std::vector<float>>> paramSets;
    bool paramSetResult = true;

    SurfaceFixture() {
        std::ofstream(dir / "surface_template.frag") << kSurfaceTemplate;
        std::filesystem::create_directories(dir / "recipes");
        std::ofstream(dir / "recipes" / "glow.frag")
            << "void kumoSurface(inout SurfaceOutputs s, in SurfaceInputs i) {\n"
               "    s.emissive = material.glowColor.rgb * material.intensity;\n"
               "}\n";
        std::ofstream(dir / "recipes" / "glow.json") << R"({"description":"test glow","params":[
{"name":"intensity","type":"float","value":2.0,"min":0,"max":10},
{"name":"glowColor","type":"vec4","value":[1.0,0.5,0.0,1.0]}],
"tags":["emissive"],"cost":"low"})";

        ShaderToolContext context;
        context.setShader = std::ref(shader);
        context.surfaceTemplatePath = dir / "surface_template.frag";
        context.recipesDir = dir / "recipes";
        context.setSurfaceParams = [this](std::uint32_t,
                                          const std::vector<SurfaceParamSpec>& specs) {
            installed = specs;
            return true;
        };
        context.setSurfaceParam = [this](std::uint32_t, const std::string& name,
                                         const std::vector<float>& values) {
            paramSets.emplace_back(name, values);
            return paramSetResult;
        };
        context.surfaceParams = [this](std::uint32_t) { return installed; };
        registerTools(std::move(context));
    }
};

} // namespace

TEST_CASE("surface_write splices the function, installs params and saves the full source") {
    SurfaceFixture f;
    const scene::EntityId id = f.addEntity(3);
    const json result = f.invoke(
        "surface_write",
        std::format(
            R"({{"entity_id":"{}","function":"void kumoSurface(inout SurfaceOutputs s, in SurfaceInputs i) {{ s.albedo = material.tint.rgb; }}","params":[{{"name":"amount","type":"float","value":0.5}},{{"name":"tint","type":"vec4","value":[1,0,0,1]}}]}})",
            formatEntityId(id)));
    REQUIRE(result["status"] == "ok");

    REQUIRE(f.shader.calls.size() == 1);
    const std::string& source = f.shader.calls[0].second;
    CHECK(source.find("float amount;") != std::string::npos);
    CHECK(source.find("vec4 tint;") != std::string::npos);
    CHECK(source.find("s.albedo = material.tint.rgb;") != std::string::npos);
    CHECK(source.find("//KUMO_SURFACE") == std::string::npos);

    REQUIRE(f.installed.size() == 2);
    CHECK(f.installed[0].name == "amount");
    CHECK(f.installed[0].offset == 64);
    CHECK(f.installed[1].name == "tint");
    CHECK(f.installed[1].offset == 80);
    CHECK(f.installed[1].value[0] == doctest::Approx(1.0f));
    CHECK(std::filesystem::exists(f.generatedDir / "material_3.frag"));
}

TEST_CASE("surface_write rejects forbidden constructs pointing at shader_write_full") {
    SurfaceFixture f;
    const scene::EntityId id = f.addEntity(0);
    const json result = f.invoke(
        "surface_write",
        std::format(
            R"({{"entity_id":"{}","function":"void kumoSurface(inout SurfaceOutputs s, in SurfaceInputs i) {{ while(true) {{}} }}"}})",
            formatEntityId(id)));
    CHECK(result["status"] == "error");
    CHECK(result["message"].get<std::string>().find("shader_write_full") != std::string::npos);
    CHECK(f.shader.calls.empty());
}

TEST_CASE("surface_write maps compile errors onto function lines") {
    SurfaceFixture f;
    f.shader.fail = true;
    // FakeShader reports line 42: outside the two-line function body.
    const scene::EntityId id = f.addEntity(0);
    const json result = f.invoke(
        "surface_write",
        std::format(
            R"({{"entity_id":"{}","function":"void kumoSurface(inout SurfaceOutputs s, in SurfaceInputs i) {{\n    s.albedo = vec3(0.5);\n}}"}})",
            formatEntityId(id)));
    CHECK(result["status"] == "error");
    REQUIRE(result["errors"].size() == 1);
    CHECK(result["errors"][0]["template_error"] == true);
}

TEST_CASE("shader_set_param forwards values and lists available names on a miss") {
    SurfaceFixture f;
    f.installed = {{.name = "glow", .isVec4 = false, .offset = 64}};
    const scene::EntityId id = f.addEntity(0);

    const json ok = f.invoke(
        "shader_set_param",
        std::format(R"({{"entity_id":"{}","name":"glow","value":2.5}})", formatEntityId(id)));
    REQUIRE(ok["status"] == "ok");
    REQUIRE(f.paramSets.size() == 1);
    CHECK(f.paramSets[0].first == "glow");
    CHECK(f.paramSets[0].second == std::vector<float>{2.5f});

    f.paramSetResult = false;
    const json miss =
        f.invoke("shader_set_param", std::format(R"({{"entity_id":"{}","name":"nope","value":1}})",
                                                 formatEntityId(id)));
    CHECK(miss["status"] == "error");
    CHECK(miss["message"].get<std::string>().find("glow") != std::string::npos);
}

TEST_CASE("recipe_list returns metadata without sources; shader_apply_recipe splices and "
          "overrides") {
    SurfaceFixture f;
    const json list = f.invoke("recipe_list", "{}");
    REQUIRE(list["status"] == "ok");
    REQUIRE(list["recipes"].size() == 1);
    CHECK(list["recipes"][0]["name"] == "glow");
    CHECK(list["recipes"][0]["description"] == "test glow");
    CHECK(list.dump().find("kumoSurface") == std::string::npos);

    const scene::EntityId id = f.addEntity(1);
    const json applied =
        f.invoke("shader_apply_recipe",
                 std::format(R"({{"entity_id":"{}","name":"glow","params":{{"intensity":5.0}}}})",
                             formatEntityId(id)));
    REQUIRE(applied["status"] == "ok");
    REQUIRE(f.shader.calls.size() == 1);
    CHECK(f.shader.calls[0].second.find("material.glowColor.rgb") != std::string::npos);
    REQUIRE(f.installed.size() == 2);
    CHECK(f.installed[0].name == "intensity");
    CHECK(f.installed[0].value[0] == doctest::Approx(5.0f)); // override beat the default 2.0
    CHECK(f.installed[1].name == "glowColor");
    CHECK(f.installed[1].value[1] == doctest::Approx(0.5f));

    const json unknown =
        f.invoke("shader_apply_recipe",
                 std::format(R"({{"entity_id":"{}","name":"missing"}})", formatEntityId(id)));
    CHECK(unknown["status"] == "error");
}

TEST_CASE("shader_write_full and its deprecated shader_write alias share the handler") {
    Fixture f;
    FakeShader shader;
    ShaderToolContext context;
    context.setShader = std::ref(shader);
    f.registerTools(std::move(context));
    const scene::EntityId id = f.addEntity(0);

    for (const char* tool : {"shader_write_full", "shader_write"}) {
        const json result =
            f.invoke(tool, std::format(R"({{"entity_id":"{}","source":"void main() {{}}"}})",
                                       formatEntityId(id)));
        REQUIRE_MESSAGE(result["status"] == "ok", tool);
    }
    CHECK(shader.calls.size() == 2);
}
