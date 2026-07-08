#pragma once

#include <kumo/scene/camera.h>
#include <kumo/scene/entity.h>
#include <kumo/scene/light.h>
#include <kumo/scene/slot_map.h>

#include <cstddef>
#include <vector>

namespace kumo::scene {

class Scene {
public:
    static constexpr std::size_t kMaxLights = 16;

    SlotMap<Entity> entities;
    std::vector<Light> lights;
    Camera camera;

    // Returns false when the fixed light budget (ADR 0026) is exhausted.
    bool addLight(const Light& light);
};

} // namespace kumo::scene
