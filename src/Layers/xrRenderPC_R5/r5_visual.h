#pragma once

// M5: R5Visual —— 轻量静态视觉包装。
// 复用引擎 IRenderVisual 接口，但绕开共享层 CModelPool/Fvisual/RCache 依赖：
// 直接解析 OGF 文件（OGF_VERTICES + OGF_INDICES），上传 DX12 VB/IB。

#include "../../Include/xrRender/RenderVisual.h"
#include "../../xrEngine/vis_common.h"
#include "../../xrEngine/Fmesh.h"
#include <d3d12.h>

// 本地最小 FVF 位定义（R5 不链接 d3d9.h，直接复用 D3DFVF 约定值）
enum
{
	R5_FVF_XYZ		= 0x002,
	R5_FVF_NORMAL	= 0x010,
	R5_FVF_DIFFUSE	= 0x040,
	R5_FVF_SPECULAR	= 0x080,
	R5_FVF_TEXMASK	= 0xf00,
	R5_FVF_TEXSHIFT	= 8,
};

class R5Visual : public IRenderVisual
{
public:
	R5Visual();
	virtual ~R5Visual();

	// IRenderVisual
	virtual vis_data&	_BCL getVisData() override { return m_vis; }
	virtual u32				getType() override { return m_type; }
	virtual shared_str		getDebugName() override { return m_name; }

	// 单个子网格（独立顶点格式/缓冲，自带 stride；规范化 verts = pos+nrm+uv 32B）
	struct SubMesh
	{
		D3D12_GPU_VIRTUAL_ADDRESS VB = 0;
		D3D12_GPU_VIRTUAL_ADDRESS IB = 0;
		UINT stride = 0;
		UINT vCount = 0;
		UINT iCount = 0;
		D3D12_GPU_DESCRIPTOR_HANDLE texSRV = {};	// M6: 子网格贴图 SRV（缺失时回退测试纹理）
	};

	// 解析 OGF 并上传几何到 GPU。失败返回 false。
	bool Load(const char* name);

	xr_vector<SubMesh> m_meshes;

	shared_str m_name;
	u32 m_type = MT_NORMAL;
	vis_data m_vis;
};

namespace r5_visual
{
	// 运行时添加的视觉（线程安全收集，渲染循环读取前由 R5 加锁）
	void AddVisual(R5Visual* v);
	void RemoveVisual(R5Visual* v);

	// 渲染循环读取快照（R5 单线程渲染期调用，无需外部锁）
	size_t LockAndSnapshot(xr_vector<R5Visual*>& out);
}