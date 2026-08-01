#include <doctest/doctest.h>

#include <kumo/core/file.h>
#include <kumo/shaderc/compiler.h>

#include <algorithm>
#include <filesystem>
#include <string>

using namespace kumo;

namespace {

constexpr const char* kMinimalVertex = R"(#version 460
void main() { gl_Position = vec4(0.0); }
)";

constexpr const char* kMinimalFragment = R"(#version 460
layout(location = 0) out vec4 color;
void main() { color = vec4(1.0); }
)";

bool contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

} // namespace

TEST_CASE("valid vertex and fragment compile to spirv and msl") {
    const auto vert = shaderc::compileGlsl(kMinimalVertex, shaderc::Stage::Vertex);
    REQUIRE(vert.has_value());
    CHECK(!vert->spirv.empty());
    CHECK(contains(vert->msl, "main0"));
    CHECK(vert->mslEntryPoint == "main0");

    const auto frag = shaderc::compileGlsl(kMinimalFragment, shaderc::Stage::Fragment);
    REQUIRE(frag.has_value());
    CHECK(!frag->spirv.empty());
    CHECK(contains(frag->msl, "main0"));
    CHECK(frag->mslEntryPoint == "main0");
}

TEST_CASE("separate texture and sampler reflect and remap") {
    constexpr const char* source = R"(#version 460
layout(set = 1, binding = 0) uniform texture2D t;
layout(set = 1, binding = 1) uniform sampler s;
layout(location = 0) out vec4 color;
void main() { color = texture(sampler2D(t, s), vec2(0.0)); }
)";
    const auto result = shaderc::compileGlsl(source, shaderc::Stage::Fragment);
    REQUIRE(result.has_value());

    REQUIRE(result->reflection.bindings.size() == 2);
    const auto& first = result->reflection.bindings[0];
    const auto& second = result->reflection.bindings[1];
    CHECK(first.set == 1);
    CHECK(first.binding == 0);
    CHECK(first.type == "sampled_texture");
    CHECK(second.set == 1);
    CHECK(second.binding == 1);
    CHECK(second.type == "sampler");

    CHECK(contains(result->msl, "[[texture(8)]]"));
    // Samplers use the denser stride-6 table (Metal has 16 sampler slots).
    CHECK(contains(result->msl, "[[sampler(7)]]"));
}

TEST_CASE("shadow sampler reflects as a sampled_texture + sampler and emits sample_compare") {
    constexpr const char* source = R"(#version 460
layout(set = 0, binding = 1) uniform texture2D shadowMap;
layout(set = 0, binding = 2) uniform samplerShadow shadowSampler;
layout(location = 0) out vec4 color;
void main() {
    color = vec4(texture(sampler2DShadow(shadowMap, shadowSampler), vec3(0.5, 0.5, 0.5)));
}
)";
    const auto result = shaderc::compileGlsl(source, shaderc::Stage::Fragment);
    REQUIRE(result.has_value());

    REQUIRE(result->reflection.bindings.size() == 2);
    const auto& texBinding = result->reflection.bindings[0];
    const auto& samplerBinding = result->reflection.bindings[1];
    CHECK(texBinding.set == 0);
    CHECK(texBinding.binding == 1);
    CHECK(texBinding.type == "sampled_texture");
    CHECK(samplerBinding.set == 0);
    CHECK(samplerBinding.binding == 2);
    CHECK(samplerBinding.type == "sampler");

    CHECK(contains(result->msl, "sample_compare"));
}

TEST_CASE("push constant reflects size and remaps to buffer 24") {
    constexpr const char* source = R"(#version 460
layout(push_constant) uniform Pc { mat4 m; } pc;
layout(location = 0) out vec4 color;
void main() { color = pc.m[0]; }
)";
    const auto result = shaderc::compileGlsl(source, shaderc::Stage::Fragment);
    REQUIRE(result.has_value());

    CHECK(result->reflection.pushConstantSize == 64);
    CHECK(contains(result->msl, "[[buffer(24)]]"));
}

TEST_CASE("syntax error yields structured error list") {
    constexpr const char* source = R"(#version 460
void main() { gl_Position = vec4(0.0) }
)";
    const auto result = shaderc::compileGlsl(source, shaderc::Stage::Vertex);
    REQUIRE(!result.has_value());
    REQUIRE(result.error().size() == 1); // summary line must not be counted as an error
    CHECK(result.error().front().line > 0);
    CHECK(!result.error().front().message.empty());
}

TEST_CASE("binding out of range fails validation") {
    constexpr const char* source = R"(#version 460
layout(set = 3, binding = 0) uniform Data { vec4 v; } data;
layout(location = 0) out vec4 color;
void main() { color = data.v; }
)";
    const auto result = shaderc::compileGlsl(source, shaderc::Stage::Fragment);
    REQUIRE(!result.has_value());
    REQUIRE(!result.error().empty());
    CHECK(result.error().front().secondStage);
    CHECK(contains(result.error().front().message, "set=3"));
}

TEST_CASE("compute kernel reflects workgroup size and remaps a storage image") {
    constexpr const char* source = R"(#version 460
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(set = 0, binding = 1, rgba16f) uniform writeonly image2D outImage;
void main() { imageStore(outImage, ivec2(gl_GlobalInvocationID.xy), vec4(1.0)); }
)";
    const auto result = shaderc::compileGlsl(source, shaderc::Stage::Compute);
    REQUIRE(result.has_value());
    CHECK(!result->spirv.empty());
    CHECK(!result->msl.empty());

    CHECK(result->reflection.workgroupSize[0] == 8);
    CHECK(result->reflection.workgroupSize[1] == 8);
    CHECK(result->reflection.workgroupSize[2] == 1);

    REQUIRE(result->reflection.bindings.size() == 1);
    CHECK(result->reflection.bindings[0].set == 0);
    CHECK(result->reflection.bindings[0].binding == 1);
    CHECK(result->reflection.bindings[0].type == "storage_texture");

    // set 0 * 8 + binding 1 == flat index 1.
    CHECK(contains(result->msl, "[[texture(1)]]"));
}

TEST_CASE("vertex stage reflects input locations") {
    constexpr const char* source = R"(#version 460
layout(location = 0) in vec3 pos;
layout(location = 1) in vec3 normal;
void main() { gl_Position = vec4(pos + normal, 1.0); }
)";
    const auto result = shaderc::compileGlsl(source, shaderc::Stage::Vertex);
    REQUIRE(result.has_value());

    REQUIRE(result->reflection.vertexInputLocations.size() == 2);
    CHECK(result->reflection.vertexInputLocations[0] == 0);
    CHECK(result->reflection.vertexInputLocations[1] == 1);
}

TEST_CASE("pbr.frag reflects the MaterialFactors uniform buffer size") {
    const std::filesystem::path path = std::filesystem::path(KUMO_SHADER_DIR) / "pbr.frag";
    auto source = readTextFile(path);
    REQUIRE_MESSAGE(source.has_value(), source.error());

    const auto result = shaderc::compileGlsl(
        *source, shaderc::Stage::Fragment,
        {.sourceName = "pbr.frag",
         .includeDirs = {(std::filesystem::path(KUMO_SHADER_DIR) / "include").string()}});
    REQUIRE(result.has_value());

    const auto& bindings = result->reflection.bindings;
    const auto materialFactors =
        std::find_if(bindings.begin(), bindings.end(), [](const shaderc::ReflectionBinding& b) {
            return b.set == 1 && b.binding == 6;
        });
    REQUIRE(materialFactors != bindings.end());
    CHECK(materialFactors->type == "uniform_buffer");
    // std140: 3 vec4 members (baseColor, metallicRoughness, emissive), 16 bytes each.
    CHECK(materialFactors->bufferSize == 48);

    const auto baseColorTex =
        std::find_if(bindings.begin(), bindings.end(), [](const shaderc::ReflectionBinding& b) {
            return b.set == 1 && b.binding == 0;
        });
    REQUIRE(baseColorTex != bindings.end());
    CHECK(baseColorTex->type == "sampled_texture");
    CHECK(baseColorTex->bufferSize == 0);
}
