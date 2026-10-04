#include "stdafx.h"
#include "dx12SamplerStateCache.h"
#include "../dx12Types.h"

dx12SamplerStateCache	SSManager;

dx12SamplerStateCache::dx12SamplerStateCache() {}
dx12SamplerStateCache::~dx12SamplerStateCache() {}

dx12SamplerState* dx12SamplerStateCache::GetState(const D3D_SAMPLER_DESC& desc)
{
	dx12SamplerState* p = new dx12SamplerState();
	p->desc = desc;
	return p;
}
