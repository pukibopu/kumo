# kumo

[中文](README.md)

A native Metal PBR renderer for macOS / iPad with an LLM-powered scene and shader assistant. Shaders are written once in GLSL, cross-compiled to MSL through SPIR-V, and hot-reload at runtime.

## Planned features

- Custom WebGPU-style RHI (the architecture was proven with dual Metal + Vulkan backends; now Metal-focused)
- Cook-Torrance PBR, IBL, HDR + ACES, MSAA 4x, directional shadows
- glTF 2.0 static scene loading
- Single-source GLSL → SPIR-V / MSL cross-compilation with runtime hot reload
- Scene assistant: add/remove entities, tweak lights, camera and materials in natural language
- Shader assistant: generate material shaders from natural language, self-correcting on compile errors, hot-reloaded live

See [docs/milestones.md](docs/milestones.md) for progress.

## Building

Requires CMake ≥ 3.24, Ninja, and a C++23 compiler (Apple Clang).

```sh
cmake --preset macos-debug
cmake --build --preset macos-debug
ctest --preset macos-debug
```

## Running

```sh
./build/macos-debug/apps/viewer/viewer                 # loads DamagedHelmet + studio HDR by default
./build/macos-debug/apps/viewer/viewer path/to/model.glb --env path/to/env.hdr
```

The viewer renders glTF scenes with Cook-Torrance PBR and IBL (MSAA 4x, ACES tone mapping):

- **Left-drag** rotates the camera, **scroll** zooms (orbit camera);
- ImGui panels tweak the directional light (direction/intensity/color) and material metallic/roughness multipliers live;
- **S** saves a screenshot as PNG in the working directory;
- Editing the GLSL sources under `shaders/` hot-reloads them; compile errors keep the last good frame.

## Assistant configuration

The scene / shader assistant needs an Anthropic Messages API compatible endpoint:

1. Copy `kumo.config.example.json` to `kumo.config.json` and fill in `base_url` and `model`;
2. Put your API key in the `ANTHROPIC_API_KEY` environment variable, or in a `.env` file (see `.env.example`).

Rendering works fine without any assistant configuration.

## License

MIT, see [LICENSE](LICENSE).
