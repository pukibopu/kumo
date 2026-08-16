# 资产来源与许可

| 文件 | 来源 | 许可 |
|---|---|---|
| `models/DamagedHelmet.glb` | [Khronos glTF-Sample-Assets](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/DamagedHelmet)，作者 theblueturtle_ / ctxwing | CC BY-NC 4.0 |
| `env/studio_small_09_2k.hdr` | [Poly Haven](https://polyhaven.com/a/studio_small_09)，作者 Oliksiy Yakovlyev | CC0 |

DamagedHelmet 的 CC BY-NC 4.0 为非商业许可，仅供演示与开发使用，不得纳入任何商业分发。

## 扩展资产库（`tools/fetch_assets.sh`）

`assets/textures/`、`assets/models/` 下除 `DamagedHelmet.glb`、`assets/env/` 下除 `studio_small_09_2k.hdr` 之外的内容都是本地拉取产物，**不入库**（见仓库根 `.gitignore`）；首次使用前运行 `./tools/fetch_assets.sh`（幂等，可重复跑，单项失败不中断）。全部 CC0：

| 目录/文件 | 来源 | 约定 |
|---|---|---|
| `textures/{sand,rock,bark,planks,grass}/` | [ambientCG](https://ambientcg.com/)（1K-JPG 包，`Ground037`/`Rock035`/`Bark012`/`Planks012`/`Grass004`；`Ground037` 代替不存在的 `GroundSand005`） | 归一化为 `albedo`/`normal`/`roughness`/`metalness`（如有）/`ao`，供 `asset::loadTextureSet` 读取 |
| `models/{Avocado,BoomBox,WaterBottle}.glb` | [Khronos glTF-Sample-Assets](https://github.com/KhronosGroup/glTF-Sample-Assets)（与 DamagedHelmet 同源，单文件 `.glb`） | 供 `EngineRuntime::instantiateModel` 按文件名解析 |
| `env/{day,sunset,night}.hdr` | [Poly Haven](https://polyhaven.com/hdris)（`kloofendal_43d_clear_puresky`/`venice_sunset`/`dikhololo_night`，2k） | 供 `EngineRuntime::applyEnvironmentFile` 按文件名解析 |

Poly Haven 的模型下载并非单文件 `.glb`（是 `.gltf` + `.bin` + 贴图的多文件包），与 `instantiateModel` 期望的 `<name>.glb` 约定不符，因此模型改用 Khronos 仓库的 CC0 单文件样例；若需要特定 Poly Haven 模型，可用 `asset_fetch` 工具（`kind:"model"`）按需下载——它会把多文件包整理为 `models/<id>/scene.gltf` + 相对路径的贴图/`.bin`，模型解析器（`resolveModelPath`）原生支持这种布局，无需手动打包。

## 模型分类包（`fetch_pack`，MA 里程碑；MS 扩容到 9 包）

`assets/models/<category>/` 下按分类存放的风格化道具包，同样是 `fetch_assets.sh` 的拉取产物，不入库；每个分类目录带一个 `pack.json`（`category`/`style`/`source`/`license`）供 `asset_index`/`viewer --thumbnails` 读取。九个 Kenney（CC0）分类包：`survival`（生存道具）、`furniture`（家具家电）、`food`（食物餐具）、`suburban`/`commercial`（城镇住宅/商业建筑）、`castle`、`graveyard`、`holiday`、`pirate`，合计约 840 个扁平单文件 `.glb`（风格化 `style:"stylized"`；部分包的 `.glb` 引用同目录 `Textures/colormap.png`，`fetch_pack` 整目录拷贝）。每个包入库前都过一遍 MS 硬闸：全量 `viewer --index` 实跑零加载/渲染失败。

模型名可带一层分类前缀（如 `survival/barrel`、`furniture/bedDouble`），`scene_add_model`/`asset_fetch` 均接受。

**手动包通道**（MS）：Quaternius/KayKit 等 CC0 源经 Drive/itch 分发、无固定直链，无法 `curl` 稳定拉取。将手动下载的 zip 命名为 `<category>__<Source Name>.zip`（双下划线分隔）放入 `assets/packs/`，跑一次 `./tools/fetch_assets.sh` 即按与 `fetch_pack` 相同的布局与 `pack.json` 约定解包整理（幂等，已有 pack.json 的分类跳过）；随后跑 `viewer --index` 验证并入索引。

**已评估并移除**：Kenney Nature Kit 曾纳入又整包移除——其 `.glb` 由 UniGLTF 导出且场景图有误（实际根节点 `tmpParent` 未出现在 `scenes[0].nodes` 里），`cgltf` 对每个文件判 `invalid_gltf`，全部 482 个模型无一可用；保留只会让 `asset_list`（磁盘优先枚举）被数百条垃圾条目淹没。上游重新导出后可在 `fetch_assets.sh` 恢复该包。Survival Kit（UnityGLTF 导出）无此问题。

## 素材索引与缩略图（`viewer --thumbnails`，MA 里程碑）

`assets/index.json` 与 `assets/.thumbnails/` 均为 `viewer --thumbnails` 的生成产物，不入库；`asset_list` 工具在 `index.json` 存在时优先读取它（分类/风格/尺寸等摘要），否则退回目录扫描。首次拉取素材后建议运行一次 `./build/macos-debug/apps/viewer/viewer --thumbnails` 生成索引与预览图；只重新渲染比模型/贴图/环境文件更旧的缩略图，`--force` 强制全部重渲染。
