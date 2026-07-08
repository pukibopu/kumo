# 架构

（随里程碑推进持续补充，当前对应 M4。）

## 应用与平台层（macOS）

Metal 侧统一使用 metal-cpp（Apple 官方 C++ 绑定），Objective-C 只保留 metal-cpp 覆盖不到的 AppKit 接缝——把 `CAMetalLayer` 挂到 NSView 的十几行 shim（`apps/viewer/metal_layer_glue.mm`）。对象生命周期遵循 CoreFoundation 计数规则：`new*/create*` 返回值用 `NS::TransferPtr` 接管，逐帧 autoreleased 对象由每帧 `NS::AutoreleasePool` 回收。

viewer（`apps/viewer/app.cpp`，组合根）：

1. 装载 glTF 场景与等距柱状 HDR（`kumo_asset`），失败即干净退出；
2. 创建 RHI 设备与 surface，`renderer::ibl::bake` 烘焙 IBL（compute，含耗时日志）；
3. `ForwardRenderer::loadScene` 上传网格/材质，glTF 节点展开为 `scene::Scene` 实体（世界变换分解为 TRS，后续 agent 可动实体变换）；
4. 每帧：轨道相机（LMB 拖拽旋转 / 滚轮缩放，写回 `scene::Camera`）→ ImGui 面板（帧率、平行光方向/强度/颜色、材质 metallic/roughness 覆盖系数）→ `ForwardRenderer::render`；
5. `S` 键经 `Queue::readTexture` 回读交换链纹理存 PNG；`--frames N` 渲染 N 帧后退出供脚本化验证。

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
所有模块 ← apps/viewer
```

依赖严格无环，由 CMake 链接关系约束。`rhi` 只含纯虚接口与 POD 描述结构，不含任何平台头文件。

目前已存在的模块：

- `engine/core`——日志、断言、文件工具、文件监视
- `engine/math`——基于 glm 的数学类型封装（`kumo::math`）
- `engine/rhi`——RHI 纯虚接口与描述结构（见 rhi.md）
- `engine/rhi_metal`——Metal 后端（metal-cpp，单翻译单元）
- `engine/shadercompiler`——GLSL→SPIR-V→MSL 进程内编译（见 shaders.md）
- `engine/scene`——场景数据模型：实体 slot map、相机、光源
- `engine/asset`——glTF/HDR/PNG 装载（cgltf + stb）
- `engine/renderer`——前向 PBR 渲染器与 IBL 烘焙：`ForwardRenderer` 类 + `renderer::ibl` 自由函数，bind group layout 由 shader 反射生成（ADR 0040）

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
