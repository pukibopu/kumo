# 助手（Agent）

（场景助手随 M5 落地；shader 助手部分为 M6 设计概要。）

viewer 内置 LLM 驱动的场景助手：自然语言增删实体、调整变换/材质/灯光/相机。M6 将加入 shader 助手（自然语言生成/修改材质 shader）。

## 接入配置

连接参数全部外置，代码不写死模型名或端点。优先级：环境变量 > `.env` 文件 > 项目根 `kumo.config.json`（参考 `kumo.config.example.json`，本体 gitignored）。

- `provider.type`：`"anthropic"`（默认，Messages API 及兼容中转端点）或 `"openai"`（Chat Completions——Ollama / LM Studio / llama.cpp 等本地端点的事实标准）；
- `provider.base_url`：缺省按 type 取 `https://api.anthropic.com` 或 `http://127.0.0.1:11434`；
- 模型：`agents.scene.model` 优先，回退 `provider.model`；另有 `provider.max_tokens`、`provider.request_timeout_seconds`；
- API key：`provider.api_key`，或环境变量 `KUMO_PROVIDER_API_KEY` / `ANTHROPIC_API_KEY`（anthropic 型）。解析层级优先：进程环境的任一名字 > `.env` 的任一名字 > 配置文件；key 永不落日志。**openai 型且 host 为本机时 key 可空**（Ollama 不校验）；
- 环境变量覆盖：`KUMO_PROVIDER_TYPE` / `KUMO_PROVIDER_BASE_URL` / `KUMO_PROVIDER_MODEL` / `KUMO_PROVIDER_API_KEY`；
- 未配置 model/key 时聊天面板禁用并给出中文提示，渲染功能完全不受影响。

本地模型一行起步（Ollama）：

```sh
KUMO_PROVIDER_TYPE=openai KUMO_PROVIDER_MODEL=qwen2.5:14b ./build/macos-debug/apps/viewer/viewer
```

网络层：单请求 120s 超时（可配）；429/5xx/连接失败指数退避重试 2 次（1s→4s 含抖动），4xx 立即失败并透出端点错误信息；重试期间聊天面板状态行显示「网络波动，重试中 (n/2)…」。

## 线程模型

LLM 往返在 session 专属 worker 线程执行；工具回调经 `MainThreadQueue` 投递主线程、每帧事件轮询后排空，场景修改对渲染帧原子。worker 全程不持引擎锁；session 析构以 abort 标志令 worker 自行解锁并 join，不在共享的队列/确认门上留下任何终态。

## 工具

七个场景工具（英文 schema 与错误、变更类只回最小确认、全貌查询走 `scene_list`——ADR 0028）：

| 工具 | 说明 |
|---|---|
| `scene_list` | 全场景：实体（id / 变换 / world AABB / 材质 / 图元溯源）+ 相机 + 灯 |
| `scene_add_entity` | 程序化图元 sphere / cube / plane，返回 entity_id |
| `scene_remove_entity` | 删除实体（唯一 destructive 工具） |
| `scene_set_transform` | 位置 / 旋转（欧拉角度数）/ 缩放，缺省字段保持现值 |
| `camera_set` | 位置 / look_at / fov / near |
| `light_set` | 修改指定 index 或省略 index 追加新灯（上限 16） |
| `material_set_param` | 实体材质因子；共享材质就地修改并报告波及实体数 |

- entity id 为 `"index:generation"` 字符串，stale id 得到干净的 not-found；
- 变更工具全部 staged 原子提交：任一字段校验失败则零副作用；非有限数值、零/负缩放、零向量光方向在工具层拦截；
- 错误一律结构化 `{"status":"error","message":"..."}` 回灌模型自行修正；
- 破坏性操作默认直接执行，`agents.confirm_destructive`（或 `--confirm-destructive`）开启中文确认弹窗，拒绝以 `{"status":"cancelled_by_user"}` 返还模型。

Shader 工具（M6 设计）：`shader_read` / `shader_write`，按目标实体的材质生成独立 fragment shader 副本（只影响该材质）。编译失败时结构化错误作为工具结果返还模型自行修正，上限 5 次；渲染器在新 pipeline 就绪前保留旧 pipeline。满意的生成结果保存在 `shaders/generated/`（不入库），人工审阅后可移入 `shaders/examples/`。

## 会话管理

历史超过 `agents.summary_threshold_tokens`（默认 8000，粗略 bytes/4 估算）后自动压缩：较早消息由模型压缩为一条状态摘要（保留 entity_id、名称、位置等关键事实），最近若干条保留原文；压缩切点绝不落在 tool_use 与其 tool_result 之间；压缩失败降级为不压缩。压缩动作在聊天面板以提示行可见。

## 面板与离线模式

- 聊天面板（ImGui，开发面板定位，产品 GUI 见 ADR 0044）：回车发送、状态行（思考中 / 执行工具 / 等待确认 / 重试提示）；
- 工具日志面板：完整不截断记录每次工具调用与结果（ADR 0022）；
- `--offline`：内置脚本回放（加金属球 → 移动 → 改红色粗糙 → 拉相机 → 调光，再发一条演示删除确认流程），零网络零配置，用于开发与验收。

## 场景持久化

viewer 内 **K** 存 / **L** 读工作目录的 `kumo_scene.json`：实体 TRS + 图元溯源（primitive/size）+ 材质因子 + 灯 + 相机；glTF 模型按路径引用不内嵌；顶层 `version` 字段标注格式不稳定（ADR 0016）。`scene_save` / `scene_load` agent 工具默认不开放。
