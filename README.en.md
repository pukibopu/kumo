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
- The chat panel drives the scene in natural language (see below); the tool log panel records every tool call in full;
- **S** saves a screenshot; **K** / **L** save / load the scene (`kumo_scene.json`, including agent-built entities and material tweaks);
- Editing the GLSL sources under `shaders/` hot-reloads them; compile errors keep the last good frame.

## Scene assistant

Add/remove entities and tweak transforms, materials, lights and the camera in natural language. Two wire protocols (`provider.type`):

- **Local models (OpenAI-compatible, the easiest start)**: Ollama / LM Studio / llama.cpp; local endpoints need no key:

  ```sh
  KUMO_PROVIDER_TYPE=openai KUMO_PROVIDER_MODEL=qwen2.5:14b ./build/macos-debug/apps/viewer/viewer
  ```

- **Anthropic Messages API** (official or a compatible relay): copy `kumo.config.example.json` to `kumo.config.json`, fill in `model`, and put your API key in the `ANTHROPIC_API_KEY` environment variable or a `.env` file (see `.env.example`).

`viewer --offline` replays a built-in scripted demo with zero network. Rendering works fine without any assistant configuration. Details (tool set, history compression, confirmation dialog) live in [docs/agents.md](docs/agents.md).

## License

MIT, see [LICENSE](LICENSE).
