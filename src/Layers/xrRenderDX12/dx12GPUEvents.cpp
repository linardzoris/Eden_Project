#include "stdafx.h"
#include "../xrRender/Debug/dxGPUEvents.h"
#include "../xrRender/Debug/dxGPUEventWrapper.h"

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
GPUEventWrapper::GPUEventWrapper(const char*, const wchar_t*) {}
GPUEventWrapper::~GPUEventWrapper() {}
#endif

#endif // USE_DX12
