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

Poly Haven 的模型下载并非单文件 `.glb`（是 `.gltf` + `.bin` + 贴图的多文件包），与 `instantiateModel` 期望的 `<name>.glb` 约定不符，因此模型改用 Khronos 仓库的 CC0 单文件样例；若需要特定 Poly Haven 模型，手动从其官网下载并自行导出/打包为 `.glb` 后放入 `assets/models/` 即可。
