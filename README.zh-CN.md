# PRTGI — Unreal Engine 6

[한국어](README.md) | [English](README.en.md) | **简体中文**

[使用指南 · GitHub Wiki](https://github.com/mazelines/UE5-PRT-GI/wiki)

`feat/runtime-surfel-gi` 分支面向 **Unreal Engine 6.0**，采用**先验证 PC，再扩展移动端**的路线。在保留静态 PRT 的同时，增加可选的 GPU 运行时光传输参考实现，用于家具布局变化。目前使用三角形 BVH，还没有实现 Surfel Radiance Cascade。参见[运行说明与限制（韩语）](docs/runtime-gi-pc.md)。下文的设置和缓存说明适用于原有静态 PRT 路径。

![PRT GI Cornell Box showcase](Plugins/PRTGI/docs/images/prt-gi-showcase-ue6-cfcd80c5.png)

## 相对 Fork 原版的实验性改进

此 `ue6` 分支基于 [wuzhy66/UE5-PRT-GI](https://github.com/wuzhy66/UE5-PRT-GI)，在 UE6 中实验改进画质与运行路径。下表的基准是 Fork 时的[原版提交 `69e08cf`](https://github.com/wuzhy66/UE5-PRT-GI/tree/69e08cfcdc3bb5c5ca1e83169c81ec95f47a67a9)，不包含上游之后的变化。**Probe/Surfel 预计算、GPU 方向光重光照、1～4 次间接反弹、独立 Volume、采集审计与近裁剪保护均为原版已有功能。**

| 项目 | Fork 时的原版 | 此 UE6 分支的变化 |
|---|---|---|
| 运行时 Surfel 表面池 | 无运行时实例所有权与 GPU 空间索引 | 新增共享局部样本、实例 GPU 槽位、移动/复制/删除更新和多级空间哈希。Showcase 的 18,312 个样本通过 GPU 对照检查。单独启用表面池不改变照明。[结构与限制](docs/runtime-gi-pc.md) |
| GPU 照明缓存实验 | 无运行时 Surfel 照明迭代 | 新增绑定三角形的辐照度缓存、双缓冲 Jacobi 迭代、按代重置/丢弃及探针 SH 投影。19 项 PRTGI 测试全部通过。默认设置与参考 SH 相差约 10.5%，发布延迟约 6.4 秒，因此保持默认关闭。[算法与测量](docs/runtime-gi-pc.md) |
| 运行时合成 | 没有运行时布局变更路径 | 增加命中遮挡物即停止的 BVH 查询，以及可选的半分辨率辐照度重建。通过深度、法线和 Volume 检查决定是否回退到完整分辨率。近景更快，但远景更慢，因此半分辨率默认关闭。[测量结果](docs/runtime-gi-pc.md) |
| UE6 集成 | UE 5.8 插件，以数值选择 GI 模式 | 添加 UE6 宿主项目和 Target，改用引擎 GI 枚举，修复 UE6 中预计算后 PRT 未激活的问题 |
| 二次命中查询 | 在命中点附近空间搜索已采集 Surfel 簇 | 将采集样本和传输源绑定到三角形，分离几何/着色法线，加入数值射线起点偏移，减少错误关联和自相交 |
| 材质查询 | 通过附近源查询复用采集材质值 | 用保留原始 UV、顶点色的每三角形 16×16 图表查询基础色、金属度和切线空间法线，并增加内部覆盖检查与受限重试 |
| 接收表面扩散能量 | 最终 PRT 接收表面没有对应的 GGX 能量修正 | 用 GBuffer 镜面参数/粗糙度与解析 GGX 近似为镜面波瓣预留能量；仅近似单个 GGX 接收材质，不是光泽传输 |
| GPU 系数 | 重光照系数读回 CPU 后用于渲染 | 增加常驻 GPU 的 SH 缓冲与发布 pass，仅诊断和 Probe 显示需要 CPU 读回；此结构变化本身不证明整帧性能提高 |
| 可见性与接触 | 首次命中平面的 Probe 可见性 | 增加深度矩、静态 BVH 线段对比及短距离接触遮挡。默认仍为平面方案；深度矩增大墙面斑纹，因此未作为默认值 |
| 曲面接收光照的插值 | 立即排除接收表面后方的 Probe，导致边界处贡献不连续 | 将 `r.PRTGI.HemisphereTransition` 从 0 改为 0.2，默认启用本分支已有的 `smoothstep` 过渡。在最小 Probe 间距的 20% 范围内平滑降低贡献，减轻球体上的圆形边界。保留后方 Probe 排除和墙体可见性检查；此设置变更无需重新预计算 |
| 反弹次数 | 最大 4 次，默认 4 次 | 最多预计算 12 次，运行默认 6 次，支持 4/8/12 对比。保留前四阶，之后累计 RGB 吞吐量最大值低于 0.01 时提前终止；更改阈值需要重建 |
| 诊断 | 已有缓存与传输检查 | 增加 v8 缓存及旧版读取、各阶路径/终止/材质命中统计、线性 HDR 对比工具，并扩展 GPU/CPU 验证和自动化测试。[曲面验证](Plugins/PRTGI/Tools/validation_curved_surface.json)补充同视角前后截图、可见性/接触遮挡对照实验及局部不连续性指标；该指标不是相对 Path Tracing 参考的误差 |

本地 Showcase 验证中，六个 v7 缓存的二次表面查询遗漏均为 0，GPU/CPU 系数相对误差低于 `3.75e-5`，五项专项测试通过。ClassicBox 的提前终止对照实验减少了 5.76% 传输项和 6.71% 缓存大小，12 阶 Probe L0 能量总和降低 0.040%。**这些是本分支内部设置及缓存阶段的对比，不是与上游在完全相同条件下的整体画质/性能基准。** 条件与原始结果见[表面/接触验证](Plugins/PRTGI/Tools/validation_showcase.json)及[反弹/终止验证](Plugins/PRTGI/Tools/validation_bounces.json)。

新增功能仍属实验。传输保持单个方向光驱动的静态 Lambert 扩散模型，不支持动态遮挡、天空/自发光传输、完整 Substrate 层和镜面 GI。提前终止是有偏近似，不保证图像误差上限。接触遮挡增加每帧 GPU 开销，12 阶缓存可能大于原版四阶缓存；MetalBox/ShinyBox 原有采集警告仍存在。

分发方式也有变化：默认文档语言为韩语，Git 不包含生成缓存。首次打开项目后，需要按下方步骤手动执行 **Build Precomputation**。

## 安装

可选的 SurfelPlus 启发实验：`r.PRTGI.Debug 5/6/7` 显示有效贡献的 Probe 数量、加权有效数量与权重集中度；`r.PRTGI.ContactHalfResolution 1` 使用半宽半高的接触遮挡计算及深度/法线引导重建，边缘退回全分辨率，默认仍为 0，无需重建缓存。Volume 的 `Transport Paths Per Sample` 默认为 1（范围 1–16）；启用 `Adaptive Transport Sampling` 需至少 2，每个采集样本另追踪两条预估路径，用与光源无关的传输变化分配实际路径预算。预估贡献不加入最终结果，实际贡献按各样本的路径数归一化。均匀 4 条可与自适应 2 条加预估 2 条比较起始路径预算，实际耗时可能不同，画质改善不保证。更改采样设置需要重新执行 **Build Precomputation**；v8 保存设置与路径统计，旧 v3–v7 缓存在均匀 1 条设置下仍可读取。详见 [English README](README.en.md)。

> **首次运行必须手动预计算。** Git 仓库不包含 `.prtgi`、`.surfels`、`.surfels.multi` 缓存。请用 UE6 Editor 打开项目，加载 `PRTGI_Showcase`，选择六个 `PRTProbeVolume`，执行 **Details → PRT → Build → Build Precomputation**。等待所有 Volume 完成后再比较 PRT 光照。**仅编译 C++ 项目或打开项目不会生成这些缓存。** 其他关卡也需要为所有参与的 Volume 在本地预计算。

仓库现在包含宿主项目 `UE_PRT_GI.uproject`，插件位于 `Plugins/PRTGI`。Game 和 Editor Target 均使用 `BuildSettingsVersion.V9` 与 `EngineIncludeOrderVersion.Unreal6_0`。此分支已在 UE6 Editor 中完成构建和验证；测量条件与结果见 [反弹验证报告](Plugins/PRTGI/Tools/validation_bounces.json)。

1. 使用随附宿主项目时，先将 `UE_PRT_GI.uproject` 关联到本地 **UE 6.0** 安装，再打开项目。仓库中的引擎关联 ID 对应开发机，其他电脑需要重新关联。安装到其他项目时，将本仓库的 `Plugins/PRTGI` 目录复制到 `YourProject/Plugins/PRTGI`，确保目录内直接包含 `PRTGI.uplugin`，不要额外嵌套一层仓库目录。
2. 使用 **UE 6.0** 编译 **UE_PRT_GIEditor**，配置为 **Win64 / Development Editor**。安装到其他项目时，使用 UE6 编译该项目的 Editor target。Blueprint 项目也需要能够编译此 C++ 插件的环境，必要时先添加一个 C++ 类以生成项目 target。
3. 在 **Edit → Plugins** 搜索 **PRT**（当前显示名称为 **UE5-PRT-GI**）并启用，按提示重启 Editor。

此分支按 **UE 6.0、Windows / Win64、Development Editor、DX12 / SM6** 的 Deferred Rendering 路径验证。GI pass 依赖 GBuffer，当前路径不支持 Forward Shading。插件引用 Renderer 内部接口，其他 UE 版本和平台需重新编译并验证；不同 Engine Build 的预编译 DLL 不保证兼容。当前实现不要求为安装插件另行修改 Engine 源码。

## 快速运行示例

1. 在 Content Browser 勾选 **Show Plugin Content**。
2. 打开 **PRTGI Content → Maps → PRTGI_Showcase**，资源路径 `/PRTGI/Maps/PRTGI_Showcase`。
3. 选择六个 `PRTProbeVolume`，执行 **Build Precomputation**，并等待全部完成。
4. 点击 Editor 视口或 PIE 游戏画面，按 **Ctrl+Alt+2** 开启 PRT，按 **Ctrl+Alt+3** 比较 Lumen。
5. 按 **Ctrl+Alt+4** 检查 PRT 仅间接光，按 **Ctrl+Alt+P** 查看 Probe。
6. 按 **Ctrl+Alt+[ / ]** 旋转太阳，观察重光照结果。

此分支使用 UE6 Renderer 接口。Lumen 硬件光追比较要求项目启用 DX12 / SM6、Ray Tracing 和相关支持；这些属于宿主项目渲染设置，不由插件强制覆盖整个项目。

比较 Hardware Ray Tracing Lumen 时，启用 **Support Hardware Ray Tracing**、**Support Compute Skin Cache** 和 **Use Hardware Ray Tracing when available**，并使用受支持的 GPU。修改需要重启的设置后重启 Editor。详见 Epic 的 [Lumen Technical Details](https://dev.epicgames.com/documentation/en-us/unreal-engine/lumen-technical-details-in-unreal-engine)。

随附示例的默认 Map Profile 会临时设置 `r.Lumen.HardwareRayTracing=1`、`r.Lumen.ScreenProbeGather.ScreenTraces=0` 和 `r.EyeAdaptation.CachedLightingPreExposure=8`，离开地图后恢复原值。因此示例的 Lumen 比较关闭了 diffuse screen traces，并非 Lumen 默认配置。这些临时覆盖不能代替项目级硬件光追支持设置。

UE6 宿主配置见 [Config/DefaultEngine.ini](Config/DefaultEngine.ini)：Plugin GI 使用 `r.DynamicGlobalIlluminationMethod=2`，反射为 `r.ReflectionMethod=0`，启用 Substrate 并使用 `r.Substrate.ProjectGBufferFormat=0`。运行时可用 `PRTGI.SetMode PRT` 自动解析引擎 GI 枚举。默认反弹次数为 **6**，预计算缓存在本地生成。安装到其他 UE6 项目时，请合并相关设置。

示例使用同一盏 UE DirectionalLight；各盒子的原面光源区域已替换为开口，正面保留观察开口。

| Volume | 本地验证设置与结果 |
|---|---|
| PRT_Global、PRT_ClassicBox、PRT_ColorBox、PRT_InvertedBox | 已构建 1～12 次间接反弹数据，运行时默认 6 次 |
| PRT_ShinyBox | 已按每面 16×16 构建；878 个受检 Probe 中排除 21 个，带采集警告 |
| PRT_MetallicBox | 已按每面 16×16 构建，保持 40 cm Probe 间距与 2 cm 表面聚合尺寸；2,212 个受检 Probe 中排除 77 个，带采集警告 |

仓库不附带缓存。请选择示例中的六个 `PRTProbeVolume` 并执行 **Build Precomputation**，完成后再比较光照。打包可运行项目时，仍需包含本地生成的缓存。

## 在自己的场景中添加 Probe Volume

1. 保存关卡，确认参与预计算的几何和材质保持静态。
2. 添加 UE **DirectionalLight**，将 Mobility 设为 **Movable** 以实时旋转。有多盏方向光时，为目标光源 Actor 添加 Tag `PRTGI.PrimaryLight`。
3. 在 Place Actors 搜索 **PRT Probe Volume** 并拖入场景。
4. 配置 Volume 范围、Probe 间距、Near Clip 与 Priority。
5. 开启 PRT GI，在该 Actor 的 Details → **PRT → Build** 中点击 **Build Precomputation**。
6. 等待完成，在 **PRT → Status** 检查 **Cache Status**、**GI Active**、**Effective Bounces** 和**Build Warnings**（构建警告），保存关卡。

Volume 只在 **PRT GI 模式**下参与间接光与 Probe 显示；切到 Lumen 后不会叠加 PRT GI。

### 参数说明

| 参数 | 作用 |
|---|---|
| Enabled | 是否启用此 Volume |
| Extent | 三轴半尺寸，单位 cm；Actor Scale 也影响范围 |
| Probe Spacing | 三轴目标最大间距，单位 cm；实际均匀间距见 Actual Spacing |
| Probe Inset | Probe 网格相对 Volume 边界的内缩距离，单位 cm |
| Priority | 局部密集区域使用更高优先级 |
| Near Clip Cm | Probe 采集近裁剪距离，默认 1 cm，范围 0.01～10 cm；不是主摄像机 Near Clip |
| Capture Resolution | 每个采集面的分辨率，默认 16，范围 4～32；提高会增加采集成本 |
| Transport Cell Size | 静态表面聚合尺寸，默认 2 cm，范围 1～100 cm；减小通常增加数据量和计算量 |
| Blend Face / Distance | 单个过渡面与距离（cm）：0=无，1/2=−/+X，3/4=−/+Y，5/6=−/+Z；只在需要衔接的开口处设置 |
| Display Mode | Hidden、Positions 或 Lighting；Lighting 显示 Probe 的光照颜色 |
| Probe Radius | 可视化球半径，单位 cm；不改变实际采样位置 |

Volume 当前为**轴对齐**体积，不支持旋转；每轴最多 32 个 Probe。超出限制时增大间距或拆分 Volume。

每个 Volume 还受独立的 **400 万采集样本预算**限制：`ProbeCounts.X × ProbeCounts.Y × ProbeCounts.Z × 6 × CaptureResolution²` 不得超过 4,000,000。默认分辨率 16 时，Probe 总数最多为 2,604，且仍需满足每轴限制。超出预算时降低采集分辨率、增大间距或拆分 Volume。

可从全局 Priority=0、约 200 cm 间距，局部 Priority=10、约 30～40 cm 间距开始试验，再按实际场景尺寸调整。不要把 Probe 放入墙内，也不要跨实体墙设置混合过渡。过大的 Near Clip 可能裁掉贴近 Probe 的几何，造成错误可见性或漏光。

### 预计算与进度

- 选中一个或多个 Volume，点击 **Build Precomputation** 将其加入构建队列；使用 **Cancel Build** 取消选中 Volume 的构建。
- **PRT → Build** 中的**Build Progress**（构建总进度）覆盖几何采集、传输计算和保存，**Build Stage**（构建阶段）说明当前工作；两者与构建按钮放在一起。已有有效几何缓存时会复用，可能只执行传输阶段。进度按阶段分配，不代表剩余时间估计。
- 当前构建结合 UE 场景采集与 CPU 传输预计算，运行时默认 GPU 重光照；整个 Build 不是纯 GPU 烘焙。
- 构建保存供 1～12 次间接反弹切换使用的数据。运行时切换已有反弹次数无需重新采集，默认使用 6 次。
- 用时取决于 Probe 数量、采集分辨率、几何复杂度、表面聚合尺寸及硬件。先构建小体积验证，再扩大范围。
- 静态几何、材质、网格布局或采集参数改变后需要重新构建。复制 Volume 会生成新 GUID，新副本需要自己的缓存。

几何校验异常或空采集的 Probe 会从传输与插值中排除。排除数不超过受检 Probe 的 5%，且剩余数据通过校验时，可以带警告完成构建；更大范围的异常会使构建失败。使用结果前检查**Build Warnings**（构建警告）、**Invalid Probes** 与受影响区域。采集校验使用 **Visibility** 通道的碰撞查询，因此参与几何需要与渲染表面匹配、且阻挡该通道的碰撞。

每个 Volume 的传输项上限为 **2,400 万项**，每项 64 bytes，最多约 **1464.8 MiB** 传输数据；文件加载上限另留 384 MiB 存放几何索引等数据。这不是总内存预算，构建临时副本、CPU 缓存和 GPU 缓冲还会占用额外内存。不会因接近上限自动降低采集分辨率。

## 内容目录

以下路径相对于插件目录 `Plugins/PRTGI`。

- `Content/Maps`：示例关卡。
- `Content/Showcase/Materials`、`Meshes` 等：示例所需的项目资源副本，引用重定向到插件内部。
- `Content/Materials`：Probe 显示和几何采集材质。
- `Content/Precomputed/Volumes`：按关卡包名和 Volume GUID 隔离的 `.surfels` / `.multi` 原始缓存；不含光源参数。

`.surfels` 保存几何采集数据，配套 `.multi` 保存多次反弹传输数据；请成对保留。它们是原始二进制文件，不会作为 `.uasset` 出现在 Content Browser。缓存并非固定太阳角度的最终图像；运行时仍计算当前光源激励与遮挡，再应用预计算传输。

`/PRTGI/` 下的插件关卡写入插件目录 `Content/Precomputed/Volumes/<关卡包名>/<GUID>.prtgi.surfels`，配套文件为 `<GUID>.prtgi.surfels.multi`。其他关卡（包括自己的 `/Game/` 关卡）写入 `YourProject/Content/PRTGI/Volumes/<关卡包名>/`。关卡包名中的斜杠会替换为下划线；移动或重命名关卡会改变缓存路径，需要检查 **Cache File** 并按需重建。

插件的 Build Rules 会将自身 `Content/Precomputed` 作为松散 **NonUFS** 文件打包。使用 `/Game/` 关卡打包时，还需通过 **Additional Non-Asset Directories To Copy** 包含 `Content/PRTGI/Volumes`（相对项目 Content 的路径填写 `PRTGI/Volumes`）。请在打包产物中验证缓存加载；随附验证报告覆盖的是 Editor 使用。

## 当前限制

- PRT 计算单个选定 DirectionalLight 驱动的间接漫反射。Point、Spot、Rect Light、SkyLight 和自发光材质不会驱动此 PRT 传输；插件不提供镜面 GI 或反射。
- 参与传输的几何与材质必须保持静态。运行时光源变化时，GPU 遮挡计算复用缓存几何，移动遮挡物不会自动纳入该缓存传输。
- 曲面可能出现与 Probe 相关的环纹，亮度也可能与 Lumen 不同。进行光照比较前，请检查自己场景中的实际结果。

## 快捷键

先点击 Editor / PIE 视口，再操作。

| 快捷键 | 作用 |
|---|---|
| Ctrl+Alt+1 | 关闭动态 GI；本示例没有离线烘焙光照 |
| Ctrl+Alt+2 | PRT GI |
| Ctrl+Alt+3 | Lumen GI |
| Ctrl+Alt+4 | 切换 PRT 仅间接漫反射 / 完整画面 |
| Ctrl+Alt+P | 显示 / 隐藏 Probe；位置或光照球由 Volume Display Mode 决定 |
| Ctrl+Alt+G | 循环切换 4 / 8 / 12 次间接反弹，需要对应缓存 |
| Ctrl+Alt+[ / ] | DirectionalLight Yaw 减 / 加 5°（默认步长） |
| Ctrl+Alt+Shift+[ / ] | DirectionalLight Pitch 减 / 加 5°（默认步长） |
| Ctrl+Alt+B | 旧版全局烘焙入口，不会构建 Actor Volumes |

局部预计算请选中对应 `PRTProbeVolume`，点击 **Build Precomputation**。默认使用 6 次间接反弹。

快捷键受焦点或键盘布局影响时，可在 UE Console 逐条执行：

```text
PRTGI.SetMode PRT
PRTGI.SetMode Lumen
PRTGI.ToggleIndirectOnly
r.PRTGI.ShowProbes 1
r.PRTGI.Bounces 4
PRTGI.RotateLight 5 0
PRTGI.Status
```

`PRTGI.SetMode Offline` 是历史命令名称，表示退出动态 GI；它不会自动生成离线光照或 Path Tracing 参考图。

v7 及以上缓存最多预计算 12 次反弹。`Transport Min Throughput` 默认 0.01：保留前四阶，之后在累计 RGB 吞吐量最大值低于阈值时提前终止路径；0 关闭此截断。修改阈值需要重新构建。该有偏近似不保证 1% 图像误差。旧 v3–v6 缓存仍可读取，但最多使用 4 次反弹。详细比较见 [English README](README.en.md#comparing-4-8-and-12-bounces)。

曲面接收光照时，探针在表面切平面处被突然排除会形成圆形边界。`r.PRTGI.HemisphereTransition` 现在默认为 `0.2`，启用已有的连续权重过渡，同时保留可见性检查；无需重新预计算。设为 `0` 可比较旧的硬切换。此修改参考 DDGI 的连续权重原则，并未移植其完整 wrap 权重或动态 GI 系统。最新文献与对照条件见 [曲面验证记录](Plugins/PRTGI/Tools/validation_curved_surface.json)；先前的 v6 和反弹次数实验仍是过渡宽度为 0 时的历史数据。
