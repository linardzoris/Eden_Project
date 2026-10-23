# 超大开放世界渲染瓶颈研究（持续更新）

> 起始日期：2026-10-22。本文是活跃研究文档：深挖过程中发现的错误或更优方案会持续更新进各节。
> 研究对象：R4 (DX11 deferred) 主渲染路径，超大关卡（参考 k02：61.7M 三角形 / 10542 visual，实测单帧 draw 数 14K+）。

---

## 1. 帧管线总览（已验证）

每帧序列（`CRender::render`，r4_R_render.cpp）：

```
Calculate(dsgraph 构建)
→ Z-prefill（R2FLAG_ZFILL，静态 only，0.25×far 短投影，PHASE_SMAP，深度 only）
→ G-buffer render_main(true)（PHASE_NORMAL，HQ/LQ 按距离 50m 档位）
→ DEFER_TEST_LIGHT_VIS（逐灯 occq 查询签发 + 光源体积绘制；结果回读在帧尾 render_lights 累积阶段）
→ 太阳级联 ×3（R_sun.cpp）
→ phase_accumulator（点光/聚光累积，体积光）
→ VSLR 离屏反射立方体 ×6（offscreen_reflecitons 开启时，静态 only，HOM 禁用）
→ forward 第二遍 render_main(false)（半透明/secondary）+ 粒子 CParticlesAsync::Wait()
→ 雨 subspace（雨效帧）
→ combine（SSAO/GTAO → SSLR → VXGI/HVGI → volumetric → bloom → tonemap → AA → 上采样 → CAS → gamma）
```

### 每帧场景遍历扇出（核心瓶颈）

| 路径 | 次数 | 位置 |
|---|---|---|
| zfill | ×1 | r4_R_render.cpp#L550 |
| G-buffer | ×1 | r4_R_render.cpp#L572 |
| 太阳级联 | ×3 | R_sun.cpp#L293 |
| 光源阴影（每个阴影 spot） | ×N | lights_render.cpp#L140 |
| VSLR 立方体（开启时） | ×6 | r4_R_render.cpp#L463-486 |
| forward 第二遍 | ×1 | r4_R_render.cpp#L818 |
| 雨 | ×1 | r4_R_rain.cpp#L216 |

开 VSLR + 雨 = **13+N 次遍历/帧**；不开也至少 6+N。
每次 `r_dsgraph_render_subspace`（r__dsgraph_render.cpp#L348）= portal/sector 递归（CSector::traverse，r__sector.cpp#L82-215）+ 静态部分对每个命中 sector 的 root visual 走 **hierrarhy 树递归**（r4.cpp#L503 add_Geometry → r__dsgraph_build.cpp#L551 add_Static：每节点 testSAABB 视锥测试 + HOM 测试，fcvFull 子树切换到 add_leafs_Static（L362）免视锥仅 HOM+SSA——剔除层级只有这棵导出期的粗粒度球/盒树，无空间索引、虚拟调用递归、无缓存友好布局）+ 动态物 `ISpatial_DB` 树查询（带 TBV 缓存，健康）。
（轮 5 修正：本节原写"sector 内 r_visuals 线性列表全量测试"——本代码库无此结构，实际是上述 hierrarhy 递归；"完全无层级剔除"的说法过重，真实低效点在于每节点独立测试的常数开销与扇出次数。）

---

## 2. 瓶颈清单（按严重度）

1. **几何遍历/绘制扇出（13+N）**——同一静态几何一帧被遍历 13+N 次、光栅化 10+N 遍；每个 visual 一次独立 draw（r__dsgraph_render.cpp#L104，`Ni.pVisual->Render(LOD)`），无静态合批/instancing。
2. **帧无重叠架构 + 渲染线程内同步 stall**——三线程（主=渲染 / GameThread / Pre-Render），帧尾 `secondary_tasks.wait()` 硬屏障（device.cpp#L287-308）；渲染线程内任何 stall（occq 忙等、纹理同步加载、D3DCompile 未命中）全额吃帧时间，无错帧缓冲。
3. **occq 同帧同步回读**——`occq_get` 忙等 `GetData`（0.1ms 超时后假装可见，池 1536）；用户是光源可见性（light_vis.cpp）+ smapvis + pixel_calculator，不是几何剔除。
4. **纹理零流送、首用同步加载**——`CTexture::Load`→`texture_load` 全链同步（dx11SH_Texture.cpp#L490-509），新材质首现 = 渲染线程 hitch；无 mip 流送/驻留管理/加载队列；卸载仅 DeferredUnload。
5. **GPU 全屏 pass 链带宽**——combine 后 ~15 个全屏 pass，1080p 下 ~200MB+/帧纯带宽。
6. **draw call 总量**——状态分组排序（FixedMAP 桶）但无合批，14K+ draw/帧。
7. **关卡几何一次性全载**——level.geom VB/IB 池常驻（IGame_Level.cpp#L110-148），61M 三角形 ≈ 1.5GB 级常驻，无分块流送。

### 已证伪/降级的初判（错误修正记录）

- ❌ "dsgraph 容器是红黑树每帧重分配风暴" → **FixedMAP = 连续数组 + 无平衡 BST + bump 分配**（FixedMap.h），`clear()` 只置 `pool=0`，节点内存跨帧复用，分配成本可忽略。残留问题：无平衡 BST 在 key 接近有序时退化 O(N)（静态几何按 sector 列表顺序插入，VS/PS key 分布小，影响中低）。
- ❌ "遮挡查询用于几何剔除" → occq 的实际用户是**光源可见性**（light_vis/smapvis）+ pixel_calculator；几何遮挡靠 HOM 软件光栅（hom.cpp，可推 Pre-Render 线程 `R2FLAG_EXP_MT_CALC`）。
- ❌ "HOM MT 是否默认开启待查" → **`r2_mt` 默认开启**（xrRender_console.cpp#L129 默认 flags 含 R2FLAG_EXP_MT_CALC），HOM 光栅与细节计算已在 Pre-Render 线程。非瓶颈。
- ❌ "草地 detail / 树 billboard 是 draw call 大户（待查）" → **两者都已批处理良好**（见 §7.2/§7.3），从待办划掉。真正的 draw 大户只有静态/动态物体本体。
- ➕ 初判遗漏的两个遍历热点：VSLR 6 面立方体、雨效 subspace（后经全调用点盘点补上）。

---

## 3. 深挖方案（三条线）

### 3.1 occq 下一帧回读（优先级 1，~60 行，草稿见 _code_temp\02）

**先例**：`smapvis` 已实现同款（light_smapvis.cpp#L70 `testQ_frame = Device.dwFrame + 1`，`flushoccq` 到期帧才读）。

**当前时序（轮 5 实测修正）**：
- 查询签发：`vis_prepare`（发查询+画光源体积）在 DEFER_TEST_LIGHT_VIS（r4_R_render.cpp#L610-674，vis_prepare 调用点 L640/651/662）——紧跟 G-buffer PART0 之后
- 忙等回读：`vis_update` 在 render_lights 累积阶段（lights_render.cpp#L195/208/240/258；调用点 r4_R_render.cpp#L785 LP_normal / L791 LP_pending）
- **两步之间隔着 ZBUFFER_COPY→PhaseWinter→WALLMARKS→FLUSH_OCCLUSION→RAIN→SUN×3 级联→SELF_ILLUM（L689-779）**——GPU 已有大半帧时间消化查询；忙等主要在 GPU 积压帧咬人
- `occq_get` 是**纯自旋**（无 Sleep/Yield，r__occlusion.cpp#L130-144），0.1ms 超时后 `fragments=5` 假装可见；每灯上限 0.1ms CPU
- 复测节奏：可见灯 +10~20 帧随机（delay_large_min/max，light_vis.cpp#L6-7），不可见灯次帧复测（轮 5 修正：原写"40-200 帧"有误）

**设计（harvest-in-prepare，smapvis 模式）**：查询照旧 F 帧签发；`vis_update` 变空操作，累积用上一轮已知可见性；下一次 `vis_prepare` 开头先收割遗留查询（至少 1 帧旧，GetData 立即返回无 stall）。可见性决策延迟从 ~1 帧变为一个复测周期（可见灯 10-20 帧/不可见灯次帧），对光源体积剔除无感。
- 坑①：空包时 `occq_refresh()` 销毁全部在途查询（r4_R_render.cpp#L612-613 + r__occlusion.cpp#L41-55）——加 `in_flight()==0` 守卫（同时修掉 smapvis 的同款潜在隐患）
- 坑②：灯带 pending 查询销毁会永久漏池槽（槽只在 occq_get 里归还）——`~light` 加 `occq_discard`
- 坑③：device reset 后 pending ID 悬挂——occq_get 的 VERIFY2 同 occq_discard 一样软化为越界即返回假可见
- `r__pixel_calculator`（LPS 加载期）保持同帧不动

**改动面**：light_vis.cpp + light.cpp（析构）+ r__occlusion.h/.cpp（occq_discard + in_flight）+ r4_R_render.cpp（refresh 守卫）。草稿：`_doc\_code_temp\02_occq_next_frame_readback.md`。

### 3.2 VSLR 面轮转（优先级 2，小改动）

**事实**：`R4FLAG_OFFSCREEN_REFLECTIONS` 门控；三档质量 128/256/512px（默认 256，ps_r4_vslr_quality）；RGBA16F+mips+独立 env depth；每帧 6 面全渲染，`_dynamic=FALSE` 只画静态，`HOM.Disable()`，无遮挡剔除；成本本质 = **CPU 遍历+draw 提交 ×6**（128-256px 像素填充可忽略）。消费端 r2_RT_env 被 SSR/SSLR 回退、VSLR 表面、combine 环境光采样——粗糙面/远场采深 mip，1-5 帧陈旧不可感知。`rt_Reflection_temp` 已有乒乓历史结构（r4_R_render.cpp#L461）。

**设计**：面轮转——每帧渲染 1-2 个面 round-robin，其余面保留上帧内容。RT 常驻，改动 = 循环边界 + 轮转计数器，零 shader 改动。收益：砍 4-5 次完整遍历 + 数千 draw 提交。草稿：`_doc\_code_temp\03_vslr_face_rotation.md`（含新控制台变量 `r4_vslr_faces_per_frame`，1..6，默认 1，6=旧行为；仅被渲染面清屏/未动面内容保留已被逐行核实）。

### 3.3 zfill/G-buffer 双桶单遍历（优先级 3，中型重构）

**根因**：element 选择在构建期（r4.cpp#L41-67 `rimp_select_sh_*`）：
- zfill：`PHASE_SMAP` → `E[SE_R2_SHADOW]` 桶，静态 only，短投影 0.25×far（ps_r2_zfill 默认 0.25），ColorWrite off
- G-buffer：`PHASE_NORMAL` → `E[SE_R2_NORMAL_HQ/LQ]`（r_dtex_range=50m 档位）桶

同一 visual 两次遍历落**不同 bucket**，这是两次独立遍历存在的原因。

**设计**：单次遍历双重插入——一次 portal 走查 + q_frustum + HOM + SSA，每个静态 visual 同时插进 SHADOW 桶和 NORMAL 桶，zfill 与 G-buffer 各自渲染自己的桶。遍历 CPU 减半，draw 仍两遍。
- 改动点：rimp_select_sh_* / add_leafs 双插入逻辑 + render_main 复用；mapDistort/scope/pmask/marker 语义逐一过
- 风险：核心构建路径，需回归验证阴影/视锥边界；zfill 现在用短投影剔除（比全投影剔除少画），合并后按全投影剔除，深度 only 多画少量几何（可接受）
- 排序：排在两个快赢之后

---

## 4. 实施顺序

> 最新版见 §8（轮 3 修订），此处保留历史排序供对照。

| 序 | 项 | 改动面 | 收益 |
|---|---|---|---|
| 1 | occq 下一帧回读 | ~50 行，有先例（smapvis） | 消除逐灯 stall，帧时间方差显著下降 |
| 2 | VSLR 面轮转 | 小，单文件 | 开 VSLR 时 CPU 遍历/提交砍 ~80% |
| 3 | 纹理异步加载队列（§5.2 骨架） | 中，有 R5 先例 | 消除首用 hitch，加载期卡顿缓解 |
| 4 | zfill/G-buffer 双桶单遍历（§3.3） | 中，核心构建路径 | 遍历 CPU 减半（zfill 仅 0.25 far，绝对收益有限，降级） |
| 5 | 静态 instancing 合批（§5.1） | 大 | draw 数量级下降，长期项，先做运行时统计 |

---

## 5. 第二迭代发现（2026-10-22 轮 2）

### 5.1 静态合批可行性（GCONTAINER 池结构）

**现状**：
- 关卡静态几何 = 少量全局 VB/IB 池（`fsL_VB`/`fsL_IB`，r4_loader.cpp#L229-304），每个 visual 通过 GCONTAINER 引用池内区间（vBase/vCount/iBase/iCount，FVisual.cpp#L46-69），每 visual 一次 `RCache.Render` 区间 draw（FVisual.cpp#L200-218）
- draw 桶 key = (VS,GS,PS,CS,State,`STextureList*`)——`STextureList*` 是 shader 编译期 `CreateTextureList` 生成的共享指针（Blender_Recorder.cpp#L148-190），**同 shader+同纹理组的 visual 天然同桶**
- per-draw 常量差异：普通静态物 transform=identity、无常量提交，可直接 instancing；树（MU 实例）的实例变换走 `m_xform/m_xform_v/wave/wind` 常量（Blender_Recorder_StandartBinding.cpp#L36-44），instancing 需先把 OGF_TREEDEF2 变换搬进实例缓冲（可行，gvox 已做过同款换算）
- 顶点声明：GCONTAINER visual 共享池的 decl（种类有限）；非池 visual（VCONTAINER/VERTICES 直载，FVisual.cpp#L107-198）按 FVF 自建 decl

**结论**：**instancing 优先于 VB 合并**——按 (decl, bucket) 分组，把组内 visual 的 (vBase,iBase,iCount) 区间列表喂给一个支持多区间的 instancing draw（需要扩 draw 提交端：区间表 SRV 或多次 DrawIndexedInstanced with StartInstance），不动磁盘格式、不动加载。HQ/LQ 是两个独立 element → 两个桶，分组天然兼容。工作量大（draw 提交端重构 + 树实例缓冲 + 回归），维持"长期项"，但可行性确认。

**遗留待查**：k02 实际池数量/decl 种类/桶分布统计（需运行时打点，动工前先做）。

### 5.2 纹理异步加载队列（设计面确认，长期项第 1 步；轮 5 重大修正）

**现状（轮 5 逐行核实，两处重要修正）**：
- hitch 触发点：`CTexture::apply_load()` 在**绑定阶段**检查 `flags.bLoaded`，未加载就同步 `Load()`（dx11SH_Texture.cpp#L141-145）——发生在渲染线程首 draw
- **修正①：staging 管道已存在，默认被 flag 关闭**——`texture_load(..., bStaging)` 三分支齐全（dx11Texture.cpp#L125-141：usage/bindFlags/cpuAccessFlags），`CTexture::Load` 对普通纹理强制传 `bStaging=true`（dx11SH_Texture.cpp#L508）；但默认 flags **RFLAG_NO_RAM_TEXTURES 已置位**（xrRender_console.cpp#L146）→ `bAllowStaging=false` → 走 DEFAULT 直建。开 `r__no_ram_textures` 关闭即是"DDS 解码进 staging 常驻 RAM + 首用时 `surface_get`→`ProcessStaging` 同步 promote"的完整机制（dx11SH_Texture.cpp#L124-131, L147+）
- **修正②：关卡加载期已有 MT 批量预载**——`CResourceManager::DeferredUpload()`（ResourceManager.cpp#L349-372）在加载收尾阶段把 m_textures 全表 Load（RFLAG_MT_TEX_LOAD 默认开时 `xr_parallel_foreach` 并行；调用点 IGame_Persistent.cpp#L140）。**hitch 只发生在关卡加载完成后新注册的纹理**
- `surface_set()` 替换机制存在（3DFluidManager 在用）；R5 有完整异步上传队列参考（`dx12::EnqueueTextureUpload`/`ProcessPendingTextureUploads`，帧首渲染线程排空、每帧字节上限、尺寸校验护栏）

**设计骨架（轮 5 修订：直接骑在现成 staging 管道上）**：
1. 保持 `r__no_ram_textures` 开启（staging 模式生效），`apply_load` 命中未 promote 的 staging 纹理 → 先绑 1x1 占位 SRV + 入 promote 队列，渲染继续
2. **不再需要独立加载线程做解码**——DeferredUpload 已在加载期把 staging 建好；剩下的只是把"首用同步 promote（ProcessStaging 的 CopyResource+SRV 重建）"搬进帧首批量排空（渲染线程，Begin 后），每帧字节上限（R5 用 96MB/帧）
3. `surface_set` 替换 + SRV 重建必须渲染线程做（现成 ProcessStaging 代码就是渲染线程，搬调用时机即可）
4. 护栏：初始化期/超大单资源走同步路径（R5 同款）；R5 教训——尺寸不匹配纹理跳过、SRV 引用持有、持久槽互斥

**预估**：工作量从"中"下调（解码/加载线程整个省掉，核心只是 apply_load 分流 + 帧首排空循环）；风险集中在 SRV 替换时序。

### 5.3 detail/LOD 系统（已健康，非瓶颈）

- **草地**：已是 instancing 批处理——`hw_BatchSize=61`（DX11，DetailManager_VS.cpp#L40-63），`hw_Render_dump` 按对象类型填充 InstanceData、满 61 实例 flush 一次 draw（L230-305）；slot 解压限 7 个/帧（DetailManager_CACHE.cpp#L207-246），可见性按 macro-cell→slot→实例三级过滤（DetailManager.cpp#L189-343）
- **树 billboard LOD**：`r_dsgraph_render_lods` 按帧动态打包 VB（4 顶点/imposter，Lock+填充），**按相同 shader element 分组成批 draw**（r__dsgraph_render_lods.cpp#L38-54, L117-120）；每树每帧 CPU 做 8 面 dotproduct 排序选朝向（L70-84）——数量大时是可观测 CPU 点，但远小于静态扇出问题
- 剔除粒度健康：slot/树单棵 SSA 都有

### 5.4 其他确认

- HOM 多线程（`r2_mt`）默认开启，HOM.Render_DB 光栅 + Raster.test 已在 Pre-Render 线程
- `ps_r2_zfill` 默认 0.25——zfill 只覆盖最近 25% 视距的静态深度（比预想更便宜），双桶合并收益仍在但绝对值有限；**优先级可再降**
- `ps_r4_vslr_quality` 默认 1（256px 面）

---

## 6. 第三迭代发现（2026-10-22 轮 3：太阳阴影 / GTAO / SSLR）

### 6.1 太阳阴影（CSM）

**现状**：
- 3 级联 15/40/160m（R_sun.cpp#L44-60），共用 2048² smap（ps_r2_smapsize 默认），每级独立清屏全量重画
- **texel snap 已实现**（R_sun.cpp#L256-271 相机像素对齐），防闪烁过关
- 每级联完整 subspace 遍历、动态物也画（`_dynamic=TRUE`，L293）；深度元素全级联共用 SE_SUN_FAR（L304，无碍），应用侧才分 NEAR/MIDDLE/FAR（L333-337）
- 采样 = 硬件 PCF SampleCmpLevelZero 4 向平均（shadow.hlsli#L42-62），无 contact hardening
- **草地进每个级联**（L310-311，R2FLAG_SUN_DETAILS）——160m 级联里草 alpha 纯浪费
- Tshadows 开启时每级联多一遍 graph(1)+sorted（L314-319）
- 应用 = 3 个全屏累积 pass + 可选 shafts

**问题**：①级联×3 全量遍历+draw（13+N 扇出里最肥固定项）；②草×3（开放世界隐藏大户）；③固定分割比不自适应。

**改进**：
1. 草只进最近 1-2 级联（per-cascade gating，几行改动；草稿见 _code_temp\04，含新变量 `r4_sun_details_max_cascade` 0..2 默认 1）
2. 级联静态缓存：xform 相机对齐后静态内容只在窗口滑动跨 texel 时重画（VXGI clipmap 思路），每帧只补动态物薄 pass——阴影遍历 ×3 → 动态 ×3 + 偶发静态（中改动）
3. 级联分割自适应（SDSM/按 far_plane 缩放）——质量收益
4. 静态 instancing 一次投入三处阴影 pass 受益

### 6.2 GTAO

**现状**：
- 默认 3 slice × 4 step，半径 0.85m，**默认全分辨率**（r4_gtao_resolution=1，vid_restart 生效）
- 2 pass：gtao_render（half_depth+G-buffer 法线+蓝噪声）→ gtao_filter（GatherRed ×16 空间滤波）；半分辨率时 combine 双线性免费上采样
- **无时域累积**；**蓝噪声静态第 0 帧**（gtao_render.ps#L184-186，注释自己写了动画化方法没做）——噪声是固定图案，TAA 积分不掉

**改进（按性价比）**：
1. 噪声动画化一行（gtao_render.ps.hlsl#L186 `0` → `uint(m_taa_jitter.w) % 32`）——配合 TAA 立即变可积分颗粒。轮 5 核实：`m_taa_jitter.w = float(Device.dwFrame)` 由常量 binder 无条件绑定（Blender_Recorder_StandartBinding.cpp#L285-288，TAA 关闭也有效），SSLR 已有同款先例（sslr_render.ps.hlsl#L65）。草稿：`_doc\_code_temp\01_gtao_noise_animate.md`
2. 加时域累积 pass（抄 SSLR temporal / HVGI temporal 现成骨架）+ 空间滤波 16→6-8 taps——成本砍半以上质量更好
3. 默认 half-res（时域上来后考虑）
4. VXGI 开启时远距离 AO 让位（gi_ao 接口已有），GTAO 只管近场

### 6.3 SSLR

**现状**：
- 3 全屏 pass：sslr_render（trace，MRT point+data，默认半分辨率）→ sslr_filter（半分辨率，距离/粗糙度分级采样）→ sslr_temporal（**全分辨率深度感知上采样 + 时域 ping-pong**，history 交换在 pass 前做）
- **固定指数步进 march**：steps=30、trace_scale=1.2、thickness=0.25；**无 HiZ 加速结构**（R4 全目录无 hiz，project_memory 的 HiZ 条目对应别的实现/已不存在）
- shader 内已有分支优化：粗糙面跳 trace 回退 s_env、低 Fresnel 便宜 trace、距离 cutoff（sslr_render.ps#L87-144）
- **temporal 比预想先进（修正）**：有 AABB 式方差裁剪 HistoryClamp（sslr_temporal.ps#L21-32, L206）+ 双历史（diffuse 走 velocity 重投影 / specular 走世界点 m_VP_old 重投影，L177-210）+ quality 档 median9 / 半分辨率档深度感知双边上采样
- 绑定面重：trace 挂 G-buffer 全家 + sky×2 + env×2 + s_env + 蓝噪声（blender_gtao.cpp#L33-58）

**问题**：①trace 全屏跑（early-out 仍付像素调用成本）；②HiZ 以现状加未必划算（要连带改 mip 行进，属重写）；③真窗口质量已好，主要剩 GPU 量。

**改进**：
1. stencil 预分类：低成本 pass 标记"值得 trace"的像素（光滑/近景/非天空），trace+filter 只画该区域——最大单项 GPU 节省
2. 粗糙度自适应步数深化（现有 mid/far/low 派生再乘 roughness——SSREF 简化版）
3. HiZ mip 行进 + mip 权重随机化（完整 SSREF）——收益大工程量最大，不推荐现在动

### 6.4 全帧 pass 清单（GPU_EVENT 全盘点，轮 3 补充）

主循环：render_main → FORWARD_REFLECTIONS(×6 面) → DEFER_ZPREFILL → DEFER_PART0 → TEST_LIGHT_VIS → ZBUFFER_COPY → PhaseWinter(积雪) → DEFER_WALLMARKS(墙印) → DEFER_FLUSH_OCCLUSION → DEFER_RAIN → DEFER_SUN → DEFER_SELF_ILLUM → DEFER_LIGHT_NO_OCCQ/OCCQ/COMBINE → phase_combine 内部（render_clouds、combine_1、Forward_puddles、Forward_rendering、distort、AA、phase_scale/上采样、combine_2、render_flares、CAS、**饱和度/暗角/色差三个独立全屏**、phase_pp LUT）→ bloom → gamma

**新发现**：饱和度/暗角/色差是三个独立全屏 pass（r4_rendertarget_phase_combine.cpp#L429-442）——可合并进一个 compute；velocity 是 G-buffer MRT 输出（全分辨率 R16G16，每 deferred draw 都写，r4_rendertarget.cpp#L562）——时域系统数据无忧但构成固定带宽项。（轮 5 注：文件名原写 phase_combine.cpp 有误，实际为 r4_rendertarget_phase_combine.cpp；三 pass 逐 flag 门控已核实。）

### 错误修正记录（轮 3）

- ❌ "SSLR temporal 大概率无方差裁剪" → **有** HistoryClamp AABB 裁剪 + 双历史 + median9/双边上采样，SSLR 时域是三大系统里最成熟的；GTAO 应直接抄它。
- ❌ "HiZ base level 半分辨率对齐 SSLR"（project_memory 旧条目）→ 当前 R4 代码**无 HiZ**，SSLR 是固定指数步进。

---

## 7. 第四迭代发现（2026-10-22 轮 4：收尾核验 + 架构评估）

### 7.1 待办清账（原 §9 逐项结论）

- ✅ **常量缓存 flush_cache 不是热点（修正）**：`flush()` 无条件调用 `flush_cache()`（dx10r_constants_cache.h#L96-100 脏检查被注释），但 `dx10ConstantBuffer::Flush()` 内部有 `m_bChanged` 脏检查（dx10ConstantBuffer.cpp#L83-100）——实际成本 = 最多 32 槽 ×6 类 = 192 次指针判空 + **仅变更缓冲**的 Map(WRITE_DISCARD)+memcpy+Unmap。中等开销，无需动。
- ✅ **velocity 始终有效**：rt_Velocity 是 `phase_scene_begin` 的 MRT3 无条件绑定（r4_rendertarget_phase_scene.cpp#L30 `u_setrt(rt_Color, rt_Normal, rt_Surface, rt_Velocity, RDepth)`），每帧清屏——TAA 关闭时 SSLR temporal / 未来 GTAO temporal 照样有速度数据。**GTAO 时域累积无障碍**。
- ✅ **饱和度/暗角/色差确认**：三个独立全屏 pass，各自 flag 门控（R2FLAG_SPP_SATURATION/VIGNETTE/ABERRATION，r4_rendertarget_phase_combine.cpp#L429-442）——多项同开时可合并为单 pass；单项开启时无收益。
- ✅ **LOD 树 8 面排序结案**：每树 8 次 dotproduct + 8 元素小排序（r__dsgraph_render_lods.cpp#L70-84），千树量级 ≈ 数十微秒，可忽略。结案，不再列待办。
- ✅ **数字复核（万无一失）**：occq 池 = `occq_size = 2*768 = 1536`（r__occlusion.h#L3）；级联尺寸 = 15/40/160m（R_sun.cpp#L46-59 init_cacades）；smap 2048（ps_r2_smapsize）；ps_r2_zfill=0.25；sslr steps=30/scale=1.2/thickness=0.25；gtao dirs=3/steps=4/radius=0.85/resolution=1——全部与正文一致。

### 7.2 VSLR 补充（新发现的相关参数）

- `r4_vslr_hud_only`（ps_r4_vslr_hud_only，默认 0）：开启后立方体 far 截到 20m（HUD 模型反射专用）——**面轮转方案的前提（消费端容忍陈旧）在 hud_only 模式下更宽松**；`r4_vslr_distance`（默认 0.7）控制立方体距离比例。面轮转与这两个参数正交。

### 7.3 帧重叠架构评估（结论：可行但不急）

**现状**：主线程渲染 + GameThread 并行，帧尾 `secondary_tasks.wait()` 硬屏障——单帧 CPU 耗时 = max(渲染, 游戏) + 汇合点开销；渲染线程内 stall（occq/纹理/D3DCompile）全额吃帧。

**错帧改造的本质**：渲染帧 N 使用游戏帧 N-1 的快照（相机/物体位姿），渲染与游戏错开一帧持续流水。前置条件：
1. occq 同帧回读消除（序位 1）——否则 stall 仍在渲染线程内，错帧无收益
2. 纹理异步加载（序位 4）——同理
3. 输入延迟 +1 帧的体验评估（X-Ray 慢节奏射击可接受）

**改动面**：大（seqRender 快照机制 + secondary_tasks 调度重排 + 输入/相机快照化）——**维持"最后做"**，且应先量化：修完 stall 项后如果帧时间已达标，此项可无限期搁置。

### 7.4 桶分布运行时打点方案（静态 instancing 前置，具体化；轮 5 补精确行号）

在 `r_dsgraph_render_graph` 的 NORMAL 遍历里统计即可（约 20 行，console 开关 `r4_stats_buckets` 门控）：
- 桶链结构（r__dsgraph_types.h#L118-130）：mapNormalPasses[0][*] = FixedMAP 嵌套 `VS → PS → AdvStages(hs/ds+mapCS) → State → STextureList* → items`（_NormalItem{ssa,pVisual}）
- 遍历位置：r__dsgraph_render.cpp#L91-115（textures→items 叶子循环，L98-105 即 per-item `Ni.pVisual->Render`）
- 累计每叶桶 `items.size()` 与桶数，输出 Top 20 桶（按 items 数）+ 总桶数/总 item 数/中位桶大小——直接回答"同桶 visual 有多少、instancing 能省多少 draw"
- 附带统计 mapMatrixPasses（动态，r__dsgraph_render.cpp#L118 起）作对照
- 注意 `_clear=true` 会在渲染后原位清桶（L105-114 `items.clear()` 等），统计须在遍历前或用计数器旁路

### 错误修正记录（轮 4）

- ❌ "flush_cache 每 draw 无条件调用，是 CPU 热点" → **实际有 m_bChanged 脏检查**，无条件的是外层 flush() 调用本身（廉价）；热点结论撤销。
- ❌ "velocity 在 TAA 关闭时有效性待确认" → **MRT3 无条件绑定+每帧清屏**，确认始终有效。

---

## 7.5 第五轮实施后复核 + 新瓶颈深挖（2026-10-23 轮 6）

### 7.5.1 序位 0-3 实施代码复核修正（已修，重新构建部署）

1. **in_flight() 语义 bug（真 bug，已修）**：初版实现返回 `used.size()`——但 `used[]` 条目收割后**不删除**（occq_get/discard 只置 `used[ID].Q=0` 留槽位），`used.size()` 是高水位（~1536）而非在途数 → 空包守卫 `in_flight()==0` 热身后永远为假 → **occq_refresh 变死代码**。已改为真实计数器 `outstanding`（occq_begin++ / occq_get·discard 有效路径-- / refresh·create·destroy 归零，带 VERIFY 对账）。
2. **GTAO 噪声动画在无时域积分器时是纯闪烁（已修）**：GTAO 无自带 temporal pass（序位 5 才加），唯一积分器是 TAA/FSR；而 `m_taa_jitter.z >= 0` 恰好只在 aa_type==3（phase_taa）或 ps_r_scale_mode>1（FSR2/DLSS）时成立（r4_R_render.cpp#L512/521，否则 z=-1）。已门控：`uint noise_slice = (m_taa_jitter.z >= 0.0f) ? (uint(m_taa_jitter.w) % 32u) : 0u;`——TAA 关闭回退静态第 0 帧（旧行为），TAA 开启才动画。
3. **VSLR RT 重建后未渲染面为未定义内容（已修）**：面轮转下首帧只有 1 面被渲染，其余 5 面 GPU 未定义内容 + GenerateMips 全图集重算 → 前几帧可能采到垃圾。已加暖启：以 `rt_Reflection->pTexture->get_SRView()` 指针为 token，指针变化（关卡加载/vid_restart）时该帧强制 6 面全渲染并重置游标。
4. **粒子等待为渲染线程自旋（已修，见 7.5.2）**。

### 7.5.2 新发现瓶颈：粒子 CPU 更新与渲染帧的耦合（已修等待方式）

**结构**（device.cpp#L239-240 + EngineThreading.cpp#L10-37 + ParticlesAsyncManager.cpp）：
- 每帧开头 `secondary_tasks.run(PreRenderThread)`（PPL task_group，**非阻塞调度**）：Discord 同步 → seqParallelRender（HOM MT_RENDER + Details MT_CALC）→ 雨 UpdateItems → **粒子 CPU 更新**（CParticlesAsync::Start）
- 渲染帧与 PreRenderThread、GameThread **三者并行**；forward pass 绘制粒子前 `CParticlesAsync::Wait()`（r4_R_render.cpp#L846）
- **两个问题**：①Wait 是 `sleep_for(0)` yield 自旋——渲染线程与粒子 worker 抢核（SMT 干扰反而拖慢粒子更新）；②**Discord RPC（IPC）排在 PreRenderThread 最前**，粒子更新启动被 Discord 同步推迟 → Wait 窗口拉大

**已修**：Wait 改 `std::condition_variable` 阻塞（busy 标志在 worker 任何工作前置位，避免 setup 窗口竞态）；Discord 同步挪到 PreRenderThread 末尾（无渲染依赖）。
**遗留**：若粒子更新本身超出重叠窗口，渲染线程仍要等（阻塞性等待，不烧 CPU）。后续可选：粒子按需并行化（ps_active 遍历 xr_parallel_for）或 1 帧迟渲染粒子（需解决 Start() 与渲染读写的竞态，改动大）。

### 7.5.3 新发现瓶颈：逐阴影光 subspace 遍历 + 无阴影缓存（13+N 的 N 主体，未实施）

**结构**（lights_render.cpp#L86-176 实读）：
- 每个阴影 spot/point 光 = **一次完整 subspace 遍历**（L140 `r_dsgraph_render_subspace(..., TRUE, ...)`，动态+静态全套）+ smap 图集渲染；N 个阴影光 = N 次遍历（13+N 扇出里最肥的线性项，室内场景尤甚）
- smap 已是**图集系统**：`optimize_smap_size` 自适应尺寸 → `LP_smap_pool` 矩形打包（L47-70，按 smap_ID 分页，每页 `phase_smap_spot_clear` 全清后同页灯一起画）
- smapvis 增量剔除已存在（invisible 列表跳过已知不可见 caster），但只省 render graph 内部，**遍历照跑**

**改进方向（阴影缓存，长期项）**：图集结构是现成基础——静态光（位姿/颜色不变）的图集矩形内容可跨帧保留，每帧只补动态物薄 pass；动态光或场景变动时整体重画。需要：图集"按矩形失效"替代整页 clear（phase_smap_spot_clear 语义改造）+ 灯位姿/场景脏标记 + 光进出视野的矩形回收。工作量与级联静态缓存（§6.1 改进 2）同族，可共用框架。
**廉价替代**：静态光 smap 降帧率（每 K 帧重画，K=2~4）——动态物阴影会 K 帧滞后（ghosting），视觉风险高，不推荐默认。

### 7.5.4 其他确认（非瓶颈，结案）

- **光源累积 pass**：accum_point/accum_spot = 每灯 3 次 scissored 体积绘制（2 遍 stencil mask + 1 遍累积，r2_rendertarget_accum_point.cpp#L44-55）+ `enable_scissor`/`enable_dbt_bounds` 裁剪——非全屏，灯数中等时廉价；accum_volumetric 同为 scissored 单 pass。**非瓶颈**（除非百灯级场景，暂无此场景证据）。
- **HOM**：MT_RENDER 挂 seqParallelRender 在 worker 线程（hom.cpp#L149-155），渲染线程只读结果——维持"非瓶颈"结论。
- **ZBUFFER_COPY**：RDepth→rt_Position 单次全屏 CopyResource（r4_R_render.cpp#L714-722），~25MB/帧带宽，1080p 亚毫秒——**非瓶颈**。
- **render_lights 开头的 vis_update→剔除链（L13-36）**：阴影光可见性判定现在吃 vis 1 周期延迟（隐匿光多画一个复测周期）——按设计接受。

---

## 8. 实施顺序（轮 5 复核：顺序无变更；序位 0-3 已实施，2026-10-23）

> 轮 4 核验后顺序不变；轮 5 逐行复核全部行号引用与参数默认值（见 §11）。**序位 0/1/2/3 已于 2026-10-23 实施并构建部署**（MixedAVX，产物已复制到 bins\DXLegacy\MixedAVX），待游戏内验证；序位 4 设计面重大简化（§5.2）。

| 序 | 项 | 改动面 | 收益 | 状态 |
|---|---|---|---|---|
| 0 | GTAO 蓝噪声动画化（一行） | 极小 | 静态伪影→可积分噪声，配合 TAA | ✅ 已实施 |
| 1 | occq 下一帧回读 | ~60 行，有先例 | 消除逐灯 CPU 自旋（收益重述见 §3.1） | ✅ 已实施（R2 守卫同步） |
| 2 | VSLR 面轮转 | 小 | 开 VSLR 时遍历/提交砍 ~80% | ✅ 已实施 |
| 3 | 草只进近级联 | 小 | 太阳阴影砍草×3 | ✅ 已实施 |
| 4 | 纹理异步 promote 队列（§5.2 骨架，轮 5 简化） | 中→中小 | 消除首用 hitch | 设计修订未暂存代码 |
| 5 | GTAO 时域累积 + 默认半分辨率 | 中 | AO 成本砍半以上 | — |
| 6 | SSLR stencil 预分类 | 中 | trace GPU 砍 30-60%（视场景） | — |
| 7 | zfill/G-buffer 双桶、级联静态缓存、静态 instancing | 大 | 长期项 | — |
| 8 | 阴影光 smap 缓存（§7.5.3，与级联静态缓存同族） | 大 | 砍 13+N 的 N 主体 | 长期项 |
| 附 | 粒子等待改 CV + Discord 挪尾（§7.5.2） | 极小 | 消除渲染线程自旋抢核 | ✅ 已实施（轮 6） |

### 序位 0-3 实施记录（2026-10-23）

- **序位 0**：gtao_render.ps.hlsl#L186 噪声 Z 切片改 `uint(m_taa_jitter.w) % 32`（运行时 D3DCompile 直读，无需部署）。**轮 6 复核补门控**：仅 `m_taa_jitter.z >= 0`（TAA/FSR 激活）时动画，否则回退静态第 0 帧——GTAO 自身无 temporal pass，无积分器时动画即闪烁。
- **序位 1**：r__occlusion.h/.cpp（occq_discard + in_flight + occq_get 软失败替代 VERIFY2，软失败检查置于等待统计块之前保持 Begin/End 配对）+ light_vis.cpp（vis_prepare 开头收割遗留查询；vis_update 置空）+ light.cpp 析构（pending 时 discard）+ r4_R_render.cpp / r2_R_render.cpp 的 occq_refresh 守卫（`in_flight()==0`）。**实施补充**：r2.h/r4.h 原本缺 `occq_discard` 的 CRender 包装器（首轮构建报 C2039），已在两个头各加一行；R1 编译已确认排除 light_vis.cpp/lights_render.cpp（xrRenderPC_R1\CMakeLists.txt#L57-58），R1 构建无恙。
- **序位 2**：r4_R_render.cpp VSLR 循环改 round-robin（默认每帧 1 面，`r4_vslr_faces_per_frame` 1..6，6=旧行为，无需 vid_restart），游标 `s_vslr_face_cursor` 在循环后推进。**轮 6 复核补暖启**：RT token（SRV 指针）变化时强制 6 面全渲染一帧，消除重建后未定义面内容。
- **序位 3**：R_sun.cpp#L312 级联门控 `cascade_ind <= ps_r4_sun_details_max_cascade`（`r4_sun_details_max_cascade` 0..2，默认 1=15/40m 保留草、160m 跳过）。
- **构建**：xrEngine（MixedAVX）通过；部署 xrEngine.exe + xrRender_R1/R2/R4.dll(+pdb) 到 bins\DXLegacy\MixedAVX。**轮 6 修正后重建再部署**（in_flight 计数器、粒子 CV、EngineThreading 挪序、VSLR 暖启）。
- **待验证**：①rs_statistic 里 RenderDUMP_Wait 应趋零、o_queries/o_culled 总量不异常；②vid_restart/alt-tab 无查询崩溃；③VSLR 开启时反射无可见跳变（观察旋转推进感）；④160m 级联无草影属预期（`r4_sun_details_max_cascade 2` 可回旧行为）。

## 9. 待深挖问题（剩余，迭代中）

- [ ] **动工项**：桶分布打点（方案见 §7.4，~20 行）→ 依据结果决定静态 instancing 立项与否
- [ ] **动工项**：全屏链逐 pass GPU 量化（rs_statistic/PIX），识别饱和度/暗角/色差合并与其它可合并 pass 的实际收益
- [ ] 帧重叠架构：评估已完成（§7.3），是否立项取决于 stall 项修完后的帧时间
- [ ] 长期：静态 instancing（§5.1）、级联静态缓存（§6.1）、zfill 双桶（§3.3）——均待打点/量化数据支撑立项
- [ ] 序位 4（纹理 promote 队列）：动工前先确认默认 `r__no_ram_textures` 置位下显存行为与 RAM 占用差异（staging 常驻 RAM 的代价），决定是否默认开

---

## 10. 关键文件索引

| 文件 | 角色 |
|---|---|
| src/Layers/xrRenderPC_R4/r4_R_render.cpp | 帧主循环、render_main、zfill、TEST_LIGHT_VIS、VSLR |
| src/Layers/xrRender/r__dsgraph_render.cpp | render_graph/render_subspace、draw 提交 |
| src/Layers/xrRender/r__dsgraph_build.cpp | add_Static/add_leafs_Static（hierrarhy 静态遍历）、add_Dynamic、SSA、HOM 测试 |
| src/Layers/xrRender/r__sector.cpp | CSector::traverse portal 递归 |
| src/Layers/xrRender/r__occlusion.h / r__occlusion.cpp | occq 池（1536）、纯自旋回读、occq_refresh 语义 |
| src/Layers/xrRender/light_vis.cpp / light_smapvis.cpp / light.cpp | 光源可见性（同帧/延迟回读先例）、light 析构清理点 |
| src/Layers/xrRender/R_sun.cpp | 3 级联太阳阴影 |
| src/Layers/xrRender/lights_render.cpp | 光源包处理、阴影 subspace、vis_update 调用点 |
| src/Layers/xrRenderPC_R4/r4.cpp#L41-67 | element 选择（phase→E[]）；#L503 add_Geometry；#L281 reset_begin |
| src/Layers/xrRender/blenders/Blender_Recorder.cpp#L159-190 | PassEnd：_CreateTextureList 生成共享 STextureList*（桶 key 成分） |
| src/Layers/xrRender/Blender_Recorder_StandartBinding.cpp#L42-53 / #L285-288 | 树变换常量 binder；m_taa_jitter.w=float(dwFrame) |
| src/xrCore/FixedMap.h | FixedMAP 实现（clear()={pool=0}） |
| src/xrEngine/device.cpp#L287-308 | 帧线程同步屏障 |
| src/Layers/xrRenderDX10/dx11SH_Texture.cpp | 纹理同步加载（apply_load L141-145）、ProcessStaging、DeferredUpload 载入链 |
| src/Layers/xrRenderDX10/dx11Texture.cpp#L119+ | texture_load（staging 三分支 L125-141） |
| src/Layers/xrRender/ResourceManager.cpp#L349-372 | DeferredUpload：加载期 MT 批量纹理预载 |
| src/Layers/xrRender/xrRender_console.cpp#L129 / #L146 / #L176-219 | r2_mt 默认开；r__common_flags 默认（NO_RAM_TEXTURES+MT_TEX_LOAD）；GTAO/SSLR/VSLR 参数默认值 |
| src/Layers/xrRenderDX10/dx10ConstantBuffer.cpp / dx10r_constants_cache.* | 常量缓存（m_bChanged 脏检查） |
| src/Layers/xrRenderPC_R4/r4_rendertarget_phase_gtao.cpp | GTAO + SSLR 全部 phase |
| src/Layers/xrRenderPC_R4/r4_rendertarget_phase_combine.cpp#L429-442 | 饱和度/暗角/色差三独立 pass |
| src/Layers/xrRenderPC_R4/r4_rendertarget_phase_scene.cpp#L26-37 | phase_scene_begin：velocity MRT3 绑定 |
| src/Layers/xrRenderPC_R4/blender_gtao.cpp | gtao/sslr 五个 element 的绑定 |
| gamedata/shaders/d3d11/gtao_render.ps.hlsl#L184-186 | 蓝噪声采样（动画化修改位） |
| gamedata/shaders/d3d11/sslr_temporal.ps.hlsl | 方差裁剪/双历史参考实现 |

---

## 11. 轮 5 记录（2026-10-22：全量行号/事实复核 + 暂存草稿）

本轮为纯复核迭代：对前四轮的全部关键断言做了逐行代码核对（约 40 处引用），并确认 §8 序位 0-4 的修改位。**代码未改**，修改草稿暂存在 `_doc\_code_temp\`：

| 草稿 | 对应序位 | 内容 |
|---|---|---|
| 01_gtao_noise_animate.md | 0 | gtao_render.ps.hlsl 一行 + 依据（m_taa_jitter.w 绑定点、SSLR 先例） |
| 02_occq_next_frame_readback.md | 1 | 4 文件 ~60 行：occq_discard/in_flight + vis_prepare 收割 + vis_update 置空 + refresh 守卫；含验证计划与 reset 软化项 |
| 03_vslr_face_rotation.md | 2 | 循环轮转 + r4_vslr_faces_per_frame（1..6）；未渲染面不清屏的可行性已逐行核实 |
| 04_sun_cascade_grass_gate.md | 3 | R_sun.cpp 级联门控 + r4_sun_details_max_cascade（0..2 默认 1） |

### 轮 5 错误修正记录

- ❌ "TEST_LIGHT_VIS 同帧忙等回读" → 忙等在帧尾 render_lights 累积阶段；签发（L610-674）与回读（L785/791）之间隔着 ZBUFFER_COPY→…→SUN×3→SELF_ILLUM 大半帧 GPU 工作——§3.1 已整体改写，收益重述为"消除 CPU 纯自旋（每灯 ≤0.1ms）+ 时序确定性"，不再是"消除大幅 GPU 等待"。
- ❌ "可见灯 40-200 帧随机复测间隔" → 实为 **10-20 帧**（delay_large_min/max，light_vis.cpp#L6-7）；不可见灯次帧复测。
- ❌ "sector 内 r_visuals 线性列表全量测试（无 BVH）" → 本代码库无 r_visuals 结构；实际是 sector root 的 **hierrarhy 树递归**（r4.cpp#L503 → add_Static r__dsgraph_build.cpp#L551，每节点 testSAABB+HOM，fcvFull 子树走 add_leafs_Static 免视锥）——§1 已改写；低效点是每节点测试常数与扇出次数，不是"完全无层级"。
- ❌ "texture_load 无 staging 中转、DEFAULT 直建" → **staging 管道完整存在**（dx11Texture.cpp#L125-141 三分支），`CTexture::Load` 恒传 bStaging=true（dx11SH_Texture.cpp#L508），仅被默认置位的 RFLAG_NO_RAM_TEXTURES 关闭（xrRender_console.cpp#L146）；且 RFLAG_MT_TEX_LOAD（默认开）驱动 DeferredUpload 关卡加载期并行预载全部已注册纹理（ResourceManager.cpp#L349-372，调用点 IGame_Persistent.cpp#L140）——§5.2 设计骨架随之简化（省掉独立加载线程，只剩 apply_load 分流 + 帧首批量 promote）。
- ❌ "phase_combine.cpp" 文件名 → 实为 **r4_rendertarget_phase_combine.cpp**（三 pass 门控行号 L429-442 不变）。
- ❌ Blender_Recorder.cpp#L148-190 → PassEnd 实为 L159-190（_CreateTextureList 在 L184）；StandartBinding 树 binder 实为 L42-53。

### 轮 5 新增确认（加固，无需改动）

- `m_taa_jitter.w = float(Device.dwFrame)` 由 binder 无条件设置（Blender_Recorder_StandartBinding.cpp#L285-288）——GTAO 噪声动画化在 TAA 关闭时同样有效。
- SSLR 蓝噪声动画先例：sslr_render.ps.hlsl#L65 `uint(m_taa_jitter.w) % 32`。
- smap 每级联 ClearDepthStencilView 全清（r4_rendertarget_phase_smap_D.cpp#L5-6）——级联静态缓存改造前每级联独立重画的断言成立。
- velocity 清屏点：r4_R_render.cpp#L520-521（rt_Generic_0/rt_Velocity 每帧 Clear）。
- 仅被渲染面清屏、其余立方体面内容保留：VSLR 循环逐行核实（ClearRenderTargetView 只作用于 pRT[i]）——面轮转无需 RT 结构改动。
- occq_refresh 现状对 smapvis 即存在"空包帧销毁在途查询"的潜在隐患（r4_R_render.cpp#L612-613）——轮 5 草稿的 in_flight 守卫同时修掉它。
- light::~light 已有 Lights_LastFrame 自清理先例（light.cpp#L59-64）——occq_discard 挂析构与现有模式一致。
- 桶 key 结构核实：VS→PS→AdvStages(hs/ds+CS表)→State→STextureList*→items（r__dsgraph_types.h#L118-130），同 shader+纹理组天然同桶（§5.1 结论成立）。
- 全部数值默认值复核通过：occq 池 1536、smap 2048、zfill 0.25、级联 15/40/160m、GTAO 3/4/0.85m/全分辨率、SSLR 30/1.2/0.25、VSLR 256px/0.7/hud_only=0、hw_BatchSize=61、r2_mt 默认开。

### 轮 5 未能复核（遗留）

- `xr_parallel_foreach` 的具体实现线程池（ResourceMgr DeferredUpload 用）——不影响结论，未展开。
- phase_combine 内部 render_clouds/combine_1/Forward_puddles/distort/AA 等子 pass 的逐个行号——轮 3 已盘点过 GPU_EVENT 顺序，本轮未重复逐行核对。
