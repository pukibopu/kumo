# RHI

（M2 起 Metal 后端、M3 起 Vulkan 后端（MoltenVK/Windows）均已实现；compute 与 MSAA resolve 随 M4 落地。）

Vulkan 后端要点：vk-bootstrap 初始化（Vulkan 1.3，dynamic rendering + synchronization2）、VMA 内存分配、直接链接 SDK loader（未用 volk——1.3 核心入口点由 loader 导出）、负高度 viewport 统一 NDC Y 方向、图像布局转换由 `layout_tracker.h`（纯逻辑，有单测）驱动、描述符按 bind group 创建期分配（增长式池）。运行需 LunarG Vulkan SDK。

布局跟踪按整张图像（而非每个 subresource）记录，因此不支持对同一图像跨 mip 的同时读写；烘焙 mip 链必须使用独立的源/目标纹理。

WebGPU 风格的图形 API 抽象层，接口聚合在 `kumo/rhi/rhi.h`（枚举在 `types.h`）。核心类型：

`Device / Queue / CommandEncoder / RenderPassEncoder / Buffer / Texture / Sampler / ShaderModule / BindGroupLayout / BindGroup / RenderPipeline / Surface`

- 资源创建统一走 POD 描述结构（`BufferDesc` 等，designated initializers），返回共享指针；创建失败返回空（详见 architecture.md 错误约定）；
- 后端须保证 pipeline / 资源对象存活至使用它们的 GPU 工作完成——Metal 由 command buffer 自动 retain，Vulkan 后端须相应延迟销毁；
- 双帧 in-flight：`Queue::createCommandEncoder()` 按 in-flight 数节流（信号量），每帧一个 encoder，`finishAndSubmit(surface)` 提交并呈现；
- `RenderPipelineDesc::bindGroupLayouts` 按 set 序号索引，空 set 用 `nullptr` 占位；
- Per-draw 小数据（≤128 字节）走 `setPushConstants`：Vulkan 原生 push constants，Metal 映射为 `setVertexBytes` / `setFragmentBytes`（buffer index 24）；
- `Queue::writeBuffer` / `writeTexture` 面向**装载期上传**，不做多帧版本化——逐帧变化的数据用 push constants 或环形缓冲（M4 的帧 uniform 方案），对在飞帧仍在读的 buffer 直接 writeBuffer 属数据竞争；
- MSAA：`TextureDesc` / `RenderPipelineDesc` 带 `sampleCount`，pass 内 resolve（M4 启用）；
- Native 逃生口（仅供 ImGui 等调试集成）：`Device::nativeHandles()`、`CommandEncoder::nativeCommandBufferHandle()`、`RenderPassEncoder::nativeEncoderHandle()` / `nativePassDescriptorHandle()`。

## Metal 绑定映射

Metal 参数表布局（与 M3 的 SPIRV-Cross 重映射共用，常量在 `kumo/rhi_metal/binding_map.h`，有快照单测）：

| 区间 | 用途 |
|---|---|
| buffer/texture/sampler index `set*8+binding`（0–23） | set 0–2 的资源绑定 |
| buffer index 24 | push constants |
| buffer index 30 向下（30–26） | 顶点流 slot 0–4 |

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
