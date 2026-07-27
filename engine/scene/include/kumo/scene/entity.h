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

    // Save/load provenance: non-empty means the mesh is a rebuildable procedural
    // primitive ("sphere"/"cube"/"plane"); empty means it came from the glTF scene.
    std::string primitive;
    float primitiveSize = 1.0f;
};

} // namespace kumo::scene
