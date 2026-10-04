﻿#include "stdafx.h"
#include "../xrRender/r_constants_cache.h"

dx12ConstantBuffer* R_constants::GetCBuffer(R_constant* C, BufferType BType)
{
	if (BType == BT_PixelBuffer)
	{
		int iBufferIndex = (C->destination&RC_dest_pixel_cb_index_mask) >> RC_dest_pixel_cb_index_shift;
		VERIFY(iBufferIndex < CBackend::MaxCBuffers);
		if (!RCache.m_aPixelConstants[iBufferIndex])
		{
			Msg("! DX12: null PS cbuffer slot %d for '%s' (dest=0x%08x)",
				iBufferIndex, C->name.c_str(), C->destination);
			return nullptr;
		}
		return RCache.m_aPixelConstants[iBufferIndex]._get();
	}
	else if (BType == BT_VertexBuffer)
	{
		int iBufferIndex = (C->destination&RC_dest_vertex_cb_index_mask) >> RC_dest_vertex_cb_index_shift;
		VERIFY(iBufferIndex < CBackend::MaxCBuffers);
		if (!RCache.m_aVertexConstants[iBufferIndex])
		{
			Msg("! DX12: null VS cbuffer slot %d for '%s' (dest=0x%08x)",
				iBufferIndex, C->name.c_str(), C->destination);
			return nullptr;
		}
		return RCache.m_aVertexConstants[iBufferIndex]._get();
	}
	else if (BType == BT_GeometryBuffer)
	{
		int iBufferIndex = (C->destination&RC_dest_geometry_cb_index_mask) >> RC_dest_geometry_cb_index_shift;
		VERIFY(iBufferIndex < CBackend::MaxCBuffers);
		if (!RCache.m_aGeometryConstants[iBufferIndex])
		{
			Msg("! DX12: null GS cbuffer slot %d for '%s' (dest=0x%08x)", iBufferIndex, C->name.c_str(), C->destination);
			return nullptr;
		}
		return RCache.m_aGeometryConstants[iBufferIndex]._get();
	}
	else if (BType == BT_HullBuffer)
	{
		int iBufferIndex = (C->destination&RC_dest_hull_cb_index_mask) >> RC_dest_hull_cb_index_shift;
		VERIFY(iBufferIndex < CBackend::MaxCBuffers);
		if (!RCache.m_aHullConstants[iBufferIndex])
		{
			Msg("! DX12: null HS cbuffer slot %d for '%s' (dest=0x%08x)", iBufferIndex, C->name.c_str(), C->destination);
			return nullptr;
		}
		return RCache.m_aHullConstants[iBufferIndex]._get();
	}
	else if (BType == BT_DomainBuffer)
	{
		int iBufferIndex = (C->destination&RC_dest_domain_cb_index_mask) >> RC_dest_domain_cb_index_shift;
		VERIFY(iBufferIndex < CBackend::MaxCBuffers);
		if (!RCache.m_aDomainConstants[iBufferIndex])
		{
			Msg("! DX12: null DS cbuffer slot %d for '%s' (dest=0x%08x)", iBufferIndex, C->name.c_str(), C->destination);
			return nullptr;
		}
		return RCache.m_aDomainConstants[iBufferIndex]._get();
	}
	else if (BType == BT_Compute)
	{
		int iBufferIndex = (C->destination&RC_dest_compute_cb_index_mask) >> RC_dest_compute_cb_index_shift;
		VERIFY(iBufferIndex < CBackend::MaxCBuffers);
		if (!RCache.m_aComputeConstants[iBufferIndex])
		{
			Msg("! DX12: null CS cbuffer slot %d for '%s' (dest=0x%08x)", iBufferIndex, C->name.c_str(), C->destination);
			return nullptr;
		}
		return RCache.m_aComputeConstants[iBufferIndex]._get();
	}

	FATAL("Unreachable code");
	return nullptr;
}

void R_constants::flush_cache()
{
	for (int i = 0; i < CBackend::MaxCBuffers; ++i)
	{
		if (RCache.m_aVertexConstants[i])	RCache.m_aVertexConstants[i]->Flush();
		if (RCache.m_aPixelConstants[i])	RCache.m_aPixelConstants[i]->Flush();
		if (RCache.m_aGeometryConstants[i])	RCache.m_aGeometryConstants[i]->Flush();
		if (RCache.m_aHullConstants[i])		RCache.m_aHullConstants[i]->Flush();
		if (RCache.m_aDomainConstants[i])	RCache.m_aDomainConstants[i]->Flush();
		if (RCache.m_aComputeConstants[i])	RCache.m_aComputeConstants[i]->Flush();
	}
}
