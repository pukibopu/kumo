#include <doctest/doctest.h>

#include <kumo/shaderc/compiler.h>

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
    CHECK(contains(result->msl, "[[sampler(9)]]"));
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
