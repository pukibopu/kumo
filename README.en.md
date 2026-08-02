# kumo

[中文](README.md)

A native Metal PBR renderer for macOS / iPad with an LLM-powered scene and shader assistant. Shaders are written once in GLSL, cross-compiled to MSL through SPIR-V, and hot-reload at runtime.

## Planned features

- Metal-only GPU facade with descriptive resource/pass APIs and direct room for Metal features
- Cook-Torrance PBR, IBL, HDR + ACES, MSAA 4x, directional shadows
- glTF 2.0 static scene loading
- Single-source GLSL → SPIR-V / MSL cross-compilation with runtime hot reload
- Scene assistant: add/remove entities, tweak lights, camera, materials and the sky environment in natural language, with built-in composition guidance, scene validation and screenshot self-review
- Shader assistant: generate material shaders from natural language, self-correcting on compile errors, hot-reloaded live
- Real asset library: textures, glTF models (including categorized prop packs) and HDRI environments (`./tools/fetch_assets.sh` fetches a CC0 starter pack), preferred by the assistants over procedural geometry; `viewer --thumbnails` builds preview images and an index
- Self-provisioning: the assistant fetches CC0 textures, environments and models from Poly Haven on demand when the library lacks a fit

See [docs/milestones.md](docs/milestones.md) for progress.

## Building

Requires CMake ≥ 3.24, Ninja, and a C++23 compiler (Apple Clang).

```sh
cmake --preset macos-debug
cmake --build --preset macos-debug
ctest --preset macos-debug
```

## Running

**Product app (SwiftUI, recommended)**:

```sh
open build/macos-debug/apps/kumo/kumo_app.app
```

Scene tree / viewport (left-drag orbits, scroll zooms, WASD pans) / inspector (transform & material editing, light panel, shader viewer, ⌘Z undo) / chat (⌘⇧J, scene + shader assistants, both screenshot-checked) / scene save-load (⌘S / ⌘O with optional restore-on-launch, custom shaders included) / settings (⌘, — API keys go to the Keychain, applied live on save).

**Dev viewer (GLFW + ImGui)**:

```sh
./build/macos-debug/apps/viewer/viewer                 # loads DamagedHelmet + studio HDR by default
./build/macos-debug/apps/viewer/viewer path/to/model.glb --env path/to/env.hdr
```

The viewer renders glTF scenes with Cook-Torrance PBR and IBL (MSAA 4x, ACES tone mapping):

- **Left-drag** rotates the camera, **scroll** zooms (orbit camera);
- ImGui panels tweak the directional light (direction/intensity/color) and material metallic/roughness multipliers live;
- The assistant window has two tabs: **场景** (drive the scene in natural language) and **Shader** (generate/edit material shaders from natural language, self-correcting on compile errors); the tool log panel records every tool call in full;
- **S** saves a screenshot; **K** / **L** save / load the scene (`kumo_scene.json`, including agent-built entities and material tweaks);
- Editing the GLSL sources under `shaders/` hot-reloads them; compile errors keep the last good frame.

## Assistants

Scene assistant: add/remove entities and tweak transforms, materials, lights, the camera and the sky environment (clear_day / sunset / overcast / night / studio presets with per-field overrides, IBL rebaked) in natural language, self-checking the result with `scene_validate` for floating, intersecting or out-of-frame objects. Shader assistant: turn a description like "make the helmet iridescent like a soap bubble" into a material-private fragment shader, self-correcting on compile errors, affecting only the target object.

Two wire protocols (`provider.type`), and each assistant can point at its own endpoint (`agents.scene.*` / `agents.shader.*` override the global provider per field):

- **OpenAI cloud (the template default)**: `cp kumo.config.example.json kumo.config.json`, set `provider.model` to a GPT model you have access to, and put your key in `.env` as `OPENAI_API_KEY`;
- **Local models (OpenAI-compatible, zero cost)**: Ollama / LM Studio / llama.cpp; local endpoints need no key:

  ```sh
  KUMO_PROVIDER_TYPE=openai KUMO_PROVIDER_MODEL=qwen2.5:14b ./build/macos-debug/apps/viewer/viewer
  ```

- **Anthropic Messages API** (official or a compatible relay): set `type` to `anthropic` and use `ANTHROPIC_API_KEY`.

`viewer --offline` replays a built-in scripted demo with zero network. Rendering works fine without any assistant configuration. Details (tool set, binding contract, history compression, confirmation dialog) live in [docs/agents.md](docs/agents.md).

### MCP

`viewer --mcp` serves an MCP endpoint over stdio, with the same tool set as the embedded assistants (the sixteen scene tools, the two shader tools, and an offscreen `viewer_screenshot` tool):

```sh
claude mcp add kumo -- "$(pwd)/build/macos-debug/apps/viewer/viewer" --mcp
```

## License

MIT, see [LICENSE](LICENSE).
