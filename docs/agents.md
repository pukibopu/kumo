# 助手（Agent）

（实现自 M5 开始，本文当前为设计概要。）

viewer 内置两个 LLM 驱动的助手：场景助手（自然语言操控场景）和 shader 助手（自然语言生成/修改材质 shader）。

## 接入配置

连接参数全部外置，代码不写死模型名或端点：

- 配置文件 `kumo.config.json`（参考 `kumo.config.example.json`）：`base_url`（任何兼容 Anthropic Messages API 的端点）、`model`、`max_tokens`、超时；
- API key 读环境变量 `ANTHROPIC_API_KEY`，回退 `.env` 文件；key 不会出现在日志中；
- 两个助手可分别指定模型（`agents.scene.model` / `agents.shader.model`）。

Provider 层是可插拔接口，后续可增加 OpenAI / Ollama 实现。网络层：120s 超时，429/5xx 指数退避重试 2 次。

## 线程模型

LLM 请求在 worker 线程执行；工具回调投递到主线程队列、每帧排空，保证场景修改与渲染帧一致。

## 工具

场景工具：`scene_list`、`scene_add_entity`、`scene_remove_entity`、`scene_set_transform`、`camera_set`、`light_set`、`material_set_param`。变更类工具返回精简结果，全貌查询走 `scene_list`。破坏性操作默认直接执行，可用 `agents.confirm_destructive` 开启确认弹窗。

Shader 工具：`shader_read` / `shader_write`，按目标实体的材质生成独立 fragment shader 副本（只影响该材质）。编译失败时结构化错误作为工具结果返还模型自行修正，上限 5 次；渲染器在新 pipeline 就绪前保留旧 pipeline，失败不影响画面。满意的生成结果保存在 `shaders/generated/`（不入库），人工审阅后可移入 `shaders/examples/`。

## 会话管理

会话历史超过阈值后自动摘要压缩：较早的对话轮由模型压缩为一条状态摘要，最近若干轮保留原文；压缩动作在聊天面板可见。
