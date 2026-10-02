## 一、左列：总体指标（坐标 10,35 起）

```
FPS/RFPS:    165.0/280.5
TPS:         2.35 M
VERT:        450000/750
POLY:        320000/533
DIP/DP:      600
xforms:      480
```

| 行 | 含义 |
|---|---|
| **FPS/RFPS** | 左=整机帧率（指数平滑后的整帧 FPS）；右=RFPS，**仅渲染提交耗时换算的帧率**（1000/RenderTOTAL）。RFPS 明显高于 FPS 说明 CPU 侧（引擎逻辑）吃掉了差额 |
| **TPS** | 每秒渲染的三角形数，单位百万（M）。用 `三角形数/渲染耗时` 算的吞吐率，衡量 GPU 填充压力 |
| **VERT a/b** | 本帧提交总顶点数 / 平均每次 draw call 顶点数 |
| **POLY a/b** | 本帧提交总三角形数 / 平均每次 draw call 三角形数 |
| **DIP/DP** | draw call 总数（DIP=DrawIndexedPrimitive）。R4 下健康值一般 <1000，超 1000 在 DEBUG 版会红字报警 |
| **xforms** | 本帧世界矩阵变换次数（物体矩阵上传次数），反映场景物体数量级 |

---

## 二、ENGINE 区块（CPU 侧耗时，全部单位 ms，%为占引擎总时间比例）

```
*** ENGINE:  4.50ms
Memory:      120.00a
uClients:    1.20ms, 26.7%, crow(45)/active(120)/total(800)
uSheduler:   0.30ms, 6.7%
uSheduler_L: 0.30ms
uParticles:  Qstart[2] Qactive[15] Qdestroy[1]
spInsert:    o[0.01ms, 0.2%], p[0.00ms, 0.0%]
spRemove:    o[0.01ms, 0.2%], p[0.00ms, 0.0%]
Physics:     0.80ms, 17.8%
  collider:  0.30ms
  solver:    0.50ms, 120
aiThink:     0.60ms, 85
  aiRange:   0.10ms, 85
  aiPath:    0.05ms, 3
  aiNode:    0.02ms, 85
aiVision:    0.40ms, 85
  Query:     0.25ms
  RayCast:   0.15ms
```

| 行 | 含义 |
|---|---|
| **ENGINE** | 引擎主循环单帧总耗时（不含纯渲染等待），下面各项的汇总基准 |
| **Memory** | 每帧内存分配器调用次数（a=allocations）。持续很高（>1500）说明有代码在每帧频繁 new/delete，会引发卡顿 |
| **uClients** | 对象 UpdateClient 耗时。crow=本帧进入更新半径的对象数，active=激活对象数，total=场景对象总数。crow 异常大说明 `al_switch_distance` 类参数把太多对象拉进在线状态 |
| **uSheduler / uSheduler_L** | 调度器耗时 / 调度负载。>3ms 在 DEBUG 版会红字警告 |
| **uParticles** | 粒子系统：本帧新启动/激活中/待销毁的粒子系统数 |
| **spInsert / spRemove** | 空间数据库（spatial DB）插入/移除耗时，o=普通对象树，p=物理对象树。物体大量生成/销毁时会飙升 |
| **Physics** | 物理总耗时。collider=碰撞检测（窄相），solver=约束求解（后面的数字是求解的接触点数量） |
| **aiThink** | AI 思考耗时，数字=本帧思考的 AI 数 |
| **aiRange** | AI 感知范围检查 |
| **aiPath / aiNode** | AI 寻路 / 图节点查询，数字=调用次数。aiPath 次数高=大量 NPC 同时在规划路径 |
| **aiVision** | AI 视觉系统总耗时。Query=可见性查询构建，RayCast=实际视线射线检测（遮挡判定） |

---

## 三、RENDER 区块（渲染侧耗时）

```
*** RENDER:  3.20ms
R_CALC:      1.10ms, 34.4%
  HOM:       0.20ms, 45
  Skeletons: 0.30ms, 60
R_DUMP:      2.10ms, 65.6%
  Wait-L:    0.05ms
  Wait-S:    0.00ms
  Skinning:  0.15ms
  DT_Vis/Cnt:0.30ms/3200
  DT_Render: 0.20ms
  DT_Cache:  0.01ms
  Wallmarks: 0.02ms, 5/2 - 120
  Glows:     0.01ms
  Lights:    0.40ms, 25
  RT:        0.10ms, 8
  HUD:       0.15ms
  P_calc:    0.10ms
  S_calc:    0.30ms
  S_render:  0.50ms, 45
```

| 行 | 含义 |
|---|---|
| **RENDER** | 渲染线程/提交总耗时 |
| **R_CALC** | CPU 侧场景计算：视锥剔除、可见集构建、排序。%占 RENDER 比例。|
| **HOM** | 遮挡剔除（Hierarchical Occlusion Map）计算耗时，数字=测试次数 |
| **Skeletons** | 骨骼动画计算耗时，数字=本帧更新的骨架数（受 `rs_skeleton_update` 节流影响） |
| **R_DUMP** | 实际向 GPU 提交绘制的耗时 |
| **Wait-L** | 等待遮挡查询结果回读（GPU→CPU 同步点），高=CPU 被 GPU 拖住 |
| **Wait-S** | `rs_fps_limit` 帧率限制的主动 sleep 时间 |
| **Skinning** | 蒙皮计算耗时 |
| **DT_Vis/Cnt** | 细节物件（草地）可见性计算耗时 / 可见草地实例数——就是 DetailManager 那条路径，实例数异常高说明 `r__detail_density/radius` 设大了 |
| **DT_Render / DT_Cache** | 草地实际渲染耗时 / 草地缓存构建 |
| **Wallmarks** | 弹孔/血迹渲染耗时，5/2=静态/动态壁印数，120=总数 |
| **Glows** | 光源光晕渲染 |
| **Lights** | 动态光源处理耗时，数字=本帧处理的光源数（下面对应中列 LT/LV 统计） |
| **RT** | 渲染目标切换耗时，数字=切换次数。R4 延迟渲染每帧要切 G-Buffer/光照/后处理多个 RT，次数异常多会拖慢 |
| **HUD** | 武器手部模型（HUD 视口）渲染耗时 |
| **P_calc / S_calc** | 阴影投射体计算 / 太阳级联阴影计算——E1 优化的直接观测点 |
| **S_render** | 阴影贴图（SMAP）实际渲染耗时，数字=渲染的阴影投射体数 |

---

## 四、中列：R4 渲染器专属（坐标 250,35，[r4.cpp](file:///d:/Eden_Project/src/Layers/xrRenderPC_R4/r4.cpp#L590-L608)）

```
 **** LT:25,LV:18 ****
    S(10)   | (8)NS
smap use[8], merge[2], finalclip[6]
 **** Occ-Q(045.0) ****
 total  : 40
 culled : 18
 **** iCULL(062.3) ****
 visible: 150
 culled : 250
```

| 行 | 含义 |
|---|---|
| **LT/LV** | 本帧光源总数 / 可见光源数。差值=被剔除掉的光源 |
| **S / NS** | 可见光源中带阴影的（Shadowed）/ 不带阴影的（No Shadow）。带阴影光源是性能大头 |
| **smap use/merge/finalclip** | 阴影贴图管理：use=实际分配渲染的 smap 数，merge=合并复用的，finalclip=最终裁剪后参与渲染的 |
| **Occ-Q** | GPU 遮挡查询：括号内是被剔除百分比，total=发起的查询总数，culled=被挡掉跳过渲染的数量。百分比高=遮挡剔除工作良好 |
| **iCULL** | 物体级交互剔除（光源视锥+遮挡联合）：visible=最终渲染的物体数，culled=被剔除数，百分比=剔除率 |

DEBUG 构建此列还有 HOM 的命中统计。

---

## 五、右列：声音/输入/碰撞/网络（坐标 500,35）

```
*** SOUND:   0.30ms
  TGT/SIM/E: 12/30/45
  HIT/MISS:  150/3
Input:       0.02ms
clRAY:       0.50ms, 200, 15K
clBOX:       0.10ms, 50, 8K
clFRUSTUM:   0.05ms, 30
netClientRecv: 0.01ms, 5
netClientSend: 0.01ms, 5
netServer:   0.00ms, 0
netClientCompressor: 0.00ms
netServerCompressor: 0.00ms
TEST 0..3:   ...
qpc[1234]
```

| 行 | 含义 |
|---|---|
| **SOUND** | 声音渲染耗时。TGT=实际混合发声的通道数，SIM=模拟中的声源数，E=本帧声音事件数 |
| **HIT/MISS** | 声音缓存命中/未命中，MISS 高=声音在频繁从磁盘读 |
| **Input** | 输入处理耗时 |
| **clRAY** | 碰撞射线检测：耗时 / 次数 / 每次平均测试的三角形数（K=千）。射击、AI 视线都走这里，每次测的三角形多说明场景碰撞模型复杂 |
| **clBOX** | 盒体碰撞查询，格式同上 |
| **clFRUSTUM** | 视锥查询次数（AI 视野等用） |
| **net*** | 网络收发/压缩耗时（单人游戏基本为 0，可忽略） |
| **TEST 0~3** | 4 个空的自定义计时器，开发时手动包代码段用（正常全为 0） |
| **qpc** | 本帧 QueryPerformanceCounter 调用次数，排查计时器滥用 |

---

## 六、其余两块

- **最顶部闪烁的三行文字**：来自 `system.ltx` 的 `[evaluation]` 段 line1/2/3，每 2000 帧前半显示后半隐藏，是评测版（evaluation build）的遗留标识，与性能无关。
- **左列底部的 static/flora/dynamic/details 分组**（[dxStatsRender.cpp](file:///d:/Eden_Project/src/Layers/xrRender/dxStatsRender.cpp#L32-L45) OutData4）：按几何类型拆分的 `顶点K数/draw call 数`——static=静态建筑地形，flora=植被（flora_lods=远景植被 LOD），dynamic=动态物体（_sw=软变换、_inst=实例化、_1B~4B=1~4 骨骼蒙皮），details=草地。

## 实用读法

- **CPU 瓶颈定位**：`ENGINE` > `RENDER` 且 `uClients`/`aiThink` 高 → 逻辑侧瓶颈；`R_CALC` 高 → 剔除/场景遍历瓶颈；`R_DUMP` 高 → 提交瓶颈。
- **GPU 瓶颈特征**：FPS 低但面板所有 ms 都很低、且 Wait-L 高 → CPU 在等 GPU，该降的是着色器负载。

注意：DEBUG 版才有的红字报警（FPS<30、Verts>500k、DIP>1k、Update>3ms 等）在 `#ifdef _DEBUG` 下编译。
