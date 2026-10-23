# PBR 材质独立后缀与编辑器 PBR 近似渲染 — 具体实现报告

基于当前仓库代码（IX-Ray）梳理的落地实现方案。所有结论均给出代码依据（文件 + 行号）。

---

## 0. 结论摘要

1. **任务 1（PBR 独立后缀）**：THM 的 material 已支持 `PBR`（`STextureParams::tmPBR_Material`，枚举值 4，THM chunk 序列化无需新增字段）。需要改动的核心是 `CTextureDescrMngr::LoadTHM`（按后缀自动派生 `<名称>_Normal` / `<名称>_Layers`）、`uber_deffer`（第二张纹理不再拼接 `#`），以及 `gamedata/shaders/d3d11/sload.hlsli` 的解码（通道布局变化）。
2. **任务 2（编辑器 PBR）**：编辑器渲染管线已经会为 PBR 材质注入 `USE_PBR` 宏（编辑器 `CRender` 直接 include `uber_deffer.cpp`），但 `gamedata/shaders/editor/sload.hlsli` 的 PBR 解码分支整体被注释，导致 PBR 纹理按传统 bump 公式解码 —— 这就是编辑器里法线显示错误的直接原因。DX9 SM3.0 完全足以执行新通道布局的解码，因此"法线修正"可以先行、不依赖 DX11；DX11 编辑器后端迁移作为第二阶段，用于获得更完整的 PBR 近似光照。
3. **通道规格注意**：新 `_Layers` 的 R/G 顺序（Roughness、Metallic）与现有 DX11 PBR 打包（`bump#`：R=Metallic、G=Roughness）**相反**，且法线从 A/G 双通道改为 RGB 三通道 —— 属于破坏性变更，需要同步修改 shader 解码并处理旧 PBR 资产迁移（见 2.3）。

---

## 1. 现状代码链路梳理

### 1.1 THM → 引擎纹理描述链路

| 步骤 | 代码位置 | 说明 |
| --- | --- | --- |
| THM 材质枚举 | `src/Layers/xrRender/ETextureParams.h:48-56` | `ETMaterial` 已含 `tmPBR_Material = 4` |
| THM UI token | `src/Layers/xrRender/ETextureParams.cpp:53-60` | `tmtl_token` 已含 `"PBR"` 条目，编辑器 THM 属性页可直接选择 |
| THM 读取/保存 | `src/Layers/xrRender/ETextureParams.cpp:180-184, 237-241` | `THM_CHUNK_MATERIAL` 读写 `material + material_weight`，无版本问题 |
| THM 解析 | `src/Layers/xrRender/TextureDescrManager.cpp:31-171` | 对 `ttImage/ttTerrain/ttNormalMap` 建 `texture_spec`；L137 `m_use_pbr = (tp.material == tmPBR_Material)`；L139-168 bump_mode 为 Use/UseParallax 时 `m_bump_name = tp.bump_name`（THM 里显式指定的 bump 贴图名，当前通常是 `xxx_bump`） |
| 描述结构 | `src/Layers/xrRender/TextureDescrManager.h:20-26` | `texture_spec { m_bump_name, m_material, m_use_steep_parallax, m_use_pbr }` |
| 纹理对象缓存 | `src/Layers/xrRenderDX9/dx9SH_Texture.cpp:143-147`、`src/Layers/xrRenderDX10/dx11SH_Texture.cpp:345` | `CTexture::Preload()`：`m_bumpmap = GetBumpName(cName)` |
| 引用层 | `src/Layers/xrRender/SH_Texture.h:117-118` | `ref_texture::bump_get()/bump_exist()` 直接读 `m_bumpmap` |
| Shader 装配 | `src/Layers/xrRender/uber_deffer.cpp:77-89` | `UsePBRTexures()` → `addShaderOption("USE_PBR","1")`；`fnameA = bump_get()`，`fnameB = fnameA + "#"`；随后 DX11 绑 `s_bump/s_bumpX`（L184-187），DX9 绑同名采样器（L219-224），编辑器额外绑 `s_material/env/sky`（L241-247） |

要点：**`s_bump` / `s_bumpX` 两个采样器名的语义就是"法线类贴图 + 附属数据贴图"**，后缀机制只影响文件名解析，不采样器名，这是本次改造可以小步进行的关键。

### 1.2 bump / bump# 通道编码现状（三种布局并存）

| 纹理 | 传统（非 PBR） | 现行 PBR（d3d11 shader） | 目标 PBR（本报告） |
| --- | --- | --- | --- |
| `s_bump`（传统 `_bump`） | R=gloss、G=Nz、B=Ny、A=Nx；视差高度在 `s_bumpX.A` | **A=Nx、G=Ny**（`Bump.wy*2-1`），R=视差高度（`UpdateTC` idx=0） | `_Normal`：**RGB=N.xyz**，A=视差高度 |
| `s_bumpX`（传统 `_bump#`） | RGB=法线高精度补偿（128 居中），A=视差高度 | **R=Metallic、G=Roughness**、B=SSS、A=AO | `_Layers`：**R=Roughness、G=Metallic**、B=SubsurfaceAmount、A=AO |

代码依据：

- 传统编译端打包：`src/utils/xrDXT/nvtt/NormalMapGen.cpp:145-172`（`it_gloss_rev`/`it_height_rev`）与 `DXTCompressBump`（L224-362，产出 `<name>.dds` 与 `<name>#.dds`，L341-347）。
- 现行 PBR 解码：`gamedata/shaders/d3d11/sload.hlsli:151-159`（`M.Normal.xy = Bump.wy*2-1`；`Metalness=BumpX.x; Roughness=BumpX.y; SSS=BumpX.z; AO=BumpX.w`）；传统解码 L160-172；视差高度通道选择 L137-141（PBR 用 `s_bump` 通道 0，传统用 `s_bumpX` 通道 3）。
- 遗留问题：d3d11 细节 bump 的 PBR 解码（L205）写作 `DetailBump.wyy - 128.0f/255.0f`，`wyy` 疑似笔误，本次应一并修正。

### 1.3 纹理加载与兜底

- DX11：`src/Layers/xrRenderDX10/dx11Texture.cpp:149-152` —— 文件名含 `_bump` 且 DDS 不存在 → `_BUMP_from_base`（L341-364）回退 `ed\ed_dummy_bump(#).dds`。
- DX9/编辑器：`src/Layers/xrRenderDX9/dx9Texture.cpp:270-271, 501-526` 同上。
- 编辑器 DDS 加载：`dx9Texture.cpp:297-310` —— `DDSTextureLoader9` 失败（如 BC7 返回 `0x80070032`）时用 `RedImage` 解码转 `A8R8G8B8`，DXT5 类 PBR 纹理可直接走硬件路径。

### 1.4 编辑器渲染链路（DX9 + 自定义管线）

- 设备层：`src/xrEngine/Device_create_render.cpp:168-182` `InitRenderDeviceEditor()` 仅 `CreateD3D9()`；DX11 分支被 `#ifndef _EDITOR` 排除（L326-333）。
- 渲染层：编辑器有自己的 `CRender/CRenderTarget`（`src/Editors/xrECore/Editor/render.cpp:29, 44-69`），并且**直接 include 了 `uber_deffer.cpp`**（L14-15）——因此 `USE_PBR` 宏、`s_bump/s_bumpX` 绑定在编辑器编译里已经生效。
- Shader 源：`includer` 从 `$game_shaders$ + getShaderPath()`（即 `gamedata/shaders/editor/`）读取（`render.cpp:696-714`），`D3DCompile` 编译 DX9 target（L920），缓存于 `shaders_cache\<VER>\editor\`（L878-890）。
- 问题所在：`gamedata/shaders/editor/sload.hlsli:34-47` —— `SloadNew` 只有传统解码 `M.Normal = Bump.wzy + BumpX.xyz - 1.0f`，PBR 分支被注释（L38-47）。PBR 材质（A=Nx/G=Ny + bump#为 Layers 数据）套用传统公式 → 法线错误。**这正是用户观察到的现象。**
- 编辑器 G-buffer：`gamedata/shaders/editor/common.hlsli:206-215` `f_editor_gbuffer`（Color / Albedo / Normal / PointZ 四张 MRT）；`editor/deffer_base.ps.hlsl:44-49` 写入 `Gloss = 1 - Roughness`、`MaterialID = xmaterial`。
- 编辑器光照：`editor/hmodel.hlsli:11-46` 通过 `tex3D(s_material, float3(hscale, hspec, m))` LUT 取光照系数，`m` 即 MaterialID。

---

## 2. 任务 1：PBR 材质独立后缀 `_Normal` / `_Layers`

### 2.1 通道规格（定稿）

- `<名称>_Normal`（建议 tfRGBA / DXT5，MipMaps 开、Kaiser 滤波）：
  - RGB：切线空间法线，`[0,1] → [-1,1]`
  - A：视差高度
- `<名称>_Layers`（建议 tfRGBA / DXT5，保留 alpha）：
  - R = Roughness，G = Metallic，B = SubsurfaceAmount，A = AO
- 法线 Y 轴朝向沿用现有 `USE_DX_NORMAL_MAP` 约定（`d3d11/sload.hlsli:240-242`），不在本次改动中变更。

### 2.2 逐文件修改

#### (1) `src/Layers/xrRender/TextureDescrManager.h`

```cpp
struct texture_spec
{
    shared_str          m_bump_name;
    shared_str          m_layers_name;      // NEW：PBR 第二张贴图
    float               m_material;
    bool                m_use_steep_parallax;
    bool                m_use_pbr;
};
// public 区新增：
shared_str GetLayersName(const shared_str& tex_name) const;
```

#### (2) `src/Layers/xrRender/TextureDescrManager.cpp` — `LoadTHM` 自动派生 + 回退

替换现有 L134-168 的 spec 填充逻辑：

```cpp
desc.m_spec = new texture_spec();
desc.m_spec->m_material = (float)tp.material + tp.material_weight;
desc.m_spec->m_use_steep_parallax = tp.bump_mode == STextureParams::tbmUseParallax;
desc.m_spec->m_use_pbr = (tp.material == STextureParams::tmPBR_Material);

if (desc.m_spec->m_use_pbr)
{
    // PBR：按后缀约定自动派生，忽略 THM 的 bump_name/bump_mode
    desc.m_spec->m_bump_name   = (xr_string(fn) + "_Normal").c_str();
    desc.m_spec->m_layers_name = (xr_string(fn) + "_Layers").c_str();
}
else if (tp.bump_mode == STextureParams::tbmUse ||
         tp.bump_mode == STextureParams::tbmUseParallax)
{
    // 非 PBR：完全保留现有逻辑（向下兼容，不再改动）
    desc.m_spec->m_bump_name = tp.bump_name;
}
```

存在性校验（沿用现有 `#ifndef MASTER_GOLD` 风格，并实现"旧资产回退"）：

```cpp
#ifndef MASTER_GOLD
if (desc.m_spec->m_use_pbr)
{
    string_path pNormal{}, pLayers{}, pLegacy{};
    FS.update_path(pNormal,  _game_textures_, desc.m_spec->m_bump_name.c_str());
    FS.update_path(pLayers,  _game_textures_, desc.m_spec->m_layers_name.c_str());
    xr_strcat(pNormal, ".dds"); xr_strcat(pLayers, ".dds");

    if (!FS.exist(pNormal) || !FS.exist(pLayers))
    {
        FS.update_path(pLegacy, _game_textures_, tp.bump_name.c_str());
        xr_strcat(pLegacy, ".dds");

        if (tp.bump_name.size() && FS.exist(pLegacy))
        {
            // 旧版 PBR 资产（_bump/_bump# 旧打包）：回退传统路径，保证不崩、法线正确
            Msg("! PBR texture [%s] missing _Normal/_Layers, falling back to legacy bump [%s]", fn, tp.bump_name.c_str());
            desc.m_spec->m_use_pbr     = false;
            desc.m_spec->m_bump_name   = tp.bump_name;
            desc.m_spec->m_layers_name = "";
        }
        else
        {
            Msg("! PBR texture [%s] references missing %s/%s", fn,
                desc.m_spec->m_bump_name.c_str(), desc.m_spec->m_layers_name.c_str());
        }
    }
}
#endif
```

新增访问器（与 `GetBumpName` L211-222 同构）：

```cpp
shared_str CTextureDescrMngr::GetLayersName(const shared_str& tex_name) const
{
    auto I = m_texture_details.find(tex_name);
    if (I != m_texture_details.end() && I->second.m_spec)
        return I->second.m_spec->m_layers_name;
    return "";
}
```

#### (3) `src/Layers/xrRender/SH_Texture.h` + 两个后端的 `CTexture::Preload`

```cpp
// SH_Texture.h（紧邻 L91 m_bumpmap）
shared_str m_layersmap;
// resptrcode_texture（L117 附近）
shared_str layers_get() { return _get()->m_layersmap; }
```

```cpp
// dx9SH_Texture.cpp:143-147 与 dx11SH_Texture.cpp:345 附近的 Preload()
m_bumpmap   = DEV->m_textures_description.GetBumpName(cName);
m_layersmap = DEV->m_textures_description.GetLayersName(cName);  // NEW
m_material  = DEV->m_textures_description.GetMaterial(cName);
```

由于 `bump_exist()` 读 `m_bumpmap`，PBR 材质无需任何额外判定即可让 `uber_deffer` 走 bump 分支。

#### (4) `src/Layers/xrRender/uber_deffer.cpp` — 第二张纹理解析规则

```cpp
const bool pbr = !!DEV->m_textures_description.UsePBRTexures(fname);
if (pbr)
    RImplementation.addShaderOption("USE_PBR", "1");   // 原 L77-79

if (bump)
{
    xr_strcpy(fnameA, pTexture.bump_get().c_str());
    if (pbr && pTexture.layers_get().size())
        xr_strcpy(fnameB, pTexture.layers_get().c_str());  // _Layers
    else
        xr_strconcat(fnameB, fnameA, "#");                 // 传统 _bump#
}
```

细节贴图同理（原 L40-59 `bHasDetailBump` 分支）：

```cpp
LPCSTR detail_bump_texture = DEV->m_textures_description.GetBumpName(dt).c_str();
// ...
if (pbr)  // PBR 细节：texDetailBumpX 取 _Layers，而不是 detail_bump + "#"
    xr_strcpy(texDetailBumpX, sizeof(texDetailBumpX), DEV->m_textures_description.GetLayersName(dt).c_str());
else
    xr_strcat(texDetailBumpX, sizeof(texDetailBumpX), detail_bump_texture), xr_strcat(texDetailBumpX, "#");
```

采样器/纹理绑定名 `s_bump`、`s_bumpX`、`s_detailBump`、`s_detailBumpX` 全部保持不变，shader 侧只改解码逻辑。

#### (5) `gamedata/shaders/d3d11/sload.hlsli` — 新解码（游戏 DX11）

视差高度通道（原 L137-141）：

```hlsl
#ifdef USE_PBR
    UpdateTC(I, I.tcdh.xy, s_bump, 3, parallaxShadow);   // 高度移到 _Normal 的 A 通道
#else
    UpdateTC(I, I.tcdh.xy, s_bumpX, 3, parallaxShadow);
#endif
```

主体解码（替换原 L151-159）：

```hlsl
#ifdef USE_PBR
    // _Normal: RGB = tangent normal, A = height
    M.Normal = Bump.rgb * 2.0 - 1.0;
    M.Normal = normalize(M.Normal);

    // _Layers: R=Roughness G=Metallic B=SSS A=AO
    M.Roughness = BumpX.r;
    M.Metalness = BumpX.g;
    M.SSS       = BumpX.b;
    M.AO        = BumpX.a;
#else
    /* 现有传统解码保持不变 */
#endif
```

细节 bump 的 PBR 分支（原 L200-208）同步：

```hlsl
float3 DetailNormal = DetailBump.rgb * 2.0 - 1.0;   // 顺带修正 wyy 笔误
```

保留 L240-244 的 `USE_DX_NORMAL_MAP` Y 翻转与 `Roughness = max(0.02f, ...)`。

#### (6) 纹理加载兜底

`src/Layers/xrRenderDX10/dx11Texture.cpp:149` 与 `src/Layers/xrRenderDX9/dx9Texture.cpp:270`：

```cpp
if (!FS.exist(fn, "$game_textures$", fname, ".dds") &&
    (strstr(fname, "_bump") || strstr(fname, "_Normal") || strstr(fname, "_Layers")))
{
    goto _BUMP_from_base;
}
```

`_BUMP_from_base`（dx11Texture.cpp:341-364 / dx9Texture.cpp:501-526）增加分支：

```cpp
if (strstr(fname, "_Normal"))
{
    R_ASSERT2(FS.exist(fn, "$game_textures$", "ed\\ed_dummy_normal", ".dds"), "ed_dummy_normal");
    /* ... 同现有 dummy 加载流程 ... */
}
if (strstr(fname, "_Layers"))
{
    R_ASSERT2(FS.exist(fn, "$game_textures$", "ed\\ed_dummy_layers", ".dds"), "ed_dummy_layers");
    /* ... */
}
```

需要新增两个占位资产：`ed\ed_dummy_normal.dds`（中灰法线 + A=0.5）、`ed\ed_dummy_layers.dds`（R=0.5, G=0, B=0, A=1）。

#### (7) THM 属性页（可选 UX）

`src/Layers/xrRender/ETextureParams.cpp` `FillProp` 的 `ttImage` 分支（L299-314）：当 `material == tmPBR_Material` 时隐藏 `Bump\Texture` 选择项，改为只读说明"将自动使用 `<名称>_Normal` / `<名称>_Layers`"。

#### (8) `gamedata/shaders/r2/sload.hlsli`（DX9 游戏 R2，可选但建议）

R2 目前 PBR 分支同样被注释（L108-121），`USE_PBR` 打开时会误用传统解码。将 (5) 的解码按 SM3.0 语法（`tex2D`）移植一份，使 DX9 游戏渲染器对新资产表现一致（光照为近似，但法线正确）。SM3.0 对该数学无任何限制。

### 2.3 旧 PBR 资产兼容策略

现有 PBR 材质以 `_bump`/`_bump#` 承载旧打包（A/G 法线 + R=Metal/G=Rough）。两种处理：

1. **推荐：存在性判定回退**（已包含在 2.2-(2)）——`_Normal` 缺失时自动回退传统 bump 并关闭 `m_use_pbr`，老资产"法线正确但无 PBR 通道"，输出迁移警告。若要求老资产保持 PBR 视觉，可增加 `USE_PBR_LEGACY` 编译分支（uber_deffer 按回退结果二选一注入宏，shader 内保留旧解码一份）。
2. **备选：一次性批量重导出**资产到 `_Normal/_Layers` 新布局，不保留 legacy 分支（代码最干净）。

### 2.4 美术资产 / 编译管线说明（无需改 DXTCompressBump）

`_Normal`/`_Layers` 是普通贴图文件，各自带 THM：

- `_Normal` 的 THM：Type = `Normal Map`（`ttNormalMap`），Format = tfRGBA 或 tfDXT5 —— 走 `DXTUtils::Compress` 的 ttNormalMap 路径（`src/utils/xrDXT/xrDXT.cpp:64-127`），NVTT 常规法线压缩即可，TGA 的 alpha 通道天然承载高度。
- `_Layers` 的 THM：Type = `Image`，Format = tfDXT5（保留 alpha）。
- 基础贴图 THM：material = `PBR`，bump_mode/bump_name 留空（引擎按后缀自动派生）。
- 专用于 `ttBumpMap` 的 `DXTCompressBump` 双文件打包（`NormalMapGen.cpp:224-362`）完全不参与新流程，不需要改动。

---

## 3. 任务 2：编辑器识别 PBR + 快速近似渲染

### 3.1 根因与总体策略

- 根因：编辑器编译链已经携带 `USE_PBR` 宏并绑定了 PBR 纹理（见 1.4），但 `editor/sload.hlsli` 没有 PBR 解码 → 错误地走传统公式。
- 策略分两阶段：
  - **阶段一（DX9 内修复解码）**：改动仅在 `gamedata/shaders/editor/` 两个 HLSL 文件，立即修正法线并给出近似 PBR 观感。新通道布局（RGB 法线 + A 高度；Layers 四通道）在 SM3.0 下可完整解码，**不依赖 DX11 改造**。
  - **阶段二（DX11 编辑器后端）**：按用户预期推进后端迁移，为完整 PBR 光照（GGX、金属度进光照）铺路。

### 3.2 阶段一：DX9 近似渲染（改动最小，可立即实施）

#### (1) `gamedata/shaders/editor/sload.hlsli` — 补齐 USE_PBR 解码

```hlsl
void SloadNew(inout p_bumped_new I, inout XrayMaterial M)
{
    M.Color = tex2D(s_base, I.tcdh.xy);

#ifdef USE_BUMP
    float4 Bump  = tex2D(s_bump,  I.tcdh.xy);
    float4 BumpX = tex2D(s_bumpX, I.tcdh.xy);

#ifdef USE_PBR
    // _Normal: RGB = tangent normal, A = height
    M.Normal = Bump.rgb * 2.0f - 1.0f;
    M.Normal = normalize(M.Normal);

    // _Layers: R=Roughness G=Metallic B=SSS A=AO
    M.Roughness = BumpX.r;
    M.Metalness = BumpX.g;
    M.SSS       = BumpX.b;
    M.AO        = BumpX.a;
#else
    M.Normal = Bump.wzy + BumpX.xyz - 1.0f;      // 现有传统解码，保持不变
    M.Normal.z *= 0.5f;
    M.Roughness = Bump.x * Bump.x;
    M.Metalness = 0.0f;
    M.SSS = 0.0;
    M.AO  = 1.0;
#endif
#else
    /* 无 bump 分支保持不变 */
#endif
    /* USE_TDETAIL 部分：PBR 分支按 d3d11 同款公式改用 DetailBump.rgb */
}
```

#### (2) `gamedata/shaders/editor/deffer_base.ps.hlsl` — 近似光照

编辑器 G-buffer 布局（`f_editor_gbuffer`：Color/Albedo/Normal/PointZ）与动态光 accum 全部不动，仅在 `main` 内做近似（`M.Normal` 变换到世界/视图空间的原有代码保留）：

```hlsl
    float Gloss = 1.0f - M.Roughness;

    // PBR 近似：AO 进 albedo；金属度压暗 albedo、抬升镜面
    float  specScale = lerp(1.0f, 4.0f, M.Metalness);
    float3 albedo    = M.Color.xyz * M.AO;
    albedo           = lerp(albedo, float3(0.04f, 0.04f, 0.04f), M.Metalness * 0.5f);

    O.Albedo = float4(albedo, Gloss);
    O.Normal = float4(M.Normal.xyz, Gloss);
    O.PointZ = float4(M.Point.xyz, MaterialID);

    /* ...原有直射光 + hmodel 组合... */
    O.Color.xyz += Light.w * Gloss * specScale + Specular * specScale;
```

预期效果：法线正确（核心诉求）、粗糙度经由现有 Gloss 链路生效、AO/金属度以廉价方式体现。

#### (3) 已知风险

- `material=PBR` 时 `m_material ≈ 4.x`（`texture_spec::m_material = material + weight`，TextureDescrManager.cpp:135）会作为 `xmaterial` 进入编辑器 G-buffer `PointZ.w`，并被 `hmodel.hlsli:29` 用作 `s_material` 3D LUT 第三维坐标 —— 需确认 `shaders\r2_material` LUT 的 Z 维度覆盖范围（传统材质 0..3+weight）。建议 PBR 材质在编辑器中固定 MaterialID（如 1.0），或扩 LUT。
- 视差：编辑器管线的 `UpdateTC` 未实现 PBR 高度通道（A），阶段一可暂不启用视差（`USE_STEEPPARALLAX` 在编辑器由 `C.bUseSteepParallax` 驱动），如需可把 d3d11 版 `UpdateTC`（`d3d11/sload.hlsli:27-130`）的 SM3.0 版本移植进 `editor/sload.hlsli`。

### 3.3 阶段二：DX11 编辑器后端改造（先行项细化）

改造入口与耦合点（均已在代码中核实）：

1. **设备入口**：`src/xrEngine/Device_create_render.cpp`
   - `InitRenderDeviceEditor()`（L168-182）硬编码 `CreateD3D9()` → 增加 APILevel 参数/配置项；
   - 移除 L326-333 对 `APILevel::DX11` 的 `#ifndef _EDITOR` 限制；`CreateD3D11()` 已有现成实现（`Device_create_render_dx11.cpp`），swapchain/DSV/SRV 回调（`GetRenderDevice/GetRenderTexture/GetDepthTexture`）在编辑器 ImGui 框架下已按 void* 抽象。
2. **编辑器 CRender**（`src/Editors/xrECore/Editor/render.cpp`）需迁移的点：
   - Shader 编译：`D3DCompile` 的 `ps_3_0/vs_3_0` → `ps_5_0/vs_5_0`；编译缓存路径 `shaders_cache\<VER>\editor\`（L878-890）建议增加 `dx11` 子目录避免与 DX9 缓存互踩；
   - HLSL 语法：`editor/*.hlsl` 使用 `tex2D/sampler2D` → 迁移为 `Texture2D.Sample`，或先加一层兼容宏（`#define tex2D(s,uv) s.Sample(s##_s,uv)` 方案仅够过渡，建议直接参照 `d3d11/common_*.hlsli` 基建改写，`d3d11/editor.vs.hlsl` 可作参照）；
   - 渲染状态：DX9 `SetRenderState/SetTexture` → 引入 `xrRenderDX10/StateManager`（工程内现成：`dx10StateManager/dx10StateCache/dx10SamplerStateCache`）；
   - 纹理层：`CTexture`/`SH_RT` 直接换用 dx11 版实现（`dx11SH_Texture.cpp`、`dx11SH_RT.cpp` 已存在，`Preload` 逻辑一致），纹理加载自动获得 DirectXTex 全格式支持（BC5/BC7 不再依赖 RedImage 兜底，`dx9Texture.cpp:297-310` 的特殊路径可退役）；
   - ImGui：`xrEUI/imgui_impl_dx9` → 复用 `xrRenderPC_R4/imgui_impl_dx11`；
   - RT/MRT：`CRenderTarget`（render.cpp:44-69）迁移为 D3D11 RTV 组合与视口管理。
3. **建议的迁移顺序**（避免一次性大爆炸）：设备与交换链 → 纹理/状态层 → shader 编译层（SM5 + 兼容宏过渡）→ RT/后期与 du_* 辅助几何。
4. **完成后收益**：编辑器可运行与游戏一致的 `USE_PBR` 路径；G-buffer 有余量存 Roughness/Metallic（`Albedo.a` 让出或新增 RT），accum/lighting 可直接借 `d3d11/metalic_roughness_*.hlsli` 做 GGX 近似，实现"所见即游戏"。

### 3.4 阶段三（可选）：工具层增强

- `src/Editors/LevelEditor/UI/UITextureViewer.cpp`：纹理查看器增加 `_Normal`/`_Layers` 通道分解预览（R/G/B/A 单通道显示、法线可视化），便于美术校对通道顺序。
- THM 批量迁移脚本：扫描 `material==PBR` 且带旧 `bump_name` 的 THM，生成缺失的 `_Normal/_Layers` THM 骨架。

---

## 4. 实施顺序与依赖

| 优先级 | 内容 | 依赖 | 覆盖任务 |
| --- | --- | --- | --- |
| P1 | TextureDescrManager + SH_Texture + uber_deffer + d3d11/sload 解码 + 加载兜底与 dummy 资产 | 无 | 任务 1 主体 |
| P2 | editor/sload.hlsli + deffer_base.ps.hlsl 近似渲染 | 无（可与 P1 并行） | 任务 2 阶段一（法线立即修正） |
| P3 | 旧 PBR 资产迁移或 `USE_PBR_LEGACY`；r2/sload.hlsli 移植（可选） | P1 | 兼容性 |
| P4 | DX11 编辑器后端（设备 → 状态/纹理 → 编译 → RT） | P2（shader 先行可减少调试变量） | 任务 2 阶段二 |
| P5 | 完整 PBR 光照预览、纹理查看器通道预览 | P4 | 任务 2 增强 |

## 5. 验证清单

1. 非 PBR 老 THM 在 DX9 游戏 / DX11 游戏 / 三个编辑器中回归：法线、视差、gloss、细节贴图均与改造前一致（`bump_name + "#"` 路径未动）。
2. 新 PBR THM（`_Normal/_Layers`）：DX11 游戏下法线、视差（高度走 `_Normal.A`）、AO/SSS/金属度生效；编辑器下法线正确、粗糙度生效、AO/金属度近似体现。
3. PBR THM 但 `_Normal`/`_Layers` 缺失：非 MASTER_GOLD 下回退或报警，不崩溃；加载 `s_bump` 时落入 `ed_dummy_normal/layers` 兜底。
4. 细节贴图组合：PBR 底 + PBR 细节（`_Layers` 解析）、PBR 底 + 非细节、非 PBR 底 + 传统细节 bump。
5. `material=PBR` 时 `xmaterial`/`s_material` LUT 采样无越界（编辑器 hmodel）。
6. 旧 PBR 资产（`_bump/_bump#` 旧打包）在迁移完成前表现符合 2.3 所选策略。
