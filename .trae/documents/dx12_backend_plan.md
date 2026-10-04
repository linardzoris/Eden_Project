# R5 转型：D3D12 后端层 + 复用引擎现有 shader 管线

## Context（为什么做这件事）

当前 `xrRender_R5` 是**自包含的自写渲染器**：自己写 HLSL（`g_hlslCube`/`g_hlslTerrain`）、自己解析 level/OGF、自己管理 G-buffer 与合成、自带 PSO 与根签名。它不编译引擎共享层（`src/Layers/xrRender`），因此与引擎的材质系统完全脱节。

后果（已在实测中逐一验证）：丢掉顶点通道（`TEXCOORD1` = 烘焙光照图 lmap UV、`COLOR0`、`TANGENT/BINORMAL.alpha`）、丢掉 blender 驱动的多通道材质（mask + 4 路 detail + lmap）、progressive 网格的 LOD 窗口（OGF\_SWIDATA）也需自行复刻。结果是：**看不到 lightmap、看不到 detail、地形抖动**，且每修一处都要重新发明引擎已有的东西。

用户明确的目标（本次确认）：

> d3d12 的目的是：**兼容现有 shader 管线**的情况下，充分利用 D3D12 特性，提升性能并引入新的 shader model 支持和 DXR 光线追踪支持。**不需要从 0 写 shader 和 render**，而是构建**能够兼容现在 shader 的管线和 render**。

已确认的三个决策：

1. **架构路线**：新建 D3D12 后端层，复用引擎 shader 系统（对齐 `xrRenderDX10` 的定位）。
2. **第一阶段目标**：先打通**静态世界正确渲染**（建筑 + 地形，含 lmap / detail / progressive LOD）。
3. **shader 来源**：**原样编译引擎 shader**（`gamedata\shaders\d3d11\deffer_*.hlsl`）+ 严格对齐引擎常量布局。

预期结果：R5 变成 D3D12 版后端，引擎的 `CBlender_*`、`CResourceManager`、`ShaderElement`、`RCache/CBackend`、`FVisual/FProgressive/CSector` 原样跑在 D3D12 上；lmap/detail/LOD 等自动正确；后续可平滑接入 SM6/DXR。

## 关键架构依据（已核实）

* 共享层靠**编译期宏 + 类型别名**解耦，不是虚函数：

  * [HW.h](file:///d:/Eden_Project/src/Layers/xrRender/HW.h#L18-L36) 已有 `USE_DX12` 分支（M0 骨架）。

  * [DXCommonTypes.h](file:///d:/Eden_Project/src/Layers/xrRenderDX10/DXCommonTypes.h) 把 `ID3D11*` 改名成通用名（`ID3DDevice`/`ID3DVertexBuffer`…），共享层全用通用名编码。

  * [R\_Backend\_Runtime.h](file:///d:/Eden_Project/src/Layers/xrRender/R_Backend_Runtime.h#L10-L15) 用 `#ifdef USE_DX11` include 后端实现 → DX12 需加分支。

* R4 通过 CMake glob 共享层与 DX10 后端（[CMakeLists.txt](file:///d:/Eden_Project/src/Layers/xrRenderPC_R4/CMakeLists.txt#L5-L44)），**R5 没有**（[CMakeLists.txt](file:///d:/Eden_Project/src/Layers/xrRenderPC_R5/CMakeLists.txt#L4-L8) 只 glob 自身）。

* R5 的 `CRender` **未继承** **`R_dsgraph_structure`**（[r5.h](file:///d:/Eden_Project/src/Layers/xrRenderPC_R5/r5.h#L9)），所以没有场景图 / 可见性 / `set_Element→RCache.Render` 这条驱动链。

* 设备层已就绪：`src/xrEngine/Device_create_render_dx12.cpp` 提供设备/队列/双缓冲 swapchain/RTV·DSV 堆/围栏，经 `extern "C"` 与 [stdafx.h](file:///d:/Eden_Project/src/Layers/xrRenderPC_R5/stdafx.h#L52-L60) 暴露给 R5。

## 目标架构

新增 `src/Layers/xrRenderDX12/`，与 `src/Layers/xrRenderDX10/` 平行：

| 文件                                  | 职责                                                                    |
| ----------------------------------- | --------------------------------------------------------------------- |
| `DXCommonTypesDX12.h`               | D3D12 类型改名与通用枚举（BLEND/FILTER/COMPARISON 沿用同值类型，缩小泄漏面）                 |
| `dx12R_Backend_Runtime.h`           | `CBackend` 的 D3D12 inline 实现（PSO 合成、根签名、描述符表、上传环、`RecordDrawIndexed`） |
| `StateManager/dx12StateManager.*`   | 状态累积 → 按 hash 缓存 PSO；根签名缓存                                            |
| `dx12ResourceManager_Resources.cpp` | `_CreateVS/PS/GS`、`_CreatePass`、`_CreateDecl`                         |
| `dx12ConstantBuffer.*`              | CB 环形上传（替代 `Map(WRITE_DISCARD)`）                                      |
| `dx12BufferUtils.*`                 | VB/IB 创建、`D3DVERTEXELEMENT9 → D3D12_INPUT_ELEMENT_DESC`               |
| `dx12TextureUtils.*`                | `D3DFORMAT → DXGI_FORMAT`（可复用 dx10 版本）                                |

R5 侧改动：

* CMake 照 `xrRender_R4` 追加 glob `../xrRender/*.cpp(.h)`、`../xrRender/Blender_*.cpp`、`../xrRenderDX12/*.cpp(.h)`。

* `CRender` 改继承 `R_dsgraph_structure`（对齐 `r4.h`）。

* `level_Load` / `Calculate` / `Render` 改走共享层。

## 分阶段实施

### 阶段 0：骨架可编译（共享层在 USE\_DX12 下编过）

* **新建**：`xrRenderDX12/` 骨架文件（CBackend / StateManager / ConstantBuffer / BufferUtils 可先空实现打通链接）。

* **修改**：[R\_Backend\_Runtime.h](file:///d:/Eden_Project/src/Layers/xrRender/R_Backend_Runtime.h) 加 `USE_DX12` include 分支；R5 `CMakeLists.txt` 接入共享层与后端；`HW.cpp`（若存在）适配。

* **适配策略（重要）**：把 `#ifdef USE_DX11` 扩为 `#if defined(USE_DX11) || defined(USE_DX12)` 并**新增 DX12 分支、保留 D3D11 分支**；不删除、不全量替换。已知 D3D11 类型泄漏仅约 12 个文件（`R_Backend.h`、`r__dsgraph_types.h`、`SH_RT.h`、`SH_Atomic.h`、`ShaderResourceTraits.h`、`R_DStreams.cpp`、`dxFontRender.cpp`、`DetailManager.h`、`dxRenderDeviceRender.cpp`、`Blender_BmmD.cpp`、`r__dsgraph_render.cpp`）。

* **验收**：`cmake --build --preset Engine-x64-Windows-MixedAVX --target xrRender_R5` 通过；引擎加载 R5 DLL 进主菜单不崩。

### 阶段 1：最小垂直切片（引擎 shader 管线跑通）

* **目标**：用引擎 `CResourceManager` + 一个 `CBlender_*` + `RCache/CBackend`，在 D3D12 上画出**一个带贴图的静态网格**。

* **复用**：`CResourceManager::_cpp_Create`（[ResourceManager.cpp](file:///d:/Eden_Project/src/Layers/xrRender/ResourceManager.cpp#L154)）、`Blender_Recorder*`、`Shader.cpp`、`dx10r_constants.cpp` 的反射解析。

* **新建/移植**：`dx12ResourceManager_Resources.cpp` 的 `_CreateVS/PS`；真 `CBackend`（`set_PS/set_VS`、`Render→DrawIndexed`、`ApplyVertexLayout`、`ApplyRTandZB`、`constants.flush`）；根签名（CBV b0..bN + SRV t0..tN + 静态采样器）+ PSO 缓存；`CRender::shader_compile` DX12 分支（先沿用引擎现有 **D3DCompile → DXBC 5.0**，D3D12 原生接受 DXBC，可复用现有 `shaders_cache\d3d11` 产物，暂不必上 DXC）。

* **验收**：RenderDoc 抓到 `DrawIndexed`，且该网格贴图/光照正确。

### 阶段 2：静态世界正确渲染（本阶段的核心目标）

* **目标**：建筑 + 地形全部正确，lmap / detail / progressive LOD **自动**生效（因为走引擎 `FVisual/FProgressive/CSector` + blender）。

* **修改**：`r5.h` 让 `CRender` 继承 `R_dsgraph_structure`；`level_Load` 改用共享 `LoadBuffers/LoadVisuals/LoadSectors`；`Calculate/Render` 走 `r__dsgraph_*`。

* **复用**：`FVisual` / `FProgressive`（OGF\_SWIDATA LOD 窗口）/ `FHierrarhyVisual` / `CSector` / `DetailManager` / `CRenderTarget` 相位。

* **新建**：`SRVSManager` 等价物（描述符环形分配）、`_CreateTexture/_CreateRT/_CreateZB`。

* **验收**：进入关卡，画面与 R4 截图对齐（含光照图阴影、地形 detail、无抖动）。

### 阶段 3：DXR / SM6 接入点

* 在 dx12 后端补 SRV/UAV 根表、BLAS/TLAS 构建（源 `FVisual` 几何）、`Render()` 后的光追 dispatch；`shader_compile` 增加 **DXC → DXIL (SM6.x)** 分支（现有 `r5_dxr.cpp` 的 DXC 封装与 `-validator-version 1.3` 经验可直接迁移）。

* **验收**：开关光追出现反射/阴影。

## 风险与坑（按严重度）

1. **共享层 USE\_DX11 分叉导致编译地狱** → 必须"并行分支 + 保留旧分支"，禁止全量替换；优先用类型别名而非改逻辑。
2. **`shader_compile`** **的反射接口**：现有走 `D3DReflect`；后续换 DXC 时须用 `IDxcContainerReflection` 并与 `R_constant_table` 对齐（否则常量静默失效）。
3. **根签名/寄存器布局**必须与 `dx10r_constants.cpp` 反射出的 CB 绑定号严格一致。
4. **CBackend 即时提交 vs 命令列表**：状态需延迟到 `Render` 时合成 PSO；D3D12 资源屏障引擎无感知，需在 `set_RT/set_Textures` 处插入 Barrier。
5. **描述符堆模型差异**：引擎是 D3D11 槽位模型，DX12 需每帧重建 shader-visible 表并 `SetGraphicsRootDescriptorTable`；CB 必须按帧轮转上传环，避免覆写。

## 现有 R5 自写代码的处置

* **保留并迁移**：`r5_resources.h/.cpp` 的 `GpuDescriptorHeap` + `UploadRing`（迁入新描述符/常量层）；`r5_dxr.*` 作为阶段 3 的 DXR 参考；`r5_rendertarget` 的 RTV/堆管理思路。

* **弃用**：`r5_pipeline` / `r5_level` / `r5_visual` / `r5_texture`（与引擎管线不兼容，功能由共享 `CResourceManager` + `FVisual` 取代）。阶段 0–1 允许与旧代码共存，阶段 2 起删除。

* `r5.cpp` 的工厂与 stub 保留外壳，改为指向共享 `CResourceManager`。

## 验证方式

* 构建：`cmake --build --preset Engine-x64-Windows-MixedAVX --target xrRender_R5`；四个配置（Mixed/MixedAVX/Release/ReleaseAVX）分别构建并部署到对应 `bins\<CFG>\`。

* 运行：`启动游戏.bat` 选 MixedAVX；引擎日志 `_profiles\logs\ixray-*.log` 检查后端初始化与 shader 编译/缓存命中行。

* 功能验收（逐阶段）：

  * 阶段 0：进主菜单不崩。

  * 阶段 1：RenderDoc 抓帧确认 `DrawIndexed` 且纹理正确。

  * 阶段 2：进入关卡，与 R4 截图对比（光照图阴影、地形 detail、转动视角无抖动）。

* 回退：R5 加载失败时引擎自动回退 R4（[EngineAPI.cpp](file:///d:/Eden_Project/src/xrEngine/EngineAPI.cpp#L54-L67)），可用作对照。

## 说明

本计划按"先兼容、后增强"排序：阶段 0–2 只做**兼容**（把引擎既有管线搬到 D3D12，画面与 R4 对齐），阶段 3 起才引入 D3D12 独有能力（SM6 / DXR）。这样每阶段都有可验证产物，避免再次陷入"手写近似 shader 永远对不齐"的困境。
