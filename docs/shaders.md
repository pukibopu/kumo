# Shader

（工具链自 M3 开始落地，本文当前为设计概要与约定。）

所有 shader 用 Vulkan 方言 GLSL 4.60 编写一份，交叉编译到两个后端：

```
GLSL ── glslang ──▶ SPIR-V ──┬──▶ Vulkan（直接使用）
                             └── SPIRV-Cross ──▶ MSL ──▶ Metal 运行时编译
```

编译在进程内完成（`shadercompiler` 模块），构建期校验与运行时热重载共用同一条路径。编译错误分三层结构化捕获：glslang 语法/语义错误、SPIRV-Cross 转换异常、Metal 运行时编译错误（附生成的 MSL）。

## 绑定约定

descriptor set 语义固定：

| set | 用途 | 更新频率 |
|---|---|---|
| 0 | 帧数据：view/proj 矩阵、相机位置、光源数组 | 每帧 |
| 1 | 材质：贴图、采样器、材质参数 | 每材质 |
| 2 | 保留给 per-draw 大数据 | 每 draw |

- Per-draw 小数据（model 矩阵等）走 `layout(push_constant)`，上限 128 字节；
- Metal 侧绑定由 SPIRV-Cross 重映射为扁平索引 `set * 8 + binding`，顶点流占 Metal buffer index 30 向下——写 shader 不需要关心，重映射由编译层完成并有快照测试保障；
- 光源为固定数组：`Light lights[16]` + `int lightCount`，超出 16 视为错误。

## 测试

CI 对 `shaders/` 全量执行完整交叉编译链（纯 CPU），并将反射出的 set/binding 布局、push constant 大小与入库 JSON 快照比对，任何绑定布局变化都会在提交粒度暴露。快照更新走显式命令并人工确认 diff。
