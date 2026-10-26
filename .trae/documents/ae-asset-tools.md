# Actor Editor 资产工具：THM / DB / OMF / OGG 四功能实现计划

## Context

用户要给 Actor Editor 增加一组资产处理工具（经典 mod 工具链：解包 db → 编辑 thm/omf/ogg → 打回 db）：

1. **THM 编辑器**：打开任意 .thm，编辑全部 STextureParams 字段，保存回 .thm（用户已确认：独立窗体）
2. **DB 档案工具**：解压 .db/.xdb 到文件夹 + 把文件夹压缩回档案
3. **OMF 编辑器**：移植 src\xrGame\ImUtils\OMFEditor.cpp（AE 不链 xrGame），加载/改名/改参数/marks 编辑，**补全缺失的 Save/Save As**（用户已确认范围：不做 Merge/Add）
4. **OGG 声音参数编辑器**：编辑 vorbis comment[0] 里的 X-Ray 参数（v3 格式），写回 = 解码 PCM → 复用 ImportWav 带 comment 重编码（用户已确认）

约束：代码注释一律英文（项目铁律）；最小实现、逐步迭代；全部 UI 走 Loc() 中文化；构建 MixedAVX 部署到 Editors\Designer。

## 已核实的关键事实（实现时直接引用）

- **STextureParams** 在 `src\Layers\xrRender\ETextureParams.h`（已编入 xrECore，AE 直接 include 复用 Load/Save）；THM chunk 常量 L201-209（VERSION=0x0810/TYPE=0x0813/TEXTUREPARAM=0x0812…）；token 表（ttype/tfmt/tmtl/tbmode）在 `ETextureParams.cpp` L6-62；THM_TEXTURE_VERSION=0x0012。参考读写：`src\Editors\xrECore\Editor\EThumbnailTexture.cpp` L84-161
- **OMF 布局权威**：写出在 `src\Editors\xrECore\Editor\ExportSkeleton.cpp`（ExportMotionKeys L1002-1350 → chunk 14 OGF_S_MOTIONS；ExportMotionDefs L1351-1459 → chunk 15 OGF_S_SMPARAMS，version=4）。引擎读端 `src\xrEngine\SkeletonMotions.cpp` L82-184。**mark 名终止符是 CRLF（0x0D 0x0A，IWriter::w_string / IReader::r_string 语义），不是单个 0xA**——加载时剥尾部 CR/LF，保存时补 CR+LF
- **OMF 移植母本**：`src\xrGame\ImUtils\OMFEditor.cpp`——OMFData 结构 L69-155（AnimData/BoneData/AnimParamsData，原始段透传设计）、加载 L231-360、flags 位映射 L399-419、骨骼重命名 L558-588。其 AnimVector 的 `char*` 改为 `xr_vector<char>`
- **DB 双轨压缩**：desc 块（chunk 1|0x80000000）= LZHUF（`_decompressLZ`，`src\xrCore\lzhuf.h` L8，XRCORE_API；写入端 IWriter::w_chunk 遇 CompressMark 自动走 `_compressLZ`）；文件数据块 = lzo1x（读 `rtc_decompress`，`src\xrCore\rt_compressor.h`；写 `lzo1x_999_compress`+`lzo1x_optimize`，lzo 经 xrCore PUBLIC 传递可用）。档案格式权威：`src\xrCore\LocatorAPI.cpp` L134-312（读）+ `src\utils\xrCompress\xrCompress.cpp`（写：write_file_header L138-164、CompressOne L166-308、OpenPack/ClosePack L310-451）。**绝不能用 _compressLZ 压文件数据**（引擎读数据走 lzo 的 rtc_decompress）
- **OGG**：注释 = comment[0] 二进制 20 字节（u32 ver=0x0003 + MinDist/MaxDist/BaseVolume f32×3 + GameType u32 + MaxAIDist f32）；读参考 `src\xrSound\SoundRender_Source_loader.cpp` L66-94；写参考 `src\Editors\xrECore\Editor\SoundManager.cpp` L148-174（MakeGameSound 五字段 CMemoryWriter）→ `XRay::Importer::Audio::ImportWav(wav_path, out_ogg, quality, comment_data, comment_size)`（`src\Editors\xrECore\Importer\AudioWav.cpp`，quality<0.5 强制 0.5，只收 16-bit PCM wav；**成功路径无 return，成功判据=输出文件存在且非空**）
- **链接**：AE 链 xrCore/xrEUI/xrEProps/xrECore（`ActorEditor\CMakeLists.txt` L45-48）；xrECore 对 vorbis 是 PRIVATE 链接 → **AE 需自加 libogg/libvorbis/libvorbisfile 链接**（见下）；新 .cpp 会被 AE CMakeLists 现有 GLOB 自动收编
- **窗体模式**：`UILodEditorForm.h/.cpp`——IEditorWnd（`src\Editors\xrEUI\EditorWnd.h`）+ static Form/Update/Show；Update 挂在 `UIMainForm.cpp` L72 后。**注意原版 Show() 有 VERIFY(!Form)，重复点菜单会断言 → 新窗体 Show 改为"已存在则置 bOpen=true"**
- **文件对话框**：`EFS.GetOpenName/GetSaveName`（`src\xrCore\FileSystem.h`），initial 必须是已注册路径别名（`$game_textures$`/`$game_meshes$`/`$fs_root$`），末参 ext 可覆盖过滤器
- **本地化**：只有 `gamedata\configs\text\chn\editor_strings.xml` 一份（eng 目录无此文件），嵌套式 `<string id="..."><text>...</text></string>`，缺 key 自动回退英文

## 改动文件清单

**新文件（全部在 `src\Editors\ActorEditor\`）**：
- `UIThmEditorForm.h/.cpp` — THM 编辑器
- `UIDbArchiveForm.h/.cpp` — DB 解包/打包（Tab: Unpack / Pack）
- `UIOmfEditorForm.h/.cpp` — OMF 编辑器
- `UIOggParamsForm.h/.cpp` — OGG 参数编辑器

**修改文件**：
- `UIMainMenuForm.cpp`：L84 "Editors" EndMenu 之后插 Tools 菜单组（THM Editor / OMF Editor / OGG Sound Params / DB Archive Tool，各调 Show()），文件头补 4 个 include
- `UIMainForm.cpp`：L72 `UILodEditorForm::Update();` 后追加 4 个 Update()；UIMainForm.h 补前置声明
- `ActorEditor\CMakeLists.txt`：链接列表后追加 libogg/libvorbis/libvorbisfile（NuGet 路径：`${CMAKE_BINARY_DIR}/packages/ImeSense.Packages.LibOgg.1.3.5.4/`、`ImeSense.Packages.LibVorbis.1.3.7.4/`，仿 xrECore\CMakeLists.txt L211-217 写法，PRIVATE 链接 + include 目录）
- `gamedata\configs\text\chn\editor_strings.xml`：追加 Tools 组 + 4 菜单项 + 各窗体按钮标签键

## 各窗体实现要点

### 1. THM 编辑器（最简单，先做）
- 成员：`STextureParams m_Params; u32 m_Type; xr_string m_FilePath; bool m_Dirty;`
- Load：FS.r_open → r_chunk(VERSION, u16)≠0x0012 报错拒开 → find_chunk(TYPE)→m_Type，非 ETTexture 提示拒编 → `m_Params.Load(*F)`
- UI：Load/Save/Save As/Close + 脏标记；Type/Format/MipFilter/Material/BumpMode combo（用现有 token 表）；Width/Height/FadeAmount InputInt；FadeDelay Slider u8；8 个标准 flags checkbox + 4 个高级位；Border/Fade color 用十六进制 InputText；detail_name/bump_name/ext_normal_map InputText + scale/weight/virtual_height DragFloat
- Save：`CMemoryWriter`：open_chunk(VERSION)+w_u16(0x0012) → open_chunk(TYPE)+w_u32(m_Type) → `m_Params.Save(F)` → save_to(path)（完全复刻 EThumbnailTexture.cpp L128-161；丢弃 THM_CHUNK_DATA 预览像素与编辑器自身行为一致）

### 2. DB 档案工具
- **Unpack**：ifstream 顺序扫顶层 chunk：id==666 → ini 头（展示 entry_point）；`(id & ~CFS_CompressMark)==1 && (id & CFS_CompressMark)` → 读 size 字节 `_decompressLZ` 得 desc。desc 记录：`u16 buffer_size; u32 size_real; u32 size_compr; u32 crc; char name[]; u32 ptr`。逐条：create_directories → seek(ptr) 读 size_compr → size_real==size_compr 直写否则 `rtc_decompress` → crc32 校验（mismatch 记警告不中断）
- **Pack**：recursive_directory_iterator 收集（相对路径小写、排序）→ 写 chunk 666 头（"[header]"+entry_point，每行 CRLF）→ open_chunk(0) 逐文件 crc32 + lzo1x_999（进程内先 lzo_init()）→ `compr+16 >= real` 回退原始存储 → 记 ptr=pack->tell() 入 desc → close_chunk → `pack->w_chunk(1|CFS_CompressMark, desc)`（自动 LZHUF）→ 关闭。压缩开关关闭 = 全原始存储
- UI：两 Tab，路径 InputText+Browse（文件夹用 SHBrowseForFolder 小助手），Unpack/Pack 按钮 + UI->ProgressStart/Inc/End 进度 + 结果统计
- v1 不做：加密档案、alias 去重、分卷、VFS 优化

### 3. OMF 编辑器（工作量最大）
- 移植 OMFData（char*→xr_vector<char> RAII）+ 三段加载函数 + 读串助手 + flags 位映射 + 骨骼重命名；SDL MessageBox 换 ELog + 窗体内错误行
- **mark 名读取修正**：原版读到 0xA 吞进名字——加载后 strip 尾部 `\r\n`
- UI：左列 Bone parts（copy to clipboard + TreeNode 列表）+ Rename bones（combo+InputText+apply，重名 hash 检测）；右列 Animation combo + Rename 弹窗 + Speed/Power/Accrue/Falloff DragFloat + 7 个 flags checkbox + Motion marks 区（ogf_version==4 才启用：mark 列表 rename/delete/add + 间隔表 t0/t1）
- **Save 布局**（CMemoryWriter + open_chunk 自动回填大小）：
  - chunk 14：`open_chunk(0)+w_u32(animations_count)` → 逐动画 `open_chunk(i+1) + w_stringZ(name) + w(data 原始块) + close_chunk` → close_chunk
  - chunk 15：w_u16(4) → w_u16(parts.size) → 逐 part `w_stringZ(name)+w_u16(bones.size)+逐 bone w_stringZ(name)+w_u32(id)` → w_u16(params count) → 逐 motion `w_stringZ(name)+w_u32(flags)+w_u16(bone_or_part)+w_u16(motion_id)+w_float×4(speed/power/accrue/falloff)+w_u32(marks.size)+逐 mark{w(name)+w_u8(13)+w_u8(10)+w_u32(iv count)+逐间隔 w_float(t0)+w_float(t1)}` → close_chunk
  - **重命名动画双写**：anims[i].name 与 params[i].name 同步改（引擎 DEBUG 校验 accel_map）；加载时校验 params[i].motion_id==i
- **黄金用例**：加载任意 .omf → 零修改 Save As → 与原文件 byte-exact（先验证这个再开编辑功能）
- Save As 用 `EFS.GetSaveName("$game_meshes$", fn, 0, -1, "*.omf")`

### 4. OGG 参数编辑器
- Load：读文件进内存 → 4 个内存 ov_callbacks → ov_open_callbacks/ov_info（检查声道≤2，否则提示拒绝）/ov_comment/ov_pcm_total → 解析 comment[0]（v3 正常；v1/v2/缺失 → 默认值 MinDist=1/MaxDist=100/BaseVolume=1/GameType=0/MaxAIDist=MaxDist 并提示）
- UI：Load/Save/Save As/Close + info 行（Hz/ch/时长）+ MinDist/MaxDist/BaseVolume/MaxAIDist DragFloat（≥0.1）+ GameType InputInt + Quality SliderFloat 0.5-1.0（默认 0.5）
- Save：ov_read 循环解出全部 PCM s16 → 写临时 WAV（16-bit PCM）→ `CMemoryWriter C` 五字段（对齐 MakeGameSound L151-157）→ `ImportWav(tmp_wav, out_ogg, Quality, C.pointer(), C.size())` → 以输出存在且非空判成功 → 删临时 wav

## 实施顺序

1. 公共基建：4 窗体空壳（Load/Close 可用）+ 菜单组 + Update 挂钩 + CMake 链接 + 本地化 → 构建验证窗体模式跑通、连点菜单不崩
2. THM 完整实现 → 构建验证
3. OGG 完整实现 → 构建验证
4. DB unpack → 验证 → pack → 验证
5. OMF：移植加载 → UI → Save → 黄金用例 byte-exact → 编辑功能回归

## 验证

构建：`cmake --build D:\Eden_Project\build\x64\Engine-Windows --target ActorEditor --config MixedAVX`
部署：关闭 AE 后 robocopy ActorEditor.exe/.pdb → `Editors\Designer`，`!ActorEditor.cmd` 启动。

- **菜单/窗体**：Tools 组四项可见中文；连点不崩；关闭重开正常
- **THM**：打开 gamedata\textures 任一 .thm，数值与 Image Editor 属性面板一致；改 material_weight/bump_mode 保存 → Image Editor 重开确认生效
- **DB**：解包现有 .db/.xdb → 抽查文件与 gamedata 原件 bit-exact（lzo 压缩与 raw 各若干）→ 打包（compress 开/关各一次）→ 自家 Unpack 回读 bit-exact → 产物挂进游戏（datapack.ltx）引擎能读
- **OMF**：黄金用例 byte-exact → 重命名动画保存后引擎加载无 "Can't find motion"（-dxdebug 日志）→ 改 speed/flags 对比生效
- **OGG**：参数与 Sound Editor 显示一致 → 只改 MinDist 保存 → Sound Editor 重开确认、试听正常、时长/码率合理
