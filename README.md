# kumo

[English](README.en.md)

macOS / iPad 原生 Metal PBR 渲染器，内置 LLM 驱动的场景与 shader 助手。shader 用 GLSL 单源编写，经 SPIR-V 交叉编译到 MSL 并支持运行时热重载。

## 功能规划

- Metal-only GPU facade：描述式资源/pass API，Metal 专属能力可直接扩展
- Cook-Torrance PBR、IBL、HDR + ACES、MSAA 4x、平行光阴影
- glTF 2.0 静态场景加载
- GLSL 单源 → SPIR-V / MSL 交叉编译，支持运行时热重载
- 场景助手：自然语言增删实体、调整灯光 / 相机 / 材质 / 天空环境，内置构图与布光指导、场景自检与截图自评
- Shader 助手：自然语言生成材质 shader，编译报错自动修正后热载入
- 真实素材库：贴图 / glTF 模型（含分类道具包）/ HDRI 环境（`./tools/fetch_assets.sh` 一键拉取，CC0），助手建场景时优先取用；`viewer --thumbnails` 生成预览图与索引
- 素材自采：素材库没有合适素材时，助手按需从 Poly Haven 现取 CC0 贴图/环境/模型

进度见 [docs/milestones.md](docs/milestones.md)。

## 构建

依赖：CMake ≥ 3.24、Ninja、支持 C++23 的编译器（Apple Clang）。

```sh
cmake --preset macos-debug
cmake --build --preset macos-debug
ctest --preset macos-debug
```

## 运行

**产品 app（SwiftUI，推荐）**：

```sh
open build/macos-debug/apps/kumo/kumo_app.app
```

场景树 / 视口（左键拖拽旋转、滚轮缩放、WASD 平移）/ inspector（变换与材质编辑、光照面板、shader 查看、⌘Z 撤销）/ 聊天（⌘⇧J，场景与 Shader 双助手，均可截图自检）/ 场景存读（⌘S / ⌘O，可启动恢复，含自定义 shader）/ 设置（⌘,，API key 入 Keychain，保存即生效）。

**开发 viewer（GLFW + ImGui）**：

```sh
./build/macos-debug/apps/viewer/viewer                 # 默认加载 DamagedHelmet + 摄影棚 HDR
./build/macos-debug/apps/viewer/viewer path/to/model.glb --env path/to/env.hdr
```

viewer 以 Cook-Torrance PBR + IBL 渲染 glTF 场景（MSAA 4x、ACES tone mapping）：

- **鼠标左键拖拽**旋转相机，**滚轮**缩放（轨道相机）；
- ImGui 面板实时调整平行光方向/强度/颜色与材质 metallic/roughness 系数；
- 「助手」窗口双页签：**场景**（自然语言操控场景）与 **Shader**（自然语言生成/修改材质 shader，编译错误自修正），「工具日志」面板完整记录每次工具调用；
- **S** 键截图；**K** / **L** 保存 / 载入场景（`kumo_scene.json`，含 agent 建的实体与材质改动）；
- 修改 `shaders/` 下的 GLSL 源码即热重载，编译错误保留旧画面不中断。

## 助手

场景助手：自然语言增删实体、调整变换/材质/灯光/相机/天空环境（clear_day / sunset / overcast / night / studio 预设 + 参数覆盖，重烘焙 IBL），建完自动用 `scene_validate` 自检悬浮、穿插与出画。Shader 助手：「让头盔变成肥皂泡彩虹色」级别的自然语言描述 → 生成该材质专属的 fragment shader，编译错误自动修正，只影响目标物体。

接入协议双轨（`provider.type`），且两个助手可各配端点（`agents.scene.*` / `agents.shader.*` 按字段覆盖全局）：

- **OpenAI 云端（模板默认）**：`cp kumo.config.example.json kumo.config.json`，改 `provider.model` 为你可用的 GPT 型号，key 放 `.env` 的 `OPENAI_API_KEY`；
- **本地模型（OpenAI 兼容，零成本）**：Ollama / LM Studio / llama.cpp，本机端点无需 key：

  ```sh
  KUMO_PROVIDER_TYPE=openai KUMO_PROVIDER_MODEL=qwen2.5:14b ./build/macos-debug/apps/viewer/viewer
  ```

- **Anthropic Messages API**（官方或兼容中转）：`type` 填 `anthropic`，key 用 `ANTHROPIC_API_KEY`。

检索链路（素材语义搜索的 embedding / 缩略图 caption）与导演/critic 角色同样可以整体或按块指向本地端点，与云端 agent 自由混搭——日常开发可以做到零 API 成本（`retrieval.base_url` + `agents.<role>.*`，配置示例见 docs/agents.md 的「零成本本地配置」）。

`viewer --offline` 运行零网络的内置演示脚本。未配置时渲染功能不受影响。细节（工具清单、绑定契约、历史压缩、确认弹窗）见 [docs/agents.md](docs/agents.md)。

### MCP

`viewer --mcp` 经 stdio 提供 MCP 端点，工具面与内嵌助手同源（场景十六工具 + shader 双工具 + 离屏截图工具 `viewer_screenshot`）：

```sh
claude mcp add kumo -- "$(pwd)/build/macos-debug/apps/viewer/viewer" --mcp
```

## 许可证

MIT，见 [LICENSE](LICENSE)。
