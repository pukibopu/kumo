# Metal GPU Facade

`kumo_gpu` 是项目唯一的 GPU 实现。它不是可插拔 RHI：公共 API 使用 concrete、非 virtual 的 PImpl 类隔离 metal-cpp，renderer/facade 只看到描述式资源、pipeline、bind group 与 pass 接口。

核心类型：

`Device / Queue / CommandEncoder / RenderPassEncoder / ComputePassEncoder / Buffer / Texture / Sampler / ShaderModule / BindGroupLayout / BindGroup / RenderPipeline / ComputePipeline / Surface / SurfaceFrame`

- 资源工厂返回 `std::shared_ptr`，Texture view 强持 parent，BindGroup 强持全部资源；Metal 对象只存在于实现 TU。
- shader module 只接收 MSL；GLSL → SPIR-V → MSL 及运行时热重载仍由 shadercompiler 提供。
- 双帧 in-flight；上传接口面向装载期，逐帧 uniform/material buffer 由 renderer 双缓冲。同步 buffer/texture readback 用于测试、golden 与开发工具。
- `Surface::acquire()` 返回 move-only `SurfaceFrame`，它强持 drawable；只有 acquire 成功后才创建 command encoder。产品 surface 保持 `framebufferOnly=true`，开发 viewer 为 drawable 截图显式允许 readback。
- `StorageMode::TransientAttachment` 在支持的 Apple GPU 上映射 memoryless，否则回退 private；仅用于不跨 pass 保存的 MSAA color/depth。
- ImGui 等平台集成通过 `kumo/gpu/metal_interop.h` 获取 typed borrowed Metal 指针；renderer/facade 不得 include 该头。

## Binding ABI

Metal 参数表布局由 `kumo/shaderabi/metal_binding.h` 单一定义，shader translation、GPU encoding 与单测共同使用：

| 区间 | 用途 |
|---|---|
| buffer/texture index `set*8+binding`（0–23） | set 0–2 的 buffer 与纹理绑定 |
| sampler index `set*6+binding`（0–15） | set 0–2 的采样器绑定 |
| buffer index 24 | push constants |
| buffer index 30 向下（30–26） | 顶点流 slot 0–4 |

## Metal 映射

| GPU facade | Metal |
|---|---|
| Device | `MTLDevice` |
| Queue | `MTLCommandQueue` |
| CommandEncoder | `MTLCommandBuffer` |
| Render/ComputePassEncoder | 对应 Metal command encoder |
| Buffer / Texture / Sampler | 对应 Metal resource/state |
| ShaderModule | runtime MSL `MTLLibrary` + `MTLFunction` |
| Render/ComputePipeline | 对应 Metal pipeline state |
| Surface / SurfaceFrame | `CAMetalLayer` / `CAMetalDrawable` |

Metal 独有能力按实际需求加入 facade；argument buffers、heaps、ICB、多 queue 等不提前抽象。
