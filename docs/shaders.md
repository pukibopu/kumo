# Shader

（工具链自 M3 落地；M4 起 PBR/IBL/后处理 shader 全集就位，bind group layout 由反射自动生成。）

所有 shader 用 Vulkan 方言 GLSL 4.60 编写，经 SPIR-V 中间表示编译为 MSL：

```
GLSL ── glslang ──▶ SPIR-V ── SPIRV-Cross ──▶ MSL ──▶ Metal 运行时编译
```

编译在进程内完成（`shadercompiler` 模块），构建期校验与运行时热重载共用同一条路径。编译错误分三层结构化捕获：glslang 语法/语义错误、SPIRV-Cross 转换异常、Metal 运行时编译错误（附生成的 MSL）。

## Shader 清单（M4）

| 文件 | 用途 |
|---|---|
| `pbr.vert` / `pbr.frag` | Cook-Torrance PBR（金属-粗糙度工作流），直接光 + IBL 环境光；切线帧由屏幕空间导数构建（cotangent frame），忽略顶点切线 |
| `skybox.vert` / `skybox.frag` | 天空盒：全屏三角形解析反投影出视线方向采样环境 cube，z=0（reversed-Z 远平面）在不透明物体之后绘制 |
| `fullscreen.vert` | `gl_VertexIndex` 全屏三角形（attachment 行 0 对应 NDC y=+1，故 V 翻转） |
| `tonemap.frag` | ACES（Narkowicz）+ linear→sRGB 编码（交换链视图为 Unorm，编码在此完成） |
| `ibl_equirect_to_cube.comp` | 等距柱状 HDR → 环境 cube（逐面 dispatch，face 走 push constant） |
| `ibl_irradiance.comp` | 漫反射辐照度 cube（半球离散积分） |
| `ibl_prefilter.comp` | 镜面预滤波 cube（GGX 重要性采样，roughness/mip 走 push constant，逐 mip 逐面 dispatch） |
| `ibl_brdf_lut.comp` | BRDF 积分 LUT（RG16Float） |

公共代码在 `shaders/include/`（`common.glsl` 帧 uniform 块、`cubemap.glsl` cube 面方向），经 `GL_GOOGLE_include_directive` 引入，shaderc 提供自定义 includer。compute 统一 `local_size` 8×8，workgroup 尺寸经反射流入 `ComputePipelineDesc`。

## 绑定约定

descriptor set 语义固定：

| set | 用途 | 更新频率 |
|---|---|---|
| 0 | 帧数据：view/proj 矩阵、相机位置、光源数组、材质覆盖系数（`include/common.glsl`） | 每帧 |
| 1 | 材质：binding 0–4 贴图（baseColor/metallicRoughness/normal/occlusion/emissive）、5 采样器、6 材质系数 UBO | 每材质 |
| 2 | IBL：binding 0 辐照度 cube、1 预滤波 cube、2 BRDF LUT、3 采样器 | 装载期 |

- Per-draw 小数据（model 矩阵 + normal 矩阵，128 字节）走 `layout(push_constant)`，上限 128 字节；
- **M4 起 renderer 用反射自动生成 `BindGroupLayout`**（ADR 0040），手写声明不再是 shader 绑定的事实来源；
- Metal 侧绑定由 SPIRV-Cross 重映射：buffer/纹理为扁平索引 `set*8+binding`，采样器为 `set*6+binding`（Metal 采样器表仅 16 槽，越界在编译期报错），顶点流占 Metal buffer index 30 向下——写 shader 不需要关心，重映射由编译层完成并有快照测试保障；
- 光源为固定数组：`Light lights[16]` + `int lightCount`，超出 16 视为错误。

## 热重载（开发期）

viewer 以 `FileWatcher`（500ms 轮询）监听 `shaders/` 源文件：变化即重编译全部渲染 shader，全部成功且绑定布局未变才整体替换 pipeline；编译失败输出带行号的结构化错误并保留旧 pipeline，画面不中断。绑定布局变化（增删 binding）需要重启——已创建的 bind group 无法迁移。

`KUMO_SHADER_DIR` 把 shader 源码目录的绝对路径烘进开发期二进制，故构建产物不可重定位；可重定位打包推迟到 M7。

## 测试

CI 对 `shaders/` 全量执行完整编译链（纯 CPU，含 `.comp`），并将反射出的 set/binding 布局、push constant 大小、顶点输入位置、compute workgroup 尺寸与入库快照（`tests/snapshots/*.reflect.txt`）比对，任何绑定布局变化都会在提交粒度暴露。快照更新：`KUMO_UPDATE_SNAPSHOTS=1 ./build/<preset>/tests/kumo_tests` 后人工确认 diff。
