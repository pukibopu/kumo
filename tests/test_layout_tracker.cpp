#include <doctest/doctest.h>

#include <kumo/rhi_vulkan/layout_tracker.h>

using namespace kumo::rhi::vulkan;

TEST_CASE("layout tracker reports transitions from the previous layout") {
    LayoutTracker tracker;

    auto toColor = tracker.request(1, ImageLayoutState::ColorAttachment);
    REQUIRE(toColor.has_value());
    CHECK(toColor->oldLayout == ImageLayoutState::Undefined);
    CHECK(toColor->newLayout == ImageLayoutState::ColorAttachment);

    auto toPresent = tracker.request(1, ImageLayoutState::PresentSrc);
    REQUIRE(toPresent.has_value());
    CHECK(toPresent->oldLayout == ImageLayoutState::ColorAttachment);
    CHECK(toPresent->newLayout == ImageLayoutState::PresentSrc);

    // Re-acquiring the same image next frame transitions from PresentSrc again.
    auto backToColor = tracker.request(1, ImageLayoutState::ColorAttachment);
    REQUIRE(backToColor.has_value());
    CHECK(backToColor->oldLayout == ImageLayoutState::PresentSrc);
}

TEST_CASE("layout tracker collapses redundant requests") {
    LayoutTracker tracker;

    CHECK(tracker.request(7, ImageLayoutState::ColorAttachment).has_value());
    CHECK_FALSE(tracker.request(7, ImageLayoutState::ColorAttachment).has_value());
    CHECK_FALSE(tracker.request(7, ImageLayoutState::ColorAttachment).has_value());
    CHECK(tracker.request(7, ImageLayoutState::ShaderReadOnly).has_value());
}

TEST_CASE("layout tracker keeps per-image state independent") {
    LayoutTracker tracker;

    CHECK(tracker.request(1, ImageLayoutState::TransferDst).has_value());
    // A different image starts fresh at Undefined.
    auto other = tracker.request(2, ImageLayoutState::ColorAttachment);
    REQUIRE(other.has_value());
    CHECK(other->oldLayout == ImageLayoutState::Undefined);

    // First image retains its own state.
    auto first = tracker.request(1, ImageLayoutState::ShaderReadOnly);
    REQUIRE(first.has_value());
    CHECK(first->oldLayout == ImageLayoutState::TransferDst);
}

TEST_CASE("layout tracker forgets images on reset") {
    LayoutTracker tracker;

    CHECK(tracker.request(3, ImageLayoutState::ColorAttachment).has_value());
    tracker.reset();
    auto afterReset = tracker.request(3, ImageLayoutState::ColorAttachment);
    REQUIRE(afterReset.has_value());
    CHECK(afterReset->oldLayout == ImageLayoutState::Undefined);
}
