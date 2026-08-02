#pragma once

#include <kumo/scene/camera.h>
#include <kumo/scene/entity.h>
#include <kumo/scene/light.h>
#include <kumo/scene/slot_map.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace kumo::scene {

class Scene {
public:
    static constexpr std::size_t kMaxLights = 16;

    SlotMap<Entity> entities;
    Camera camera;

    // Next Entity::assemblyId to hand out (MP); monotonic within a session.
    // Plain member so it travels with Scene copies: undo snapshots restore it
    // together with the entities, and scene load bumps it past the loaded
    // maximum so new assemblies never collide with restored ones.
    std::int32_t nextAssemblyId = 1;

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

    // False when index is out of range; remaining lights shift down to fill
    // the gap (the light array has no stable ids, unlike SlotMap entities).
    bool removeLight(std::size_t index) {
        if (index >= lights_.size()) {
            return false;
        }
        lights_.erase(lights_.begin() + static_cast<std::ptrdiff_t>(index));
        return true;
    }

private:
    std::vector<Light> lights_;
};

} // namespace kumo::scene
