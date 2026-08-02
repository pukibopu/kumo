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

    // Save/load provenance for material_set_texture (M6.99): non-empty names a
    // texture set under <assetDir>/textures/ bound to this entity's material;
    // empty means the material carries no texture set (still just flat
    // factors, or came in pre-textured from a glTF/model source, which does
    // not go through this field). Set on every entity sharing the material
    // (material_set_texture already scans for sharers), so reload rebinds all
    // of them, not just the one the tool call named.
    std::string textureSet;
};

} // namespace kumo::scene
