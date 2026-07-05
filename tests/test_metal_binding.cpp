#include <doctest/doctest.h>

#include <kumo/rhi_metal/binding_map.h>

using namespace kumo::rhi::metal;

TEST_CASE("metal binding map keeps argument table regions disjoint") {
    CHECK(resourceIndex(0, 0) == 0);
    CHECK(resourceIndex(1, 0) == 8);
    CHECK(resourceIndex(1, 1) == 9);
    CHECK(resourceIndex(2, kMaxBindingsPerSet - 1) == 23);

    CHECK(resourceIndex(kMaxBindGroups - 1, kMaxBindingsPerSet - 1) < kPushConstantBufferIndex);

    CHECK(vertexBufferIndex(0) == 30);
    CHECK(vertexBufferIndex(kMaxVertexBufferSlots - 1) > kPushConstantBufferIndex);
}
