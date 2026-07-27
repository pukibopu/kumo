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
- 「助手」聊天面板用自然语言操控场景（见下），「工具日志」面板完整记录每次工具调用；
- **S** 键截图；**K** / **L** 保存 / 载入场景（`kumo_scene.json`，含 agent 建的实体与材质改动）；
- 修改 `shaders/` 下的 GLSL 源码即热重载，编译错误保留旧画面不中断。

## 场景助手

自然语言增删实体、调整变换/材质/灯光/相机。两种接入协议（`provider.type`）：

- **本地模型（OpenAI 兼容，推荐起步）**：Ollama / LM Studio / llama.cpp 等，本机端点无需 key：

  ```sh
  KUMO_PROVIDER_TYPE=openai KUMO_PROVIDER_MODEL=qwen2.5:14b ./build/macos-debug/apps/viewer/viewer
  ```

- **Anthropic Messages API**（官方或兼容中转）：复制 `kumo.config.example.json` 为 `kumo.config.json` 填入 `model`，API key 放环境变量 `ANTHROPIC_API_KEY` 或 `.env` 文件（参考 `.env.example`）。

`viewer --offline` 运行零网络的内置演示脚本。未配置时渲染功能不受影响。细节（工具清单、历史压缩、确认弹窗）见 [docs/agents.md](docs/agents.md)。

## 许可证

MIT，见 [LICENSE](LICENSE)。
