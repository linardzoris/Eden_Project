#include "stdafx.h"
#include "r5_visual.h"
#include "r5_resources.h"
#include "r5_texture.h"

#include <d3d12.h>

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
// 便捷命名空间别名（R5 无 d3d9.h，本地 FVF 约定值见头文件）
// ---------------------------------------------------------------------------

// 计算 OGF 顶点步长
// 标准 D3DFVF 位（XYZ/NORMAL/DIFFUSE/SPECULAR/TEXn）或骨架 sentinel（OGF_VERTEXFORMAT_FVF_*L）
static UINT ComputeVertexStride(u32 fvf)
{
	// 骨架网格 sentinel / 旧式编码（pack(2) 的 vertBoned*W 布局）
	switch (fvf)
	{
	case 1:  return 60;	// 旧式 1-Link（引擎 OGF_VERTEXFORMAT_FVF_1L 的兼容 case）
	case 2:  return 64;	// 旧式 2-Link
	case 0x12071980: return 60;	// OGF_VERTEXFORMAT_FVF_1L: vertBoned1W
	case 0x240E3300: return 64;	// OGF_VERTEXFORMAT_FVF_2L: vertBoned2W
	case 0x481CC600: return 70;	// OGF_VERTEXFORMAT_FVF_3L: vertBoned3W
	case 0x5A2B1F80: return 76;	// OGF_VERTEXFORMAT_FVF_4L: vertBoned4W
	case 0x36154C80: return 64;	// OGF_VERTEXFORMAT_FVF_NL（同 2L）
	default: break;
	}

	// 标准 FVF 位
	UINT s = 0;
	const u32 posMask = fvf & 0x00e; // D3DFVF_POSITION_MASK
	switch (posMask)
	{
	case R5_FVF_XYZ: s += 12; break;
	default: return 0; // 不支持
	}
	if (fvf & R5_FVF_NORMAL)  s += 12;
	if (fvf & R5_FVF_DIFFUSE) s += 4;
	if (fvf & R5_FVF_SPECULAR) s += 4;
	UINT nTex = (fvf & R5_FVF_TEXMASK) >> R5_FVF_TEXSHIFT;
	s += nTex * 8; // TEXn=FLOAT2
	return s;
}

// ---------------------------------------------------------------------------
// 收集一段 OGF 流（chunk 流）中的首个几何（VERTICES+INDICES）
// 递归可用于 OGF_CHILDREN 的子网格
// ---------------------------------------------------------------------------

static UINT ComputeVertexStride(u32 fvf); // fwd

// 各 FVF 格式中 POSITION/NORMAL 的字节偏移（pack(2) vertBoned*W 布局）
// 各 FVF 格式中 POSITION/NORMAL/UV 的字节偏移（pack(2) vertBoned*W 布局）
// 返回 uvOff；hasUv=false 表示无 UV（标准 FVF 无 TEX 时）
static void GetPosNrmUvOffset(u32 fvf, UINT& posOff, UINT& nrmOff, UINT& uvOff, bool& hasNrm, bool& hasUv)
{
	hasNrm = true;
	hasUv = true;
	switch (fvf)
	{
	case 1:  case 0x12071980: posOff = 0;  nrmOff = 12; uvOff = 48; break;	// vertBoned1W: P@0 N@12 uv@48
	case 2:  case 0x240E3300:
	case 0x36154C80:           posOff = 4;  nrmOff = 16; uvOff = 56; break;	// vertBoned2W: m[2]@0 P@4 N@16 uv@56
	case 0x481CC600:           posOff = 6;  nrmOff = 18; uvOff = 62; break;	// vertBoned3W: m[3]@0 P@6 N@18 uv@62
	case 0x5A2B1F80:           posOff = 8;  nrmOff = 20; uvOff = 68; break;	// vertBoned4W: m[4]@0 P@8 N@20 uv@68
	default:
	{
		// 标准 FVF：XYZ[+NORMAL[+DIFFUSE[+SPECULAR]]][+TEX0..n]
		posOff = 0;
		UINT o = 12; // XYZ
		if (fvf & R5_FVF_NORMAL)  { nrmOff = o; o += 12; } else { nrmOff = o; hasNrm = false; }
		if (fvf & R5_FVF_DIFFUSE) o += 4;
		if (fvf & R5_FVF_SPECULAR) o += 4;
		UINT nTex = (fvf & R5_FVF_TEXMASK) >> R5_FVF_TEXSHIFT;
		if (nTex > 0) { uvOff = o; } else { uvOff = 0; hasUv = false; }
		break;
	}
	}
}

// 单个已解析子网格（GEOMDEF 流，规范化 verts: pos + nrm + uv = 32B）
struct CollectedMesh
{
	UINT vCount = 0;
	UINT iCount = 0;
	UINT stride = 32;	// 规范化步长
	xr_vector<u8> verts;	// 32B/顶点：pos@0, nrm@12, uv@24
	xr_vector<u8> inds;		// 索引字节（u16）
	string_path texName;	// M6: 子网格贴图名（OGF_TEXTURE 首个 Z-string），空表示无
	bool ok = false;
};

// 解析一段嵌套 OGF 流（GEOMDEF 引用的实际网格 chunk）中的几何
static void CollectGeometry(const u8* p, size_t size, CollectedMesh& out)
{
	size_t o = 0;
	while (o + 8 <= size)
	{
		u32 id = *(const u32*)(p + o);
		u32 sz = *(const u32*)(p + o + 4);
		if (o + 8 + sz > size)
			break;
		const u8* d = p + o + 8;

		if (id == OGF_VERTICES && sz >= 8 && out.verts.empty())
		{
			u32 fvfTmp = *(const u32*)d;
			u32 vc = *(const u32*)(d + 4);
			UINT srcStride = ComputeVertexStride(fvfTmp);
			if (srcStride && 8u + (u64)vc * srcStride <= sz)
			{
				UINT posOff, nrmOff, uvOff;
				bool hasNrm, hasUv;
				GetPosNrmUvOffset(fvfTmp, posOff, nrmOff, uvOff, hasNrm, hasUv);

				out.vCount = vc;
				out.stride = 32;
				out.verts.resize((size_t)vc * 32);
				const u8* s = d + 8;
				for (u32 i = 0; i < vc; ++i)
				{
					const u8* sv = s + (size_t)i * srcStride;
					u8* dv = out.verts.data() + (size_t)i * 32;
					memcpy(dv, sv + posOff, 12);
					if (hasNrm)
						memcpy(dv + 12, sv + nrmOff, 12);
					else
					{
						dv[12] = 0; dv[13] = 0; dv[14] = 0x3F; dv[15] = 0;
						memset(dv + 16, 0, 8);
					}
					if (hasUv)
						memcpy(dv + 24, sv + uvOff, 8);
					else
						memset(dv + 24, 0, 8);
				}
			}
		}
		else if (id == OGF_INDICES && sz >= 4 && out.inds.empty())
		{
			u32 ic = *(const u32*)d;
			if (4u + (u64)ic * 2 <= sz)
			{
				out.iCount = ic;
				out.inds.assign(d + 4, d + 4 + (size_t)ic * 2);
			}
		}
		else if (id == OGF_TEXTURE && sz >= 1 && out.texName[0] == 0)
		{
			// OGF_TEXTURE: Z-string 贴图名 + Z-string shader 名
			size_t n = 0;
			while (n < sz && d[n] != 0 && n < sizeof(out.texName) - 1)
				++n;
			if (n > 0)
			{
				memcpy(out.texName, d, n);
				out.texName[n] = 0;
			}
		}
		o += 8 + sz;
	}
	if (!out.verts.empty() && !out.inds.empty())
		out.ok = true;
}

R5Visual::R5Visual()
{
}

R5Visual::~R5Visual()
{
}

bool R5Visual::Load(const char* name)
{
	// 解析 gamedata\meshes\<name>.ogf（cwd = 游戏根目录）
	string_path fullPath;
	xr_strconcat(fullPath, "gamedata\\meshes\\", name, ".ogf");

	FILE* f = fopen(fullPath, "rb");
	if (!f)
	{
		Msg("! R5Visual: open %s failed", fullPath);
		return false;
	}

	// 读入整个文件（堆分配，避免大栈帧导致启动期栈溢出）
	fseek(f, 0, SEEK_END);
	long fsize = ftell(f);
	fseek(f, 0, SEEK_SET);
	xr_vector<u8> fileData(fsize);
	if (fsize > 0 && fread(fileData.data(), 1, fsize, f) != (size_t)fsize)
	{
		Msg("! R5Visual: read %s failed", fullPath);
		fclose(f);
		return false;
	}
	fclose(f);

	const u8* base = fileData.data();
	size_t total = fileData.size();

	// 顶层 chunk 流：读 header + 尝试直接收集几何
	{
		size_t o = 0;
		while (o + 8 <= total)
		{
			u32 id = *(const u32*)(base + o);
			u32 sz = *(const u32*)(base + o + 4);
			if (o + 8 + sz > total)
				break;
			const u8* d = base + o + 8;

			if (id == OGF_HEADER && sz >= sizeof(ogf_header))
			{
				ogf_header hdr;
				memcpy(&hdr, d, sizeof(hdr));
				m_type = hdr.type;
				m_vis.box.set(hdr.bb.min, hdr.bb.max);
				m_vis.sphere.set(hdr.bs.c, hdr.bs.r);
			}
			o += 8 + sz;
		}
	}
	// 顶层静态网格（MT_NORMAL）：几何直接在顶层
	CollectedMesh topMesh;
	topMesh.texName[0] = 0;	// string_path 是栈数组，必须先清空
	CollectGeometry(base, total, topMesh);
	if (topMesh.ok)
	{
		D3D12_GPU_VIRTUAL_ADDRESS vAddr, iAddr;
		void* vdst = r5_res::g_upload.Alloc((u64)topMesh.vCount * topMesh.stride, vAddr);
		memcpy(vdst, topMesh.verts.data(), (size_t)topMesh.vCount * topMesh.stride);
		void* idst = r5_res::g_upload.Alloc((u64)topMesh.iCount * 2, iAddr);
		memcpy(idst, topMesh.inds.data(), (size_t)topMesh.iCount * 2);
		D3D12_GPU_DESCRIPTOR_HANDLE tex = r5_texture::Load(topMesh.texName[0] ? topMesh.texName : "$shadertest");
		m_meshes.push_back({ vAddr, iAddr, topMesh.stride, topMesh.vCount, topMesh.iCount, tex });
	}

	// 骨架网格（MT_SKELETON_RIGID=10 / MT_SKELETON_ANIM=3 / GEOMDEF）：几何在 OGF_CHILDREN 子网格
	// 每个子网格可能是不同顶点格式（1W/2W/3W/4W，stride 不同），各自作为独立 submesh
	{
		size_t o = 0;
		while (o + 8 <= total)
		{
			u32 id = *(const u32*)(base + o);
			u32 sz = *(const u32*)(base + o + 4);
			if (o + 8 + sz > total)
				break;
			const u8* d = base + o + 8;
			if (id == OGF_CHILDREN)
			{
				size_t c = 0;
				while (c + 8 <= sz)
				{
					u32 subSz = *(const u32*)(d + c + 4);
					if (c + 8 + subSz > sz)
						break;
					CollectedMesh sub;
					sub.texName[0] = 0;	// string_path 是栈数组，必须先清空（否则复用旧子网格的贴图名）
					CollectGeometry(d + c + 8, subSz, sub);
					if (sub.ok)
					{
						// 上传该子网格到 upload ring 持久区
						D3D12_GPU_VIRTUAL_ADDRESS vAddr, iAddr;
						void* vdst = r5_res::g_upload.Alloc((u64)sub.vCount * sub.stride, vAddr);
						memcpy(vdst, sub.verts.data(), (size_t)sub.vCount * sub.stride);
						void* idst = r5_res::g_upload.Alloc((u64)sub.iCount * 2, iAddr);
						memcpy(idst, sub.inds.data(), (size_t)sub.iCount * 2);

						// M6: 加载子网格贴图（缺失/加载失败回退测试纹理）
						D3D12_GPU_DESCRIPTOR_HANDLE tex = {};
						if (sub.texName[0])
							tex = r5_texture::Load(sub.texName);
						if (tex.ptr == 0)
							tex = r5_texture::GetTestSRV();

						m_meshes.push_back({ vAddr, iAddr, sub.stride, sub.vCount, sub.iCount, tex });
					}
					c += 8 + subSz;
				}
			}
			o += 8 + sz;
		}
	}

	if (m_meshes.empty())
	{
		Msg("! R5Visual: %s parse incomplete (no geometry)", fullPath);
		return false;
	}

	m_name = name;
	for (size_t i = 0; i < m_meshes.size(); ++i)
		Msg("  R5 submesh[%u]: v=%u i=%u stride=%u", (u32)i, m_meshes[i].vCount, m_meshes[i].iCount, m_meshes[i].stride);
	Msg("* R5Visual: %s loaded (submeshes=%u type=%u)", name, (u32)m_meshes.size(), m_type);
	return true;
}

// ---------------------------------------------------------------------------
// 线程安全收集
// ---------------------------------------------------------------------------

namespace r5_visual
{
	static xrCriticalSection s_lock;
	static xr_vector<R5Visual*> s_visuals;

	void AddVisual(R5Visual* v)
	{
		xrCriticalSectionGuard g(s_lock);
		s_visuals.push_back(v);
	}

	void RemoveVisual(R5Visual* v)
	{
		xrCriticalSectionGuard g(s_lock);
		for (size_t i = 0; i < s_visuals.size(); ++i)
			if (s_visuals[i] == v) { s_visuals.erase(s_visuals.begin() + long(i)); break; }
	}

	// 渲染循环读取快照（R5 单线程渲染期调用，无需外部锁）
	size_t LockAndSnapshot(xr_vector<R5Visual*>& out)
	{
		xrCriticalSectionGuard g(s_lock);
		out.assign(s_visuals.begin(), s_visuals.end());
		return out.size();
	}
}