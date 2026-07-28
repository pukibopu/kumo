# 架构

（随里程碑推进持续补充，当前对应 M6.75。）

## 应用与平台层（macOS）

Metal 侧统一使用 metal-cpp（Apple 官方 C++ 绑定），Objective-C 只保留 metal-cpp 覆盖不到的 AppKit 接缝——把 `CAMetalLayer` 挂到 NSView 的十几行 shim（`apps/viewer/metal_layer_glue.mm`）。对象生命周期遵循 CoreFoundation 计数规则：`new*/create*` 返回值用 `NS::TransferPtr` 接管，逐帧 autoreleased 对象由每帧 `NS::AutoreleasePool` 回收。

viewer（`apps/viewer/app.cpp`，组合根）：

1. 装载 glTF 场景与等距柱状 HDR（`kumo_asset`），失败即干净退出；
2. 创建 RHI 设备与 surface，`renderer::ibl::bake` 烘焙 IBL（compute，含耗时日志）；
3. `ForwardRenderer::loadScene` 上传网格/材质，glTF 节点展开为 `scene::Scene` 实体（世界变换分解为 TRS，agent 可动实体变换）；
4. 装配 agent 栈（M5/M6）：`MainThreadQueue` + 两个 `ToolRegistry`（场景工具 / scene_list+shader 工具）+ 按 `kumo.config.json` 的 per-agent 端点装配的两个 provider 与 `AgentSession`（场景 / shader，或 `--offline` 的脚本回放单会话）——声明顺序保证 session 先于其依赖析构（栈逆序）；
5. 每帧：`glfwPollEvents` 后排空 `MainThreadQueue`（工具回调对帧原子）→ 轨道相机（用户输入时写相机、否则从相机反向同步，agent 的 camera_set 不被覆盖；灯光滑条同理）→ ImGui 面板（帧率、灯光、材质覆盖、聊天、工具日志、确认弹窗）→ `ForwardRenderer::render`；
6. `S` 键截图；`K`/`L` 存/读场景 JSON（`kumo_scene.json`）；`--frames N` 渲染 N 帧后退出供脚本化验证；
7. `--mcp`（M6.5）：日志切到 stderr，独立 `ToolRegistry`（场景七工具 + shader 双工具 + `viewer_screenshot` 离屏截图工具）交给 `McpServer`，一条读线程把 stdin 的 JSON-RPC 行经 `MainThreadQueue` 投递主线程处理、响应写回 stdout；主循环每帧检测客户端断开（stdin EOF）并干净退出。

## 渲染帧流程（M4）

```
装载期：equirect HDR ─ compute ─▶ 环境 cube（mip 链）─▶ 辐照度 cube + 预滤波 cube + BRDF LUT
每帧：pass 1  MSAA 4x HDR（RGBA16F）：PBR 网格 + 天空盒（reversed-Z 远平面）→ resolve
      pass 2  ACES tonemap + sRGB 编码 → 交换链，随后同 pass 叠加 ImGui
```

帧 uniform（set 0）双 buffer 轮换对应双帧 in-flight，避免写入与 GPU 读取竞争；per-draw 数据（model/normal 矩阵）走 push constants。IBL 烘焙的每一步只读一张纹理、写另一张（mip 链生成除外，由 RHI 内部处理）。

## 模块与依赖

```
core ← math ← rhi ← rhi_metal
core ← shadercompiler
math ← scene ← asset
{ rhi, scene, asset, shadercompiler } ← renderer
{ scene, renderer, shadercompiler } ← agent
{ agent, renderer, scene, asset } ← facade
facade ← apps/viewer（GLFW 开发壳）
facade ← apps/kumo（SwiftUI 产品壳，经 ObjC++ 桥）
```

依赖严格无环，由 CMake 链接关系约束。`rhi` 只含纯虚接口与 POD 描述结构，不含任何平台头文件。

目前已存在的模块：

- `engine/core`——日志、断言、文件工具、文件监视
- `engine/math`——基于 glm 的数学类型封装（`kumo::math`）
- `engine/rhi`——RHI 纯虚接口与描述结构（见 rhi.md）
- `engine/rhi_metal`——Metal 后端（metal-cpp，单翻译单元）
- `engine/shadercompiler`——GLSL→SPIR-V→MSL 进程内编译（见 shaders.md）
- `engine/scene`——场景数据模型：实体 slot map（generational id）、相机、光源、场景 JSON 持久化（ADR 0016）
- `engine/asset`——glTF/HDR/PNG 装载（cgltf + stb）、程序化图元、per-mesh AABB
- `engine/renderer`——前向 PBR 渲染器与 IBL 烘焙：`ForwardRenderer` 类 + `renderer::ibl` 自由函数，bind group layout 由 shader 反射生成（ADR 0040），增量上传接口供 agent 建实体；M6 起支持材质级定制 fragment pipeline 与重建式热重载（见 shaders.md）
- `engine/agent`——LLM 助手（见 agents.md）：provider 栈（NSURLSession shim + 重试退避 + OpenAI/Claude 双 codec、per-agent 端点）、工具注册表（场景七工具 + shader 双工具，按助手收窄）、worker 线程会话（历史压缩、队列化确认门）、MCP server（stdio JSON-RPC，复用同一工具注册表，ADR 0041）
- `engine/facade`——壳共享的组合层（ADR 0044）：`EngineRuntime`（窗口层之上的一切装配：资产/IBL/渲染器/双 agent/三 registry/MCP 泵/持久化）、快照撤销栈（pending-commit，agent/MCP/inspector 变更同栈）、共享轨道相机（输入 vs agent 仲裁）、按需渲染脏标记

## 产品壳（apps/kumo，M6.75）

SwiftUI（macOS 先行）：NavigationSplitView（场景树 / CAMetalLayer 视口 / inspector）+ 可折叠聊天栏（场景/Shader 页签）+ 工具日志 + 破坏性确认弹窗 + 设置（per-agent 端点表单，API key 入 Keychain、启动时以不覆盖真实环境变量的方式注入 env）。引擎↔Swift 经 `KumoEngine` ObjC++ facade（纯值类型接口，ADR 0044 语言边界：引擎零 Swift、Swift 零渲染逻辑）；display link 驱动 tick、节流到主队列至多挂一个，场景无变更时跳过 drawable 获取（按需渲染）。inspector 含材质 shader 只读查看器（还原 / 访达显示），⌘Z/⇧⌘Z 走统一撤销栈。

（M3 的 Vulkan 后端已于 M4 移除，项目聚焦 Metal。）

## 全局约定

- **语言**：C++23；标识符与注释使用英文，注释只写代码无法表达的约束。
- **坐标系**：右手系，+Y 向上，相机默认朝 -Z（与 glTF 一致）；裁剪空间深度范围 [0, 1]。
- **深度**：reversed-Z——近平面映射到 1、远平面映射到 0（投影用无穷远 reversed-Z），depth compare 用 GreaterEqual，clear 值为 0。调试深度图时黑白与直觉相反。
- **数学库入口**：统一 `#include <kumo/math/math.h>`，不直接包含 glm（保证 `GLM_FORCE_DEPTH_ZERO_TO_ONE` 等配置一致）。
- **错误处理**：编程错误用 `KUMO_ASSERT`（debug 断言，release 返回空 + 日志）；常态性可失败操作（如 shader 编译）返回 `std::expected`。
- **同步模型**：RHI 不暴露 barrier，后端在 pass 边界自动处理（M2 起）。

## 构建系统

- 根 `CMakeLists.txt` + `CMakePresets.json`；第三方依赖全部经 FetchContent 拉取，pin 在 `cmake/dependencies.cmake`（固定 commit 哈希）。
- 单测用 doctest，`ctest` 驱动，测试文件在 `tests/`；golden image 测试（`ctest -L golden`）仅本地运行，基准图在 `tests/golden/metal/`，`KUMO_UPDATE_GOLDEN=1` 更新。
- CI（GitHub Actions）：macOS 构建 + 测试（排除 golden）、clang-format 检查。GPU 相关验证只在本地进行。
