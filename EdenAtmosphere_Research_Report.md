# IXRay 引擎 Eden Atmosphere 真·大气散射与体积云植入方案研究报告

> 研究对象：IXRay（X-Ray 引擎的开源 DX11 重制版）仓库
> 范围：`engine`（src/xrEngine + src/Layers/xrRenderPC_R4 + gamedata/shaders/d3d11）与 `leveleditor`（src/Editors/LevelEditor）
> 目标：提出在两条产品线中植入物理大气散射天空（Eden Atmosphere，非贴图、真大气）与体积云的完整实现方案，重点利用 LUT、ComputeShader、离线烘焙等手段控制开销，并与现有 SkyCube 方案做性能对比。

---

## 1. 执行摘要

IXRay 当前的天空是典型的"SkyCube"路线：一个 12 顶点的半盒体（half-box）几何，采样两张 TextureCube 天空贴图并按天气权重交叉淡化，配合一张平面云层四边形与 combine 阶段的线性距离雾 [dxEnvironmentRender.cpp](file:///workspace/src/Layers/xrRender/dxEnvironmentRender.cpp#L208-L262)、[sky.ps.hlsl](file:///workspace/gamedata/shaders/d3d11/sky.ps.hlsl#L14-L48)。该方案 GPU 开销几乎可忽略，但视觉完全依赖美术预烘焙贴图，时间/天气连续性、太阳位置一致性、空气透视均不可物理自洽。

本报告建议采用 Hillaire (EGSR 2020) 的生产级天空大气方案[cite:1]作为 Eden Atmosphere 的核心：以 Transmittance LUT + 多重散射近似 LUT + Sky-View LUT + Aerial Perspective LUT 四张小型查找表替代高维预计算[cite:1][cite:9]，全部由 R4 层现成的 ComputeShader 基础设施烘焙[ComputeShader.h](file:///workspace/src/Layers/xrRenderPC_R4/ComputeShader.h#L10-L36)。体积云采用 Schneider (SIGGRAPH 2015/2017) 的天气图 + Perlin-Worley 3D 噪声 + 光线步进体系[cite:4][cite:5]，噪声离线烘焙、渲染半分辨率 + 时间重投影。整套系统可在默认档位下将新增 GPU 开销控制在约 **1.2–2.5 ms/帧（1080p，中端 GPU，估算值）**，其中体积云占 80% 以上；纯大气天空部分（LUT 更新 + Sky-View 采样）仅 **约 0.05–0.3 ms/帧（估算值）**。相比 SkyCube 的近零开销，代价换来的是物理自洽的全天候、全时段大气，且历史缓冲、速度缓冲、UAV、半分辨率 RT、TAA 等全部可复用现有代码资产。

---

## 2. 现状分析：SkyCube 天空系统的架构与性能画像

### 2.1 天气/环境数据层

天气系统由 `CEnvironment` 驱动：按时间轴从 `WeatherCycles`/`WeatherFXs` 中选取两个 `CEnvDescriptor`，经 `CEnvDescriptorMixer::lerp` 混合出 `CurrentEnv`[Environment.h](file:///workspace/src/xrEngine/Environment.h#L204-L233)。每个天气描述符的字段直接来自天气 .ltx 配置：`sky_texture`、`clouds_texture`、`sky_color`、`sky_rotation`、`fog_color/fog_density/fog_distance`、`ambient_color`、`sun_color`、`hemi_color` 等[Environment_misc.cpp](file:///workspace/src/xrEngine/Environment_misc.cpp#L315-L357)。这一层是纯 CPU 的、每帧一次的插值，成本低且稳定，Eden Atmosphere 应当完全复用它（详见 §4.3）。

### 2.2 天空渲染路径

引擎侧入口 `CEnvironment::RenderSky()` 经 `FactoryPtr<IEnvironmentRender>` 转发到渲染层[Environment_render.cpp](file:///workspace/src/xrEngine/Environment_render.cpp#L21-L39)。`dxEnvironmentRender.cpp` 为 DX9/DX11 双后端共享文件，下述按游戏 R4/DX11 编译描述（编辑器 DX9 编译分支见 §3.5）：

- **绘制时机（复核修正）**：R4 的天空**不在 phase_combine**，而是在 `CRender::Render()` 主流程中、deferred 场景之前绘制：先 `u_setrt(rt_Generic_0, rt_Velocity)` 绑定颜色+速度双 MRT 并清屏（无深度/模板绑定），随后直接 `RenderSky()`，之后才进入 Z-prefill 与 G-Buffer 场景渲染——即"**先天空、后场景覆盖**"的背景层模式[r4_R_render.cpp](file:///workspace/src/Layers/xrRenderPC_R4/r4_R_render.cpp#L539-L594)。phase_combine 内仅绘制云（L101）与镜头光晕（L421）[r4_rendertarget_phase_combine.cpp](file:///workspace/src/Layers/xrRenderPC_R4/r4_rendertarget_phase_combine.cpp#L98-L104)。作为对照，R2 老路径才是"combine 内绘制 + stencil >= 1 合成"[r2_rendertarget_phase_combine.cpp](file:///workspace/src/Layers/xrRenderPC_R2/r2_rendertarget_phase_combine.cpp#L66-L75)。
- **几何**：静态半盒体 `hbox_verts`（12 顶点 / 20 三角形），每帧上载顶点/索引，世界矩阵为 `rotateY(sky_rotation) + translate(相机位置)`[dxEnvironmentRender.cpp](file:///workspace/src/Layers/xrRender/dxEnvironmentRender.cpp#L208-L262)。
- **纹理与交叉淡化**：`$user$sky0/$user$sky1` 两张用户纹理在 `dxEnvironmentRender` 构造时创建[dxEnvironmentRender.cpp](file:///workspace/src/Layers/xrRender/dxEnvironmentRender.cpp#L163-L166)，`OnFrame` 每帧把两个相邻天气的天空贴图 `surface_set` 进去[dxEnvironmentRender.cpp](file:///workspace/src/Layers/xrRender/dxEnvironmentRender.cpp#L168-L185)（`sky_r_textures[2]` 在天气 `lerp()` 时填充[dxEnvironmentRender.cpp](file:///workspace/src/Layers/xrRender/dxEnvironmentRender.cpp#L128-L144)）。
- **着色**：[sky.ps.hlsl](file:///workspace/gamedata/shaders/d3d11/sky.ps.hlsl#L14-L48) 对两张 **TextureCube** 各采样一次，`lerp(s0, s1, factor.w)`（权重编码在顶点色 alpha），乘以 `L_sky_color`（binder 实现 = `sky_color × ps_r2_sun_lumscale_sky`，w 分量携带 `sky_rotation`[Blender_Recorder_StandartBinding.cpp](file:///workspace/src/Layers/xrRender/Blender_Recorder_StandartBinding.cpp#L404-L425)），走 `PushGamma` 或 legacy tonemap，并把 TAA 速度写入 `SV_Target1`——与主流程 MRT `rt_Generic_0 + rt_Velocity` 布局严格对应。
- **顶点着色**：`m_WVP`/`m_WVP_old` 双变换 + `m_taa_jitter` 偏移，保证天空与 TAA/重投影兼容[sky.vs.hlsl](file:///workspace/gamedata/shaders/d3d11/sky.vs.hlsl#L23-L34)。

### 2.3 云层

云是一个贴近相机的大平面（`scale(10, 0.4, 10)`），顶点色携带风向与云颜色，像素着色器对两张 2D 云纹理求和后乘色输出[clouds.ps.hlsl](file:///workspace/gamedata/shaders/d3d11/clouds.ps.hlsl#L10-L25)、[dxEnvironmentRender.cpp](file:///workspace/src/Layers/xrRender/dxEnvironmentRender.cpp#L264-L300)。绘制发生在 `phase_combine` 开头（绑定 `RDepth` 深度、`rmFar` 远平面深度、无 stencil 测试），依靠深度测试被场景几何遮挡，只在天空区域可见[r4_rendertarget_phase_combine.cpp](file:///workspace/src/Layers/xrRenderPC_R4/r4_rendertarget_phase_combine.cpp#L92-L104)。没有厚度、没有光照、没有与太阳的遮挡关系，本质上是一张会平移的"云贴图壁纸"。

### 2.4 雾与光照积分

- **雾**：combine 阶段用最简单的线性模型 `Fog = saturate(ViewDist * fog_params.w + fog_params.x)`，把场景颜色 `lerp` 到 `fog_color`[combine_1.ps.hlsl](file:///workspace/gamedata/shaders/d3d11/combine_1.ps.hlsl#L46-L55)；材质内另有 `calc_fogging()` 同模型[common_functions.hlsli](file:///workspace/gamedata/shaders/d3d11/common_functions.hlsli#L219-L221)。雾色与天空色之间无物理联系。
- **太阳**：3 级 CSM 级联（覆盖半径 15/40/160 m，共享 bias 系数）从 `Lights.sun_adapted` 取方向与颜色[R_sun.cpp](file:///workspace/src/Layers/xrRender/R_sun.cpp#L44-L60)、[r_sun_cascades.h](file:///workspace/src/Layers/xrRender/r_sun_cascades.h#L15-L24)；`sun_adapted` 由 `CLight_DB` **每帧**从 `CurrentEnv` 重建：方向 = `(0,-0.75,0) + E.sun_dir` 归一化（带 `E.sun_dir.y < 0` 的地平线校验），颜色 = `E.sun_color × ps_r2_sun_lumscale`[Light_DB.cpp](file:///workspace/src/Layers/xrRender/Light_DB.cpp#L236-L262)。太阳颜色/方向完全由天气配置表驱动，与天空像素无一致性约束。combine 阶段把 `Ldynamic_color/Ldynamic_dir`、`L_ambient`、`env_color`（hemi）、`fog_color` 等常量送入着色[r4_rendertarget_phase_combine.cpp](file:///workspace/src/Layers/xrRenderPC_R4/r4_rendertarget_phase_combine.cpp#L115-L151)。
- **环境反射**：天气描述符的 `sky_texture_env` 作为小环境贴图绑定到 combine 的 `env_s0/env_s1` 供高光环境项使用[blender_combine.cpp](file:///workspace/src/Layers/xrRenderPC_R4/blender_combine.cpp#L26-L33)。

### 2.5 SkyCube 的性能画像与局限

**性能画像**：1 次 draw call、20 个三角形、仅天空像素着色（典型占屏 30%–50%），每像素 2 次 TextureCube 采样 + 1 次混合 + 1 次 tonemap。在现代 GPU 上属于"免费"级别（1080p 下数量级约 0.1–0.3 ms，视天空占屏比与带宽，估算值）。CPU 侧每帧还有两次小规模顶点/索引缓冲上载。

**根本局限**（均属结构性缺陷，无法靠调参解决）：

| 维度 | 现状表现 |
|---|---|
| 视觉 | 天空外观锁死在预烘焙 cube 贴图（每个天气时点一张）上，昼夜过渡靠两组贴图 alpha 混合，色彩不连续 |
| 物理 | 太阳颜色/天空亮度/雾色三者互相独立，黄昏时太阳是红的而天空不会跟着变 |
| 雾 | 线性距离雾与天空割裂，远山"贴"在一层均匀色纸上 |
| 云 | 平面贴图云，无体积、无太阳散射、无云影 |
| 扩展 | 任何新天气都要美术重画 cube map；大气组成（浊度、臭氧）不可参数化 |

---

## 3. 引擎能力盘点：可直接复用的 DX11 基础设施

### 3.1 ComputeShader 与 UAV：现成的烘焙管线

R4 层已内置完整的 CS 框架：`ComputeShader` 类支持常量缓冲、SRV、Sampler、UAV 绑定与 `Dispatch(dimx, dimy, dimz)`[ComputeShader.h](file:///workspace/src/Layers/xrRenderPC_R4/ComputeShader.h#L10-L36)、[ComputeShader.cpp](file:///workspace/src/Layers/xrRenderPC_R4/ComputeShader.cpp#L43-L97)；`CSCompiler` 提供 `begin/defTexture/defOutput/defSampler/end` 的声明式编译 API[CSCompiler.h](file:///workspace/src/Layers/xrRenderPC_R4/CSCompiler.h#L12-L37)。底层 `CRT` 在 `USE_UAV_FLAG` 下创建 `D3D11_BIND_UNORDERED_ACCESS` 资源与 UAV 视图[dx11SH_RT.cpp](file:///workspace/src/Layers/xrRenderDX10/dx11SH_RT.cpp#L103-L161)。**大气 LUT 烘焙不需要新造任何抽象**，Eden 的每个 CS 都走 `CSCompiler` 声明输入输出即可。

### 3.2 渲染目标体系：HDR、UAV、速度、历史缓冲全部就位

- 主 RT 全部为 `R16G16B16A16_FLOAT` HDR（`rt_Accumulator`、`rt_Generic_0/2`、back buffer、SSLR 族、`rt_Reflection`）；UAV 标志仅在 FL11.0+ 启用，且当前只有 `rt_Generic` 带 `USE_UAV_FLAG` 创建[r4_rendertarget.cpp](file:///workspace/src/Layers/xrRenderPC_R4/r4_rendertarget.cpp#L539-L567)——Eden 的 LUT 资源自建 CRT（带 UAV）即可，不依赖改造现有 RT。
- `rt_Velocity`（R16G16_FLOAT）在主渲染流程作为第二个 MRT 承担 TAA/FSR2/DLSS/XeSS 运动向量，天空着色器直接写入其中[r4_rendertarget.cpp](file:///workspace/src/Layers/xrRenderPC_R4/r4_rendertarget.cpp#L562)、[r4_R_render.cpp](file:///workspace/src/Layers/xrRenderPC_R4/r4_R_render.cpp#L539-L543)。
- 已有半分辨率 + temporal 历史 RT 的成熟先例（SSLR 的 `rt_sslr_point/data/temp` 与 `rt_sslr_old`）[r4_rendertarget.cpp](file:///workspace/src/Layers/xrRenderPC_R4/r4_rendertarget.cpp#L583-L585)，体积云的半分辨率时间累积可直接复制该模式。

### 3.3 3DFluid：引擎里的体积先例

`src/Layers/xrRenderDX10/3DFluid` 已实现 3D 纹理网格上的烟雾/流体模拟与光线投射渲染：`CreateTexture3D` + 3D RTV 创建[dx103DFluidManager.cpp](file:///workspace/src/Layers/xrRenderDX10/3DFluid/dx103DFluidManager.cpp#L199-L221)，模拟走 blender 编译的切片着色器[dx103DFluidManager.cpp](file:///workspace/src/Layers/xrRenderDX10/3DFluid/dx103DFluidManager.cpp#L158-L183)。它是 PS（光栅化切片）时代的产物，但证明了：3D 资源管理、3D 采样、体积 raymarch 输出合成（`fluid_raycast_quad` 系列着色器）在本引擎可用。Eden 云不需要流体模拟，但可借用其 3D 纹理与 raymarch 合成代码骨架。

### 3.4 帧相位结构

R4 一帧的主序为 `主流程：绑定 MRT(rt_Generic_0, rt_Velocity) → RenderSky（背景层）→ Z-prefill → phase_scene_prepare → phase_smap_D（CSM）→ phase_scene（G-Buffer）→ phase_occq → phase_accumulator（光照累积）→ phase_combine（云/AO/SSLR/环境光/雾/forward/distort/tonemap）→ TAA/FSR2 → bloom/PP`[r4_R_render.cpp](file:///workspace/src/Layers/xrRenderPC_R4/r4_R_render.cpp#L539-L594)、[r4_R_render.cpp](file:///workspace/src/Layers/xrRenderPC_R4/r4_R_render.cpp#L824)。Eden 的挂载点明确：**LUT 烘焙 CS 在帧首按需调度；天空替换点 = 主流程 `RenderSky()` 调用处（背景层，先于场景）；云 raymarch 在 phase_combine 开头（现 `RenderClouds` 位置）；空气透视在 combine_1 中替换雾；云影在 phase_smap_D 后、accumulator 前应用。**

### 3.5 编辑器侧（复核结论：DX9 + xrECore 自定义管线，不走引擎 R4）

- **LevelEditor 的渲染根基是 DX9 + xrECore 自定义管线**，与游戏引擎 R4 渲染层完全独立：
  - 链接构成：LevelEditor 仅链接 `xrCore/xrEUI/xrEProps/xrECore/xrSound/xrAbstractions/xrScripts`，**不含** `xrRenderPC_R4` [LevelEditor/CMakeLists.txt](file:///workspace/src/Editors/LevelEditor/CMakeLists.txt#L53-L60)；
  - 设备层：`CEditorRenderDevice` 的渲染上下文/设备/交换链/深度宏全部为 `IDirect3DDevice9*`[device.h](file:///workspace/src/Editors/xrECore/Editor/device.h#L24-L29)；
  - 渲染层：xrECore 编译共享层 `Layers/xrRender/dxEnvironmentRender.*` 等 + `xrRenderDX9/dx9r_constants_cache.*`[xrECore/CMakeLists.txt](file:///workspace/src/Editors/xrECore/CMakeLists.txt#L36-L57)，配编辑器自己的 `CRender : IRender_interface`（内部为 `IDirect3DVertexBuffer9*` 等裸 DX9 资源）[render.h](file:///workspace/src/Editors/xrECore/Editor/render.h#L102-L135)。
  - **勘误**：本报告初版称"LevelEditor 视口走引擎 R4"系误判——Viewports 中的 `((CRender*)::Render)` 是 xrECore 的编辑器 `CRender`，与 R4 的 `CRender` 是**同名不同类**；Eden 的 R4 实现不会自动出现在编辑器视口。
- **但环境数据层是共享的**：编辑器经 `XrGamePersistentEditors` 创建引擎 `CEnvironment`[XrGamePersistentEditors.cpp](file:///workspace/src/Editors/xrECore/Engine/XrGamePersistentEditors.cpp#L15-L18)；`rsEnvironment` 设备标志开启或 Play-In-Editor 时调用 `Environment().RenderSky()/RenderClouds()`[UI_LevelTools.cpp](file:///workspace/src/Editors/LevelEditor/Editor/UI_LevelTools.cpp#L483-L496)，实际执行的是 `dxEnvironmentRender` 的 **DX9 编译分支**（该文件为 DX9/DX11 双后端共享，DX9 分支用 `D3DRS_FOGCOLOR/D3DRS_FOGSTART/FOGEND` 固定管线雾状态[dxEnvironmentRender.cpp](file:///workspace/src/Layers/xrRender/dxEnvironmentRender.cpp#L188-L202)）。
- 游戏内天气编辑器 `UIEditorWeather`（ImGui）位于 **xrPlay（R4 游戏进程）**，直接编辑 `CEnvDescriptor` 全部字段并写回天气 .ltx[UIEditorWeather.cpp](file:///workspace/src/xrPlay/UIEditorWeather.cpp#L72-L100)，是新增大气/云参数面板与**高保真预览**的理想宿主。
- 需要澄清：LevelEditor 的 "Environment Generator"（`EEnvGenerator`）是植被散布工具（按 RGBA 热力图散布对象），与天气环境无关[EnvGenerator.h](file:///workspace/src/Editors/LevelEditor/Editor/Utils/EnvGenerator.h#L8-L12)，方案中不应把烘焙工具挂错位置。
- 另存在新一代渲染接口层 `xrRenderDS_R0`（含独立的 `CDS0_EnvironmentRender::RenderSky` 骨架）[XRayEnvironmentRender.cpp](file:///workspace/src/Layers/xrRenderDS_R0/Interface/Environment/XRayEnvironmentRender.cpp#L90)，目前为 WIP；Eden 应先落在 R4，DS_R0 后续通过同一 `IEnvironmentRender` 接口接入。

---

## 4. Eden Atmosphere 总体设计

### 4.1 设计目标与原则

1. **真大气**：天空颜色、太阳辐照度、空气透视全部从大气物理模型（Rayleigh/Mie/臭氧散射吸收）实时推导，非贴图。
2. **非侵入**：以 `IEnvironmentRender` 接口替换与 shader option 开关实现，保留 SkyCube 作为可回退路径（`r_atmosphere_mode 0/1/2`）；Eden 仅落地 R4/DX11 游戏路径，DX9 编辑器路径（LevelEditor 等）维持 legacy 天空（§3.5、§7）。
3. **开销可控**：一切昂贵计算进 LUT；LUT 更新按脏标记 + 帧间摊销；体积云半分辨率 + 时间重投影 + 离线烘焙噪声。
4. **全链路一致**：同一模型同时驱动天空像素、太阳光颜色、环境光/半球光、雾（空气透视）、云光照与云影。

### 4.2 物理模型选型

| 候选 | 思路 | 优势 | 劣势 | 结论 |
|---|---|---|---|---|
| Bruneton 2008[cite:2] | 预计算 4D LUT（8 张以上 3D/2D 表） | 精度标杆，有开源参考实现[cite:3] | LUT 维度高、更新贵，动态天气受限；低太阳角伪影 | 作为**离线烘焙/验证基准** |
| 解析模型（Preetham/Hosek 等） | 拟合公式 | 极便宜 | 仅地面视角、单一大气、无臭氧等 | 不采用 |
| **Hillaire 2020**[cite:1] | 2D Transmittance LUT + 3D 多重散射近似 LUT + 2D Sky-View LUT + 3D 空气透视 LUT，全部低维、可逐帧更新 | UE5 Sky Atmosphere 生产验证[cite:9]；手机到高端 GPU 全覆盖[cite:1]；天气参数可动态改而不需要重烘焙高维表[cite:1] | 多重散射为近似（第二散射后视各向同性）[cite:1] | **采用** |

选型理由与引擎现状高度契合：Hillaire 方案的关键词是"低维 LUT + 便宜更新"，正好能塞进 R4 现成的 CS 编译/派发框架；而 Frostbite 2016 已示范过同一套大气如何统一驱动天空、云、雾与阴影着色[cite:6]，是本方案集成路线的直接参照。

### 4.3 与现有天气系统的兼容层

Eden 不另起炉灶，而是把 `CEnvDescriptor` 映射为大气参数，沿用现有 lerp 时序：

| CEnvDescriptor 现有字段 | Eden 映射 |
|---|---|
| `sun_dir`（由 `calculate_dynamic_sun_dir` 计算） | 直接作为太阳方向输入，不需要新时间模型 |
| `sun_color` | 退役为"太阳标称强度"锚点；实际颜色 = 标称强度 × Transmittance(sun_zenith)（物理化） |
| `sky_color` | 退役为 legacy 回退；Sky-View LUT 输出替代 |
| `fog_color/fog_density/fog_distance` | 映射为空气透视的 Mie 散射系数与"视觉距离"标定（保持旧天气配置下的能见度手感） |
| `hemi_color/ambient` | 由 Sky-View 辐照度积分（小 Irradiance LUT）驱动，保留 `r2_sun_lumscale_hemi/amb` 缩放链[xrRender_console.cpp](file:///workspace/src/Layers/xrRender/xrRender_console.cpp#L171-L174) |
| `sky_rotation` | 物理天空不需要；保留仅用于 legacy 模式 |
| 新增 | `turbidity`（Mie 浓度）、`ozone`、`ground_albedo`、`cloud_*` 体积云参数 |

`AtmosphereParameters` 结构只在天气插值跨越参数差阈值时重建（脏标记），每帧 CPU 成本近似为零。

---

## 5. 大气散射实现方案

### 5.1 LUT 清单与资源规格

| LUT | 规格 | 格式 | 内容 | 更新时机 |
|---|---|---|---|---|
| Transmittance | 256×64 2D | RGBA16F (~128 KB) | 大气顶到观测点的透过率，参数化 (r, μ) | 参数脏标记时 |
| MultiScatter | 32×32×32 3D | RGBA16F (~256 KB) | Hillaire 多重散射近似（无限阶收敛）[cite:1] | 与 Transmittance 同批 |
| Sky-View | 192×108 2D | RGBA16F (~160 KB) | 地面视角远处天空亮度（含太阳盘外）[cite:1] | 太阳方向/参数变化帧 |
| Aerial Perspective | 32×32×32 3D | RGBA16F (~256 KB) | 距离-视角维度的空气透视透射+内散射 | 每帧或每 N 帧 |
| Irradiance | 32×32 2D | RGBA16F (8 KB) | 天空对水平面的辐照度（喂 ambient/hemi） | 与 Sky-View 同批 |
| 云影图 | 128×128 2D | R8 | 云层沿太阳方向投影密度 | 每 4 帧 |

合计常驻显存 **约 1 MB**。规格与 Hillaire 论文的推荐一致（Sky-View 192×108[cite:1]），没必要更大。

### 5.2 ComputeShader 烘焙流程

新增 `xrRenderPC_R4/EdenAtmosphere.h/.cpp` 管理器，每张 LUT 一个 `CRT`（UAV 标志）+ 一个 CS（走 `CSCompiler`）：

```text
帧首调度（EdenAtmosphere::Update())：
  if (atmo_dirty)                       // 天气lerp参数变化
      CS_Transmittance -> 一次 Dispatch(256/8, 64/8, 1)     // ~0.05ms 级
      CS_MultiScatter  -> 一次 Dispatch(32/4, 32/4, 32/4)
      atmo_dirty = false
  if (sundir_dirty || first_frame)      // 太阳移动/每帧（时间流速快时）
      CS_SkyView      -> Dispatch(192/8, 108/8, 1)
      CS_Irradiance   -> Dispatch(32/8, 32/8, 1)
  if (frame % 2 == 0)
      CS_AerialPerspective -> Dispatch(32/4, 32/4, 32/4)
```

 Sky-View/Aerial-Perspective 的重算仅依赖 Transmittance + MultiScatter 两个小 LUT，这正是该方案"改大气不需要重烘焙高维表"的核心优势[cite:1]。若追求极致，可将 Sky-View 更新跨帧分块（每次重算一半扫描线），但默认档无需。

### 5.3 天空渲染替换

`dxEnvironmentRender::RenderSky` 在 R4/DX11 编译分支下分为 Eden 路径（DX9 编辑器分支不动，见 §7）。**挂载点 = 主渲染流程 `RenderSky()` 调用处**（`r4_R_render.cpp` 中绑定 MRT `rt_Generic_0 + rt_Velocity` 并清屏之后、场景渲染之前[r4_R_render.cpp](file:///workspace/src/Layers/xrRenderPC_R4/r4_R_render.cpp#L539-L548)），保持"先天空、后场景覆盖"的背景层语义：

- **几何**：改为全屏三角形（复用 `stub_fullscreen_triangle.vs.hlsl`）。现有 R4 路径天空绘制时本就无深度/模板绑定（`u_setrt` 第三参为 0，`set_Stencil(FALSE)`[r4_R_render.cpp](file:///workspace/src/Layers/xrRenderPC_R4/r4_R_render.cpp#L539-L546)），天空像素完全依靠后续场景几何的深度覆盖来遮挡——全屏三角形在此语义下等价且更优（省去 12 顶点盒体与每帧 VB 上载）。
- **着色**（`eden_sky.ps.hlsl`）：
  1. 由像素射线求 `viewZenithCosAngle/viewSunCosAngle`，采样 Sky-View LUT；
  2. 叠加太阳盘（角直径 0.53°，亮度取 Transmittance 加权后的辐亮度）；
  3. 保留现有输出协议：写 `SV_Target0` 颜色 + `SV_Target1` 速度（速度为 0 但保留 MRT 布局），自然兼容 TAA/FSR2[sky.ps.hlsl](file:///workspace/gamedata/shaders/d3d11/sky.ps.hlsl#L17-L21)。
- **双纹理交叉淡化机制退役**：物理天空由连续参数驱动，`tsky0/tsky1` 与 `factor.w` 权重混合不再是必需品（Eden 开启时不创建天空纹理，节省其显存与加载时间）。
- **环境贴图**：`sky_texture_env` 的角色（combine 阶段 `env_s0/env_s1`）由 Eden 生成的 32×6 镜像立方体替代——太阳变化时用一次 CS 从 Sky-View LUT 填充，供 `CompureSpecularIrradance` 与 SSR 边界着色。

### 5.4 空气透视替换线性雾

`combine_1.ps.hlsl` 的雾分支在 `USE_EDEN_ATMOSPHERE` 下改为：

```hlsl
// 世界坐标由 s_position 重建；相机到像素的距离 d
float4 ap = s_aerial_perspective.SampleLevel(smp_rtlinear,
             float3(ndcToLutUV(I.tc0.xy), d / maxAPDistance), 0);
Color = Color * ap.rgb + ap.a;      // 透射 + 内散射
```

要点：

- 32³ 的 3D LUT 以"世界 XZ + 视距"为参数化[cite:1]，远山自然获得随距离渐变的蓝移/亮度衰减，替代 `fog_color` 均匀色纸；
- **先天空模式的结构性利好**：R4 天空在场景渲染前已作为背景层完成（§2.2），天空像素不会经过 combine_1 的雾处理——空气透视只需作用于场景像素，天空/远景的物理一致性天然成立，无需为天空像素做任何排除逻辑；
- **能见度兼容**：现有玩法依赖 `fog_distance/far_plane` 控制可见距离，映射策略是反解 Mie 系数使"等效能见度"（消光到 e^-2 的距离）等于旧 `fog_distance`，保证旧关卡观感不漂移；
- 高度雾/体积雾（fog volume 编辑器工具）继续走原有路径，仅把其颜色输入对齐到大气内散射量。

### 5.5 光照链路耦合

| 积分点 | 现状 | Eden 改造 |
|---|---|---|
| 太阳直射 | `Lights.sun_adapted` 每帧由 `CLight_DB` 从 `E.sun_color/sun_dir` 重建[Light_DB.cpp](file:///workspace/src/Layers/xrRender/Light_DB.cpp#L236-L262) | **唯一修改点在 `CLight_DB`**：`set_color` 输入由 `E.sun_color` 换为"标称辐照度 × TransmittanceLUT(太阳天顶角)"，黄昏自然变红；CSM 级联、`phase_accumulator` 直射累积、combine 的 `Ldynamic_color`、体积光柱全部读取 `sun_adapted`，自动全链路继承 |
| 环境光/半球 | `L_ambient`/`env_color` 来自 `ambient/hemi_color` 常量 | 来自 Irradiance LUT（天空辐照度积分），仍过 `r2_sun_lumscale_amb/hemi` 缩放，老游戏平衡可调回 |
| 体积光柱 | `accum_volumetric_sun` 以常量太阳色 march | 太阳色已被物理化，自动一致；可选把云层密度并入光柱 march |
| 反射环境 | 天气 `sky_texture_env` | Eden 生成的小环境立方体（§5.3） |
| 云影 | 无 | 云影图 R8 在 `phase_accumulator` 太阳累积时乘到 `Ldynamic_color`（§6.4） |

---

## 6. 体积云实现方案

### 6.1 建模（Schneider 体系）[cite:4][cite:5]

- **基础形状**：128³ RGBA8 的 Perlin-Worley 3D 噪声（R: Perlin-Worley，GBA: 三频 Worley），**离线烘焙**为 .dds（3D DDS）。
- **细节侵蚀**：两张 32³ RGBA8 Worley 高频噪声，运行时按世界坐标对基础密度做 Beer 腐蚀，形成碎边。
- **高度剖面**：云层底 1500 m / 顶 4000 m，按高度混合 stratus/cumulus/cirrus 概率函数。
- **天气图**：1024×1024 RG8（coverage + type），世界 XZ 平铺，决定哪里有云、什么云；可与现有 `wind_velocity/wind_direction` 字段联动产生平移演化。
- 若希望免发布资产：上述噪声可由一个首次运行时执行的 CS 生成器烘焙（约 2–5 s，一次性），但推荐离线烘焙入库以省启动时间与保证可控。

### 6.2 渲染管线

```text
phase_combine 开头（现 RenderClouds 位置、combine_1 之前，绑定 RDepth 依靠深度遮挡合成）：
  1. CloudRaymarch（1/2 分辨率，rt_Cloud_0 RGBA16F）
     - 相机射线与云层壳求交，得 [tStart, tEnd]
     - 近视距步进 48–64 步 + 远距离低频步进；每步：
         天气图采样 -> 高度剖面 -> 128³/32³ 噪声 -> 密度
         光照：向太阳 6 步稀疏 march + Beer-Powder + Hillaire 多重散射近似项
     - 输出 RGB（云散射，HDR 物理单位）+ A（透射率）+ 速度（可输出到独立 MRT）
  2. CloudTemporal（全分辨率上采样 + 时间重投影）
     - 用 rt_Velocity + 云层自运动偏移做重投影
     - 邻域 clamp 去鬼影（复制 SSLR temporal 模式[r4_rendertarget.cpp](file:///workspace/src/Layers/xrRenderPC_R4/r4_rendertarget.cpp#L570-L586)）
  3. 合成：combine_1 中在空气透视之前
     Color = Sky + CloudRGB；对云后方场景 Color *= CloudA
```

关键工程决策：

- **噪声烘焙进显存一次，全局共享**：8 MB 的 128³ 噪声是全系统最大的新增资产，却是所有云共用的；对比 SkyCube 每个天气周期 2–12 张大尺寸 cube map，总显存反而可能下降（§8.3）。
- **太阳光照复用大气 LUT**：云的太阳透射直接查 Transmittance/MultiScatter LUT（Hillaire 论文明确支持该加速路径[cite:1]），云与天空在黄昏自动同色。
- **半分辨率 + 时间累积**参照 HZD：整个天空体积云在 PS4 上 <2 ms 实时渲染[cite:8]，本引擎在 PC 中端 GPU + 半分辨率 + 时间重投影的同等档位下目标 1.0–2.0 ms（估算，§8.2）。

### 6.3 与天气/特效系统的联动

- `rain_density` > 0 时天气图 coverage 强制抬升、云底压低，与现有雨/雷暴系统共享触发时机（`CEffect_Rain`/`CEffect_Thunderbolt` 由同一 `CurrentEnv` 驱动，无需新耦合点）。
- 雷暴闪电：复用 `bolt_period/bolt_duration` 字段，在云 raymarch 中对随机位置的密度做短暂自发光注入（Nubis 2022 的内部光照近似思路[cite:7]）。

### 6.4 云影

每 4 帧一次 CS：沿太阳方向对天气图×高度剖面做 4 步投影积分，写 128×128 R8；`phase_accumulator` 中太阳累积时把阴影图乘入 `Ldynamic_color`，使地面出现移动的云斑——这是 SkyCube 体系完全无法提供的画面信息，且开销可忽略（<0.1 ms 估算）。

---

## 7. LevelEditor 植入方案

### 7.1 根基与边界（复核后的结论）

LevelEditor 是 **DX9 + xrECore 自定义管线**（§3.5）：无 ComputeShader/UAV、无 HDR RT、无现有 temporal 基础设施。因此：

- **Eden 的 R4/CS 实现不会自动出现在 LevelEditor 视口**，任何"自动生效"的预期都不成立；
- 但 `CEnvironment` 数据层（天气 lerp、`CEnvDescriptor`）为引擎层代码、编辑器与游戏共享[XrGamePersistentEditors.cpp](file:///workspace/src/Editors/xrECore/Engine/XrGamePersistentEditors.cpp#L15-L18)，**大气参数的编辑与存储与渲染后端无关**，可以完整落地；
- 编辑器视口的天空预览现走 `rsEnvironment` 开关下的 `dxEnvironmentRender` DX9 分支[UI_LevelTools.cpp](file:///workspace/src/Editors/LevelEditor/Editor/UI_LevelTools.cpp#L483-L496)。

据此，LevelEditor 侧的植入拆为三层：数据层（完整实现）、预览层（分阶段）、工具层（烘焙菜单）。

### 7.2 数据层：天气编辑 UI 扩展（零障碍）

- **LevelEditor 现状（V4 复核更新）**：编辑器**已有** "Weather Properties" 表单——经共享编辑器命令系统注册（`REGISTER_CMD_SE(COMMAND_WEATHER_PROPERTIES, ...)`[UI_MainCommand.cpp](file:///workspace/src/Editors/xrECore/Editor/UI_MainCommand.cpp#L730)），由 xrECore 的 `UIWeatherPropForm` 实现：0–24h 时间滑条（拖动即 `SetGameTime` + `env.lerp()`，视口即时刷新[UIWeatherPropForm.cpp](file:///workspace/src/Editors/xrECore/Editor/UIWeatherPropForm.cpp#L89-L111)）、时间倍率、天气周期选择（`WeatherCycles` 枚举 + `SetWeather`[UIWeatherPropForm.cpp](file:///workspace/src/Editors/xrECore/Editor/UIWeatherPropForm.cpp#L126-L162)）、太阳显示/太阳方向/半球选项。缺的是**逐字段天气参数编辑**（字段级编辑目前只在 xrPlay 的 `UIEditorWeather`）；另 `UIWorldPropertiesFrom` 是场景对象属性面板（`Scene->FillProp`），与天气无关[UIWorldPropertiesFrom.cpp](file:///workspace/src/Editors/LevelEditor/UI/UIWorldPropertiesFrom.cpp#L11-L20)。
- Eden 参数编辑的双宿主：**xrPlay 的 `UIEditorWeather`**（R4 进程，全字段编辑 + `saveWeather` 写回 .ltx[UIEditorWeather.cpp](file:///workspace/src/xrPlay/UIEditorWeather.cpp#L72-L100)）为主；LevelEditor 侧**扩展既有 `UIWeatherPropForm`**，新增 "Eden Atmosphere" 参数组（浊度/臭氧/地面反照率/云覆盖率/云类型/云高/云密度），直接读写共享 `CEnvDescriptor` 并复用 `saveWeather` 落盘；
- 旧字段（`sky_color/fog_*`）在 Eden 模式下灰显为只读映射预览（按 §4.3 的映射表反显示）；
- 两处 UI 编辑同一份 `CEnvDescriptor` 数据结构与同一批 .ltx 文件，游戏与编辑器天然一致。

### 7.3 预览层：分阶段策略

| 阶段 | 方案 | 说明 |
|---|---|---|
| 基线（随 M1 交付） | LevelEditor 视口**维持 DX9 legacy 天空**，Eden 开关只影响游戏；时间轴 scrub 预览能力**已存在**（`UIWeatherPropForm` 拖动时间即 `env.lerp()` 刷新 legacy 天空[UIWeatherPropForm.cpp](file:///workspace/src/Editors/xrECore/Editor/UIWeatherPropForm.cpp#L97-L105)），Eden 的参数脏标记机制天然接入该链路（`Invalidate` → 重烘 CPU/CS LUT）；高保真预览走 xrPlay 的 `UIEditorWeather`（R4 进程） | 零风险、零额外渲染开发；一致性由同一份天气 .ltx 保证 |
| 增强（M4+，可选） | **CPU 烘焙小型 Sky-View LUT**（建议 96×64 RGBA8，仅参数脏标记时重建，CPU 端每次数十毫秒级可接受），写入一张 2D 纹理；编辑器 DX9 天空 blender 改为采样该 LUT（加 shader option 切换），实现低保真物理大气预览；太阳盘/太阳色同步给出 | DX9 SM3.0 采样 2D LUT 无压力；精度足够编辑器取景使用 |
| 云的编辑器预览 | 不做真体积 raymarch；可选把云影图（128² R8，CPU 同样可烘）作为视口内的 2D 光照乘子叠加，传达"云影在动"的最小信息 | 避免在 DX9 上重建体积管线 |
| 不推荐 | 为 LevelEditor 移植 R4 管线或在编辑器中嵌 R4 离屏预览窗口 | 与 xrECore 现状冲突、工程量大；若未来需要高保真编辑器预览，更现实的路径是等 `xrRenderDS_R0` 新渲染接口层成熟后统一接入 Eden |

### 7.4 工具层：离线烘焙入口

- 噪声 3D DDS 与天气图模板的烘焙器实现为 LevelEditor 独立菜单项（纯 CPU/离线输出到 gamedata，不依赖渲染后端），**不**挂在现有 "Environment Generator"（植被散布工具）下[EnvGenerator.h](file:///workspace/src/Editors/LevelEditor/Editor/Utils/EnvGenerator.h#L8-L12)；
- 烘焙产物（.dds + 天气图模板）同时服务游戏运行时与 §7.3 的编辑器 CPU 预览，一份资产两端复用。

---

## 8. 性能分析：Eden Atmosphere vs SkyCube

### 8.1 方法声明

仓库内无 GPU 计时数据，以下全部为**架构推导 + 文献锚定的工程估算**（标注[估算]），供量级决策使用，不作为实测承诺。锚点：HZD 全天空体积云 PS4 <2 ms[cite:8]；Hillaire 方案在 iPhone 6s 到高端 PC 全程实时[cite:1]；UE5 Sky Atmosphere 生产应用[cite:9]。

### 8.2 GPU 帧开销对比（1080p，中端 GPU，默认档）

| Pass | SkyCube 现状 | Eden Atmosphere | 增量 Δ |
|---|---|---|---|
| 天空像素着色 | 0.1–0.3 ms（2×cube 采样+混合+tonemap）[估算] | 0.05–0.15 ms（1×Sky-View LUT 采样+太阳盘）[估算] | ≈持平 |
| LUT 烘焙 CS | 无 | 0.03–0.10 ms/帧（脏标记+摊销后均值）[估算] | +0.03–0.10 |
| 空气透视 | 包含在 combine_1（1 次 lerp） | +1 次 3D LUT 采样/像素 | +0.05–0.10[估算] |
| 体积云 raymarch | 无 | 1.0–2.0 ms（半分辨率 48–64 步）[估算，锚定[cite:8]] | **+1.0–2.0** |
| 云时间累积+上采样 | 无 | 0.1–0.25 ms（半分辨率 temporal，复用 SSLR 模式）[估算] | +0.1–0.25 |
| 云影 CS | 无 | <0.05 ms（每 4 帧 128² R8）[估算] | ≈+0.01 |
| **合计** | **≈0.1–0.3 ms** | **≈1.3–2.6 ms** | **≈+1.2–2.3 ms** |

结构性结论：

- **纯大气（不开云）开销 ≈ 0.1–0.3 ms，与 SkyCube 同数量级**——LUT 化后天空像素采样甚至比双 TextureCube 更便宜，新增的只有零星小尺寸 CS。
- **体积云占新增开销的 80%+**，且与分辨率解耦（半分辨率 + 时间累积），与 SkyCube 的差距本质是"贴图 vs 参与介质"的物理成本差，无免费午餐。
- 档位建议：`Low` 关闭云、天空走离线烘焙的 Bruneton LUT（启动时一次 CS 烘焙后零更新，内存同 1 MB）[cite:2][cite:3]；`Medium` 默认档（上表）；`High` 云步进翻倍 + 全帧 AP 更新。

### 8.3 显存与带宽对比

| 资源 | SkyCube | Eden Atmosphere |
|---|---|---|
| 天空纹理 | 每天气周期 1 组 cube（含 env 版本），运行时双组共存；粗估 10–40 MB（取决于贴图尺寸与周期数） | 0（退役）；仅环境反射小 cube ~50 KB |
| LUT 族 | 无 | ~1 MB |
| 云噪声 3D | 无 | 8 MB + 0.25 MB |
| 云渲染 RT | 无 | 半分辨率 RGBA16F×3（当前/历史/临时）≈ 12 MB@1080p |
| 云影图 | 无 | 16 KB |
| **净变化** | — | **约 −20 ~ +21 MB（与旧天气贴图规模对冲，可近似认为持平）** |

带宽方面，体积云是唯一大头（半分辨率读写 + 噪声采样），可通过 3D 纹理 BC 压缩（BC4/BC7 分块 3D，DX11.1+）把噪声压到 2–4 MB 并降低采样带宽——Low 档推荐开启。

### 8.4 CPU 与其他系统影响

| 维度 | 影响 |
|---|---|
| Draw call | SkyCube: 2（天空+云）→ Eden: 全屏 tri + 云 2 pass + 3–5 次 CS Dispatch；CPU 提交成本相当 |
| 顶点上载 | 现有天空/云每帧 VB/IB 上载可全部移除（全屏 tri + 静态资源） |
| 天气 lerp | 不变（CPU 原逻辑保留）；增加一个参数差阈值脏标记 |
| 编辑器（DX9） | 数据层共享、参数编辑 UI 正常工作；视口预览维持 legacy 天空，无额外 GPU/CPU 开销（§7.3）；可选 CPU LUT 预览仅在参数变更时付出数十毫秒一次性成本 |
| 加载时间 | 天气 cube 贴图退役可省加载；噪声 DDS 新增一次性读取 |
| 既有效果兼容 | 太阳光柱/SSLR/GTAO/TAA 无需改动；天空速度输出协议保留 |

### 8.5 结论

以约 **+1.2–2.3 ms GPU（默认档）[估算]、显存近似持平** 的代价，换取：物理自洽的全时段天空与太阳、随距离正确演化的空气透视、有体积有光照有云影的云层、可参数化的大气组成，以及"拖时间轴即时预览"的工作流（xrPlay/R4 天气编辑器与 LevelEditor 的 Weather Properties 表单均具备时间 scrub，后者叠加低保真 CPU-LUT 预览，见 §7）。对照 HZD/UE5/Frostbite 的生产实践[cite:8][cite:9][cite:6]，该开销在本世代 PC 上属于可接受区间；且系统可整体降级回 SkyCube 路径，风险有兜底。

---

## 9. 实施路线图与风险

### 9.1 里程碑

| 阶段 | 内容 | 涉及文件（新增为主） |
|---|---|---|
| M0 | CS 烘焙骨架 + Transmittance/MultiScatter LUT | `EdenAtmosphere.h/.cpp`、`eden_transmittance.ps.hlsl`（CS） |
| M1 | Sky-View 天空替换（主流程背景层挂载）+ 太阳/ambient 耦合 + 控制台开关 + 编辑器数据层（天气 UI 新字段） | `eden_sky.ps.hlsl`、`r4_R_render.cpp` RenderSky 分支、`r4_rendertarget_phase_combine.cpp` 常量改造、`UIEditorWeather.cpp` |
| M2 | Aerial Perspective 替换线性雾 + 能见度标定 | `combine_1.ps.hlsl` option、`eden_ap.ps.hlsl` |
| M3 | 体积云（噪声烘焙入库、raymarch、temporal） | `eden_clouds.ps.hlsl`、`rt_Cloud_*`、temporal 复用 |
| M4 | 云影 + 雷暴联动 + 编辑器低保真预览（可选 CPU-LUT）+ 烘焙菜单 | `UIEditorWeather.cpp`、`UIWeatherPropForm.cpp`、LevelEditor 菜单 |
| M5 | 档位/压缩/性能验证（PIX、`benchmark.script`、GPU_EVENT 剖面） | 既有工具链 |

### 9.2 风险与缓解

| 风险 | 缓解 |
|---|---|
| 美术风格漂移：X-Ray 的 PushGamma/legacy tonemap 平衡体系基于贴图天空调校 | Eden 输出物理单位后仍走现有 tonemap/exposure 链（combine_2 已有 s_tonemap[blender_combine.cpp](file:///workspace/src/Layers/xrRenderPC_R4/blender_combine.cpp#L64-L80)），提供 `eden_sun_intensity` 等锚点变量逐天气校准 |
| 旧关卡观感依赖 fog_distance/far_plane 的"雾墙" | 能见度等效反解（§5.4）+ `r_atmosphere_mode` 一键回退 |
| 背景/遮挡语义破坏（天空像素判定） | R4 天空本就无 stencil、依靠场景深度覆盖（"先天空后场景"）[r4_R_render.cpp](file:///workspace/src/Layers/xrRenderPC_R4/r4_R_render.cpp#L539-L548)，全屏三角形等价替换；R1/R2 legacy 路径的 stencil 合成保持原样不动 |
| FSR2/DLSS 下云 temporal 鬼影 | 云输出速度参与重投影；邻域 clamp + 云层自运动补偿 |
| DX10/DX9 路径（R1/R2）不支持 CS | Eden 仅挂 R4/DX11（游戏与 xrPlay 天气编辑器）；legacy 路径维持 SkyCube |
| LevelEditor（DX9 自定义管线）无 CS/HDR 能力，Eden 不自动生效 | 按 §7 三层策略：数据层完整落地、视口维持 legacy 天空、高保真预览走 xrPlay；可选 CPU-LUT 低保真预览作为增强，不阻塞主线 |
| 半分辨率云与光线步进伪影（条带/闪烁） | 蓝噪声抖动步进 + 时间累积 + 高度分层步长 |

---

## 10. 结论

IXRay 的 SkyCube 天空是零成本但零自由度的"贴图穹顶"：几何为 12 顶点半盒体，颜色来自双 TextureCube 权重混合（R4 下在主渲染流程中先于场景绘制为背景层，速度直写 rt_Velocity），雾为线性距离模型，云为平面贴图。而 R4 层已有的 ComputeShader/UAV 框架、HDR RT、速度缓冲、半分辨率 temporal 先例与 3DFluid 体积经验，意味着在游戏引擎侧植入一套 LUT 驱动的真大气系统所需的全部基础设施**已经存在**；需要特别指出的是，LevelEditor 并不使用 R4，而是 DX9 + xrECore 自定义管线（同名 `CRender` 曾造成误判），因此 Eden 在编辑器侧的落地是"数据层完整 + 预览分层"而非视口自动升级。Eden Atmosphere 采用 Hillaire 2020 的四 LUT 方案 + Schneider 体系的离线烘焙噪声体积云，可做到：纯大气开销与 SkyCube 同数量级（≈0.1–0.3 ms [估算]），体积云主导的完整方案 ≈1.3–2.6 ms/帧（1080p 默认档 [估算]），显存对冲后基本持平，并保留从全功能到关云降档直至完全回退 SkyCube 的三档弹性。建议按 M0–M5 路线推进，M1（主流程背景层挂载的天空替换 + 光照耦合 + 编辑器数据层）为价值/成本比最高的首个可交付点。

---

## Sources

- [cite:1] Hillaire, S. *A Scalable and Production Ready Sky and Atmosphere Rendering Technique*. EGSR 2020. https://sebh.github.io/publications/egsr2020.pdf
- [cite:2] Bruneton, E., Neyret, F. *Precomputed Atmospheric Scattering*. EGSR 2008. https://hal.inria.fr/inria-00288758/document
- [cite:3] Bruneton, E. *Precomputed Atmospheric Scattering: a New Implementation* (开源参考实现). https://ebruneton.github.io/precomputed_atmospheric_scattering/
- [cite:4] Schneider, A. *The Real-Time Volumetric Cloudscapes of Horizon Zero Dawn*. SIGGRAPH 2015. https://www.guerrilla-games.com/read/the-real-time-volumetric-cloudscapes-of-horizon-zero-dawn
- [cite:5] Schneider, A. *Nubis: Authoring Real-Time Volumetric Cloudscapes with the Decima Engine*. SIGGRAPH 2017. https://advances.realtimerendering.com/s2017/Nubis%20-%20Authoring%20Realtime%20Volumetric%20Cloudscapes%20with%20the%20Decima%20Engine%20-%20Final%20.pdf
- [cite:6] Hillaire, S. *Physically Based Sky, Atmosphere and Cloud Rendering in Frostbite*. SIGGRAPH 2016. https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/s2016-pbs-frostbite-sky-clouds-new.pdf
- [cite:7] Schneider, A. *Nubis, Evolved: Real-time Volumetric Clouds for Skies, Environments and VFX*. SIGGRAPH 2022. https://advances.realtimerendering.com/s2022/SIGGRAPH2022-Advances-NubisEvolved-NoVideos.pdf
- [cite:8] Schneider, A. 个人主页（HZD 云系统"PS4 上 2 ms 内渲染整个天空"表述）. https://www.schneidervfx.com/
- [cite:9] Epic Games. *Sky Atmosphere Component in Unreal Engine*（UE5 官方文档，Sky Atmosphere 基于 Hillaire 2020）. https://dev.epicgames.com/documentation/en-us/unreal-engine/sky-atmosphere-component-in-unreal-engine
