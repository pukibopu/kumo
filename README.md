# kumo

[English](README.en.md)

跨平台 PBR 渲染器，内置 LLM 驱动的场景与 shader 助手。macOS / iPad 使用原生 Metal，Windows 使用 Vulkan，shader 以一份 GLSL 源码交叉编译到两个后端。

## 功能规划

- 自研 WebGPU 风格 RHI，Metal / Vulkan 双后端
- Cook-Torrance PBR、IBL、HDR + ACES、MSAA 4x、平行光阴影
- glTF 2.0 静态场景加载
- GLSL 单源 → SPIR-V / MSL 交叉编译，支持运行时热重载
- 场景助手：自然语言增删实体、调整灯光 / 相机 / 材质
- Shader 助手：自然语言生成材质 shader，编译报错自动修正后热载入

进度见 [docs/milestones.md](docs/milestones.md)。

## 构建

依赖：CMake ≥ 3.24、Ninja、支持 C++23 的编译器（Apple Clang / MSVC 2022）。

```sh
cmake --preset macos-debug
cmake --build --preset macos-debug
ctest --preset macos-debug
```

Windows 使用 `windows` preset（Visual Studio 2022）。从 M3 起需要安装 [LunarG Vulkan SDK](https://vulkan.lunarg.com/)（macOS 版本内含 MoltenVK）。

## 助手配置

场景 / shader 助手需要一个兼容 Anthropic Messages API 的端点：

1. 复制 `kumo.config.example.json` 为 `kumo.config.json`，填入 `base_url` 与 `model`；
2. API key 放入环境变量 `ANTHROPIC_API_KEY`，或写入 `.env` 文件（参考 `.env.example`）。

未配置时渲染功能不受影响。

## 许可证

MIT，见 [LICENSE](LICENSE)。
