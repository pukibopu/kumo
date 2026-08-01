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

    // Save/load provenance for EngineRuntime::instantiateModel: non-empty
    // names the glb file (under <assetDir>/models/) the entity came from;
    // `modelMesh` is then its index within that model's flattened mesh list
    // (asset::SceneAsset::nodes order), used to remap mesh/material indices
    // after the model is re-uploaded. Empty `model` means not model-sourced.
    std::string model;
    std::int32_t modelMesh = -1;
};

} // namespace kumo::scene
