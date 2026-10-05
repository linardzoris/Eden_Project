#include "stdafx.h"
#include "../xrRender/Debug/dxGPUEvents.h"
#include "../xrRender/Debug/dxGPUEventWrapper.h"
#include "dx12Backend.h"

// DX12 阶段：GPU 事件/标注走 no-op（后续可接 ID3D12GraphicsCommandList 的 BeginEvent/EndEvent）。
#ifdef USE_DX12

static gpu_events_perf s_emptyPerf = {};

const gpu_events_perf& GPUEvents_Statistics()
{
	return s_emptyPerf;
}

void GPUEvents_BeginRendering() {}
void GPUEvents_EndRendering() {}

#ifdef DEBUG_DRAW
// 每个 GPU_EVENT 同时写一条 GPU 面包屑（WriteBufferImmediate → READBACK 缓冲）。
// 设备挂起（TDR / 死循环 / 超大派发）时 GBV 帮不上忙，但面包屑能告诉我们是
// "GPU 执行到哪个阶段"卡住的 —— 阶段名即 GPU_EVENT 的参数名。
GPUEventWrapper::GPUEventWrapper(const char* name, const wchar_t*)
{
	dx12::WriteBreadcrumb(dx12::RegisterBreadcrumbName(name));
}
GPUEventWrapper::~GPUEventWrapper() {}
#endif

#endif // USE_DX12
