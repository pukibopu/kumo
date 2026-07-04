# RHI

（实现自 M2 开始，本文当前为设计概要。）

WebGPU 风格的图形 API 抽象层。核心类型：

`Device / Queue / CommandEncoder / RenderPassEncoder / ComputePassEncoder / Buffer / Texture / Sampler / ShaderModule / BindGroupLayout / BindGroup / RenderPipeline / ComputePipeline / Surface`

- 资源创建统一走 POD 描述结构（`BufferDesc` 等），返回共享指针；创建失败返回空（详见 architecture.md 错误约定）。
- 双帧 in-flight；每帧独立命令分配；帧级 uniform 走环形缓冲。
- Per-draw 小数据（≤128 字节）走 push constants：Vulkan 原生，Metal 映射为 `setVertexBytes` / `setFragmentBytes`。
- MSAA：`TextureDesc` / `RenderPipelineDesc` 带 `sampleCount`，pass 内 resolve。

## 后端映射

| RHI | Metal | Vulkan |
|---|---|---|
| Device | `MTLDevice` | `VkDevice`（vk-bootstrap 初始化） |
| Queue | `MTLCommandQueue` | 单一 graphics+compute+present 队列族 |
| CommandEncoder | `MTLCommandBuffer` | `VkCommandBuffer`（每帧命令池） |
| RenderPassEncoder | `MTLRenderCommandEncoder` | dynamic rendering（Vulkan 1.3） |
| Buffer | `MTLBuffer` | `VkBuffer` + VMA |
| Texture | `MTLTexture` | `VkImage` + `VkImageView` + VMA |
| ShaderModule | 运行时 MSL 编译出的 `MTLLibrary` | SPIR-V `VkShaderModule` |
| RenderPipeline | `MTLRenderPipelineState` + depth-stencil state | `VkPipeline`（动态 viewport/scissor） |
| BindGroup | 按槽位映射的 set 调用 | `VkDescriptorSet` |
| Surface | `CAMetalLayer` | `VkSurfaceKHR` + `VkSwapchainKHR` |

窗口层：桌面用 GLFW（`GLFW_NO_API`），Metal 侧由一个 Objective-C++ shim 将 `CAMetalLayer` 挂到 NSView；iPad 用 `layerClass` 为 `CAMetalLayer` 的 UIView。引擎只见 `CAMetalLayer*`，不区分平台。
