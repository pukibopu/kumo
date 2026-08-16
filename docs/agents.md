# 助手（Agent）

（场景助手随 M5 落地，shader 助手随 M6 落地。）

viewer 内置两个 LLM 驱动的助手：场景助手（自然语言增删实体、调整变换/材质/灯光/相机）与 shader 助手（自然语言生成/修改材质 fragment shader，编译错误自修正）。两者在「助手」窗口以页签切换，各自独立会话、可配不同模型端点。

## 接入配置

连接参数全部外置，代码不写死模型名或端点。优先级：环境变量 > `.env` 文件 > 项目根 `kumo.config.json`（参考 `kumo.config.example.json`，本体 gitignored）。

- `provider.type`：`"anthropic"`（Messages API 及兼容中转端点）或 `"openai"`（Chat Completions——OpenAI 官方及 Ollama / LM Studio / llama.cpp 等本地端点的事实标准）；
- `provider.base_url`：缺省按 type 取 `https://api.anthropic.com` 或 `http://127.0.0.1:11434`（OpenAI 云端填 `https://api.openai.com`）；
- **per-agent 端点**（M6）：`agents.scene.*` / `agents.shader.*` 各自可覆盖 `type` / `base_url` / `api_key` / `model`，留空字段按字段继承 `provider.*`——场景/shader 助手可各配一套端点混跑（如 scene 本地 Ollama + shader 云端 GPT），也可全走同一云端；
- 另有 `provider.max_tokens`（模板默认 16384——shader 整文件替换需要大输出预算）、`provider.request_timeout_seconds`、`provider.reasoning_effort`（留空不发送，兼容旧模型与本地端点）；`reasoning_effort` 自 MB 起为 per-agent 第 5 键（`agents.<agent>.reasoning_effort` 覆盖 provider 种子），且带工具的会话在 OpenAI 型端点上由 planSessions 统一强制为 `"none"`（该 API 拒绝函数工具与推理档位并存；无工具的导演/critic 会话保留配置值，MC 里程碑消费）；
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

十八个场景工具（英文 schema 与错误、变更类只回最小确认、全貌查询走 `scene_list`——ADR 0028）：

| 工具 | 说明 |
|---|---|
| `scene_list` | 全场景：实体（id / 变换 / world AABB / 材质 / 图元溯源）+ 相机 + 灯 + 已定义组名 |
| `asset_list` | 只读列出素材库（M6.98 PR-2；v2 见 MA）：`textures/` 下每套贴图的名字与现有贴图种类、模型名（含分类前缀）、`env/*.hdr` 环境文件；assetDir 未配置则报不支持 |
| `asset_search` | 按描述检索素材库（MR）：结构化硬筛选（kind / category / style / max_dimension）∩（FTS ∪ 查询 embedding 余弦）经 RRF 融合，top 3-5 带结构化元数据，前 3 名附缩略图；recipe/spec 条目不出现在此工具；无 index 报错指路 `viewer --index`，无 embedding 降级纯 FTS |
| `scene_add_entity` | 程序化图元 sphere / cube / plane / cylinder / cone / torus / capsule，返回 entity_id 与 world AABB；缺省材质为非金属灰（metallic 0 / roughness 0.6）；摆放参数见下（MP） |
| `scene_add_entities` | 批量创建（单次 ≤128），先全量校验后落地，中途 GPU 失败整体回滚 |
| `scene_add_model` | 从素材库放置真实 glTF 模型（名字来自 `asset_list`，可带一层分类前缀如 `survival/barrel`），返回每个 mesh 节点对应的 entity_id 与聚合 world AABB；根缩放限均匀；摆放参数见下（MP） |
| `scene_remove_entity` | 删除实体（destructive） |
| `scene_set_transform` | 位置 / 旋转（欧拉角度数）/ 缩放，缺省字段保持现值 |
| `camera_set` | 位置 / look_at / fov / near |
| `light_set` | 修改指定 index 或省略 index 追加新灯（上限 16） |
| `light_remove` | 按 index 删灯（destructive）；后续 index 前移，响应回显剩余灯全表 |
| `material_set_param` | 实体材质因子（metallic / roughness 截取到 0-1）；共享材质就地修改并报告波及实体数 |
| `material_set_texture` | 把素材库的一套真实贴图（名字来自 `asset_list`）绑定到实体材质，可选 UV tiling；按贴图套名缓存上传，同名贴图多个实体只传一次；共享材质同 `material_set_param` 语义 |
| `asset_fetch` | 素材自采（M6.99；模型见 MA）：按英文短语从 Poly Haven（CC0）现取一套贴图、一张 HDR 环境或一个 glTF 模型到素材库，成功后名字即可像库内素材一样喂给 `material_set_texture` / `environment_set` / `scene_add_model`；无匹配时错误信息带近似候选名 |
| `scene_define_group` | 定义可复用组合体（≤32 成员），只存不落场景 |
| `scene_instance_group` | 按显式变换或散布（count / area / seed，确定性）实例化组；实例×成员 ≤256，实例缩放限均匀；scatter 支持 `min_spacing`（实例 XZ 边距）、`avoid_existing`（避开现有实体）、`max_attempts`（采样预算，缺省 max(10×count, 64)），放不满则整体失败零落地并回报 requested / accepted / area / min_spacing（MP） |
| `environment_set` | 真实 HDR 文件（名字来自 `asset_list`）或程序化天空环境：预设 clear_day / sunset / overcast / night / studio + 逐字段覆盖，重烘焙 IBL；file 与预设/逐字段参数互斥；可撤销、随场景持久化 |
| `scene_validate` | 只读自检：悬浮 / AABB 穿插 / 相机在几何内 / 视锥外实体 / 灯光失衡，只报告不修改；overlap 发现带 `other_entity_id` / `overlap_depth` / `overlap_ratio`，支撑接触（垂直堆叠界面处 ≤2cm 的 Y 向浅穿透）info、其余穿插一律 warning（薄壁侧埋不因最小轴浅而豁免）、≤1mm 忽略，单对只报一次；同一装配体内部（同次 scene_add_model / 同一组实例）的穿插视为有意设计不报（MP） |

**摆放约束**（MP）：`scene_add_entity` / `scene_add_model` 共享三个可选参数——`snap_to_ground`（缺省 false：按候选 AABB 把底面落到离地 `clearance` 高度，不假设 pivot 在底部，返回修正后 position）、`clearance`（缺省 0.01m，只作贴地间隙；碰撞/支撑容差固定 0.02m 不受其影响，防止大 clearance 把真穿插洗成支撑接触）、`avoid_overlap`（缺省 false：落地前用候选 AABB 对全场景做碰撞预检，冲突则在任何 scene/renderer 变更前整体拒绝，报 `conflicting_entity_ids` / `overlap_depth` / `requested_position` 与确定性环形搜索得到的 `suggested_position`；模型未上传过时先 CPU 解析 glTF 算包围盒，预检通过才上传，被拒不留任何 GPU 资源）。候选包围盒全部在落地前于 CPU 侧算出（图元用 `makePrimitive` 局部 AABB，模型用网格 AABB 按节点聚合），共享数学在 `engine/agent/src/placement.{h,cpp}`。「支撑接触」定义收紧为垂直堆叠界面处的浅 Y 向穿透（≤容差且一方底面贴另一方顶面），薄壁侧埋不因最小轴穿透浅而豁免。同次 `scene_add_model` 的全部节点与 `scene_instance_group` 的每个实例共享一个 assembly id（随场景保存），装配体内部穿插不计入 `scene_validate`。

- entity id 为 `"index:generation"` 字符串，stale id 得到干净的 not-found；
- 变更工具全部 staged 原子提交：任一字段校验失败则零副作用；非有限数值、零/负缩放、零向量光方向、越界辐照参数在工具层拦截；
- 错误一律结构化 `{"status":"error","message":"..."}` 回灌模型自行修正；
- 破坏性操作默认直接执行，`agents.confirm_destructive`（或 `--confirm-destructive`）开启中文确认弹窗，拒绝以 `{"status":"cancelled_by_user"}` 返还模型；
- 单轮工具轮数上限 `agents.max_tool_rounds`（默认 24，最低 2——末轮不执行工具）。

**工具面按助手收窄**：场景助手持有上表十七个工具 + `viewer_screenshot`（离屏渲染降采样 PNG，结果附图）；shader 助手持有 `scene_list` + shader 工具组（MD 起 6 个：读/表面写/参数/recipe×2/整文件写）+ `viewer_screenshot`（写完编译通过后自查改完的材质）——本地小模型碰不到 shader 写入，shader 模型删不了实体。

**多视图审图**（MB）：`viewer_screenshot` 接受 `views`（1-4 个：`main` 成品帧 / `clay` 平光素模看形体构图 / `normal` 世界法线 / `depth` 视深灰度近白远黑，缺省 `["main"]`）、`long_side`（64-1024，缺省 640）、`detail`（`low`/`high`，控制发给模型的图像精细档）；结果带 `image_paths` 数组（`image_path` 保留兼容单图消费者），session 与 MCP 都按数组全量附图，文件名带 pid + 调用序号 + 视图名不互相覆盖。调试视图由 renderer 的三条共享布局管线绘制（custom shader 材质也被强制走调试管线，critic 检查形体不受风格化干扰），截图后恢复原状态。

**参考图上传**（MB）：app 聊天输入区支持附加最多 3 张 png/jpeg（文件选择或剪贴板粘贴）+ 低/高画质档位；桥接层读文件转 base64 交 `AgentSession::submit` 的带图重载，双 codec 以 user 消息 content parts 发送（OpenAI 带 detail 档位，Anthropic image blocks）。参考图长驻会话历史（不参与工具截图的"最新一张"逐出策略），prompt 指示助手对照参考图核对氛围、配色与构图。

**视觉闭环**（M6.97）：场景助手建完场景后截图自评（构图/曝光/比例），修正后最多补一张确认图，随后必须交付。工具结果带 `image_path` 时会话层自动读文件转 base64 附进消息：OpenAI 协议经紧随的 user 图片消息回灌（`detail:"low"`），Anthropic 原生 tool_result 带图；历史中最多保留一张图（新图逐出旧图），压缩估算按每图固定 512 token 计。**需要视觉模型**（如 GPT-4o 系及以上）；非视觉模型调用截图工具会得到端点报错。

**艺术指导**：场景助手的 system prompt 内嵌成稿流程（主题 → 主体 → 前中后景 → 相机 → 布光 → 材质 → 素材）、默认丰富度底线（地面 + 主体 + 支撑元素 + 匹配环境 + 分层布光 + 材质区分 + 贴图化地面/结构物）、按包围盒计算取景、三点布光配方、素材优先原则（真实模型/贴图优于程序化图元与纯色材质，调用 `material_set_texture` 时按贴图实际尺寸估算 tiling）与验证闭环（建完调 `scene_validate` 修完再回复，截图后按构图/焦点/层次/材质/光照/细节六点评审，形体或空间问题用 clay/normal/depth 视图诊断，有参考图则对照氛围配色构图）；shader 助手内嵌材质意图词表（拉丝金属 / 磨砂玻璃近似 / 混凝土等 → 因子组合与手法）与两助手职责边界（场景助手管摆放与 PBR 因子，shader 助手管因子表达不了的效果）。

**素材库**（M6.98 PR-2）：`assetDir` 下 `textures/<name>/{albedo,normal,roughness,metalness,ao}.{png,jpg}`（albedo 必需，其余可选）、`models/<name>.glb`（或多文件 glTF 布局，见 MA）、`env/<name>.hdr` 三类真实素材，供 `asset_list` 枚举、`material_set_texture` / `scene_add_model` / `environment_set`（`file` 字段）消费；除仓库自带的 `DamagedHelmet.glb` 与 `studio_small_09_2k.hdr` 外均为本地拉取产物不入库，首次使用前跑 `./tools/fetch_assets.sh`（幂等、单项失败不中断，全部 CC0，来源见 `assets/README.md`）；`assetDir` 未配置时三个素材工具统一报不支持，与 `environment_set` 无 `applyEnvironment` 回调时的降级方式一致。

**素材自采**（M6.99）：素材库找不到合适的贴图/环境时，场景助手会调用 `asset_fetch` 现从 Poly Haven（无需鉴权、纯 CC0）按查询词找一个最匹配的素材下载进素材库，同名目录/文件已存在则直接判定已取无需联网；查无匹配时错误信息带最多 3 个近似候选名供改写查询词重试。下载在调用工具的主线程同步执行（与其余工具一致的执行模型）——一张 2k HDR（约 5-15MB）可能卡住画面数秒；异步工具是后续里程碑的事。

**素材库 v2**（MA）：模型名可带一层分类前缀（`<category>/<name>`，`kumo::isPlainAssetPath`），解析顺序 `models/<name>.glb` → `models/<name>/<name>.gltf` → `models/<name>/scene.gltf` → 同三种布局套一层分类目录（`asset::resolveModelPath`），首个存在的文件生效；`asset_fetch` 的 `kind:"model"` 从 Poly Haven 下载多文件 glTF 包，整理为 `models/<id>/scene.gltf` + 相对路径贴图/`.bin`；`tools/fetch_assets.sh` 的 `fetch_pack` 拉取 Kenney 等分类风格化道具包（每分类一个 `pack.json`：`category`/`style`/`source`/`license`）。`viewer --thumbnails`（离屏、无窗口）为模型/贴图套/环境各渲染一张 256px 预览图到 `assets/.thumbnails/`，并（重）写 `assets/index.json`（含分类/风格/尺寸/三角形数等摘要）；`asset_list` 始终扫目录枚举实际存在的素材（目录才是真相源，避免索引滞后于 `asset_fetch`/手动拷贝新增的素材），`index.json` 存在时仅作元数据叠加（按 id 匹配补充分类/风格/尺寸等字段，磁盘上没有的条目不出现，索引没有的条目不带额外字段）；两者均为生成产物不入库，单个模型加载失败只记日志跳过，不中断整批。曾纳入的 Kenney Nature Kit 因上游 UniGLTF 导出缺陷（`cgltf` 全量拒绝解析）已整包移除，见 `assets/README.md`。

**Model-first 政策**（MS）：可见道具必须用真实库模型（`asset_search` → `scene_add_model`，其次 `asset_fetch`），图元只允许做建筑基元（地面/墙体/平台/水面），库中确无匹配才允许图元组合并须在回复中说明；`scene_validate` 新增 `primitive_heavy` 检查（非 plane 图元实体 >5 个且占形体实体过半 → warning 指路 asset_search）；导演 spec 的 elements 要求引用库资产名，critic 把「图元拼凑感」计为 build issue。支撑该政策的库容量：9 个 Kenney 分类包约 845 个模型（见 assets/README.md，含手动包通道接 Quaternius/KayKit）。

**零成本本地配置**（MS）：检索链路可整体指向本地 OpenAI 兼容端点（Ollama/LM Studio），与云端 agent 自由混搭——`retrieval.base_url`（本地免 key）+ `retrieval.embedding_model`（如 nomic-embed-text，换模型后 `--index` 全量重嵌，本地免费）+ 可选 `retrieval.caption_model`（本地视觉模型出缩略图 caption，未设则沿用 agent 端点）。agent 角色同理按块混搭（如 `agents.director/critic` 指本地小模型、场景/shader 留云端）。实测参考（Apple M2 + Ollama）：870 条目 nomic-embed-text 全量重嵌约 30 秒；英文语义查询命中与云端 embedding 相当，中文查询是本地小 embedding 模型弱项——prompt 本就引导 agent 用英文检索词。示例：

```json
"retrieval": {"embedding_model": "nomic-embed-text", "base_url": "http://127.0.0.1:11434"},
"agents": {"director": {"type": "openai", "base_url": "http://127.0.0.1:11434", "model": "qwen2.5:14b"},
           "critic":   {"type": "openai", "base_url": "http://127.0.0.1:11434", "model": "qwen2.5:14b"}}
```

**专用检索**（MR）：`viewer --index` 是 `--thumbnails` 的超集——除模型/贴图/环境外，还把 shader recipe（标准球实渲预览图）与 `assets/specs/*.json` 场景模板（人工模板，导演流水线的基调参考）纳入 `assets/index.json`，为每张缩略图经配置的视觉模型生成一句话 caption（`--no-captions` 跳过；重跑只为变化的条目付费），并把 `caption+名字+tags+分类` 文本经 OpenAI 兼容端点 `/v1/embeddings` 批量向量化（64 条/请求，模型走 `retrieval.embedding_model`，默认 text-embedding-3-small）写进 sidecar `index_embeddings.bin`（float32 连续行，条目存行号）；index 与 sidecar 均临时文件+rename 原子落盘，文本未变的条目复用旧向量零请求。查询时 `asset_search` / `material_recipe_search` 共享一个 mtime 失效的进程内缓存：硬筛选 → FTS（id/名字 > tags > caption > 分类分层计分）与查询向量余弦各自排序 → RRF（k=60）融合 → top 3-5，检索核心为纯函数（`engine/agent/src/asset_search.{h,cpp}`）全 fixture 可测。降级链条确定性：无 embedding 端点/离线 → 纯 FTS+筛选；无 index → 报错指路 `--index`（`asset_list` / `recipe_list` 始终可用）。**反目标**（固化）：不索引 `shaders/generated/`、不做文档知识库、检索结果永不含 shader 源码、候选封顶 5。

Shader 工具（M6）：

| 工具 | 说明 |
|---|---|
| `shader_read` | 按实体取其材质当前生效的完整 fragment 源码（未定制时返回 pbr 模板） |
| `surface_write` | **默认轨**（MD）：只提交 `void kumoSurface(inout SurfaceOutputs s, in SurfaceInputs i)` 表面函数 + 命名参数（float/vec4 ≤16 个、块 ≤192B），引擎拼进 `pbr_surface_template.frag` 的标准光照壳；禁 uniform/main/layout/while/do/`frame.`/gl_FragCoord（越界报错指路 `shader_write_full`）；编译错误行号回映到函数内（`function_line`），拼接区外标 `template_error` |
| `shader_set_param` | 免重编译改一个表面参数值；随 undo/存档走，Inspector 有对应滑杆/取色器 |
| `recipe_list` | 列 recipe 库（`shaders/recipes/`）：名字/描述/参数 schema/tags/cost，**不含源码** |
| `material_recipe_search` | 按描述检索 recipe 库（MR）：与 `asset_search` 同一混合检索核心（kind=recipe 预筛），结果含参数 schema/适用几何/成本与效果缩略图，**同样不含源码**——应用一律走 `shader_apply_recipe` |
| `shader_apply_recipe` | 应用 recipe（首发 wood_grain / brushed_metal / rust / marble / emissive_pulse）并可覆盖参数，与 `surface_write` 同一拼接管线 |
| `shader_write_full` | **高级轨**：整文件替换该材质的 fragment shader（ADR 0029），只影响该材质（ADR 0011）；仅限自定义光照/全风格化或用户点名；`shader_write` 为保留一版的兼容别名 |

- 参数存储：拼接参数以 std140 追加在 `MaterialFactors` 64B 前缀之后（偏移引擎权威计算），值经 `flushDirtyMaterials` 的第二段写入下发；随 undo 快照与场景存档（`surface_params` 按名字+类型持久化，偏移载入时重算）；
- 编译失败时结构化错误（file / line / `second_stage` 阶段路由）作为工具结果返还模型自行修正，每材质连续失败上限 5 次后要求停下向用户说明；渲染器在新 pipeline 就绪前保留旧 pipeline，失败不影响画面；
- `viewer --check-shaders` 批量编译 `shaders/generated/` 全部产物，模板/ABI 变更的兼容性一次性验证（recipe 的渲染健康由 golden 二进制的 NaN 台覆盖：标准球 × 每 recipe × 默认+极值参数，断言非黑帧）；
- 绑定契约双重防线（ADR 0029）：system prompt 嵌入约定（set 0/2 与模板逐字节一致、set 1 只允许在 `MaterialFactors` 尾部追加成员、push constant 不可动），反射兼容校验在引擎侧强制（不兼容同样以编译错误形态回灌）；
- 成功的生成结果落盘 `shaders/generated/material_<N>.frag`（gitignored），人工审阅后可移入 `shaders/examples/`；
- 编译在主线程串行执行（`compileGlsl` 非线程安全，工具本就经主线程队列执行，天然满足）。

## 导演流水线（MC）

App 聊天面板第三段「导演」：一句话 brief + 档位（标准/精修）→ 编排器驱动多 agent 流水线自动出片。角色配置在 `agents.director` / `agents.critic`（缺省逐字段继承 `provider`；两者均为**无工具会话**，不受 OpenAI tools+reasoning 互斥限制，可开高推理）。状态机：`Directing → Building → Materials → Critique →（RepairBuild → RepairMaterials → Critique）× N → Done/Failed/Cancelled`，修复轮预算标准 1 / 精修 3。

- **导演**产出 SceneSpec（style_tier/palette/camera/assets/elements[build+material_intent]/lighting/banned/budgets 的单 JSON 对象，`engine/facade/scene_spec.{h,cpp}` 宽容解析：剥栅栏找配平大括号，解析失败一次纠偏重提）；上下文注入 asset_list 快照与检索到的 top-2 spec 模板（`assets/specs/`，仅作基调参考）；接受的 spec 原文随场景存档（`director_spec`）
- **搭建/材质**阶段把 spec 按阶段切片作为前缀 user 消息交给场景/shader 会话（session prompt 不动，转录自带可观察性）；**评审**由编排器按轮次表确定性截图（r1: main/640/low；r2+: main+clay+normal/1024/high）经参考图通道喂给 critic，verdict JSON（pass/revise + issues[target/severity]）路由修复：lighting/camera/layout 归 RepairBuild、材质归 RepairMaterials
- **回合协议**：每阶段提交前记 `expected = completedTurns()+1`（`AgentSession` 的非破坏访问器，MC-1），发现多余回合即外部注入 → 重提阶段指令；流水线运行期间双页签聊天输入禁用、`reloadAgentSessions` 拒绝
- **降级矩阵**：无 director → brief 直建；无 shader → 跳过材质阶段；无 critic → 搭完即 Done；critic verdict 两次不可解析 → 按现状交付；软取消在当前阶段完成后生效

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

`viewer --mcp` 经 stdio 提供 MCP 端点：外部 MCP 客户端与内嵌助手消费同一工具注册表，工具语义单源（ADR 0041）。工具面 = 场景十七工具 + shader 双工具 + `viewer_screenshot`（离屏渲染当前场景，结果附 PNG 图像，供视觉验证）；该模式下日志全部走 stderr，stdout 只承载 JSON-RPC。

接入示例：

```sh
claude mcp add kumo -- "$(pwd)/build/macos-debug/apps/viewer/viewer" --mcp
```
