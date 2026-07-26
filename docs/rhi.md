# RHI

（M2 起 Metal 后端已实现；compute、MSAA resolve 与纹理回读随 M4 落地。M3 引入的 Vulkan 后端已于 M4 移除，项目聚焦 Metal。）

WebGPU 风格的图形 API 抽象层，接口聚合在 `kumo/rhi/rhi.h`（枚举在 `types.h`）。核心类型：

`Device / Queue / CommandEncoder / RenderPassEncoder / ComputePassEncoder / Buffer / Texture / Sampler / ShaderModule / BindGroupLayout / BindGroup / RenderPipeline / ComputePipeline / Surface`

- 资源创建统一走 POD 描述结构（`BufferDesc` 等，designated initializers），返回共享指针；创建失败返回空（详见 architecture.md 错误约定）；
- 后端须保证 pipeline / 资源对象存活至使用它们的 GPU 工作完成（Metal 由 command buffer 自动 retain）；BindGroup 持有其引用的资源；
- 双帧 in-flight：`Queue::createCommandEncoder()` 按 in-flight 数节流（信号量），每帧一个 encoder，`finishAndSubmit(surface)` 提交并呈现；
- `RenderPipelineDesc::bindGroupLayouts` 按 set 序号索引，空 set 用 `nullptr` 占位；
- Per-draw 小数据（≤128 字节）走 `setPushConstants`：Metal 映射为 `setVertexBytes` / `setFragmentBytes`（buffer index 24）；
- `Queue::writeBuffer` / `writeTexture` 面向**装载期上传**，不做多帧版本化——逐帧变化的数据用 push constants 或环形缓冲（M4 帧 uniform 用双 buffer 轮换），对在飞帧仍在读的 buffer 直接 writeBuffer 属数据竞争；
- `Queue::readTexture` 同步回读 mip 0（blit 到 shared buffer + waitUntilCompleted）：调用方须先提交生产内容的命令（同队列按提交序执行），供截图与 golden 测试使用；
- 纹理视图 `createTextureView`：cube 逐面 2D 视图用于 compute 写入（IBL 烘焙），视图 retain 父纹理；
- MSAA：`TextureDesc` / `RenderPipelineDesc` 带 `sampleCount`（4x），color attachment 设 `resolveTarget` 在 pass 末 resolve；
- Surface：`CAMetalLayer` 包装；`framebufferOnly` 置 false 以允许从 drawable 回读（截图），代价是放弃部分仅帧缓冲的带宽优化；
- Native 逃生口（仅供 ImGui 等调试集成）：`Device::nativeHandles()`、`CommandEncoder::nativeCommandBufferHandle()`、`RenderPassEncoder::nativeEncoderHandle()` / `nativePassDescriptorHandle()`。

## Metal 绑定映射

Metal 参数表布局（与 SPIRV-Cross 重映射共用，常量在 `kumo/rhi_metal/binding_map.h`，有快照单测）：

| 区间 | 用途 |
|---|---|
| buffer/texture index `set*8+binding`（0–23） | set 0–2 的 buffer 与纹理绑定 |
| sampler index `set*6+binding`（0–15） | set 0–2 的采样器绑定（Metal 采样器表仅 16 槽，编译期校验越界） |
| buffer index 24 | push constants |
| buffer index 30 向下（30–26） | 顶点流 slot 0–4 |

## 后端映射

| RHI | Metal |
|---|---|
| Device | `MTLDevice` |
| Queue | `MTLCommandQueue` |
| CommandEncoder | `MTLCommandBuffer` |
| RenderPassEncoder | `MTLRenderCommandEncoder` |
| ComputePassEncoder | `MTLComputeCommandEncoder` |
| Buffer | `MTLBuffer`（shared storage） |
| Texture | `MTLTexture`（视图经 `newTextureView`） |
| ShaderModule | 运行时 MSL 编译出的 `MTLLibrary` |
| RenderPipeline | `MTLRenderPipelineState` + depth-stencil state |
| ComputePipeline | `MTLComputePipelineState`（threadgroup 尺寸取自 shader 反射） |
| BindGroup | 按槽位映射的 set 调用 |
| Surface | `CAMetalLayer` |

窗口层：桌面用 GLFW（`GLFW_NO_API`），Metal 侧由一个 Objective-C++ shim 将 `CAMetalLayer` 挂到 NSView；iPad 用 `layerClass` 为 `CAMetalLayer` 的 UIView。引擎只见 `CAMetalLayer*`，不区分平台。
