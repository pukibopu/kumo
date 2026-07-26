# kumo

[English](README.en.md)

macOS / iPad 原生 Metal PBR 渲染器，内置 LLM 驱动的场景与 shader 助手。shader 用 GLSL 单源编写，经 SPIR-V 交叉编译到 MSL 并支持运行时热重载。

## 功能规划

- 自研 WebGPU 风格 RHI（架构曾以 Metal + Vulkan 双后端验证，现专注 Metal）
- Cook-Torrance PBR、IBL、HDR + ACES、MSAA 4x、平行光阴影
- glTF 2.0 静态场景加载
- GLSL 单源 → SPIR-V / MSL 交叉编译，支持运行时热重载
- 场景助手：自然语言增删实体、调整灯光 / 相机 / 材质
- Shader 助手：自然语言生成材质 shader，编译报错自动修正后热载入

进度见 [docs/milestones.md](docs/milestones.md)。

## 构建

依赖：CMake ≥ 3.24、Ninja、支持 C++23 的编译器（Apple Clang）。

```sh
cmake --preset macos-debug
cmake --build --preset macos-debug
ctest --preset macos-debug
```

## 运行

```sh
./build/macos-debug/apps/viewer/viewer                 # 默认加载 DamagedHelmet + 摄影棚 HDR
./build/macos-debug/apps/viewer/viewer path/to/model.glb --env path/to/env.hdr
```

viewer 以 Cook-Torrance PBR + IBL 渲染 glTF 场景（MSAA 4x、ACES tone mapping）：

- **鼠标左键拖拽**旋转相机，**滚轮**缩放（轨道相机）；
- ImGui 面板实时调整平行光方向/强度/颜色与材质 metallic/roughness 系数；
- **S** 键截图，PNG 存至当前目录；
- 修改 `shaders/` 下的 GLSL 源码即热重载，编译错误保留旧画面不中断。

## 助手配置

场景 / shader 助手需要一个兼容 Anthropic Messages API 的端点：

1. 复制 `kumo.config.example.json` 为 `kumo.config.json`，填入 `base_url` 与 `model`；
2. API key 放入环境变量 `ANTHROPIC_API_KEY`，或写入 `.env` 文件（参考 `.env.example`）。

未配置时渲染功能不受影响。

## 许可证

MIT，见 [LICENSE](LICENSE)。
