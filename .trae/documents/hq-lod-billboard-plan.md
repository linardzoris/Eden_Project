# Make HQ LOD 改造为独立高分辨率 Billboard 计划

## Context（背景与目标）

当前 Multiple Usage 物体的 LOD 贴图无论 Make HQ 还是 Make LQ 都输出 512x64（64x64 单元格 x 8 向）图集，唯一区别是法线采样光线数（HQ=25 / LQ=1）。物体被强制按 1:1 捕捉和渲染，非 1:1 物体（树木、卡车）比例失调且分辨率极低，远处 LOD 模糊严重。

本次改造目标：

1. **Make LQ**：存储与引擎读取逻辑完全不变（512x64 色彩+法线 TGA → `gamedata\textures\lod\`，DDS 同目录，关卡构建时合并进 `level_lods.dds`），仅把采样质量提升为 HQ 级（25 射线）。
2. **Make HQ**：按物体 AABB 真实比例生成高分辨率 8 向图集（W>H 竖排 8 格、W<H 或 1:1 横排 8 格，图集 W、H 各 ≤2048），输出 TGA+DDS 到 `gamedata\textures\_shared\billboards\`，编辑器预览与关卡构建直接引用该贴图（不再合并进 level_lods）。

## 现状管线（已核实的关键代码）

- 生成入口：`src\Editors\ActorEditor\UI_ActorTools.cpp:1021` `RealGenerateLOD(hq)` → `ImageLib.CreateLODTexture(..., hq ? 4 : 1)`（[UI_ActorTools.cpp:1048](file:///d:/Eden_Project/src/Editors/ActorEditor/UI_ActorTools.cpp#L1048)）。Level Editor 库面板有同样入口：`src\Editors\LevelEditor\UI\UIEditLibrary.cpp:217` `GenerateLOD(bHighQuality)`。
- 像素生成：`src\Editors\xrECore\Editor\ImageManagerLOD.cpp`
  - `CreateLODSamples`（:83）：8 个水平角度正交投影截图。捕捉窗口：宽 `R=2*max(S.x,S.z)`（`Fbox::getradius` 返回半尺寸，见 `src\xrCore\_fbox.h:112`），高 `bb.max.y-bb.min.y`，即**窗口比例 = 物体真实 XZ 宽 : Y 高**。内部 8 倍超采样后 box 滤波写入 512x64 图集（每帧 64 宽条带）。
  - `CImageManager::CreateLODTexture`（:153）：逐像素射线求法线（`quality` 控制 5x5/1x1 采样网格）+ 半球 AO，写入法线图 alpha。输出写 TGA 到 `_textures_` 并 `SynchronizeTexture` 转 DDS。
- 命名：`CEditableObject::GetLODTextureName()`（[EditObjectEditor.cpp:278](file:///d:/Eden_Project/src/Editors/xrECore/Editor/EditObjectEditor.cpp#L278)）→ `"lod_库名"` 经 `UpdateFileName`（`EFS.AppendFolderToName`）变成 `"lod\库名"`，物理文件 `gamedata\textures\lod\库名.dds`。
- UV：`CalculateLODTC(frame, 8, 1, ...)`（[EditObjectEditor.cpp:205](file:///d:/Eden_Project/src/Editors/xrECore/Editor/EditObjectEditor.cpp#L205)）按 8 列 x 1 行归一化 UV；`GetLODFrame`（:215）生成 8 帧广告牌几何（四边形尺寸=物体包围盒足迹，比例真实）。
- 关卡构建：`src\Editors\LevelEditor\Editor\Builder\BuilderLODs.cpp:39` `BuildObjectLOD`：材质固定用 `"level_lods"` 贴图，加载每张 512x64 数据存入 `e_b_lod`（`Builder.h:25`）。
- 合并：`src\Editors\LevelEditor\Editor\Builder\BuilderRemote.cpp:1451-1501`（`CompileStatic`）：所有 `e_b_lod` 通过 `ImageLib.CreateMergedTexture` 合成 `level_lods.dds` + `level_lods_nm.dds` 写到关卡目录，并 `MergedTextureRemapUV` 重映射 UV。
- 编译器/运行时：xrLC `xrMU_Model_export_OGF.cpp:58` 从 `pBuild->lods` + 材质贴图名导出 `OGF_LODDEF2`；`src\utils\xrLC\Build_Load.cpp:347` 无 thm 时优雅降级（不加载表面），`:375` 对 `lod\` 前缀跳过表面加载 → **编译器无需改动**。运行时按材质贴图名直接找 DDS（标准子目录解析，`_shared\billboards\...` 天然可用）。
- 编辑器预览：`DefferedLoadRP`（EditObjectEditor.cpp:295）用 `GetLODTextureName()` 创建 LOD shader；`RenderLOD`（:242）选帧渲染。

## 设计决策

### HQ 图集布局规则（按你的描述实现）

- 捕捉比例 `r = viewW / viewH`，其中 `viewW = 2*max(bbox半尺寸.x, .z)`，`viewH = bbox.max.y - bbox.min.y`（与 `CreateLODSamples` 正交窗口完全一致，保证不变形）。
- `r > 1`（W>H）→ 8 格**竖排**：图集 = `cell_w x (8*cell_h)`，校验 `8*cell_h ≤ 2048`。
- `r ≤ 1`（W<H 或 1:1）→ 8 格**横排**：图集 = `(8*cell_w) x cell_h`，校验 `8*cell_w ≤ 2048`。
- 单元格尺寸量化为 2 的幂（64/128/256/512/1024/2048，满足 `IsValidSize` 的 POT 要求与 DXT/mipmap 约束），比例就近吸附到 2^n 档位（1:8…8:1）。
- 任一维超限或无法容纳 → 回退默认：1:1、cell 256x256 → 输出 2048x256 横排。

**待确认点（单元格尺寸策略）**：

- **方案 B（推荐，用满 2048 预算）**：短边固定 256，长边 = 256/r 就近取 POT：
  - 1:1 → 2048x256；1:2 → cell 256x512 → 图集 2048x512；2:1 → cell 512x256 → 图集 512x2048（竖）；1:8 → cell 256x2048 → 图集 2048x2048。
  - 分辨率最大化，直接解决模糊问题；单物体最大体积 DXT1 色 5.3MB + DXT5 法线 10.6MB。
- **方案 A（你示例中的字面值：单元格长边固定 256）**：
  - 1:1 → 2048x256；1:2 → cell 128x256 → 图集 1024x256；2:1 → cell 256x128 → 图集 256x1024（竖）。
  - 体积小，但极端比例（如 1:8）单元格长边封顶后保真不足。

> 若不另行说明，按方案 B 实现。

### DDS 格式

色彩：DXT3（含 alpha 边缘遮罩）；法线：DXT5（alpha 通道存 hemi/AO）。同时写出 `.thm`（携带这些参数），使编辑器批量同步（`SynchronizeTextures` 递归扫描 `_textures_`）不会用默认参数覆盖。

### 引用规则（存在即用）

`_shared\billboards\<lod_name>.dds` 存在 → 编辑器预览/关卡构建使用 HQ billboard（UV 按对应横/竖布局）；不存在 → 回退现有 LQ 路径（`lod\<lod_name>`，横排 8x1 UV）。这样无需在 .object 中新增标志位，生成即生效。

## 改动清单

### 1. 共享布局计算（xrECore）

- `src\Editors\xrECore\Editor\EditObject.h / EditObjectEditor.cpp`：新增
  - `void CEditableObject::GetLODAtlasParams(u32& cell_w, u32& cell_h, bool& vertical, u32& atlas_w, u32& atlas_h)`：按上述规则从 `m_BBox` 计算；
  - `bool CEditableObject::HasHQBillboard()`：`FS.exist(_game_textures_, "_shared\billboards\<GetLODTextureName()>.dds")`；
  - `void CEditableObject::GetLODGridForRender(int& w_cnt, int& h_cnt)`：有 HQ billboard 且竖排 → (1,8)，否则 (8,1)（LQ 恒为 (8,1)）。

### 2. 像素生成泛化（ImageManagerLOD.cpp）

- `CreateLODSamples`：参数从 `(bbox, tgt_data, tgt_w, tgt_h)` 扩展为携带 `cell_w/cell_h/vertical/atlas_w/atlas_h`；每帧正交窗口不变，截图降采样到 cell 后按帧号写入对应条带（横排：列偏移 `frame*cell_w`；竖排：行偏移 `frame*cell_h`）。
- `CImageManager::CreateLODTexture`（像素版）：三重循环重构为 `帧 × cell行 × cell列`，`dW=viewW/cell_w`、`dH=viewH/cell_h`，法线/AO 逻辑不变；`ApplyBorders` 传图集尺寸。
- 新增 `CImageManager::CreateBillboardTexture(CEditableObject*, tex_name, age, int quality)`：调用上述泛化生成，写 TGA 到 `_shared\billboards\`，创建带显式参数的 thm（色 DXT3 / 法线 DXT5、生成 mipmap、kMIPFilterAdvanced），`MakeGameTexture` 输出两个 DDS。保留原 `CreateLODTexture` 供 LQ 使用。
- 声明加到 `src\Editors\xrECore\Editor\ImageManager.h`。

### 3. Actor Editor 入口（UI_ActorTools.cpp / UI_ActorToolsProps.cpp）

- `RealGenerateLOD`：`hq==false` 分支仅把 quality `1` → `4`（25 射线），其余不动；`hq==true` 分支改调 `CreateBillboardTexture`，贴图名 `"_shared\billboards\" + GetLODTextureName()`，完成后 `O->OnDeviceDestroy()` 刷新预览。
- 顺手修复 `OnMakeLODClick`（[UI_ActorToolsProps.cpp:556-567](file:///d:/Eden_Project/src/Editors/ActorEditor/UI_ActorToolsProps.cpp#L556-L567)）`case 0` 缺 `break` 的穿透问题。

### 4. UV / 预览 / Shader（EditObjectEditor.cpp）

- `GetLODFrame` 增加 `w_cnt/h_cnt` 参数（默认 8,1），`CalculateLODTC` 已支持任意行列数；`RenderLOD` 调用处传 `GetLODGridForRender()` 结果。
- `DefferedLoadRP`：`HasHQBillboard()` 为真时 shader 贴图名用 `"_shared\billboards\" + GetLODTextureName()`。
- 半纹素 inset（`offs_x/offs_y`，EditObjectEditor.cpp:17-22）：HQ 时按图集实际尺寸计算。

### 5. 关卡构建器（LevelEditor）

- `src\Editors\LevelEditor\Editor\Builder\Builder.h`：`e_b_lod` 增加 `bool hq;`（默认 false）。
- `BuilderLODs.cpp` `BuildObjectLOD`：
  - 有 HQ billboard → 材质 `BuildTexture("_shared\billboards\<lod_name>")`；`GetLODFrame` 传 HQ 网格（竖排 (1,8)）；跳过 512x64 数据加载（不参与合并）；`hq=true`。
  - 无 → 现有 LQ 路径不变。
- `BuilderRemote.cpp` `CompileStatic` 合并块（:1451-1501）：`images` 列表与 UV 重映射循环跳过 `hq==true` 的 `e_b_lod`（注意重映射用独立计数器对齐非 HQ 项）；全部为 HQ 时跳过 `level_lods*.dds` 写出。

### 6. Level Editor 库面板同步（UIEditLibrary.cpp:217）

`GenerateLOD(bHighQuality)` 与 Actor Editor 同构：`bHighQuality==true` → billboard 路径；否则 LQ + quality 4。

### 无需改动

- xrLC（`Build_Load.cpp` 对无 thm 纹理优雅降级，billboard 纹理仅需名字导出 OGF）、`xrMU_Model_export_OGF.cpp`、引擎渲染侧（材质贴图名自然携带 `_shared\billboards\` 子路径，运行时按名解析）。

## 验证

1. 编译 Actor Editor 与 Level Editor（工程解决方案，确认 xrECore/ActorEditor/LevelEditor 三个目标零错误）。
2. 用非 1:1 MU 物体（如树木 1:4、卡车 3:1）：
   - Make HQ → 检查 `gamedata\textures\_shared\billboards\lod\<名>.tga/.dds/.thm` 与 `_nm` 系列：图集尺寸符合规则（如 1:4 → 2048x1024 横排、8 格等高）、法线 alpha 有 AO；
   - 编辑器视口远处预览：比例正确（不再压扁）、清晰度明显提升；
   - Make LQ → `gamedata\textures\lod\` 产物仍为 512x64，法线质量与原 HQ 一致（对同一物体分别用新 LQ 与旧逻辑对比 alpha/normal 平滑度）。
3. 关卡构建：场景含 1 个 HQ 物体 + 1 个仅 LQ 物体 → `levels\<关卡>\level_lods.dds` 只含 LQ 物体条带；HQ 物体对应 `b_lod` 材质贴图名为 `_shared\billboards\...`；xrLC 编译通过，导出 ogf 中 LOD 材质引用正确。
4. 回归：无 HQ billboard 的旧物体构建行为与改动前完全一致。
