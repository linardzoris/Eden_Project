#pragma once

// M8: R5 最小化静态世界加载器
// 自解析 level.geom（共享 VB/IB 池）+ level（VISUALS/SECTORS），
// 上传 DX12 规范几何（pos+nrm+uv 32B），支持静态世界渲染。

#include <d3d12.h>
#include "../../xrEngine/Fmesh.h"

// 单条 VB 记录（level.geom fsL_VB 池中一条，规范化为 32B 顶点）
struct R5GeomVB
{
	D3D12_GPU_VIRTUAL_ADDRESS addr = 0;
	UINT vCount = 0;
	UINT stride = 32;
};

// 单条 IB 记录（level.geom fsL_IB 池中一条，u16 索引）
struct R5GeomIB
{
	D3D12_GPU_VIRTUAL_ADDRESS addr = 0;
	UINT iCount = 0;
};

// 一个静态网格（引用 VB/IB 池区间；vBase/iBase 为池内偏移）
struct R5StaticMesh
{
	UINT vb = 0xFFFFFFFF;
	UINT vBase = 0;
	UINT vCount = 0;
	UINT ib = 0xFFFFFFFF;
	UINT iBase = 0;
	UINT iCount = 0;
	u32 shader = 0xFFFFFFFF;	// OGF_HEADER.shader_id -> world.shaderNames 索引（M8 真实贴图）

	// M8: MT_PROGRESSIVE（OGF_SWIDATA）LOD 窗口。lodIndices==0 表示非 progressive，按全量 iCount 绘制。
	// 仿 FProgressive::Render：(vBase, 0, SW.num_verts, iBase+SW.offset, SW.num_tris)
	UINT lodIndexOffset = 0;	// SW.offset（相对 iBase 的索引偏移）
	UINT lodIndices = 0;		// SW.num_tris*3
};

struct R5LevelWorld
{
	xr_vector<R5GeomVB> vbs;
	xr_vector<R5GeomIB> ibs;
	xr_vector<R5StaticMesh> meshes;
	Fbox aabb;				// 世界包围盒（M8: 直接加载时地图轨道相机用）
	xr_vector<shared_str> shaderNames;					// fsL_SHADERS：每个 shader_id 的主贴图路径
	xr_vector<D3D12_GPU_DESCRIPTOR_HANDLE> shaderSRVs;	// 预加载的 SRV（无效为 {}，回退测试纹理）
	bool direct = false;	// true = 直接读磁盘地图（无游戏相机），false = 引擎关卡
	bool loaded = false;
};

namespace r5_level
{
	// 解析 level.geom + level 的 VISUALS/SECTORS，填充 world（几何上传 upload ring 持久区）
	bool Load(class IReader* fs, R5LevelWorld& world);
	// 直接从磁盘读 gamedata\levels\<mapName>\ 的 level.geom + level（绕过游戏/存档，M8 调试）
	bool LoadDirect(const char* mapName, R5LevelWorld& world);
	// M8: 预加载全部 shader 主贴图（create 阶段/首帧前调用，避免每帧 g_gpuHeap Alloc）
	void PreloadTextures(R5LevelWorld& world);
	void Clear(R5LevelWorld& world);
}
