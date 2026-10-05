#include "stdafx.h"
#include "dx12ShaderResourceStateCache.h"

dx12ShaderResourceStateCache	SRVSManager;

dx12ShaderResourceStateCache::dx12ShaderResourceStateCache() {}
dx12ShaderResourceStateCache::~dx12ShaderResourceStateCache()
{
	// 进程退出时归还持有的引用（此时 SRV 对象因本引用仍然存活，Release 安全）
	ResetDeviceState();
}

void dx12ShaderResourceStateCache::ResetDeviceState()
{
	// 先归还引用再清零：这些指针在 vid_restart 后指向旧设备的 SRV 对象。
	// 我们持引用期间它们的 dtor（含 persistentSrv 槽位归还）不会触发；
	// 这里 Release 后若引用归零即正常析构，堆内存随即回收。
	for (UINT i = 0; i < kSlots; ++i)
	{
		if (m_ps[i]) { m_ps[i]->Release(); m_ps[i] = nullptr; }
		if (m_vs[i]) { m_vs[i]->Release(); m_vs[i] = nullptr; }
		if (m_gs[i]) { m_gs[i]->Release(); m_gs[i] = nullptr; }
		if (m_hs[i]) { m_hs[i]->Release(); m_hs[i] = nullptr; }
		if (m_ds[i]) { m_ds[i]->Release(); m_ds[i] = nullptr; }
		if (m_cs[i]) { m_cs[i]->Release(); m_cs[i] = nullptr; }
	}
}

void dx12ShaderResourceStateCache::Apply()
{
	// DX12：描述符在 Draw 时绑定，这里无需提交。
}

//------------------------------------------------------------------------------
// Set 系列持引用（AddRef/Release）：
//
// 流送线程可能在 Apply 与 Draw 之间替换/销毁纹理的 SRV（CTexture::surface_set
// 会释放旧 m_pSRView 再建新视图；关卡加载刚结束时这种替换风暴最密集）。缓存里
// 若只存裸指针，FlushPipeline 会在渲染线程解引用已释放的 dx12ShaderResourceView
//（读 srv->cpu / dv->resource / dv->srcBuffer 全是 UAF）→ 拷入全零或垃圾描述符
// → GPU 在随机 Draw 上页错误（实测：无 dxdebug 时 game_loaded 后 1~2 帧 TDR
// 0x887a0006 / PageFaultVA=0x0，死亡位置随崩溃点漂移；GBV 拖慢 GPU 后竞争窗口
// 缩小到不可见故 -dxdebug 不复现）。
// 持引用期间对象与 persistentSrv 槽位保证存活，UAF 从根上消除；替换绑定时
// 归还旧引用即可。
//------------------------------------------------------------------------------
void dx12ShaderResourceStateCache::SetPSResource(UINT uiSlot, ID3DShaderResourceView* pRes)
{
	if (uiSlot >= kSlots) return;
	if (m_ps[uiSlot] == pRes) return;
	if (m_ps[uiSlot]) m_ps[uiSlot]->Release();
	m_ps[uiSlot] = pRes;
	if (pRes) pRes->AddRef();
}
void dx12ShaderResourceStateCache::SetVSResource(UINT uiSlot, ID3DShaderResourceView* pRes)
{
	if (uiSlot >= kSlots) return;
	if (m_vs[uiSlot] == pRes) return;
	if (m_vs[uiSlot]) m_vs[uiSlot]->Release();
	m_vs[uiSlot] = pRes;
	if (pRes) pRes->AddRef();
}
void dx12ShaderResourceStateCache::SetGSResource(UINT uiSlot, ID3DShaderResourceView* pRes)
{
	if (uiSlot >= kSlots) return;
	if (m_gs[uiSlot] == pRes) return;
	if (m_gs[uiSlot]) m_gs[uiSlot]->Release();
	m_gs[uiSlot] = pRes;
	if (pRes) pRes->AddRef();
}
void dx12ShaderResourceStateCache::SetHSResource(UINT uiSlot, ID3DShaderResourceView* pRes)
{
	if (uiSlot >= kSlots) return;
	if (m_hs[uiSlot] == pRes) return;
	if (m_hs[uiSlot]) m_hs[uiSlot]->Release();
	m_hs[uiSlot] = pRes;
	if (pRes) pRes->AddRef();
}
void dx12ShaderResourceStateCache::SetDSResource(UINT uiSlot, ID3DShaderResourceView* pRes)
{
	if (uiSlot >= kSlots) return;
	if (m_ds[uiSlot] == pRes) return;
	if (m_ds[uiSlot]) m_ds[uiSlot]->Release();
	m_ds[uiSlot] = pRes;
	if (pRes) pRes->AddRef();
}
void dx12ShaderResourceStateCache::SetCSResource(UINT uiSlot, ID3DShaderResourceView* pRes)
{
	if (uiSlot >= kSlots) return;
	if (m_cs[uiSlot] == pRes) return;
	if (m_cs[uiSlot]) m_cs[uiSlot]->Release();
	m_cs[uiSlot] = pRes;
	if (pRes) pRes->AddRef();
}
