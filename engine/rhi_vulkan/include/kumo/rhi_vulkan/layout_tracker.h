#pragma once

#include <cstdint>
#include <optional>
#include <unordered_map>

namespace kumo::rhi::vulkan {

enum class ImageLayoutState {
    Undefined,
    ColorAttachment,
    DepthAttachment,
    ShaderReadOnly,
    TransferDst,
    PresentSrc,
};

struct LayoutTransition {
    ImageLayoutState oldLayout;
    ImageLayoutState newLayout;
};

// Per-image layout bookkeeping. Images start Undefined; request returns the
// transition needed to reach the desired layout, or nullopt when already there.
class LayoutTracker {
public:
    std::optional<LayoutTransition> request(std::uint64_t imageId, ImageLayoutState desired) {
        ImageLayoutState current = ImageLayoutState::Undefined;
        auto it = states_.find(imageId);
        if (it != states_.end()) {
            current = it->second;
        }
        if (current == desired) {
            return std::nullopt;
        }
        states_[imageId] = desired;
        return LayoutTransition{current, desired};
    }

    void forget(std::uint64_t imageId) { states_.erase(imageId); }
    void reset() { states_.clear(); }

private:
    std::unordered_map<std::uint64_t, ImageLayoutState> states_;
};

} // namespace kumo::rhi::vulkan
