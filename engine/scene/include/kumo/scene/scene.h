#pragma once

#include <kumo/scene/camera.h>
#include <kumo/scene/entity.h>
#include <kumo/scene/light.h>
#include <kumo/scene/slot_map.h>

#include <cstddef>
#include <span>
#include <vector>

namespace kumo::scene {

class Scene {
public:
    static constexpr std::size_t kMaxLights = 16;

    SlotMap<Entity> entities;
    Camera camera;

    // Returns false when the fixed light budget (ADR 0026) is exhausted; the cap
    // mirrors the GPU-side fixed array, so lights_ stays private to enforce it.
    bool addLight(const Light& light) {
        if (lights_.size() >= kMaxLights) {
            return false;
        }
        lights_.push_back(light);
        return true;
    }

    std::span<const Light> lights() const { return lights_; }
    Light* light(std::size_t index) { return index < lights_.size() ? &lights_[index] : nullptr; }
    void clearLights() { lights_.clear(); }

private:
    std::vector<Light> lights_;
};

} // namespace kumo::scene
