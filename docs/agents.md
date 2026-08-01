# 助手（Agent）

（场景助手随 M5 落地，shader 助手随 M6 落地。）

viewer 内置两个 LLM 驱动的助手：场景助手（自然语言增删实体、调整变换/材质/灯光/相机）与 shader 助手（自然语言生成/修改材质 fragment shader，编译错误自修正）。两者在「助手」窗口以页签切换，各自独立会话、可配不同模型端点。

## 接入配置

连接参数全部外置，代码不写死模型名或端点。优先级：环境变量 > `.env` 文件 > 项目根 `kumo.config.json`（参考 `kumo.config.example.json`，本体 gitignored）。

- `provider.type`：`"anthropic"`（Messages API 及兼容中转端点）或 `"openai"`（Chat Completions——OpenAI 官方及 Ollama / LM Studio / llama.cpp 等本地端点的事实标准）；
- `provider.base_url`：缺省按 type 取 `https://api.anthropic.com` 或 `http://127.0.0.1:11434`（OpenAI 云端填 `https://api.openai.com`）；
- **per-agent 端点**（M6）：`agents.scene.*` / `agents.shader.*` 各自可覆盖 `type` / `base_url` / `api_key` / `model`，留空字段按字段继承 `provider.*`——场景/shader 助手可各配一套端点混跑（如 scene 本地 Ollama + shader 云端 GPT），也可全走同一云端；
- 另有 `provider.max_tokens`、`provider.request_timeout_seconds`；
- API key：`provider.api_key`（或 per-agent 的 `api_key`），或环境变量 `KUMO_PROVIDER_API_KEY` / `ANTHROPIC_API_KEY`（anthropic 型端点）/ `OPENAI_API_KEY`（openai 型端点）。解析按端点各自进行、层级优先：进程环境的任一名字 > `.env` 的任一名字 > 配置文件；key 永不落日志。**openai 型且 host 为本机时 key 可空**（Ollama 不校验）；
- 环境变量覆盖（作用于全局 provider，per-agent 覆盖在其上生效）：`KUMO_PROVIDER_TYPE` / `KUMO_PROVIDER_BASE_URL` / `KUMO_PROVIDER_MODEL`；
- 某个助手缺 model/key 时仅该页签禁用并给出中文提示，渲染功能完全不受影响。

快速起步：`cp kumo.config.example.json kumo.config.json`（模板默认两个助手都走 OpenAI 云端，改 `provider.model` 为你可用的型号、key 放 `.env` 的 `OPENAI_API_KEY`）；或零配置本地模型：

```sh
KUMO_PROVIDER_TYPE=openai KUMO_PROVIDER_MODEL=qwen2.5:14b ./build/macos-debug/apps/viewer/viewer
```

网络层：单请求 120s 超时（可配）；429/5xx/连接失败指数退避重试 2 次（1s→4s 含抖动），4xx 立即失败并透出端点错误信息；重试期间聊天面板状态行显示「网络波动，重试中 (n/2)…」。

## 线程模型

LLM 往返在 session 专属 worker 线程执行；工具回调经 `MainThreadQueue` 投递主线程、每帧事件轮询后排空，场景修改对渲染帧原子。worker 全程不持引擎锁；session 析构以 abort 标志令 worker 自行解锁并 join，不在共享的队列/确认门上留下任何终态。

## 工具

十三个场景工具（英文 schema 与错误、变更类只回最小确认、全貌查询走 `scene_list`——ADR 0028）：

| 工具 | 说明 |
|---|---|
| `scene_list` | 全场景：实体（id / 变换 / world AABB / 材质 / 图元溯源）+ 相机 + 灯 + 已定义组名 |
| `scene_add_entity` | 程序化图元 sphere / cube / plane / cylinder / cone / torus / capsule，返回 entity_id；缺省材质为非金属灰（metallic 0 / roughness 0.6） |
| `scene_add_entities` | 批量创建（单次 ≤128），先全量校验后落地，中途 GPU 失败整体回滚 |
| `scene_remove_entity` | 删除实体（destructive） |
| `scene_set_transform` | 位置 / 旋转（欧拉角度数）/ 缩放，缺省字段保持现值 |
| `camera_set` | 位置 / look_at / fov / near |
| `light_set` | 修改指定 index 或省略 index 追加新灯（上限 16） |
| `light_remove` | 按 index 删灯（destructive）；后续 index 前移，响应回显剩余灯全表 |
| `material_set_param` | 实体材质因子（metallic / roughness 截取到 0-1）；共享材质就地修改并报告波及实体数 |
| `scene_define_group` | 定义可复用组合体（≤32 成员），只存不落场景 |
| `scene_instance_group` | 按显式变换或散布（count / area / seed，确定性）实例化组；实例×成员 ≤256，实例缩放限均匀 |
| `environment_set` | 程序化天空环境：预设 clear_day / sunset / overcast / night / studio + 逐字段覆盖，重烘焙 IBL；可撤销、随场景持久化 |
| `scene_validate` | 只读自检：悬浮 / AABB 穿插 / 相机在几何内 / 视锥外实体 / 灯光失衡，只报告不修改 |

- entity id 为 `"index:generation"` 字符串，stale id 得到干净的 not-found；
- 变更工具全部 staged 原子提交：任一字段校验失败则零副作用；非有限数值、零/负缩放、零向量光方向、越界辐照参数在工具层拦截；
- 错误一律结构化 `{"status":"error","message":"..."}` 回灌模型自行修正；
- 破坏性操作默认直接执行，`agents.confirm_destructive`（或 `--confirm-destructive`）开启中文确认弹窗，拒绝以 `{"status":"cancelled_by_user"}` 返还模型；
- 单轮工具轮数上限 `agents.max_tool_rounds`（默认 24，最低 2——末轮不执行工具）。

**工具面按助手收窄**：场景助手持有上表十三个工具 + `viewer_screenshot`（离屏渲染降采样 PNG，结果附图）；shader 助手持有 `scene_list` + 下面两个 shader 工具——本地小模型碰不到 shader_write，shader 模型删不了实体。

**视觉闭环**（M6.97）：场景助手建完场景后截图自评（构图/曝光/比例），修正后最多补一张确认图，随后必须交付。工具结果带 `image_path` 时会话层自动读文件转 base64 附进消息：OpenAI 协议经紧随的 user 图片消息回灌（`detail:"low"`），Anthropic 原生 tool_result 带图；历史中最多保留一张图（新图逐出旧图），压缩估算按每图固定 512 token 计。**需要视觉模型**（如 GPT-4o 系及以上）；非视觉模型调用截图工具会得到端点报错。

**艺术指导**：场景助手的 system prompt 内嵌成稿流程（主题 → 主体 → 前中后景 → 相机 → 布光 → 材质）、默认丰富度底线（地面 + 主体 + 支撑元素 + 匹配环境 + 分层布光 + 材质区分）、按包围盒计算取景、三点布光配方与验证闭环（建完调 `scene_validate` 修完再回复）；shader 助手内嵌材质意图词表（拉丝金属 / 磨砂玻璃近似 / 混凝土等 → 因子组合与手法）与两助手职责边界（场景助手管摆放与 PBR 因子，shader 助手管因子表达不了的效果）。

Shader 工具（M6）：

| 工具 | 说明 |
|---|---|
| `shader_read` | 按实体取其材质当前生效的完整 fragment 源码（未定制时返回 pbr 模板） |
| `shader_write` | 整文件替换该材质的 fragment shader（ADR 0029），只影响该材质（ADR 0011） |

- 编译失败时结构化错误（file / line / `second_stage` 阶段路由）作为工具结果返还模型自行修正，每材质连续失败上限 5 次后要求停下向用户说明；渲染器在新 pipeline 就绪前保留旧 pipeline，失败不影响画面；
- 绑定契约双重防线（ADR 0029）：system prompt 嵌入约定（set 0/2 与模板逐字节一致、set 1 只允许在 `MaterialFactors` 尾部追加成员、push constant 不可动），反射兼容校验在引擎侧强制（不兼容同样以编译错误形态回灌）；
- 成功的生成结果落盘 `shaders/generated/material_<N>.frag`（gitignored），人工审阅后可移入 `shaders/examples/`；
- 编译在主线程串行执行（`compileGlsl` 非线程安全，工具本就经主线程队列执行，天然满足）。

## 会话管理

历史超过 `agents.summary_threshold_tokens`（默认 8000，粗略 bytes/4 估算）后自动压缩：较早消息由模型压缩为一条状态摘要（保留 entity_id、名称、位置等关键事实），最近若干条保留原文；压缩切点绝不落在 tool_use 与其 tool_result 之间；压缩失败降级为不压缩。压缩动作在聊天面板以提示行可见。

## 面板与离线模式

- 聊天面板（ImGui，开发面板定位，产品 GUI 见 ADR 0044）：「场景 / Shader」页签各对应一个会话，回车发送、状态行（思考中 / 执行工具 / 等待确认 / 重试提示）；两个会话的转录每帧都排空，与活动页签无关；
- 工具日志面板：完整不截断记录两个助手的每次工具调用与结果（ADR 0022）；
- 破坏性确认（ADR 0022）：两个会话共用一个确认门，请求排队、弹窗一次显示一个、每个决定只落到发起它的调用；
- `--offline`：内置脚本回放（加金属球 → 移动 → 改红色粗糙 → 拉相机 → 调光，再发一条演示删除确认流程），零网络零配置，用于开发与验收。

## 场景持久化

viewer 内 **K** 存 / **L** 读工作目录的 `kumo_scene.json`：实体 TRS + 图元溯源（primitive/size）+ 材质因子 + 灯 + 相机；glTF 模型按路径引用不内嵌；顶层 `version` 字段标注格式不稳定（ADR 0016）。`scene_save` / `scene_load` agent 工具默认不开放。

## MCP server（M6.5）

`viewer --mcp` 经 stdio 提供 MCP 端点：外部 MCP 客户端与内嵌助手消费同一工具注册表，工具语义单源（ADR 0041）。工具面 = 场景十三工具 + shader 双工具 + `viewer_screenshot`（离屏渲染当前场景，结果附 PNG 图像，供视觉验证）；该模式下日志全部走 stderr，stdout 只承载 JSON-RPC。

接入示例：

```sh
claude mcp add kumo -- "$(pwd)/build/macos-debug/apps/viewer/viewer" --mcp
```
