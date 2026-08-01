#include <doctest/doctest.h>

// Private kumo_renderer header (engine/renderer/src), not a public interface;
// reached into here to unit-test detail::setMismatch directly (ADR 0011/0029).
#include "shader_load.h"

#include <kumo/core/file.h>
#include <kumo/shaderc/compiler.h>

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

using namespace kumo;

namespace {

shaderc::Reflection makeSharedReflection() {
    shaderc::Reflection reflection;
    reflection.bindings = {
        {.set = 0,
         .binding = 0,
         .type = "uniform_buffer",
         .name = "FrameUniforms",
         .bufferSize = 64},
        {.set = 1,
         .binding = 0,
         .type = "sampled_texture",
         .name = "baseColorTex",
         .bufferSize = 0},
        {.set = 1, .binding = 5, .type = "sampler", .name = "materialSampler", .bufferSize = 0},
        {.set = 1,
         .binding = 6,
         .type = "uniform_buffer",
         .name = "MaterialFactors",
         .bufferSize = 64},
    };
    return reflection;
}

bool contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

std::string replaceOnce(std::string source, const std::string& anchor,
                        const std::string& replacement) {
    const std::size_t pos = source.find(anchor);
    REQUIRE(pos != std::string::npos);
    source.replace(pos, anchor.size(), replacement);
    return source;
}

} // namespace

TEST_CASE("setMismatch: identical sets are compatible") {
    const shaderc::Reflection shared = makeSharedReflection();
    const shaderc::Reflection custom = makeSharedReflection();
    CHECK(!renderer::detail::setMismatch(custom, shared, 0).has_value());
    CHECK(!renderer::detail::setMismatch(custom, shared, 1).has_value());
}

TEST_CASE("setMismatch: changed binding type is a mismatch") {
    const shaderc::Reflection shared = makeSharedReflection();
    shaderc::Reflection custom = makeSharedReflection();
    // set 1 binding 0 flips from a sampled_texture to a uniform_buffer.
    custom.bindings[1].type = "uniform_buffer";

    const auto mismatch = renderer::detail::setMismatch(custom, shared, 1);
    REQUIRE(mismatch.has_value());
    CHECK(contains(*mismatch, "set 1"));
    CHECK(contains(*mismatch, "binding 0"));
}

TEST_CASE("setMismatch: an added binding is a mismatch") {
    const shaderc::Reflection shared = makeSharedReflection();
    shaderc::Reflection custom = makeSharedReflection();
    custom.bindings.push_back(
        {.set = 0, .binding = 1, .type = "sampled_texture", .name = "extra", .bufferSize = 0});

    const auto mismatch = renderer::detail::setMismatch(custom, shared, 0);
    REQUIRE(mismatch.has_value());
    CHECK(contains(*mismatch, "set 0"));
    CHECK(contains(*mismatch, "binding 1"));
}

TEST_CASE("setMismatch: a removed binding is a mismatch") {
    const shaderc::Reflection shared = makeSharedReflection();
    shaderc::Reflection custom = makeSharedReflection();
    // Drop set 1 binding 5 (materialSampler).
    std::erase_if(custom.bindings, [](const shaderc::ReflectionBinding& binding) {
        return binding.set == 1 && binding.binding == 5;
    });

    const auto mismatch = renderer::detail::setMismatch(custom, shared, 1);
    REQUIRE(mismatch.has_value());
    CHECK(contains(*mismatch, "set 1"));
    CHECK(contains(*mismatch, "binding 5"));
}

TEST_CASE("setMismatch: a changed uniform buffer size is a mismatch") {
    const shaderc::Reflection shared = makeSharedReflection();
    shaderc::Reflection custom = makeSharedReflection();
    // set 1 binding 6 is MaterialFactors; grow it past its shared declared size.
    custom.bindings[3].bufferSize = 80;

    const auto mismatch = renderer::detail::setMismatch(custom, shared, 1);
    REQUIRE(mismatch.has_value());
    CHECK(contains(*mismatch, "set 1"));
    CHECK(contains(*mismatch, "binding 6"));
}

namespace {

// Real pbr.frag compiled with shaderc, exercising setMismatch against actual
// reflection output rather than hand-built fixtures.
shaderc::CompileResult compilePbrFragVariant(const std::string& source, const char* sourceName) {
    return shaderc::compileGlsl(
        source, shaderc::Stage::Fragment,
        {.sourceName = sourceName,
         .includeDirs = {(std::filesystem::path(KUMO_SHADER_DIR) / "include").string()}});
}

} // namespace

TEST_CASE("setMismatch: a custom fragment that adds a set 0 binding is rejected") {
    const auto pbrFragPath = std::filesystem::path(KUMO_SHADER_DIR) / "pbr.frag";
    const auto source = readTextFile(pbrFragPath);
    REQUIRE(source.has_value());

    const auto shared = compilePbrFragVariant(*source, "pbr.frag");
    REQUIRE(shared.has_value());

    const std::string variantSource =
        replaceOnce(*source, "#include \"common.glsl\"\n",
                    "#include \"common.glsl\"\n"
                    "layout(set = 0, binding = 4) uniform texture2D extraDebugTex;\n");
    const auto variant = compilePbrFragVariant(variantSource, "variant.frag");
    REQUIRE(variant.has_value());

    const auto mismatch = renderer::detail::setMismatch(variant->reflection, shared->reflection, 0);
    REQUIRE(mismatch.has_value());
    CHECK(contains(*mismatch, "set 0"));
    CHECK(contains(*mismatch, "binding 4"));

    // set 2 is untouched by this edit.
    CHECK(!renderer::detail::setMismatch(variant->reflection, shared->reflection, 2).has_value());
}

TEST_CASE("setMismatch: a material fragment with common.glsl but not shadow.glsl stays set 0/2 "
          "compatible") {
    const auto pbrFragPath = std::filesystem::path(KUMO_SHADER_DIR) / "pbr.frag";
    const auto source = readTextFile(pbrFragPath);
    REQUIRE(source.has_value());

    const auto shared = compilePbrFragVariant(*source, "pbr.frag");
    REQUIRE(shared.has_value());

    // shadowMap/shadowSampler live in common.glsl, not shadow.glsl (ADR 0009);
    // dropping the include (and the one call site) must not change set 0/2.
    std::string variantSource = replaceOnce(*source, "#include \"shadow.glsl\"\n", "");
    variantSource = replaceOnce(
        variantSource,
        "float shadow = i == int(frame.shadowParams.w) ? kumoShadowPcf(vWorldPos, N) : 1.0;",
        "float shadow = 1.0;");
    const auto variant = compilePbrFragVariant(variantSource, "no_shadow.frag");
    REQUIRE(variant.has_value());

    CHECK(!renderer::detail::setMismatch(variant->reflection, shared->reflection, 0).has_value());
    CHECK(!renderer::detail::setMismatch(variant->reflection, shared->reflection, 2).has_value());
}

TEST_CASE("setMismatch: adding a MaterialFactors member stays set 0/2 compatible") {
    const auto pbrFragPath = std::filesystem::path(KUMO_SHADER_DIR) / "pbr.frag";
    const auto source = readTextFile(pbrFragPath);
    REQUIRE(source.has_value());

    const auto shared = compilePbrFragVariant(*source, "pbr.frag");
    REQUIRE(shared.has_value());

    const std::string variantSource =
        replaceOnce(*source, "vec4 uvTiling;          // xy uv scale, zw reserved\n}",
                    "vec4 uvTiling;          // xy uv scale, zw reserved\n    vec4 extra;\n}");
    const auto variant = compilePbrFragVariant(variantSource, "variant.frag");
    REQUIRE(variant.has_value());

    CHECK(!renderer::detail::setMismatch(variant->reflection, shared->reflection, 0).has_value());
    CHECK(!renderer::detail::setMismatch(variant->reflection, shared->reflection, 2).has_value());

    const shaderc::ReflectionBinding* sharedFactors = nullptr;
    for (const shaderc::ReflectionBinding& binding : shared->reflection.bindings) {
        if (binding.set == 1 && binding.type == "uniform_buffer") {
            sharedFactors = &binding;
        }
    }
    const shaderc::ReflectionBinding* variantFactors = nullptr;
    for (const shaderc::ReflectionBinding& binding : variant->reflection.bindings) {
        if (binding.set == 1 && binding.type == "uniform_buffer") {
            variantFactors = &binding;
        }
    }
    REQUIRE(sharedFactors != nullptr);
    REQUIRE(variantFactors != nullptr);
    CHECK(sharedFactors->bufferSize == 64);
    CHECK(variantFactors->bufferSize > 64);
}

TEST_CASE("factorPrefixSize: uvTiling as the 4th member selects the full 64B") {
    shaderc::ReflectionBinding factors;
    factors.bufferSize = 64;
    factors.members = {"baseColor", "metallicRoughness", "emissive", "uvTiling"};
    CHECK(renderer::detail::factorPrefixSize(factors) == 64);
}

TEST_CASE("factorPrefixSize: a different 4th member selects the 48B prefix") {
    // Same bufferSize as the uvTiling block under std140 padding, but the 4th
    // member is the shader author's own, not uvTiling.
    shaderc::ReflectionBinding factors;
    factors.bufferSize = 64;
    factors.members = {"baseColor", "metallicRoughness", "emissive", "highlightOffset"};
    CHECK(renderer::detail::factorPrefixSize(factors) == 48);
}

TEST_CASE("factorPrefixSize: fewer than 4 members defensively selects the 48B prefix") {
    shaderc::ReflectionBinding factors;
    factors.bufferSize = 48;
    factors.members = {"baseColor", "metallicRoughness", "emissive"};
    CHECK(renderer::detail::factorPrefixSize(factors) == 48);
}

TEST_CASE("factorPrefixSize: no members at all defensively selects the 48B prefix") {
    shaderc::ReflectionBinding factors;
    factors.bufferSize = 64;
    CHECK(renderer::detail::factorPrefixSize(factors) == 48);
}

TEST_CASE("factorPrefixSize: result is clamped to a smaller declared bufferSize") {
    shaderc::ReflectionBinding factors;
    factors.bufferSize = 32;
    factors.members = {"baseColor", "metallicRoughness", "emissive", "uvTiling"};
    CHECK(renderer::detail::factorPrefixSize(factors) == 32);
}

TEST_CASE("factorPrefixSize: shared pbr.frag's MaterialFactors chooses the full 64B write") {
    const auto pbrFragPath = std::filesystem::path(KUMO_SHADER_DIR) / "pbr.frag";
    const auto source = readTextFile(pbrFragPath);
    REQUIRE(source.has_value());

    const auto shared = compilePbrFragVariant(*source, "pbr.frag");
    REQUIRE(shared.has_value());

    const shaderc::ReflectionBinding* factors = nullptr;
    for (const shaderc::ReflectionBinding& binding : shared->reflection.bindings) {
        if (binding.set == 1 && binding.type == "uniform_buffer") {
            factors = &binding;
        }
    }
    REQUIRE(factors != nullptr);
    CHECK(renderer::detail::factorPrefixSize(*factors) == 64);
}

TEST_CASE("factorPrefixSize: a pre-uvTiling custom member is not mistaken for uvTiling") {
    const auto pbrFragPath = std::filesystem::path(KUMO_SHADER_DIR) / "pbr.frag";
    const auto source = readTextFile(pbrFragPath);
    REQUIRE(source.has_value());

    // Mirrors a real pre-M6.98 custom shader: baseColor/metallicRoughness/
    // emissive plus its own 4th member of the same vec4 shape, just not named
    // uvTiling. Byte size is identical to the current block either way — the
    // write size can only be decided by member name.
    std::string variantSource =
        replaceOnce(*source, "vec4 uvTiling;          // xy uv scale, zw reserved\n}",
                    "vec4 highlightOffset;\n}");
    variantSource = replaceOnce(variantSource, "material.uvTiling.xy", "vec2(1.0)");
    const auto variant = compilePbrFragVariant(variantSource, "variant.frag");
    REQUIRE(variant.has_value());

    const shaderc::ReflectionBinding* factors = nullptr;
    for (const shaderc::ReflectionBinding& binding : variant->reflection.bindings) {
        if (binding.set == 1 && binding.type == "uniform_buffer") {
            factors = &binding;
        }
    }
    REQUIRE(factors != nullptr);
    REQUIRE(factors->bufferSize == 64); // same size as the real uvTiling block (std140 padding)
    CHECK(renderer::detail::factorPrefixSize(*factors) == 48);
}

TEST_CASE("sourceReferencesTime: absent, present and inside a comment") {
    CHECK(!renderer::detail::sourceReferencesTime("void main() { fragColor = vec4(1.0); }"));
    CHECK(renderer::detail::sourceReferencesTime("void main() { float t = frame.timeParams.x; }"));
    // A substring match is deliberate: a mention in a comment still counts, and
    // the only cost of a false positive is a spurious redraw.
    CHECK(renderer::detail::sourceReferencesTime("// uses timeParams for the flicker\n"));
}
