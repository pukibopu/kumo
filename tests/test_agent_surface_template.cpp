#include <doctest/doctest.h>

// Private kumo_agent header (engine/agent/src), same access pattern as
// placement.h in test_agent_placement.cpp.
#include "surface_template.h"

#include <kumo/core/file.h>
#include <kumo/shaderc/compiler.h>

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace kumo;
using namespace kumo::agent;

namespace {

std::string templateText() {
    const auto text =
        readTextFile(std::filesystem::path(KUMO_SHADER_DIR) / "pbr_surface_template.frag");
    REQUIRE(text.has_value());
    return *text;
}

constexpr const char* kIdentityFunction =
    "void kumoSurface(inout SurfaceOutputs s, in SurfaceInputs i) {\n"
    "    s.albedo = vec3(1.0, 0.0, 0.0);\n"
    "}\n";

} // namespace

TEST_CASE("spliceSurface computes std140 offsets for mixed float/vec4 params") {
    const std::vector<surface::ParamDecl> params{
        {.name = "scale", .isVec4 = false, .value = {4.0f}},
        {.name = "tint", .isVec4 = true, .value = {1.0f, 0.5f, 0.25f, 1.0f}},
        {.name = "amount", .isVec4 = false, .value = {0.5f}},
        {.name = "speed", .isVec4 = false, .value = {2.0f}},
    };
    const auto spliced = surface::spliceSurface(templateText(), kIdentityFunction, params);
    REQUIRE(spliced.has_value());

    REQUIRE(spliced->layout.size() == 4);
    CHECK(spliced->layout[0].offset == 64);  // float right after the prefix
    CHECK(spliced->layout[1].offset == 80);  // vec4 aligns up to 16
    CHECK(spliced->layout[2].offset == 96);  // float packs after the vec4
    CHECK(spliced->layout[3].offset == 100); // floats pack tightly
    CHECK(spliced->dataSize == 40);

    CHECK(spliced->source.find("float scale;") != std::string::npos);
    CHECK(spliced->source.find("vec4 tint;") != std::string::npos);
    CHECK(spliced->source.find("s.albedo = vec3(1.0, 0.0, 0.0);") != std::string::npos);
    CHECK(spliced->source.find("//KUMO_SURFACE") == std::string::npos);
}

TEST_CASE("spliced source compiles and its reflected block size matches the computed layout") {
    const std::vector<surface::ParamDecl> params{
        {.name = "grainScale", .isVec4 = false, .value = {8.0f}},
        {.name = "ringColor", .isVec4 = true, .value = {0.4f, 0.25f, 0.1f, 1.0f}},
    };
    const char* function = "void kumoSurface(inout SurfaceOutputs s, in SurfaceInputs i) {\n"
                           "    float rings = fract(i.worldPos.x * material.grainScale);\n"
                           "    s.albedo = mix(s.albedo, material.ringColor.rgb, rings);\n"
                           "    s.roughness = mix(0.4, 0.9, rings);\n"
                           "}\n";
    const auto spliced = surface::spliceSurface(templateText(), function, params);
    REQUIRE(spliced.has_value());

    const auto compiled =
        shaderc::compileGlsl(spliced->source, shaderc::Stage::Fragment,
                             {.sourceName = "spliced",
                              .includeDirs = {std::filesystem::path(KUMO_SHADER_DIR) / "include"}});
    if (!compiled.has_value()) {
        FAIL(compiled.error().front().message);
    }

    bool foundFactors = false;
    for (const shaderc::ReflectionBinding& binding : compiled->reflection.bindings) {
        if (binding.name == "MaterialFactors") {
            foundFactors = true;
            CHECK(binding.bufferSize == surface::kPrefixSize + spliced->dataSize);
            REQUIRE(binding.members.size() == 6);
            CHECK(binding.members[4] == "grainScale");
            CHECK(binding.members[5] == "ringColor");
        }
    }
    CHECK(foundFactors);
}

TEST_CASE("the shipped template compiles standalone with the identity surface") {
    const auto compiled =
        shaderc::compileGlsl(templateText(), shaderc::Stage::Fragment,
                             {.sourceName = "template",
                              .includeDirs = {std::filesystem::path(KUMO_SHADER_DIR) / "include"}});
    if (!compiled.has_value()) {
        FAIL(compiled.error().front().message);
    }
}

TEST_CASE("spliceSurface rejects forbidden constructs with a pointer to shader_write_full") {
    const char* cases[] = {
        "void kumoSurface(inout SurfaceOutputs s, in SurfaceInputs i) { while (true) {} }",
        "void kumoSurface(inout SurfaceOutputs s, in SurfaceInputs i) { do {} while(false); }",
        "#include \"x\"\nvoid kumoSurface(inout SurfaceOutputs s, in SurfaceInputs i) {}",
        "#version 460\nvoid kumoSurface(inout SurfaceOutputs s, in SurfaceInputs i) {}",
        "uniform float x;\nvoid kumoSurface(inout SurfaceOutputs s, in SurfaceInputs i) {}",
        "layout(set=0) uniform X{float a;};\nvoid kumoSurface(inout SurfaceOutputs s, in "
        "SurfaceInputs i) {}",
        "void kumoSurface(inout SurfaceOutputs s, in SurfaceInputs i) { s.albedo = "
        "frame.cameraPos.xyz; }",
        "void kumoSurface(inout SurfaceOutputs s, in SurfaceInputs i) { vec2 p = gl_FragCoord.xy; "
        "}",
        "void main() {}\nvoid kumoSurface(inout SurfaceOutputs s, in SurfaceInputs i) {}",
    };
    for (const char* source : cases) {
        const auto spliced = surface::spliceSurface(templateText(), source, {});
        REQUIRE_MESSAGE(!spliced.has_value(), source);
        CHECK(spliced.error().find("shader_write_full") != std::string::npos);
    }
    // Identifiers merely containing a forbidden word stay legal.
    const auto legal = surface::spliceSurface(
        templateText(),
        "void kumoSurface(inout SurfaceOutputs s, in SurfaceInputs i) {\n"
        "    float domain = 1.0; float remainder = 0.5; s.metallic = domain - remainder;\n"
        "}\n",
        {});
    CHECK(legal.has_value());
}

TEST_CASE("spliceSurface validates the signature, param names and budget caps") {
    const std::string tmpl = templateText();
    CHECK(!surface::spliceSurface(tmpl, "vec3 helper() { return vec3(0); }", {}).has_value());

    const std::vector<surface::ParamDecl> badName{{.name = "1bad", .isVec4 = false}};
    CHECK(!surface::spliceSurface(tmpl, kIdentityFunction, badName).has_value());
    const std::vector<surface::ParamDecl> reserved{{.name = "uvTiling", .isVec4 = false}};
    CHECK(!surface::spliceSurface(tmpl, kIdentityFunction, reserved).has_value());
    const std::vector<surface::ParamDecl> dup{{.name = "x", .isVec4 = false},
                                              {.name = "x", .isVec4 = true}};
    CHECK(!surface::spliceSurface(tmpl, kIdentityFunction, dup).has_value());

    std::vector<surface::ParamDecl> tooMany(17, {.name = "p", .isVec4 = false});
    for (std::size_t i = 0; i < tooMany.size(); ++i) {
        tooMany[i].name = "p" + std::to_string(i);
    }
    CHECK(!surface::spliceSurface(tmpl, kIdentityFunction, tooMany).has_value());

    // Nine vec4s end at 64 + 144 = 208 > 192.
    std::vector<surface::ParamDecl> tooBig(9, {.name = "v", .isVec4 = true});
    for (std::size_t i = 0; i < tooBig.size(); ++i) {
        tooBig[i].name = "v" + std::to_string(i);
    }
    CHECK(!surface::spliceSurface(tmpl, kIdentityFunction, tooBig).has_value());
    // Eight vec4s end exactly at 192.
    tooBig.pop_back();
    CHECK(surface::spliceSurface(tmpl, kIdentityFunction, tooBig).has_value());
}

TEST_CASE("packDefaults writes floats and vec4s at their computed offsets") {
    const std::vector<surface::ParamDecl> params{
        {.name = "a", .isVec4 = false, .value = {1.5f}},
        {.name = "b", .isVec4 = true, .value = {0.1f, 0.2f, 0.3f, 0.4f}},
    };
    const auto spliced = surface::spliceSurface(templateText(), kIdentityFunction, params);
    REQUIRE(spliced.has_value());
    const std::vector<std::byte> data = surface::packDefaults(params, spliced->layout);
    REQUIRE(data.size() == spliced->dataSize);

    float a = 0.0f;
    std::memcpy(&a, data.data() + (spliced->layout[0].offset - surface::kPrefixSize), 4);
    CHECK(a == doctest::Approx(1.5f));
    float b[4] = {};
    std::memcpy(b, data.data() + (spliced->layout[1].offset - surface::kPrefixSize), 16);
    CHECK(b[3] == doctest::Approx(0.4f));
}

TEST_CASE("functionLine maps compile errors back onto the function text only") {
    const std::vector<surface::ParamDecl> params{{.name = "x", .isVec4 = false}};
    const char* function = "void kumoSurface(inout SurfaceOutputs s, in SurfaceInputs i) {\n"
                           "    s.albedo = vec3(0.5);\n"
                           "}\n";
    const auto spliced = surface::spliceSurface(templateText(), function, params);
    REQUIRE(spliced.has_value());

    CHECK(surface::functionLine(*spliced, spliced->functionFirstLine) == 1);
    CHECK(surface::functionLine(*spliced, spliced->functionFirstLine + 1) == 2);
    CHECK(!surface::functionLine(*spliced, spliced->functionFirstLine - 1).has_value());
    CHECK(!surface::functionLine(*spliced, spliced->functionFirstLine + 3).has_value());

    // A real compile error inside the function lands on a mappable line.
    const char* broken = "void kumoSurface(inout SurfaceOutputs s, in SurfaceInputs i) {\n"
                         "    s.albedo = badIdentifier;\n"
                         "}\n";
    const auto brokenSpliced = surface::spliceSurface(templateText(), broken, params);
    REQUIRE(brokenSpliced.has_value());
    const auto compiled =
        shaderc::compileGlsl(brokenSpliced->source, shaderc::Stage::Fragment,
                             {.sourceName = "spliced",
                              .includeDirs = {std::filesystem::path(KUMO_SHADER_DIR) / "include"}});
    REQUIRE(!compiled.has_value());
    bool mapped = false;
    for (const shaderc::CompileError& error : compiled.error()) {
        if (surface::functionLine(*brokenSpliced, error.line) == 2) {
            mapped = true;
        }
    }
    CHECK(mapped);
}
