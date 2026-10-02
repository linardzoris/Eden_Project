# 一、引擎核心指令（xrEngine）

注册位置：[xr_ioc_cmd.cpp](file:///d:/Eden_Project/src/xrEngine/xr_ioc_cmd.cpp)

## 1.1 系统控制类（CCC 自定义类，执行动作而非设值）

| 指令 | 作用 |
| --- | --- |
| `help` | 列出全部指令或查询单条指令用法，例：`help r2_sun` |
| `quit` | 直接退出游戏到桌面 |
| `start` | 启动引擎加载流程（参数 `server(地图)/client(地址)`），如 `start server(l01_escape)` |
| `disconnect` | 断开当前连接/退出到主菜单 |
| `cfg_save 文件名` | 把当前所有指令值保存为 ltx，例：`cfg_save my_preset` |
| `cfg_load 文件名` | 加载指定配置，例：`cfg_load my_preset` |
| `hide` | 隐藏控制台窗口 |
| `flush` / `clear_log` | 强制把日志缓冲写盘 / 清空日志 |
| `crash` | 主动触发崩溃，用于测试崩溃上报/转储 |
| `renderer` | 切换渲染器 DLL（CCC_r2 自定义类） |
| `snd_device 设备名` | 切换音频输出设备 |
| `snd_restart` | 重启声音子系统（改 `snd_acceleration` 等后需要） |
| `stat_memory` | 打印内存统计 |
| `stat_motions` / `stat_textures` | 打印动画/纹理资源统计 |
| `e_list` | 列出引擎事件（Event）队列 |
| `e_signal 事件名` | 手动触发一个引擎事件，脚本/任务调试用 |
| `error_line_count` | int，6~1024，错误窗口保留的行数 |

## 1.2 渲染统计与调试开关 `rs_*`（CCC_Mask，用 on/off）

| 指令 | 作用 |
|---|---|
| `rs_stats on` | 屏幕左上显示帧率/draw call/三角形等完整统计（最常用） |
| `rs_fps_show on/off` | 只显示 FPS |
| `rs_fps_limit N` | int 0~1000，帧率上限，0=不限制。笔记本省电可 `rs_fps_limit 60` |
| `rs_v_sync on/off` | 垂直同步 |
| `rs_fullscreen on/off` | 全屏/窗口化 |
| `rs_vis_distance 0.4~1.0` | 可视距离系数，乘到场景物件的可视半径上。调低可裁剪远处物体提帧 |
| `rs_wireframe on` | 全局线框渲染 |
| `rs_clear_bb on` | 用纯色清后缓冲（调试剔除错误时画面闪色块） |
| `rs_occlusion on/off` | HOM/遮挡剔除总开关，关掉做剔除对照实验 |
| `rs_occ_draw on` / `rs_occ_stats on` | 可视化遮挡体 / 遮挡统计 |
| `rs_render_details on/off` | 是否渲染草地等 detail 物件 |
| `rs_render_statics on/off` | 是否渲染静态几何 |
| `rs_render_dynamics on/off` | 是否渲染动态物体（NPC、武器等） |
| `rs_render_portals on/off` | 是否画 Portal 调试线框 |
| `rs_device_active on/off` | 设备激活状态（切后台是否继续渲染） |
| `rs_cam_pos on` | 屏幕上显示相机坐标 |
| `r_actor_shadow on/off` | 主角自身是否投影 |
| `rs_skeleton_update 2~128` | 骨骼动画更新节流参数 |

## 1.3 亮度/色彩（CCC_Gamma，滑杆型）

`rs_c_gamma` / `rs_c_brightness` / `rs_c_contrast` —— 对应设置里的伽马/亮度/对比度，取值 0.5~1.5 区间滑杆。例：`rs_c_gamma 0.8` 提亮暗部。

## 1.4 视频模式

| 指令 | 类型 | 说明 |
|---|---|---|
| `vid_mode` | Token（动态生成分辨率列表） | 如 `vid_mode 2560x1600`，改完需 `vid_restart` |
| `vid_restart` | 动作 | 重建渲染设备，所有标注"需重启渲染器"的改动靠它生效 |
| `vid_bpp` | Token：`16`/`32` | 色深 |
| `vid_scale_preset` | Token：`st_scale_native / st_scale_quality / st_scale_balanced / st_scale_performance / st_scale_ultraperformance / st_scale_custom` | 分辨率缩放预设（你的环境为 native） |
| `vid_scale` | float 0.3~2.0 | 自定义缩放比（preset=custom 时生效），>1 为超采样 |
| `vid_scale_mode` | Token：`st_filter_linear / st_filter_dlss / st_filter_fsr`（DEBUG 下还有 point） | 上采样算法，DLSS/FSR 需对应运行库 |
| `texture_lod` | int 0~4 | 纹理 LOD 偏移（Mip 质量档位） |

## 1.5 声音

| 指令 | 说明 |
|---|---|
| `snd_volume_eff 0~1` | 音效音量 |
| `snd_volume_music 0~1` | 音乐音量 |
| `snd_acceleration on/off` | 硬件加速音频 |
| `snd_efx on/off` | EFX 环境音效（混响等） |
| `snd_stats*` 一组 6 个 | 声音调试统计显示：最近/最远/AI 距离、声源名、发声物体名 |

## 1.6 输入与相机

| 指令 | 说明 |
|---|---|
| `mouse_invert on/off` | 鼠标 Y 轴反转 |
| `mouse_sens 0.001~0.6` | 鼠标灵敏度 |
| `mouse_sens_ui 0.01~2` | UI 界面光标速度 |
| `input_exclusive_mode` | 独占输入模式（解决部分多屏/远程桌面输入丢失） |
| `input_enable_gamepad on/off` | 手柄开关 |
| `use_smoothed_delta on/off` | 平滑帧间隔 delta（帧时间滤波） |
| `cam_inert 0~0.9` | 镜头惯性（视角迟滞），0=完全跟手 |
| `cam_slide_inert` | 第三人称滑动惯性 |
| `cam_viewport_near EPS~10` | 世界相机近裁剪面，穿模/裁剪异常时调 |
| `cam_hud_viewport_near EPS~10` | HUD（武器手部）相机近裁剪面，改武器 FOV 配套用 |
| `developer_float_1~4` | 引擎层通用调试浮点变量，±100000 |

## 1.7 多线程开关 `mt_*`（CCC_Mask）

`mt_sound`、`mt_physics`、`mt_network`、`mt_particles` —— 对应子系统是否走独立线程。排查多线程 bug 时逐个关闭做二分定位，例如怀疑物理线程竞争：`mt_physics off`。

## 1.8 其他

`net_dedicated_sleep 0~64`（专用服务器每帧 sleep 毫秒）、`net_dbg_dump_export_obj/import_obj 0/1`（网络对象导入导出转储）、`sv_dedicated_server_update_rate 1~1000`（DS 更新频率 Hz）、`sv_shedule_scale 0~5`（DS 调度时间缩放）、`debug_destroy 0/1`（允许引擎销毁调试对象）、`ui_dbg_weather` / `ui_dbg_draw` / `ui_dbg_cmd_vars` / `ui_dbg_cmd_console`（内置 Dear ImGui 编辑器面板开关）。

## 1.9 按键绑定指令（[xr_level_controller.cpp](file:///d:/Eden_Project/src/xrEngine/xr_level_controller.cpp)）

| 指令 | 说明 | 示例 |
|---|---|---|
| `bind 动作 键名` | 绑定主键 | `bind jump space` |
| `bind_sec 动作 键名` | 绑定副键 | `bind_sec forward up` |
| `unbind 动作` / `unbind_sec` | 解绑 | `unbind jump` |
| `unbindall` | 清空全部绑定 | — |
| `default_controls` | 恢复默认键位 | — |
| `bind_list` | 列出当前全部绑定 | — |
| `list_actions` | 列出全部可绑定动作名 | — |
| `bind_console 键名` | 绑定打开控制台的键 | `bind_console grav` |
| `unbind_console` | 解绑控制台键 | — |

---

# 二、渲染器指令（xrRender_R4）

注册位置：[xrRender_console.cpp](file:///d:/Eden_Project/src/Layers/xrRender/xrRender_console.cpp)。这是指令最密集的模块（137 条），按功能分组。

## 2.1 画质总预设

`_preset` —— Token：`Minimum/Low/Default/High/Extreme/Ultra`（默认 `Default`）。一键应用整组画质参数组合，UI 画质滑块背后就是它。例：`_preset Extreme`。

## 2.2 几何/细节/纹理

| 指令 | 范围 | 说明 |
|---|---|---|
| `r__geometry_lod` | 0.1~1.2 | 几何 LOD 系数，越小越早切换到低模（提帧），越大远景越精细 |
| `r__detail_density` | 0.2~0.8 | 草地密度 |
| `r__detail_radius` | 10~300 | 草地渲染半径（米），CCC_DetailRadius 自定义类 |
| `r__detail_l_ambient` | 0.5~0.95 | 草的环境光亮度 |
| `r__detail_l_aniso` | 0.1~0.5 | 草的各向异性光照 |
| `r__dtex_range` | 5~175 | 细节纹理(detail texture)作用距离 |
| `r__tf_aniso` | 1~16 | 各向异性过滤级别，例：`r__tf_aniso 16` |
| `r__tf_mipbias` | -3~+3 | Mip 偏移，负值更锐利（配合 TAA 常用 -0.5） |
| `r__no_ram_textures on` | mask | 纹理不驻留内存（省内存增卡顿风险） |
| `r__mt_texture_load on` | mask | 多线程纹理加载 |
| `r__wallmark_ttl` | 1~600 秒 | 弹孔/血迹留存时间 |
| `r__wallmark_shift_pp / _v` | 0~1 | 壁印防 z-fighting 偏移 |
| `r__fast_details_update on` | mask | 草地快速异步更新 |
| `r__optimize_static_geom` / `r__optimize_dynamic_geom` | 0~2 | 静态/动态几何提交级优化（合并顶点缓存） |
| `r__optimize_shadow_geom on` | mask | 阴影几何优化 |
| `r__shader_cache on` | mask | 着色器缓存（首次进游戏后大幅加速后续加载） |
| `r__lsleep_frames` | 4~30 | 光源休眠帧数（光源多时提帧） |
| `r__ssa_glod_start / _end` | 128~512 / 16~96 | 几何 LOD 的 SSA 起止距离 |
| `r__screenshot_format` | `ss_jpg/ss_tga/ss_png` | 截图格式（默认 PNG） |
| `screenshot` | 动作 | 立即截图 |

## 2.3 R1 遗留指令（R4 下多数无效，仅为兼容保留）

`r1_ssa_lod_a/b`、`r1_lmodel_lerp`、`r1_dlights`、`r1_dlights_clip`、`r1_pps_u/v`（R1 投影灯偏移）、`r1_glows_per_frame`、`r1_detail_textures`、`r1_fog_luminance`、`r1_use_terrain_mask`。**你用 R4 延迟渲染器，这些基本可以忽略**。

## 2.4 色调映射与泛光

| 指令 | 范围 | 说明 |
|---|---|---|
| `r2_tonemap on/off` | mask | 色调映射总开关 |
| `r2_tonemap_middlegray` | 0~2 | 中间灰，整体曝光 |
| `r2_tonemap_adaptation` | 0.01~10 | 明暗适应速度（进暗室瞳孔适应） |
| `r2_tonemap_lowlum` | 0.0001~1 | 低亮度裁剪 |
| `r2_tonemap_amount` | 0~1 | 色调映射混合强度 |
| `r2_ls_bloom_threshold` | 0~1 | 泛光亮度阈值，越低满屏越糊 |
| `r2_ls_bloom_kernel_scale` | 0.5~2 | 泛光扩散半径 |
| `r2_ls_bloom_kernel_g / _b` | 1~7 / 0.01~1 | 高斯核参数/亮度系数 |
| `r2_ls_bloom_speed` | 0~100 | 泛光滑动速度 |
| `r2_ls_bloom_fast on` | mask | 快速泛光（低质量高性能） |
| `r2_ls_squality` | 0.5~10 | 光照质量系数 |
| `r2_gloss_factor` | 0~10 | 高光强度全局倍率 |

例：夜景太黑先别动 gamma，试 `r2_tonemap_middlegray 0.6`。

## 2.5 太阳/阴影（你之前优化的 PCSS 相关）

| 指令 | 范围 | 说明 |
|---|---|---|
| `r2_sun on/off` | mask | 太阳光总开关 |
| `r2_sun_quality` | `st_opt_low/medium/high/ultra/extreme` | 太阳阴影质量（Extreme=PCSS） |
| `r2_smap_size` | `1024/2048/3072/4096` | 阴影贴图分辨率 |
| `r2_sun_details on` | mask | 草地等细节物件投影（即你修过的 DetailManager 阴影路径） |
| `r2_sun_bias` | -0.5~0.5 | 阴影深度偏移，治阴影粉刺/漏光 |
| `r2_sun_near` | 1~50 | 太阳级联近端 |
| `r2_sun_far` | 51~180 | 太阳级联远端（默认覆盖到 160m 级联） |
| `r2_sun_depth_far_scale / near_scale` | 0.5~1.5 | 级联深度缩放 |
| `r2_sun_lumscale` | -1~3 | 太阳光强 |
| `r2_sun_lumscale_hemi / _amb / _sky` | 0~3 | 半球光/环境光/天空光强度 |
| `r2_ls_depth_scale / _bias` | 0.5~1.5 / -0.5~0.5 | 光照深度缩放/偏移（阴影 acne 微调） |
| `r2_shadow_cascede_zcul on` | mask | 级联 Z 剔除（与 E1 级联合并遍历联动） |
| `r2_sun_shafts` | `off/low/medium/high` | 体积光束（god rays） |
| `r2_volumetric_lights on` | mask | 体积光 |
| `r2_dhemi_count` | 4~25 | 动态半球光采样数 |
| `r2_dhemi_sky_scale / light_scale / light_flow / smooth` | — | 半球光细节参数 |

## 2.6 SSAO / GTAO

| 指令 | 说明 |
|---|---|
| `r2_ssao_mode` | Token：`st_opt_off / ui_mm_ssao / ui_mm_gtao` —— AO 模式选择 |
| `r4_gtao_intensity` | float 0.1~4.0，即时生效，GTAO 强度（`pow(occ, intensity)`，上次会话新增） |
| `r4_gtao_resolution` | 0=全分辨率/1=半分辨率，需 `vid_restart`（上次会话新增） |

## 2.7 其他 R2/R3/R4 特性

| 指令 | 说明 |
|---|---|
| `r2_dof_enable on/off` | 景深总开关（P0 优化对象） |
| `r2_dof_kernel` | 0~10，DOF 模糊核 |
| `r2_dof_sky` | ±10000，天空盒 DOF 处理 |
| `r2_parallax_h` | 0~0.5，视差贴图高度 |
| `r2_parallax_range` | 5~175，视差作用距离 |
| `r2_steep_parallax on` | 陡峭视差（更高质量的 POM） |
| `r2_detail_bump on` | 细节法线 |
| `r2_use_bump on` | 法线贴图总开关 |
| `r2_soft_water on` / `r2_soft_particles on` | 软水面/软粒子（深度淡出交界） |
| `r2_slight_fade` | 0.2~1，光源淡出 |
| `r2_zfill on` + `r2_zfill_depth` | 预填 Z（老优化手段，延迟渲染一般不开） |
| `r2_allow_r1_lights on` | 允许 R1 风格光源（性能换兼容） |
| `r2_use_nvdbt on` | NVDBT 阴影工具 |
| `r2_mt on` | 渲染多线程计算 |
| `r2_exp_donttest_shad / _uns` | 实验性：不测试阴影中/非阴影物体（提帧实验） |
| `r2_exp_splitscene on` | 实验性场景分割 |
| `r2_aref_quality` | 70~200，alpha-test 质量控制 |
| `r2em` | 材质自发光系数（CCC_R2GM） |
| `r2_vignette / r2_aberration / r2_saturation on` | 后处理：暗角/色差/饱和度开关 |
| `r3_volumetric_smoke on` | 体积烟雾（P5 优化对象） |
| `r3_dynamic_wet_surfaces on` + `_near 10~70` / `_far 30~100` / `_sm_res 64~2048` | 动态湿表面（下雨反光）及参数 |
| `r3_fog_reload` | 动作，重载雾配置 |
| `r4_enable_tessellation on` | 曲面细分 |
| `r4_sslr_water on` / `r4_sslr_reflections on` | 水面/世界空间屏幕反射（你仅开水面） |
| `r4_sslr_quality` | 0~2，需 vid_restart |
| `r4_enable_vslr on` + `r4_vslr_distance 0.4~1` | 离屏反射系统 |
| `r4_cas_sharpening` | 0~1，CAS 锐化强度（TAA 软画面补偿） |
| `r4_hud_shadows on` | 屏幕空间 HUD 投影（武器手部自阴影） |
| `r4_hashed_alpha_test on` | 哈希 alpha 测试（植被边缘更平滑，配合 TAA） |
| `r4_puddles on` | 动态水洼 |
| `r4_wireframe on` | R4 线框（需重启） |
| `r_aa` | Token：`st_opt_off / fxaa / smaa / taa`（你为 off） |
| `r_taa_jitter_scale` | Vector3，TAA 抖动幅度微调 |
| `r2_wait_sleep` | 0/1，渲染等待方式 |
| `rs_hom_depth_draw on` | HOM 深度绘制调试 |
| `rs_dbg_draw_depth` | 0/1，深度调试绘制 |

## 2.8 渲染调试动作类

`build_ssa`（重算 SSA 可见集）、`rdoc_start`/`rdoc_end`（RenderDoc 捕获触发，配 `renderdoc.dll` 注入用）、`dump_resources`（转储全部渲染资源）、`render_memory_stats`（渲染显存统计）、`r_restore_quad_ib_data`（修复全屏 quad 索引缓存）、`ui_dbg_graphic`（ImGui 着色器面板）、`r_developer_float_1~4`（±1000 万，传给着色器的测试常量，你调着色器时可直接用这 4 个做运行时参数，无需重编译引擎）。

---

# 三、单人游戏指令（xrGame）

注册位置：[console_commands.cpp](file:///d:/Eden_Project/src/xrGame/console_commands.cpp)，273 条，量最大。

## 3.1 游戏性 `g_*`（多为 CCC_Mask，on/off）

| 指令 | 说明 | 示例 |
|---|---|---|
| `g_game_difficulty` | 难度（自定义类，执行时调 `OnDifficultyChanged`） | `g_game_difficulty master` |
| `g_god on` | 无敌 | — |
| `g_unlimitedammo on` | 无限弹药（不换弹匣） | — |
| `g_unlimited_fire on` | 武器不过热/无限开火 | — |
| `g_unlimited_durability on` | 装备不掉耐久 | — |
| `g_no_clip on` | 穿墙飞行 | — |
| `g_autopickup on` | 自动拾取 | — |
| `g_backrun on` | 允许倒退跑 | — |
| `g_crouch_toggle on` | 蹲改为切换式 | — |
| `g_dynamic_music on` | 动态音乐 | — |
| `g_important_save on` | 重要节点自动存档 | — |
| `g_hit_slowmo on` | 击杀慢动作 | — |
| `g_money 金额` | 给钱（作弊） | `g_money 100000` |
| `g_spawn section名` | 在准星处生成物体 | `g_spawn wpn_ak74` |
| `g_spawn_inv section名` | 直接生成到背包 | `g_spawn_inv medkit` |
| `g_spawn_squad 小队名` | 生成整个 ALife 小队 | — |
| `g_info id` / `d_info id` | 给予/移除 info_portion（剧情标记） | 解卡剧情用 |
| `g_character_community 阵营` | 改主角阵营 | `g_character_community killer` |
| `g_monster_community 阵营` | 改怪物阵营归属 | — |
| `g_ps_test` | 粒子系统测试 | — |
| `g_fight_fast_respawn 0/1` | 战斗快速重生 | — |
| `g_bobbing_factor 0.3~1` | 走路镜头晃动系数 | — |
| `g_bullet_time_factor 0~10` | 子弹时间系数 | `g_bullet_time_factor 0.3` |
| `g_sleep_time 1~24` | 睡觉时长（小时） | — |
| `g_3d_scopes on/off` | 3D 瞄准镜（画中画） | 帧数低可关 |
| `g_cam_fp_zoom 0/1` | 第一人称缩放相机 | — |
| `g_swapteams` | 交换阵营（MP） | — |
| `g_restart` / `g_restart_fast` | 重开关卡（完整/快速） | — |
| `g_kill` | 杀死玩家（MP 用） | — |
| `g_eventdelay 0~1000` | 网络事件延迟（MP） | — |
| `g_corpsenum 0~100` | 尸体最大保留数（MP） | — |

## 3.2 HUD 与视角

| 指令 | 说明 |
|---|---|
| `hud_weapon on/off` | 显示武器模型 |
| `hud_info on/off` | 显示 HUD 信息 |
| `hud_draw on/off` | HUD 总开关（截图用 `hud_draw off`） |
| `hud_crosshair on/off` | 准星 |
| `hud_crosshair_point on/off` | 中心点 |
| `hud_crosshair_dist on/off` | 距离显示 |
| `cl_dynamiccrosshair on/off` | 动态扩散准星 |
| `hud_fov 5~180` | 武器手部 FOV（独立于世界 FOV，常用 0.5~0.6 的比例值如 `hud_fov 0.55` 视引擎换算） |
| `fov 5~180` | 世界 FOV，例：`fov 75` |
| `wpn_aim_toggle on/off` | 瞄准改切换式 |
| `hud_adj_delta_pos / _rot 0.0001~1` | HUD 武器位置/旋转调整步进（配 `hud_adjust` 模式调枪位） |
| `con_sensitive 0.01~1` | 控制台输入灵敏度 |
| `slot_0~3 "物品section"` | 快捷栏绑定，例：`slot_0 "medkit"` |

## 3.3 ALife（模拟层）

| 指令 | 说明 |
|---|---|
| `al_time_factor 0~1000` | ALife 时间流速 |
| `al_switch_distance` | 在线/离线切换距离（自定义类） |
| `al_process_time` | 单次 ALife 处理时长 |
| `al_objects_per_update` | 每帧处理的离线对象数 |
| `al_switch_factor` | 切换因子 |
| `al_path` | ALife 寻路构建调试 |

## 3.4 存档/关卡跳转

`save 存档名` / `load 存档名` / `load_last_save`、`jump_to_level 关卡名`（如 `jump_to_level l02_garbage`）、`set_actor_position x y z`（传送主角）、`set_weather 天气名`（如 `set_weather clear`）、`set_game_time 时 分`、`start_time_single`、`time_factor_single 0~10000`（单人时间流速）。

## 3.5 AI 调试（`ai_*`，共 30+ 条 mask/开关）

- **行为开关**：`ai_obstacles_avoiding`（障碍规避）、`ai_obstacles_avoiding_static`、`ai_use_smart_covers`（智能掩体）、`ai_use_smart_covers_animation_slots`、`ai_use_torch_dynamic_lights`（手电动态光）、`ai_ignore_actor`（AI 无视玩家，跑图测试神器）、`ai_aim_use_smooth_aim`
- **数值**：`ai_smart_factor 0~1e6`（掩体评分因子）、`ai_smart_cover_animation_speed_factor 0.1~10`、`ai_dbg_inactive_time`、`lua_gcstep 1~1000`（Lua GC 步进）
- **调试显示**：`ai_debug`、`ai_dbg_brain/motion/frustum/funcs/alife/goap/goap_script/goap_object/cover/anim/vision/monster/stalker/lua/dialogs/infoportion`、`ai_stats`、`ai_dbg_destroy/serialize`、`ai_dbg_sight`、`ai_debug_doors`、`ai_nil_object_access`、`ai_draw_visibility_rays`（可视射线）、`ai_animation_stats`
- **游戏图（game graph）可视化**：`ai_draw_game_graph`（+`_stalkers`/`_objects`/`_real_pos`），动作类 `ai_draw_game_graph_all` / `ai_draw_game_graph_current_level` / `ai_draw_game_graph_level`
- **动作类**：`ai_dbg_node`（当前节点信息）、`ai_monster_info`（怪物状态打印）、`ai_show_animation_stats`

例：测试战斗 AI 时 `ai_ignore_actor on` + `ai_dbg_vision on` + `ai_draw_visibility_rays on`，可站着观察 NPC 视线判定。

## 3.6 物理 `ph_*` 与物理调试绘制

| 指令 | 说明 |
|---|---|
| `ph_frequency` | 物理步进频率（Hz） |
| `ph_iterations` | 求解迭代次数 |
| `ph_gravity` | 重力（默认 -9.81 量级，改 -1 体验月球跳跃） |
| `ph_timefactor 1e-6~1000` | 物理时间缩放 |
| `ph_break_common_factor` / `ph_rigid_break_weapon_factor` | 可破坏物/武器破坏阈值因子 |
| `ph_tri_clear_disable_count 0~255` / `ph_tri_query_ex_aabb_rate 1.01~3` | 三角形缓存管理 |

`dbg_draw_ph_*` 系列约 25 条 mask：碰撞接触点、AABB、相交三角形、质量中心、车辆动力学曲线、IK 目标/限制/预测/碰撞/混合、布娃娃、爆点位置、ZBuffer 关闭显示等。`dbg_ph_ladder`（梯子调试）、`dbg_draw_bullet_hit 0/1`（弹道命中点可视化，调武器散布必用）、`dbg_draw_fb_crosshair`（首发弹道准星）、`dbg_track_obj`（追踪指定物体的动画混合状态，配套 11 条 `dbg_track_obj_blends_*` 过滤项）。

## 3.7 受击动画调参（hit anims）

`hit_anims_power` / `_rotational_power` / `_side_sensitivity_threshold` / `_channel_factor` / `_block_blend` / `_reduce_blend` / `_reduce_blend_factor`（均 float 范围各异）+ `hit_anims_tune 0/1`（调参模式开关）。调 NPC 中弹反应动画强度用。

## 3.8 杂项调试/工具

| 指令 | 说明 |
|---|---|
| `demo_record 名称` / `demo_play 名称` | 录制/回放相机 demo |
| `demo_set_cam_position` | demo 录制中设相机位 |
| `run_script 脚本名` | 执行 Lua 脚本文件，例：`run_script my_debug` |
| `run_string "代码"` | 直接执行 Lua 语句，例：`run_string "alife():create('wpn_ak74',db.actor:position(),db.actor:level_vertex_id(),db.actor:game_vertex_id(),db.actor:id())"` |
| `lua_help` | 列出导出给 Lua 的引擎函数 |
| `reload_system_ltx` | 热重载 system.ltx（改配置免重启） |
| `ui_reload` | 热重载 UI xml |
| `language` | 切换语言 |
| `main_menu` | 回主菜单 |
| `stat_memory` | 内存统计 |
| `debug_dump_model_bones` | 转储准星目标模型的骨骼 |
| `debug_fonts` | 字体调试 |
| `dump_infos / dump_tasks / dump_map / dump_creatures / dump_all_objects` | 转储剧情信息/任务/地图/生物/全部对象到日志 |
| `dbg_adjust_attachable_item` | 调挂件（手电等）挂点偏移的交互工具 |
| `dbg_var` | 通用调试变量 |
| `dbg_text_height_scale 0.2~5` | 调试文字大小 |
| `string_table_error_msg 0/1` | 字符串表缺失是否写日志 |
| `inv_upgrades_hierarchy` / `inv_upgrades_cur_item` / `inv_upgrades_log 0/1` | 武器改装树调试 |
| `inv_drop_all_items` | 丢空背包 |
| `stalker_death_anim "名称"` | 指定死亡动画测试 |
| `death_anim_debug/velocity` | 死亡动画调试 |
| `dbg_imotion_*`（5 条） | 惯性动作系统调试 |
| `show_wnd_rect_all 0/1` | 显示全部 UI 矩形边框 |
| `dbg_show_ani_info 0/1` | 武器动画信息（调你之前做的 HUD 动画 key 时用） |
| `dbg_dump_physics_step 0/1` | 物理步进转储 |
| `dbg_bones_snd_player 0/1` | 骨骼音效调试 |
| `ik_cam_shift` + `_tolerance` + `_speed`、`ik_allign_free_foot` / `ik_local_blending` / `ik_blend_free_foot` / `ik_collide_blend` | IK 相机与脚步 IK 参数 |
| `dbg_draw_doors` / `dbg_draw_camera_collision` / `camera_collision_character_shift_z` / `_skin_depth` | 门与第三人称相机碰撞 |
| `debug_step_info` / `_load` / `debug_character_material_load` | 脚步声/角色材质加载调试 |
| `dbg_draw_ragdoll_spawn` | 布娃娃生成调试 |
| `psp_cam_offset_r / _l` | Vector3，第二人称相机（CCD 类）偏移 |
| `set_actor_position` 传送后配合 `g_no_clip on` 可自由取景 |
| `keypress_on_start 0/1` | 启动即响应按键 |
| `air_resistance_epsilon 0~1` | 空气阻力参数 |
| `enable_dof_reload` / `enable_dof_talk` | 换弹/对话时 DOF 开关 |
| `get_console_colors` | 打印控制台配色 |
| `chZLoggerTest` | 日志系统测试 |
| `rank_for_buymenu 0~4` | （DEBUG 版）商店购买菜单军衔门槛调试 |

`dbg_draw_*` 通用调试绘制（非物理类）：`dbg_draw_actor_alive/dead`、`dbg_draw_customzone`（异常区）、`dbg_draw_teamzone`、`dbg_draw_invitem`、`dbg_draw_actor_phys`、`dbg_draw_customdetector`、`dbg_draw_autopickupbox`、`dbg_draw_rp`（重生点）、`dbg_draw_climbable`（可攀爬）、`dbg_draw_skeleton`（骨骼线框）、`dbg_draw_lchangers`（关卡切换点）、`dbg_destroy`。

## 3.9 多人连接菜单指令（[UIOptConCom.cpp](file:///d:/Eden_Project/src/xrGame/ui/UIOptConCom.cpp)）

`mm_net_player_name`（玩家名）、`mm_net_srv_name`（服务器名）、`mm_net_srv_maxplayers 2~32`、`mm_net_srv_gamemode`（dm/tdm/ah/cta token）、`mm_mm_net_srv_dedicated`、`mm_net_con_publicserver`、`mm_net_con_spectator_on` + `mm_net_con_spectator 1~32`、`mm_net_srv_reinforcement_type 0~2`、`mm_net_weather_rateofchange 0~100`、服务器浏览器过滤器 `mm_net_filter_empty/full/pass/wo_pass/wo_ff/listen`。

---

# 四、多人游戏指令（xrGame MP）

注册位置：[console_commands_mp.cpp](file:///d:/Eden_Project/src/xrGame/console_commands_mp.cpp)，141 条。你的项目是单人向，这组多数只在开服务器时有意义，挑重点说。

## 4.1 网络同步 `net_*`

| 指令 | 范围 | 说明 |
|---|---|---|
| `net_cl_interpolation` | -1~1 | 客户端插值系数 |
| `net_cl_icurvetype` | 0~2 | 插值曲线类型（线性/三次等） |
| `net_cl_icurvesize` | 0~2000 | 插值点数 |
| `net_cl_update_rate` | 20~100 | 客户端发包频率 |
| `net_sv_update_rate` | 1~100 | 服务器广播频率 |
| `net_cl_pending_lim` / `net_sv_pending_lim` | 0~10 | 堆积包限制 |
| `net_sv_gpmode` | 0~2 | 可靠包模式 |
| `net_sv_log_data` / `net_cl_log_data` / `net_dump_size` | mask | 网络包日志/体积转储 |
| `net_cl_resync` | 动作 | 客户端强制重新同步 |
| `net_cl_clearstats` / `net_sv_clearstats` | 动作 | 清网络统计 |
| `net_dbg_objects` | 动作 | 转储网络对象数 |
| `net_dbg_dump_update_write/read` | 0/1 | update 读写转储 |
| `net_compressor_status` | 动作 | 压缩器状态 |
| `net_compressor_enabled` / `_gather_stats` | 0/1 | 流量压缩开关/统计 |
| `cdkey` | 字符串 | 设置 CDKey |

## 4.2 服务器管理 `sv_*`

- **踢封**：`sv_kick 名字` / `sv_kick_id ID`、`sv_banplayer`（按 CDKey）/ `sv_banplayer_by_digest` / `sv_banplayer_ip`、`sv_unbanplayer`（按索引）/ `sv_unbanplayer_ip`、`sv_listplayers` / `sv_listplayers_banned`
- **地图/模式**：`sv_changegametype` / `sv_changelevel` / `sv_changelevelgametype`、`sv_addmap` / `sv_listmaps` / `sv_nextmap` / `sv_prevmap` / `sv_nextanomalyset`
- **规则数值**（CCC_SV_Integer/Float，改完全服务器同步广播）：`sv_forcerespawn 0~3600s`、`sv_fraglimit 0~1000`、`sv_timelimit 0~180min`、`sv_warm_up 0~3600s`、`sv_dmgblocktime 0~600s`、`sv_invincible_time 0~60s`、`sv_max_ping_limit 1~2000`、`sv_friendlyfire 0~2`（友伤系数）、`sv_teamkill_limit 0~100`、`sv_auto_team_balance/swap`、`sv_friendly_indicators/names`、`sv_anomalies_enabled` + `sv_anomalies_length`、`sv_pda_hunt`、`sv_remove_weapon/-1~1` / `sv_remove_corpse`、`sv_rpoint_freeze_time 0~60000`
- **ArtefactHunt/CTA 模式**：`sv_artefact_respawn_delta 0~600s`、`sv_artefacts_count 1~100`、`sv_artefact_stay_time 0~180min`、`sv_reinforcement_time -1~3600s`、`sv_bearercantsprint`、`sv_shieldedbases`、`sv_returnplayers`、`sv_artefact_returning_time`、`sv_activated_return`、`sv_show_player_scores_time`、`sv_cta_runkup_to_arts_div`
- **观察者**：`sv_spectr_freefly/firsteye/lookat/freelook/teamcamera`（观战相机模式开关）
- **投票**：`sv_vote_enabled 0~0xFF`、`sv_vote_participants`、`sv_vote_quota 0~1`（通过比例）、`sv_vote_time 0.5~10min`、`sv_votestop`；客户端对应 `cl_votestart` / `cl_voteyes` / `cl_voteno`
- **其他**：`sv_status`（服务器状态）、`sv_startteammoney`（起始资金）、`sv_hail_to_winner_time 0~60`、`sv_client_reconnect_time 0~60`、`sv_skip_winner_waiting`、`sv_wait_for_players_ready`、`sv_return_to_base`、`sv_no_auth_check`（关 CDKey 校验，CCC_AuthCheck）、`sv_artefact_spawn_force`、`sv_statistic_collect` + `sv_statistic_save` + `sv_dump_online_statistics` + `_period`、`sv_adm_menu_ban_time`（token 时长表）+ `sv_adm_menu_ping_limit`、`sv_write_update_bin`、`sv_traffic_optimization_level 0~7`、`sv_savescreenshots`/`sv_saveconfigs`（存反作弊截图/配置）

## 4.3 反作弊与 MP demo

`make_screenshot` / `make_config_dump`（强制指定客户端回传截图/配置）、`screenshot_all` / `config_dump_all`、`dbg_make_screenshot`、`draw_downloads 0/1`、`mpdemoplay_speed_set` / `_pause_on` / `_cancel_pause_on` / `_rewind_until` / `_stop_rewind` / `_restart` / `_mulspeed` / `_divspeed`（MP demo 回放控制组）。

## 4.4 其他

`spawn_on_position section x y z`（定点生成）、`give_money 金额` / `transfer_money`（MP 经济）、`ra 命令`（radmin 远程管理）、`name 名字`（改名）、`chat 文本`（服务器广播聊天）、`cl_dbg_min_ping/max_ping 0~1000`（本地延迟模拟器，测网络代码用）、`get_server_address`、`snd_volume_players 0~1` / `snd_volume_recorder 0~1` / `snd_recorder_mode 0/1` / `snd_recorder_denoise 0/1`（语音聊天相关）。
