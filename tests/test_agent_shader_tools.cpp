#include <doctest/doctest.h>

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
