# DX12 渲染管线（xrRender_R5）开发计划

## Context

当前引擎仅有 D3D11 延迟渲染器 xrRender_R4（含最近完成的 GTAO/SSLR/VSLR/HiZ/CAS 等特性）。目标：新增 DX12 渲染管线 `xrRender_R5`，与 R4 并存（`renderer_r5` 控制台/启动参数切换），为 DXR/高级特性铺路，并最终降低 CPU 开销、提升帧率。采用渐进式路线：先骨架后移植，每个里程碑可独立验证；着色器直接迁移 DXIL（DXC 编译 SM6.x）。

**已拍板决策**：渐进式（先骨架后移植）；R5 与 R4 并存；着色器直接迁移 DXIL；短期目标 = 最小 demo 跑通 → DXR 铺路 → 性能收益。

## 架构事实（已核实）

- 渲染器 DLL 加载：[EngineAPI.cpp](file:///d:/Eden_Project/src/xrEngine/EngineAPI.cpp#L45-L126) `LoadLibraryA("xrRender_R4.dll")`；DLL 入口在共享层 [EntryPoint.cpp](file:///d:/Eden_Project/src/Layers/xrRender/EntryPoint.cpp)（DllMain 设置 `::Render`/`::RenderFactory` 全局），**R5 通过 GLOB 包含 `../xrRender` 自动获得入口，无需新写导出代码**。
- 设备创建在引擎层：[Device_create_render.cpp](file:///d:/Eden_Project/src/xrEngine/Device_create_render.cpp#L183-L346) 按 `APILevel`（[device.h L23-27](file:///d:/Eden_Project/src/xrEngine/device.h#L23-L27)，当前仅 DX9/DX11）分发；D3D11 实现在 [Device_create_render_dx11.cpp](file:///d:/Eden_Project/src/xrEngine/Device_create_render_dx11.cpp)，全局 `void* HWRenderDevice/HWRenderContext/HWSwapchain/RenderRTV/RenderSRV/RenderDSV/SwapChainRTV`。
- 渲染器选择：`renderer renderer_r4` → [xr_ioc_cmd.cpp L524](file:///d:/Eden_Project/src/xrEngine/xr_ioc_cmd.cpp#L524) 置 `rsR4`（[defines.h L41](file:///d:/Eden_Project/src/xrEngine/defines.h#L41)）；启动流程 [xrPlay.cpp L154-168](file:///d:/Eden_Project/src/xrPlay/xrPlay.cpp#L154-L168)。
- R4 构成：[xrRenderPC_R4](file:///d:/Eden_Project/src/Layers/xrRenderPC_R4/CMakeLists.txt)（97 文件，含 CRender/CRenderTarget/phase_*）+ 共享 `../xrRender`（230 文件，API 无关）+ `../xrRenderDX10`（58 文件，D3D11 后端：RCache/SRVSManager/StateManager/BufferUtils/Texture）。
- 后端调用面集中在 [dx10R_Backend_Runtime.h](file:///d:/Eden_Project/src/Layers/xrRenderDX10/dx10R_Backend_Runtime.h) 的 CBackend（Render→SRVSManager→ApplyRTandZB→StateManager→DrawIndexed）。
- 着色器：`gamedata/shaders/d3d11/` 160+ HLSL，运行时 D3DCompile（[r4.cpp L1198](file:///d:/Eden_Project/src/Layers/xrRenderPC_R4/r4.cpp#L1198)），已用显式 `register(tN)` 绑定纹理，cbuffer 无显式 register。
- 引擎 UI：ImGui + SDL3 + D3D11 后端，[Device_create_render.cpp DrawMainViewport](file:///d:/Eden_Project/src/xrEngine/Device_create_render.cpp#L25-L52) 直接强转 `RenderSRV` 为 `ImTextureID`——**R5 需抽象此点**。

## 里程碑总览

| 里程碑 | 目标 | 验证标准 |
|---|---|---|
| **M0 骨架** | R5 dll 加载、DX12 设备/交换链、清屏 + ImGui | `xrEngine.exe -r5` 启动，ImGui UI 可见，纯色背景，控制台可切回 r4 |
| **M1 后端核心** | DX12 CBackend 全套（资源/状态/常量/PSO/描述符/屏障）+ DXC 编译链 | debug pass 画三角形/全屏 quad；一个简单 HLSL SM6.0 编译出图 |
| **M2 前向路径** | 关卡加载 + 前向简化渲染 + HUD | 主菜单 → 进图看到静态物体/HUD/console，颜色正确，无设备丢失 |
| **M3 延迟管线** | G-buffer + 灯光累积 + SMAP + occq | 同场景截屏对比 R4，光照/阴影误差 <5%，无闪烁 |
| **M4 后处理** | luminance/bloom/combine/AA/SPP | 完整帧像素差 <2%，HDR/曝光正确 |
| **M5 高级特性+性能** | GTAO/SSLR/VSLR/HiZ/CAS/FSR2(DX12)/DLSS(NGX DX12)；异步 compute、多命令列表 | 全特性 FPS ≥ R4；PIX 无 barrier 警告 |

phase 移植顺序按依赖：scene → accumulator → smap → occq → accum_* → luminance → bloom → combine → AA → screen_postprocess → gtao → sslr/vslr/hiz → cas/fsr。blender 与同名 phase 同批移植。

---

## M0 详细实施（首个执行目标）

### 新建 `src/Layers/xrRenderPC_R5/`

| 文件 | 内容 |
|---|---|
| `stdafx.h/.cpp` | PCH；私有注入 D3D12 类型别名（见风险#2） |
| `r5.h/r5.cpp` | `class CRender : public R_dsgraph_structure, public IDeviceRender` 最小实现：`create/destroy/reset_begin/reset_end/Begin/End`；`Render()` 仅对 backbuffer 做 clear；不建 CRenderTarget |
| `r5_rendertarget.h/.cpp` | 空壳 CRenderTarget |
| `CMakeLists.txt` | 仿 R4：GLOB 聚合自身 + `../xrRender`（**不含** `../xrRenderDX10`）；宏 `XRRENDER_R5_EXPORTS`+`USE_DX12`+`_USRDLL`；链 `d3d12.lib dxgi.lib dxguid.lib`；FSR2/DLSS 暂不链 |

注意：`EntryPoint.cpp` 中 `RImplementation`/`RenderFactoryImpl` 等符号由 `../xrRender` 共享源提供——R5 编译共享层时这些符号解析到 R5 的 CRender 实例。需核实共享层 `stdafx.h` 对 `USE_DX11` 的条件包含（`dx11HW.h` 等），R5 用 `USE_DX12` 走新分支。

### 修改引擎侧

| 文件 | 修改 |
|---|---|
| [device.h](file:///d:/Eden_Project/src/xrEngine/device.h#L23-L27) | `APILevel` 增加 `DX12` |
| **新建** `src/xrEngine/Device_create_render_dx12.cpp` | 仿 dx11 版：`D3D12CreateDevice`（FL 12_0）→ direct queue → `IDXGISwapChain3`（flip-model，3 backbuffer）→ RTV/DSV heap → 3×(command allocator) + command list → fence + event。填充 `HWRenderDevice`(ID3D12Device*)、`HWRenderContext`(当前帧 ID3D12GraphicsCommandList*)、`HWSwapchain`。导出 `CreateD3D12/UpdateBuffersD3D12/ResizeBuffersD3D12/DestroyD3D12`，及帧边界 `BeginFrameD3D12`（等 fence、reset allocator/list）/`EndFrameD3D12`（PRESENT barrier、Execute、Present、signal fence） |
| [Device_create_render.cpp](file:///d:/Eden_Project/src/xrEngine/Device_create_render.cpp) | switch 加 `case APILevel::DX12`（Create/Resize/Destroy 三处）；`DrawMainViewport` 中 `ImGui::Image(RenderSRV,...)` 对 DX12 暂跳过（M0 背景=清屏色） |
| [defines.h L41](file:///d:/Eden_Project/src/xrEngine/defines.h#L41) | 新增 `rsR5 = (1ul<<22ul)` |
| [xr_ioc_cmd.cpp L524](file:///d:/Eden_Project/src/xrEngine/xr_ioc_cmd.cpp#L524) | token 增加 `renderer_r5` → `rsR5` |
| [EngineAPI.cpp](file:///d:/Eden_Project/src/xrEngine/EngineAPI.cpp#L45-L72) | `r5_name="xrRender_R5.dll"`，优先序 R5>R4>R2；L203-237 token 列表加 `renderer_r5` |
| [xrPlay.cpp L154-168](file:///d:/Eden_Project/src/xrPlay/xrPlay.cpp#L154-L168) | 默认仍 r4；命令行 `-r5` 时执行 `renderer renderer_r5` |
| [src/CMakeLists.txt L32-35](file:///d:/Eden_Project/src/CMakeLists.txt#L32-L35) | `IXR_TEST_CI` 分支内加 `add_subdirectory("Layers/xrRenderPC_R5")` |

### M0 帧循环接法

`CRender::Begin()`（R5）内调 `BeginFrameD3D12` 并清屏 backbuffer；`CRender::End()` 内调 `EndFrameD3D12`。ImGui 渲染 M0 先走 SDL3 platform 层但 **跳过 GPU 绘制**（或接 imgui_impl_dx12 的最小集成：渲染到 backbuffer RTV，描述符用独立小堆）。M0 允许 UI 只画窗口边框文字即可，优先保证进程稳定。

### M0 验证

1. 构建：`cmake --build --preset Engine-x64-Windows-MixedAVX --target xrRender_R5`（及 xrEngine/xrPlay）。
2. 运行：复制产物到游戏 bins 目录，`xrEngine.exe -r5`（或控制台 `renderer renderer_r5` + 重启）。
3. 预期：窗口出现、日志含 `Loading DLL: xrRender_R5.dll`、背景清屏色、无 D3D12 debug layer ERROR（先以 `-dxdebug` 验证层全开跑一遍）。
4. 回退：删除 `-r5` 即回 R4，互不影响。

---

## M1 后端核心设计（骨架稳定后执行）

新建 `src/Layers/xrRenderDX12/`（对应 xrRenderDX10 的 DX12 版，R5 模块 GLOB 包含）：

- `dx12HW.cpp`：从引擎取 device/queue，管理 caps。
- `dx12R_Backend_Runtime.h`：DX12 版 CBackend，实现 §API 面全部分组（资源/状态/常量/几何绘制/RT/帧边界/occ query）。
- **资源屏障**：内嵌资源状态跟踪表（resource→state，按 subresource 折叠）。插入点：`ApplyRTandZB`（→RENDER_TARGET/DEPTH_WRITE）、`set_Textures`（RT/UAV 写态 → SRV 读态）、`Compute`（→UAV + UAV barrier）、`EndFrame`（→PRESENT）。首帧 COMMON 兜底；帧末清表（M3 后改跨帧持久态）。
- **根签名**：per-PSO、DXC 反射生成、按 `hash(vs,ps,gs,hs,ds)` 缓存 `ID3D12RootSignature`。cbuffer 走 CBV descriptor table + upload ring（64KB×3 帧槽，256B 对齐 carve）；纹理 SRV 按连续 slot 合并 table；采样器用 **static samplers**（Top32 组合内嵌根签名，罕见组合 M3 起 fallback 动态 sampler heap）。
- **描述符堆**：CBV/SRV/UAV shader-visible 单堆（1M），per-frame arena bump 分配 + `CopyDescriptorsSimple` 从持久 CPU heap 拷贝；RTV/DSV 持久 CPU-only + freelist。
- **帧同步**：frames-in-flight=3；WaitForFence→ResetAllocator→录制→Execute→Present→Signal。upload ring 16MB/帧；readback ring 独立（occ query/screenshot）。
- **着色器编译**：运行时 `dxcompiler.dll`（IDxcCompiler3，vs/ps/cs_6_0，`-O3`，保留反射）；`IDxcIncludeHandler` 桥接 FS 兼容 `.hlsli`；宏定义从现有 `D3D_SHADER_MACRO` 直转；磁盘 cache `shaders_cache/d3d12/{hash}.cso`（hash=source+macros+dxc 版本），附反射 dump。
- 着色器目录：`gamedata/shaders/d3d12/`（从 d3d11 复制起步，逐个修 SM6 兼容问题：如 `register` 冲突、已废弃 intrinsic）。
- **状态→PSO**：StateManager 的 blend/RS/DSS + input layout + shaders + RT formats 组合成 `D3D12_GRAPHICS_PIPELINE_STATE_DESC`，hash 缓存 `ID3D12PipelineState`。
- **occ query**：`ID3D12QueryHeap` + `Predication`（DX12 必须 fence 等帧，会暴露 R4 隐藏的一帧延迟，M2 起接入）。

## 风险清单（按概率）

1. **共享层 D3D11 类型泄漏**：[r4.h L96-98](file:///d:/Eden_Project/src/Layers/xrRenderPC_R4/r4.h#L96-L98) 等处 `ID3DVertexBuffer*`/`ID3DIndexBuffer*` 成员；`../xrRender` 共享头里亦有 `ID3DRenderTargetView*` 形参。对策：R5 不动共享头，PCH 注入 `using ID3DVertexBuffer = ID3D12Resource;` 等别名；冲突处再下沉到模块私有结构。
2. **ImGui DX12 后端**：`RenderSRV` 语义从 `ID3D11ShaderResourceView*` 变为 `D3D12_GPU_DESCRIPTOR_HANDLE`，`DrawMainViewport` 需抽象分支。
3. **CSCompiler 启动时全量预编译** blend 组合 → DX12 下必须异步/磁盘 cache，否则首启动卡死。
4. **VSLR cubemap 逐面捕获**：cubemap per-face RTV + 每面 barrier 序列（M5）。
5. **FSR2/DLSS/XeSS DX12 SDK** 接入（`FfxFsr2GetInterfaceDX12`、NGX DX12 path），CMake 并列新增。
6. **occ query 时序**：D3D11 阻塞 GetData → DX12 延迟一帧，R4 逻辑可能依赖即时结果。
7. **渲染器热切换不支持**：EngineAPI 假设启动时一次选择，M0 仅支持启动参数切换。
8. **Win10+ 限定**：DX12 切断 Win7 兼容分支。

## 执行顺序

1. **本次执行 M0**（骨架）：预计改动 ~12 文件 + 新建 ~8 文件。
2. M0 验证通过后，依次推进 M1（后端，最大工程量）→ M2（前向可见）→ M3/M4（parity）→ M5（特性+性能）。
3. 每个里程碑结束部署到 `bins\MixedAVX` 等 4 配置目录并删 shaders_cache 验证。

## 回滚方案

R5 全部新增于独立目录 + 引擎侧均为增量分支（`case APILevel::DX12`、新 token），不触碰 R4 现有代码路径；任何阶段出问题，删掉 `xrRender_R5.dll` 和不带 `-r5` 启动即完全回到现状。
