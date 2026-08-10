# 里程碑

每个里程碑对应一个可运行、可验证的状态，完成时打 annotated tag。

| 版本 | 里程碑 | 内容 | 状态 |
|---|---|---|---|
| v0.1.0 | M0 | 工程脚手架：构建系统、核心模块、单测、CI | 完成 |
| v0.2.0 | M1 | macOS Metal 清屏窗口 | 完成 |
| v0.3.0 | M2 | RHI 抽象 + Metal 后端，贴图三角形 + ImGui | 完成 |
| v0.4.0 | M3 | GLSL 交叉编译工具链 + Vulkan 后端 + 热重载 | 完成¹ |
| v0.5.0 | M4 | PBR、glTF 静态场景、IBL、HDR / MSAA / ACES | 完成 |
| v0.6.0 | M5 | 场景助手：LLM 框架 + 场景控制工具 + 聊天面板 | 完成 |
| v0.7.0 | M6 | Shader 助手：生成、自修正、热重载 | 完成 |
| v0.8.0 | M6.5 | MCP server：外部客户端操控 viewer + 截图工具 | 完成 |
| v0.9.0 | M6.75 | SwiftUI 产品界面（场景树 / 视口 / 检查器 / 聊天），macOS 先行 | 完成 |
| v0.9.5 | M6.9 | 场景组装：图元扩充 + 批量创建 + 组合体/散布 | 完成 |
| v0.10.0 | M4.5 | 平行光阴影（PCF shadow map） | 完成 |
| v0.10.5 | M6.95 | 场景质量：程序化天空环境 + 场景验证 + 双助手艺术指导 | 完成 |
| v0.10.6 | M6.97 | 视觉闭环：场景助手截图自评（双协议图片回灌） | 完成 |
| v0.10.8 | M6.98 | 素材管线：真贴图 + glTF 模型库 + 真 HDRI 环境 | 完成 |
| v0.10.9 | M6.99 | 素材自采：asset_fetch 按需下载 CC0 素材（Poly Haven） | 完成 |
| v0.11.0 | MA | 素材库 v2：GLB 双源 + 多文件 glTF + 索引/缩略图 | 完成 |
| v0.11.1 | MP | 摆放约束 v1：贴地/避撞放置 + 碰撞感知 scatter + 重叠验证升级 | 完成 |
| v0.11.2 | MB | 反馈闭环 v2：多视图审图 + 参考图上传 + per-agent 模型配置 | 完成 |
| v0.11.3 | MD | Shader 表面函数：surface_write + 参数滑杆 + recipe 库 | 完成 |
| v0.11.4 | MR | 专用检索：asset_search + material_recipe_search | 完成 |
| v0.11.5 | MC | 导演流水线：SceneSpec + 编排 + 视觉 Critic | — |
| v0.11.6 | ME | 渲染扩容：曝光/白平衡 + spot light + 雾 + bloom | — |

¹ Vulkan 后端随 M4 移除，项目聚焦 Metal；完整双后端实现保留在 v0.4.0。

² M4 之后路线以助手能力为先：阴影（M4.5）后置到产品界面之后。

³ M7 iPad 与 AO/alpha/transmission/clearcoat/顶点位移、参数化构造器（原 M7.0）、MobileCLIP 图像检索一并列入 roadmap，v0.11 冲刺完成后再排期。
