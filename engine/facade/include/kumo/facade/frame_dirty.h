#pragma once
#include <cstdint>

namespace kumo::facade {

// Render-on-demand bookkeeping: a change requests `slots` frames so every
// per-frame-slot resource (double-buffered uniforms) gets rewritten before the
// app goes idle again.
class FrameDirty {
public:
    explicit FrameDirty(std::uint32_t slots) : slots_(slots), pending_(slots) {}
    void mark() { pending_ = slots_; }
    // True while a frame should render; decrements one pending frame.
    bool consume() {
        if (pending_ == 0) {
            return false;
        }
        --pending_;
        return true;
    }

private:
    std::uint32_t slots_;
    std::uint32_t pending_;
};

} // namespace kumo::facade
