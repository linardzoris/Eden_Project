# 编辑器引入 DX11 实时渲染可行性研究

研究对象：LevelEditor / ActorEditor（xrECore 渲染栈），目标：评估引入 DX11，使编辑器具备实时渲染 engine 级（或近似）画面、快速预览最终效果。

---

## 1. 结论（TL;DR）

**可行，且仓库已经为此铺好了 70% 的路。** 核心依据：

1. 引擎公共层 `src/Layers/xrRender` 本来就是 **DX9/DX11 双后端设计**：`USE_DX11` 宏遍布 `Blender_Recorder`、`SH_RT.h`、`tss_def.h`、`r__dsgraph_render.cpp` 等文件；`r_constants_cache.h` 按 `USE_DX11` 在 `dx10r_constants_cache` 与 `dx9r_constants_cache` 之间切换。
2. 引擎设备层 `CRenderDevice` 已实现 `CreateD3D9()/CreateD3D11()` 双路径（[Device_create_render.cpp](file:///workspace/src/xrEngine/Device_create_render.cpp)），只是 `InitRenderDeviceEditor()` 目前硬编码 DX9、DX11 分支被 `#ifndef _EDITOR` 排除——**编辑器 DX11 化的口子是故意留着的**。
3. 编辑器渲染栈（xrECore）已经在用引擎公共层的模型/骨骼/shader 体系（`CModelPool`、`SkeletonX`、`Blender_Recorder_R2`），离"engine 近似"只差光照管线与后处理。
4. R4 里现成有 `imgui_impl_dx11.cpp`（[xrRenderPC_R4/imgui_impl_dx11.cpp](file:///workspace/src/Layers/xrRenderPC_R4/imgui_impl_dx11.cpp)），ImGui 后端可直接搬。

推荐路线：**编辑器渲染栈后端替换（dx9 → dx10/11）**，分三阶段推进；不建议把 R4 整个塞进编辑器（见 §4）。

---

## 2. 现状架构

### 2.1 编辑器渲染栈是"自拼的公共层 + DX9 后端"

xrECore 不链接 R1/R2/R4，而是把引擎公共层源码 + DX9 后端源码**直接编进 xrECore.dll**（[xrECore/CMakeLists.txt:35-124](file:///workspace/src/Editors/xrECore/CMakeLists.txt#L35-L124)）：

| 组成 | 文件 |
|---|---|
| 编辑器 CRender（自实现 `IRender_interface`） | [render.h:135](file:///workspace/src/Editors/xrECore/Editor/render.h#L135) `class CRender : public IRender_interface`，实例在 [render.cpp:29](file:///workspace/src/Editors/xrECore/Editor/render.cpp#L29) |
| 编辑器设备（继承引擎 `CRenderDevice`） | [device.h:31](file:///workspace/src/Editors/xrECore/Editor/device.h#L31) `CEditorRenderDevice` |
| 公共层 | R_Backend、Shader、ResourceManager、SkeletonX/FVisual/FSkinned、ModelPool、ParticleEffect、Blender_Lm(EbB)/Model_EbB/Editor_Wire/Editor_Selection 等 |
| DX9 后端 | dx9HW、dx9Texture、dx9SH_RT、dx9SH_Texture、dx9r_constants_cache、dx9ResourceManager_Resources/Scripting |
| RCache 绑定 | [r_constants_cache.h:8-10](file:///workspace/src/Layers/xrRender/r_constants_cache.h#L8-L10)：`USE_DX11` 时用 dx10 版——**公共层这边的切换是现成的** |

### 2.2 编辑器渲染的"引擎近似度"现状

- 模型/骨骼/动画：**已经是引擎路径**。`CModelPool` 加载 .ogf，ActorEditor 直接 `RImplementation.model_RenderSingle(...)`（[UI_ActorTools.cpp:80](file:///workspace/src/Editors/ActorEditor/UI_ActorTools.cpp#L80)）；蒙皮走公共层 `FSkinned/SkeletonX`（USE_DX11 下有 dx10 版蒙皮常量缓冲）。
- Shader：走 `Blender_Recorder`（R2 风格，`get_generation()=GENERATION_R2`、`addShaderOption("USE_LMAP","1")`，[render.cpp:967](file:///workspace/src/Editors/xrECore/Editor/render.cpp#L967)），shader 源码与引擎同一套（`$game_shaders$` + ID3DInclude）。
- 光照：自建轻量 `CRenderTarget : IRender_Target`（[render.h:47](file:///workspace/src/Editors/xrECore/Editor/render.h#L47)），复用 R2 的 `r2_rendertarget_accum_spot/point_geom.cpp` 做点光/聚光灯累积；**无太阳阴影、无 SSAO/Bloom/Tonemap 等 RT 链**（`set_blur/set_gray/...` 全是空实现）。
- 环境：`rsEnvironment` 打开时已接 `dxEnvDescriptorMixerRender` 天空纹理（[render.cpp:399-404](file:///workspace/src/Editors/xrECore/Editor/render.cpp#L399-L404)）。

### 2.3 设备层与 ImGui

- 设备创建：编辑器 `CEditorRenderDevice::Create()` → `InitRenderDeviceEditor()`（[Device_create_render.cpp:168-182](file:///workspace/src/xrEngine/Device_create_render.cpp#L168-L182)，硬编码 `CreateD3D9()`）；引擎同文件已有完整 `CreateD3D11()/ResizeBuffersD3D11()/DestroyD3D11()`。
- ImGui：xrEUI 的 `XrUIManager::Initialize(HWND, IDirect3DDevice9*, ini)`（[XrUIManager.cpp:52](file:///workspace/src/Editors/xrEUI/XrUIManager.cpp#L52)），imgui_impl_dx9 后端；xrEUI 链接的是独立 `imgui` 库（[xrEUI/CMakeLists.txt:38](file:///workspace/src/Editors/xrEUI/CMakeLists.txt#L38)）。
- 多视口：`UIRenderForm::DrawVP()` 用 `RDevice->StretchRect` 把视口 RT 拷到冻结 RT（[UIRenderForm.cpp:139](file:///workspace/src/Editors/xrECore/Editor/UIRenderForm.cpp#L139)），视口 RT 管理在 `ui_main.cpp`（约 30 处 DX9 引用）。引擎侧已有对应 DX11 模式：`DrawMainViewport()` 用 `ImGui::Image(RenderSRV, ...)` 显示一帧（[Device_create_render.cpp:26-53](file:///workspace/src/xrEngine/Device_create_render.cpp#L26-L53)）。

### 2.4 引擎 R4 的构成（参照模板）

[r4/CMakeLists.txt](file:///workspace/src/Layers/xrRenderPC_R4/CMakeLists.txt)：`xrRenderPC_R4/*.cpp` + **全部 `xrRender/*.cpp`（公共层）** + **全部 `xrRenderDX10/*.cpp`（DX10/11 共用代码）**，`USE_DX11` 定义，链接 d3d11/dxgi/d3dcompiler（+FSR2/DLSS）。也就是说 **xrRenderDX10 = "DX11 通用后端"**，R4 只是它的场景级封装。

### 2.5 编辑器与引擎的接口边界

- 编辑器通过 `RenderFactory = &RenderFactoryImpl; UIRender = &UIRenderImpl; RImplementation.Initialize();` 注入渲染实现（[device.cpp:121-142](file:///workspace/src/Editors/xrECore/Editor/device.cpp#L121-L142)）。
- LevelEditor 还链接 xrAbstractions/xrScripts/xrSound，依赖 xrGame + xrSE_Factory 构建 spawn/AI 数据。
- 引擎运行时用 `xrRender_R4.dll` 动态加载（[EngineAPI.cpp:48](file:///workspace/src/xrEngine/EngineAPI.cpp#L48)），编辑器则静态编入自己的 CRender——**两条装配线互不干扰，接口都是 `IRender_interface`**。

---

## 3. DX9 耦合点清单（改造工作量盘点）

编辑器内 DX9 专属符号引用约 **259 处 / 30 个文件**，其中 ~85% 集中在 xrECore 的 10 个文件：

| 耦合点 | 位置 | DX11 对策 |
|---|---|---|
| `REDevice/REContext/RETarget/...` 宏硬编码 `IDirect3DDevice9*` | [device.h:24-29](file:///workspace/src/Editors/xrECore/Editor/device.h#L24-L29) | 改成 `HW.h:18-32` 同款 `#ifdef USE_DX11` 双定义（`ID3D11Device*/ID3D11DeviceContext*/RTV/DSV/IDXGISwapChain*`） |
| 设备创建 | `InitRenderDeviceEditor()` | 去掉 `#ifndef _EDITOR` 门禁（[Device_create_render.cpp:326-333](file:///workspace/src/xrEngine/Device_create_render.cpp#L326-L333)），改调 `CreateD3D11()` |
| 固定管线状态 `SetRS/SetSS/LightEnable/SetMaterial(D3DLIGHT9/D3DMATERIAL9)` | [device.h:114-145](file:///workspace/src/Editors/xrECore/Editor/device.h#L114-L145) + 各处调用 | 状态子集映射（引擎 `dx10StateUtils`/`Blender_Recorder` 的 USE_DX11 分支已有先例）；灯光/材质 → 常量缓冲 + 编辑器光照 HLSL |
| 图元绘制 `DP/DIP(D3DPT_*)`、FVF 顶点 | [device.h:111-112](file:///workspace/src/Editors/xrECore/Editor/device.h#L111-L112)、[EditMeshRender.cpp](file:///workspace/src/Editors/xrECore/Editor/EditMeshRender.cpp)（25 处）、`D3DUtils.cpp`（72 处） | FVF → `D3D11_INPUT_ELEMENT_DESC` 翻译 + 小型 HLSL（线框/纯色/带贴图三支）；`D3DUtils` 的 Draw_Grid/Box/Gizmo 原语走同一路径 |
| `BeginScene/EndScene/Present/TestCooperativeLevel` | [device.cpp:490-536](file:///workspace/src/Editors/xrECore/Editor/device.cpp#L490-L536) | DX11 无 Begin/EndScene：OMSetRenderTargets + Present；设备丢失检测改为 DXGI `DXGI_ERROR_DEVICE_REMOVED` |
| `Clear(D3DCLEAR_*)`、`D3DVIEWPORT9`、`rmNear/rmFar` | [device.cpp:214](file:///workspace/src/Editors/xrECore/Editor/device.cpp#L214)、[render.cpp:451-468](file:///workspace/src/Editors/xrECore/Editor/render.cpp#L451-L468) | `ClearRenderTargetView/ClearDepthStencilView`、`RSSetViewports`（near/far 改投影矩阵或 RevZ） |
| 视口 RT 与 `StretchRect` | [UIRenderForm.cpp:139](file:///workspace/src/Editors/xrECore/Editor/UIRenderForm.cpp#L139)、`ui_main.cpp` | `CopyResource`（冻结帧）或直接 `ImGui::Image(SRV)`；RT 结构从 Surface 改 Texture2D+RTV+SRV |
| Shader 创建 `CreateVertexShader/CreatePixelShader` + CTAB 常量解析 | [render.cpp:728-795](file:///workspace/src/Editors/xrECore/Editor/render.cpp#L728-L795) | 换 `dx10ShaderCompile` 路径（R4 同款：反射取常量，目标自动升到 vs_5_0/ps_5_0） |
| ImGui 后端 | xrEUI 全套 + 各编辑器 `imgui_impl_dx9.cpp` | 搬 R4 的 `imgui_impl_dx11.*` 到 xrEUI；`XrUIManager::Initialize` 改收 `ID3D11Device*` |
| D3DX9 依赖（d3dx9.lib、D3DX 矩阵/纹理函数） | xrECore CMakeLists 192-196、D3DUtils、TLSPRITE | 矩阵运算 → xrCore/DirectXMath；纹理处理已有 DirectXTex（链接项已存在） |
| 零散工具引用（纹理查看器、Wallmark、AIMap Render、Cursor3D、EShape 等约 20 个文件） | LevelEditor/ParticleEditor 各处 | 跟随宏自动切换，个别 `REDevice->SetRenderState` 需要走新的状态层 |
| 编辑器 CRenderTarget 的 `IDirect3DVertexBuffer9/9IB` | [render.h:102-106](file:///workspace/src/Editors/xrECore/Editor/render.h#L102-L106) | R4 的 `accum_*_geom` DX11 版在 R2 累积几何文件里已有宏分支，直接编 dx11 分支 |

好消息：**编辑器用的所有公共层 blender（Blender_Editor_Wire/Selection、Blender_Lm(EbB)、Model_EbB、Particle、tree、detail_still）在 `USE_DX11` 下都能走 dx10R_Backend 编译出 SM5**，因为 R4 正是这么用它们的。

---

## 4. 方案对比

| 方案 | 内容 | 收益 | 成本/风险 |
|---|---|---|---|
| **A. 后端替换（推荐）** | xrECore 按 R4 的配方重编：dx9 文件集 → dx10 文件集 + `USE_DX11`；清掉 §3 耦合点 | 编辑器跑在 DX11 上，shader 与 R4 同为 SM5；去掉 d3dx9 遗产；为质量升级铺路 | 中等（§3 清单，核心 10 文件）；不动引擎，不碰 R4 场景管线 |
| B. 内嵌 R4 | 编辑器动态加载 `xrRender_R4.dll`，用完整 dsgraph/env/RT 管线渲染 | 真 engine 级画面 | 高：R4 深度耦合引擎（CRenderTarget 二十几个 phase、环境系统、GamePersistent、level CDB）；编辑器场景对象（EScene*Object）不是 engine visual，喂不进 dsgraph；LevelEditor 里"未构建的关卡"无法直接渲染 |
| C. 双轨 | 编辑器内保持 DX9 预览 + 一键导出/构建后在 xrEngine 打开（引擎已是 DX11 + 内置 ImGui 编辑器 UI） | 零渲染改造 | "预览"不是实时的，割裂；且引擎内编辑器 UI 是另一套栈 |

方案 A 是 B/C 的前置：先 A，后按需演进到"R4-lite"（见 §5 Phase 2），B 的诉求大半会被覆盖。

---

## 5. 分阶段实施计划

### Phase 0：DX11 化（画面不变，后端升级）

1. **构建**：xrECore CMakeLists——把 §2.1 的 dx9 文件集替换为 R4 同款 `xrRenderDX10/*.cpp` 集 + 定义 `USE_DX11` + 链接 `d3d11.lib dxgi.lib d3dcompiler.lib`（模板即 [r4/CMakeLists.txt:60-71](file:///workspace/src/Layers/xrRenderPC_R4/CMakeLists.txt#L60-L71)）；移除 d3dx9。
2. **设备**：`InitRenderDeviceEditor()` 走 `CreateD3D11()`（去掉 `_EDITOR` 门禁）；`device.h` 宏改双定义。
3. **ImGui**：`imgui_impl_dx11.*` 入 xrEUI；`XrUIManager` 换签名。
4. **绘制层**：DP/DIP + FVF → input layout + 编辑器 HLSL；`SetRS/SetSS` 子集映射；灯光/材质 → 常量缓冲（`D3DUtils`/`EditMeshRender`/`ui_main` 为主要战场）。
5. **视口**：`ui_main.cpp` RT 结构改 Texture2D+RTV+SRV；`StretchRect` → `CopyResource`/`ImGui::Image`。
6. **shader 编译**：`create_shader`（[render.cpp:728](file:///workspace/src/Editors/xrECore/Editor/render.cpp#L728)）换 dx10 编译路径。
7. 扫尾：DeviceScreenshot、TLSPRITE、EThumbnail、各工具零散点。

验收：四个编辑器全部功能回归（LOD 生成、纹理查看、粒子、AIMap），画面与现在一致，RenderDoc 可抓帧。

### Phase 1：近似 engine 画质（在编辑器 CRender 上做加法）

1. **太阳光 + 阴影图**：编辑器 `CRender` 已收集 `m_spotlights`（`light` 类是公共层，带 `bShadow` 与 `LR.compute_xf_spot`，[render.cpp:349-367](file:///workspace/src/Editors/xrECore/Editor/render.cpp#L349-L367)），加一个 directional "sun" 相位（复用 R2/R4 的 shadow_volume/accum 代码路径，`bShadow=TRUE`），这是编辑器与游戏内观感差距最大的一项。
2. **RT 链 lite**：场景渲到离屏 RT → 简单 bloom + tonemap 回 blit 到视口（shader 全部用现成 `$game_shaders$` R2/R4 资产，走 `s_accum` 同款流程）。
3. **环境增强**：天空盒全屏 pass（sky blender）+ env map 已有，补 `set_color_add` 类后处理参数。

### Phase 2：engine 级预览（可选、按需）

- LevelEditor：对**已构建关卡**（.level 存在时）加载 CDB/scene 数据直接走 dsgraph 渲染（LevelEditor 已链接 xrGame/xrSE_Factory 和 `DetailManager*`）；或利用引擎内置编辑器 UI（[Device_create_render.cpp:184-346](file:///workspace/src/xrEngine/Device_create_render.cpp#L184-L346) 的 EditorUI）做"发送到引擎预览"。
- ActorEditor：预览模型已是引擎 visual，Phase 1 完成后画质已接近游戏内；可再加第一人称 HUD/武器姿态预览。

---

## 6. 风险与限制

1. **固定管线模拟面**：`D3DUtils`（72 处）+ 工具零散调用是纯 DX9 习惯写法，需统一收敛到新绘制层——这是最大的一块体力活，但边界清晰。
2. **`D3DSAMP_MIPMAPLODBIAS` 等 sampler 状态**：DX11 采样器对象语义不同（负 bias 无对应），`_SetupStates()`（[device.cpp:303-326](file:///workspace/src/Editors/xrECore/Editor/device.cpp#L303-L326)）需重写。
3. **视口 near/far 裁剪**（rmNear/rmFar 用 D3D9 viewport z-range）：DX11 viewport MinDepth/MaxDepth 语义相同，可直接映射，风险低。
4. **编辑器 shader 依赖 `USE_LMAP=1` 的 R2 分支**：SM5 下 Lm(EbB) 系 blender 的兼容性由公共层保证（R4 实测在用），但编辑器 `editor\` 目录 shader 若引用 ps_2_0 专属指令需小修。
5. **ActorEditor/ParticleEditor 共用 xrECore**：一次改造全编辑器受益，但回归测试范围是全部四个编辑器。
6. **min spec**：DX11 FL 11_0（引擎 R4 现行要求一致）。

---

## 7. 关键代码索引

| 主题 | 位置 |
|---|---|
| DX9/DX11 宏切换样板 | [Layers/xrRender/HW.h:18-32](file:///workspace/src/Layers/xrRender/HW.h#L18-L32) |
| 引擎 DX11 设备创建 | [Device_create_render.cpp](file:///workspace/src/xrEngine/Device_create_render.cpp)（CreateD3D11 / DrawMainViewport / InitRenderDevice） |
| R4 构建配方 | [xrRenderPC_R4/CMakeLists.txt](file:///workspace/src/Layers/xrRenderPC_R4/CMakeLists.txt) |
| DX10/11 共用后端 | [src/Layers/xrRenderDX10](file:///workspace/src/Layers/xrRenderDX10)（r_constants_cache、R_Backend_Runtime、StateUtils、Texture、ConstantBuffer） |
| 编辑器设备/渲染栈 | [xrECore/Editor/device.h](file:///workspace/src/Editors/xrECore/Editor/device.h) · [device.cpp](file:///workspace/src/Editors/xrECore/Editor/device.cpp) · [render.h](file:///workspace/src/Editors/xrECore/Editor/render.h) · [render.cpp](file:///workspace/src/Editors/xrECore/Editor/render.cpp) |
| 编辑器 DX9 后端文件集（待替换） | [xrECore/CMakeLists.txt:35-124](file:///workspace/src/Editors/xrECore/CMakeLists.txt#L35-L124) |
| ImGui DX11 后端 | [xrRenderPC_R4/imgui_impl_dx11.cpp](file:///workspace/src/Layers/xrRenderPC_R4/imgui_impl_dx11.cpp) |
| 编辑器 ImGui 管理器（待换后端） | [XrUIManager.cpp:52-172](file:///workspace/src/Editors/xrEUI/XrUIManager.cpp#L52-L172) |
| 视口/RT 管理（待 DX11 化） | [UIRenderForm.cpp](file:///workspace/src/Editors/xrECore/Editor/UIRenderForm.cpp) · ui_main.cpp |
| 编辑器绘制原语（待收敛） | [D3DUtils.cpp](file:///workspace/src/Editors/xrECore/Editor/D3DUtils.cpp) · [EditMeshRender.cpp](file:///workspace/src/Editors/xrECore/Editor/EditMeshRender.cpp) |
