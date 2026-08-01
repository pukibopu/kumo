#include <doctest/doctest.h>

#include <kumo/shaderabi/metal_binding.h>

using namespace kumo::shaderabi::metal;

TEST_CASE("metal binding map keeps argument table regions disjoint") {
    CHECK(resourceIndex(0, 0) == 0);
    CHECK(resourceIndex(1, 0) == 8);
    CHECK(resourceIndex(1, 1) == 9);
    CHECK(resourceIndex(2, kMaxBindingsPerSet - 1) == 23);

    CHECK(resourceIndex(kMaxBindGroups - 1, kMaxBindingsPerSet - 1) < kPushConstantBufferIndex);

    CHECK(vertexBufferIndex(0) == 30);
    CHECK(vertexBufferIndex(kMaxVertexBufferSlots - 1) > kPushConstantBufferIndex);
}

TEST_CASE("metal sampler table fits the 16-slot hardware limit") {
    CHECK(samplerIndex(0, 1) == 1);
    CHECK(samplerIndex(1, 5) == 11);
    CHECK(samplerIndex(2, 3) == 15);
    CHECK(samplerIndex(kMaxBindGroups - 1, kSamplerTableStride - 1) > kMaxSamplerIndex);
}
