# kumo

[中文](README.md)

A cross-platform PBR renderer with an LLM-powered scene and shader assistant. Native Metal on macOS / iPad, Vulkan on Windows, with shaders written once in GLSL and cross-compiled to both backends.

## Planned features

- Custom WebGPU-style RHI with Metal and Vulkan backends
- Cook-Torrance PBR, IBL, HDR + ACES, MSAA 4x, directional shadows
- glTF 2.0 static scene loading
- Single-source GLSL → SPIR-V / MSL cross-compilation with runtime hot reload
- Scene assistant: add/remove entities, tweak lights, camera and materials in natural language
- Shader assistant: generate material shaders from natural language, self-correcting on compile errors, hot-reloaded live

See [docs/milestones.md](docs/milestones.md) for progress.

## Building

Requires CMake ≥ 3.24, Ninja, and a C++23 compiler (Apple Clang / MSVC 2022).

```sh
cmake --preset macos-debug
cmake --build --preset macos-debug
ctest --preset macos-debug
```

On Windows use the `windows` preset (Visual Studio 2022). From M3 onwards the [LunarG Vulkan SDK](https://vulkan.lunarg.com/) is required (the macOS version bundles MoltenVK).

## Assistant configuration

The scene / shader assistant needs an Anthropic Messages API compatible endpoint:

1. Copy `kumo.config.example.json` to `kumo.config.json` and fill in `base_url` and `model`;
2. Put your API key in the `ANTHROPIC_API_KEY` environment variable, or in a `.env` file (see `.env.example`).

Rendering works fine without any assistant configuration.

## License

MIT, see [LICENSE](LICENSE).
