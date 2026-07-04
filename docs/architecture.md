# 架构

（随里程碑推进持续补充，当前对应 M1。）

## 应用与平台层（macOS）

viewer 的启动与主循环：

1. GLFW 以 `GLFW_NO_API` 创建窗口（1280×720，可缩放）；
2. 创建 `CAMetalLayer` 并挂到 NSWindow 的 contentView（`apps/viewer/app_metal.mm`）；
3. `MTLCreateSystemDefaultDevice` + command queue；
4. 每帧：`glfwPollEvents` → 检查 framebuffer 尺寸变化并同步 `drawableSize`（resize 处理）→ `nextDrawable` → 清屏 render pass → present + commit。

垂直同步由 `CAMetalLayer` 默认的 display sync 提供（约 60 fps）。`viewer --frames N` 渲染 N 帧后自动退出，供脚本化验证使用。

当前 `app_metal.mm` 是刻意直写的临时实现，M2 引入 RHI 后整体替换。

## 模块与依赖

```
core ← math ← rhi ← { rhi_metal, rhi_vulkan }
core ← shadercompiler
math ← scene ← asset
{ rhi, scene, shadercompiler } ← renderer
{ scene, renderer, shadercompiler } ← agent
所有模块 ← apps/viewer
```

依赖严格无环，由 CMake 链接关系约束。`rhi` 只含纯虚接口与 POD 描述结构，不含任何平台头文件；应用启动时通过工厂函数选择 Metal 或 Vulkan 后端。

目前已存在的模块：

- `engine/core`——日志、断言等基础设施
- `engine/math`——基于 glm 的数学类型封装（`kumo::math`）

## 全局约定

- **语言**：C++23；标识符与注释使用英文，注释只写代码无法表达的约束。
- **坐标系**：右手系，+Y 向上，相机默认朝 -Z（与 glTF 一致）；裁剪空间深度范围 [0, 1]。
- **深度**：reversed-Z——近平面映射到 1、远平面映射到 0，depth compare 用 GreaterEqual，clear 值为 0。调试深度图时黑白与直觉相反。
- **数学库入口**：统一 `#include <kumo/math/math.h>`，不直接包含 glm（保证 `GLM_FORCE_DEPTH_ZERO_TO_ONE` 等配置一致）。
- **错误处理**：编程错误用 `KUMO_ASSERT`（debug 断言，release 返回空 + 日志）；常态性可失败操作（如 shader 编译）返回 `std::expected`。
- **同步模型**：RHI 不暴露 barrier，后端在 pass 边界自动处理（M2 起）。

## 构建系统

- 根 `CMakeLists.txt` + `CMakePresets.json`；第三方依赖全部经 FetchContent 拉取，pin 在 `cmake/dependencies.cmake`（固定 commit 哈希）。
- 单测用 doctest，`ctest` 驱动，测试文件在 `tests/`。
- CI（GitHub Actions）：macOS 构建 + 测试、Windows 构建 + 测试、clang-format 检查。GPU 相关验证只在本地进行。
