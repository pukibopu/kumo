#pragma once

#include <kumo/scene/transform.h>

#include <cstdint>
#include <string>

namespace kumo::scene {

struct Entity {
    std::string name;
    Transform transform;
    std::int32_t meshIndex = -1;
    std::int32_t materialIndex = -1;
};

} // namespace kumo::scene
