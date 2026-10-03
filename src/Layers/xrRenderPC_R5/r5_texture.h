#pragma once

// M4c: DX12 纹理加载（DirectXTex DDS -> DX12 纹理 + SRV）
// M6: 通用纹理缓存（多纹理去重加载，首帧统一上传）

#include <d3d12.h>

namespace r5_texture
{
	bool Init();
	void Shutdown();

	// 首帧调用：在打开的命令列表上记录全部纹理的 CopyTextureRegion + 屏障
	// Init 阶段命令列表处于关闭状态，GPU 命令会被丢弃，必须延迟到首帧
	void UploadFirstFrame(ID3D12GraphicsCommandList* cmd);

	// 测试纹理的 SRV（GPU 描述符句柄，Init 阶段固定分配）
	D3D12_GPU_DESCRIPTOR_HANDLE GetTestSRV();
	// CPU-only 持久 SRV 源（DrawCube 每帧 CopyDescriptorsSimple 到 GPU 堆）
	D3D12_CPU_DESCRIPTOR_HANDLE GetTestSRV_CPU();

	// M6: 通用纹理加载（按相对 gamedata\textures 的路径，自动补 .dds，缓存去重）
	// 返回 GPU 堆中的 SRV 句柄（供 SetGraphicsRootDescriptorTable）；失败返回 {}
	D3D12_GPU_DESCRIPTOR_HANDLE Load(const char* relPath);
}
