#include "stdafx.h"
#include "r5_level.h"
#include "r5_resources.h"
#include "r5_texture.h"
#include "../../xrCore/stream_reader.h"	// CStreamReader
#include "../../xrEngine/xrLevel.h"		// fsL_VB / fsL_IB / fsL_VISUALS / fsL_SHADERS

#include <d3d12.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
// level.geom / level 二进制布局（与 R4 r4_loader.cpp 一致）
// level.geom: chunk 流 [u32 id][u32 size][data]
//   fsL_VB(9):   u32 count; 每条: [D3DVERTEXELEMENT9 元素...(Stream==0xFF 终止)] + u32 vCount + 顶点数据
//   fsL_IB(10):  u32 count; 每条: u32 iCount + iCount*2 u16 索引
// level:        chunk 流 [u32 id][u32 size][data]
//   fsL_VISUALS(3): 嵌套子 chunk(index 0,1,2...)，每个是 OGF 流：OGF_HEADER + OGF_GCONTAINER
// ---------------------------------------------------------------------------

// D3DVERTEXELEMENT9 的本地等价（8B），避免依赖 d3d9.h
struct R5DeclElem
{
	u16 Stream;
	u16 Offset;
	u8 Type;
	u8 Method;
	u8 Usage;
	u8 UsageIndex;
};

// D3DDECL_END：Stream == 0xFF 终止
static bool IsDeclEnd(const R5DeclElem& e)
{
	return e.Stream == 0xFF;
}

// Type -> 字节大小（D3DDECLTYPE 完整映射，M8 修正：SHORT4/SHORT4N/USHORT4N/FLOAT16_4 为 8B）
static UINT TypeSize(u8 type)
{
	switch (type)
	{
	case 0: return 4;  // FLOAT1
	case 1: return 8;  // FLOAT2
	case 2: return 12; // FLOAT3
	case 3: return 16; // FLOAT4
	case 4: return 4;  // D3DCOLOR
	case 5: return 4;  // UBYTE4
	case 6: return 4;  // SHORT2
	case 7: return 8;  // SHORT4
	case 8: return 4;  // UBYTE4N
	case 9: return 4;  // SHORT2N
	case 10: return 8; // SHORT4N
	case 11: return 4; // USHORT2N
	case 12: return 8; // USHORT4N
	case 13: return 4; // UDEC3
	case 14: return 4; // DEC3N
	case 15: return 4; // FLOAT16_2
	case 16: return 8; // FLOAT16_4
	default: return 4;
	}
}

// 半精度浮点 -> 单精度
static float HalfToFloat(u16 h)
{
	u32 sign = (h >> 15) & 1;
	u32 exp = (h >> 10) & 0x1F;
	u32 man = h & 0x3FF;
	u32 f;
	if (exp == 0)
	{
		if (man == 0) f = sign << 31;
		else
		{
			exp = 127 - 15 + 1;
			while (!(man & 0x400)) { man <<= 1; exp--; }
			man &= 0x3FF;
			f = (sign << 31) | (exp << 23) | (man << 13);
		}
	}
	else if (exp == 31)
		f = (sign << 31) | (0xFF << 23) | (man << 13);
	else
		f = (sign << 31) | ((exp - 15 + 127) << 23) | (man << 13);
	float r;
	memcpy(&r, &f, 4);
	return r;
}

// 扫描声明器，求：总步长 + POSITION/NORMAL/TEXCOORD0 偏移
// nElem 为声明元素个数（不含终止元素），步长 = 各元素 TypeSize 之和
// nrmType/uvType 返回 NORMAL / TEXCOORD0 元素的 D3DDECLTYPE（用于按格式解码，0xFF=无）
// tanOff/binOff 为 TANGENT/BINORMAL 偏移（D3DCOLOR，其 alpha 是 UV tiling 偏移 du/dv，M8 修正）
static void AnalyzeDecl(const R5DeclElem* dcl, UINT nElem, UINT& stride, UINT& posOff, UINT& nrmOff, UINT& uvOff,
	bool& hasNrm, bool& hasUv, UINT& nrmType, UINT& uvType,
	UINT& tanOff, UINT& binOff, bool& hasTan, bool& hasBin)
{
	stride = 0; posOff = 0; nrmOff = 0; uvOff = 0;
	hasNrm = false; hasUv = false; nrmType = 0xFF; uvType = 0xFF;
	tanOff = 0; binOff = 0; hasTan = false; hasBin = false;
	for (UINT i = 0; i < nElem; ++i)
	{
		const R5DeclElem& e = dcl[i];
		UINT sz = TypeSize(e.Type);
		if (e.Usage == 0 && e.UsageIndex == 0) posOff = e.Offset;       // POSITION
		else if (e.Usage == 3 && e.UsageIndex == 0) { nrmOff = e.Offset; hasNrm = true; nrmType = e.Type; } // NORMAL
		else if (e.Usage == 5 && e.UsageIndex == 0) { uvOff = e.Offset; hasUv = true; uvType = e.Type; }   // TEXCOORD0
		else if (e.Usage == 6 && e.UsageIndex == 0) { tanOff = e.Offset; hasTan = true; }                  // TANGENT
		else if (e.Usage == 7 && e.UsageIndex == 0) { binOff = e.Offset; hasBin = true; }                  // BINORMAL
		stride += sz;
	}
}

// ---------------------------------------------------------------------------
// level.geom -> 共享 VB/IB 池（规范化上传）
// ---------------------------------------------------------------------------

static bool LoadGeomVB(const u8* data, size_t size, R5LevelWorld& world)
{
	size_t o = 0;
	if (o + 4 > size) return false;
	u32 count = *(const u32*)(data + o); o += 4;

	for (u32 i = 0; i < count; ++i)
	{
		// M8 修正：decl 为可变长元素列表（[u8*8 元素 ...][Stream==0xFF 终止]），
		// vCount 紧跟终止元素之后，不能固定跳 520 字节
		const u8* dclBase = data + o;
		UINT nElem = 0;
		while (nElem < 65)
		{
			if (o + 8 > size) return false;
			R5DeclElem e;
			memcpy(&e, data + o, sizeof(R5DeclElem));
			if (IsDeclEnd(e)) break;
			++nElem;
			o += 8;
		}
		if (nElem == 0) return false;
		o += 8;	// 跳过终止元素

		UINT stride, posOff, nrmOff, uvOff;
		bool hasNrm, hasUv;
		UINT nrmType, uvType;
		UINT tanOff, binOff;
		bool hasTan, hasBin;
		AnalyzeDecl((const R5DeclElem*)dclBase, nElem, stride, posOff, nrmOff, uvOff, hasNrm, hasUv, nrmType, uvType,
			tanOff, binOff, hasTan, hasBin);
		if (stride == 0) return false;

		if (o + 4 > size) return false;
		u32 vCount = *(const u32*)(data + o); o += 4;
		if (o + (u64)vCount * stride > size) return false;
		const u8* vdata = data + o; o += (u64)vCount * stride;

		// 规范化为 32B {pos, nrm, uv}，上传 upload ring 持久区
		// M8 修正：UV/法线按 D3DDECLTYPE 实际格式解码（SHORT 类 /32768，D3DCOLOR 打包，(v/255)*2-1）
		R5GeomVB gvb;
		D3D12_GPU_VIRTUAL_ADDRESS addr;
		void* dst = r5_res::g_upload.Alloc((u64)vCount * 32, addr);
		gvb.addr = addr;
		gvb.vCount = vCount;

		for (u32 v = 0; v < vCount; ++v)
		{
			const u8* sv = vdata + (u64)v * stride;
			u8* dv = (u8*)dst + (u64)v * 32;
			memcpy(dv, sv + posOff, 12);

			float* nrm = (float*)(dv + 12);
			if (hasNrm && nrmType == 4)	// D3DCOLOR 打包法线 -> (v/255)*2-1
			{
				const u8* c = sv + nrmOff;
				nrm[0] = (c[0] / 255.0f) * 2.0f - 1.0f;
				nrm[1] = (c[1] / 255.0f) * 2.0f - 1.0f;
				nrm[2] = (c[2] / 255.0f) * 2.0f - 1.0f;
			}
			else if (hasNrm && nrmType == 10)	// SHORT4N -> /32767
			{
				const s16* s = (const s16*)(sv + nrmOff);
				nrm[0] = s[0] / 32767.0f; nrm[1] = s[1] / 32767.0f; nrm[2] = s[2] / 32767.0f;
			}
			else if (hasNrm)	// FLOAT3 等 -> 直接拷贝
			{
				memcpy(dv + 12, sv + nrmOff, 12);
			}
			else
			{
				dv[12] = 0; dv[13] = 0; dv[14] = 0x3F; dv[15] = 0; memset(dv + 16, 0, 8);
			}

			float* uv = (float*)(dv + 24);
			if (hasUv)
			{
				if (uvType == 1) memcpy(dv + 24, sv + uvOff, 8);			// FLOAT2
				else if (uvType == 6 || uvType == 7)						// SHORT2 / SHORT4（取前 2 个）
				{
					// CoP 静态物体 UV = (tc + (T.a,B.a)) * (32/32768)（unpack_tc_base：
					// tc 为 s16，TANGENT/BINORMAL D3DCOLOR 的 alpha 是 tiling 偏移 du/dv。
					// 证据 gamedata\shaders\common_functions.hlsli: unpack_tc_base）
					const s16* s = (const s16*)(sv + uvOff);
					float du = 0.f, dv = 0.f;
					if (hasTan && tanOff + 4 <= stride) du = sv[tanOff + 3] / 255.0f;
					if (hasBin && binOff + 4 <= stride) dv = sv[binOff + 3] / 255.0f;
					uv[0] = (s[0] + du) * (32.0f / 32768.0f);
					uv[1] = (s[1] + dv) * (32.0f / 32768.0f);
				}
				else if (uvType == 10)										// SHORT4N
				{
					const s16* s = (const s16*)(sv + uvOff);
					uv[0] = s[0] / 32767.0f; uv[1] = s[1] / 32767.0f;
				}
				else if (uvType == 11)										// USHORT2N
				{
					const u16* s = (const u16*)(sv + uvOff);
					uv[0] = s[0] / 65535.0f; uv[1] = s[1] / 65535.0f;
				}
				else if (uvType == 15)										// FLOAT16_2
				{
					const u16* h = (const u16*)(sv + uvOff);
					uv[0] = HalfToFloat(h[0]); uv[1] = HalfToFloat(h[1]);
				}
				else if (uvType == 4)										// D3DCOLOR -> RGBA 字节 /255
				{
					const u8* c = sv + uvOff;
					uv[0] = c[0] / 255.0f; uv[1] = c[1] / 255.0f;
				}
				else { uv[0] = 0.0f; uv[1] = 0.0f; }
			}
			else { uv[0] = 0.0f; uv[1] = 0.0f; }

			// M8: 跟踪世界包围盒（直接加载时轨道相机用）
			const float* p = (const float*)sv + posOff / 4;
			Fvector pv;
			pv.set(p[0], p[1], p[2]);
			world.aabb.modify(pv);
		}
		world.vbs.push_back(gvb);
		Msg("  R5 level VB[%u]: v=%u stride=%u%s%s nrmT=%u uvT=%u", i, vCount, stride, hasNrm ? "" : " noNrm", hasUv ? "" : " noUv", nrmType, uvType);
	}
	return true;
}

static bool LoadGeomIB(const u8* data, size_t size, R5LevelWorld& world)
{
	size_t o = 0;
	if (o + 4 > size) return false;
	u32 count = *(const u32*)(data + o); o += 4;

	for (u32 i = 0; i < count; ++i)
	{
		if (o + 4 > size) return false;
		u32 iCount = *(const u32*)(data + o); o += 4;
		if (o + (u64)iCount * 2 > size) return false;

		R5GeomIB gib;
		D3D12_GPU_VIRTUAL_ADDRESS addr;
		void* dst = r5_res::g_upload.Alloc((u64)iCount * 2, addr);
		memcpy(dst, data + o, (u64)iCount * 2);
		o += (u64)iCount * 2;

		gib.addr = addr;
		gib.iCount = iCount;
		world.ibs.push_back(gib);
	}
	return true;
}

// ---------------------------------------------------------------------------
// level -> SHADERS（fsL_SHADERS: u32 count + stringZ 条目）
// 条目格式：'blender名/主贴图,lmap1,lmap2...'，主贴图 = '/' 后到逗号前的部分
// M8: terrain 贴图（以 'terrain\' 开头）实际在 levels\<map>\terrain\ 下（用户确认），
// mapName 非空时转换路径为 'levels\<map>\terrain\<rest>'
// ---------------------------------------------------------------------------

static void ParseShaderNames(const u8* data, size_t size, R5LevelWorld& world, const char* mapName = nullptr)
{
	size_t o = 0;
	if (o + 4 > size) return;
	u32 count = *(const u32*)(data + o); o += 4;
	world.shaderNames.reserve(count);
	for (u32 i = 0; i < count; ++i)
	{
		if (o >= size) break;
		const char* s = (const char*)(data + o);
		size_t len = 0;
		while (o + len < size && data[o + len] != 0) ++len;
		o += len + 1;

		string_path tex = "";
		if (len > 0)
		{
			const char* slash = strchr(s, '/');
			if (slash)
			{
				slash += 1;
				const char* comma = strchr(slash, ',');
				size_t tlen = comma ? (size_t)(comma - slash) : xr_strlen(slash);
				if (tlen >= sizeof(string_path)) tlen = sizeof(string_path) - 1;
				memcpy(tex, slash, tlen);
				tex[tlen] = 0;

				// terrain base/lmap 在 levels\<map>\terrain\ 下
				if (mapName && tlen > 8 && _strnicmp(tex, "terrain\\", 8) == 0)
				{
					char mapped[MAX_PATH];
					xr_strconcat(mapped, "levels\\", mapName, "\\", tex);
					xr_strcpy(tex, mapped);
				}
			}
			else
			{
				// 无 '/'：整串作为贴图名（罕见，尽力而为）
				size_t tlen = len;
				if (tlen >= sizeof(string_path)) tlen = sizeof(string_path) - 1;
				memcpy(tex, s, tlen);
				tex[tlen] = 0;
			}
		}
		world.shaderNames.emplace_back(tex);
	}
	Msg("  R5 level shaders: %u names parsed", count);
}

// ---------------------------------------------------------------------------
// level -> VISUALS（OGF_HEADER + OGF_GCONTAINER）
// ---------------------------------------------------------------------------

static void ParseVisual(const u8* p, size_t size, R5LevelWorld& world)
{
	// 注意：visual 流中 OGF_HEADER(1) 位于 GCONTAINER(21) 之后！
	// 必须先扫 header 拿 shader_id，再收集 GCONTAINER（单遍扫描会把 shader 存成 0xFFFFFFFF）
	u32 shaderId = 0xFFFFFFFF;
	{
		size_t o = 0;
		while (o + 8 <= size)
		{
			u32 id = *(const u32*)(p + o);
			u32 sz = *(const u32*)(p + o + 4);
			if (o + 8 + sz > size) break;
			if (id == OGF_HEADER && sz >= 4)
			{
				// ogf_header: format_version(1B) + type(1B) + shader_id(u16)
				shaderId = *(const u16*)(p + o + 10);
				break;
			}
			o += 8 + sz;
		}
	}
	{
		size_t o = 0;
		while (o + 8 <= size)
		{
			u32 id = *(const u32*)(p + o);
			u32 sz = *(const u32*)(p + o + 4);
			if (o + 8 + sz > size) break;
			const u8* d = p + o + 8;

			if (id == OGF_GCONTAINER && sz >= 24)
			{
				u32 vbID = *(const u32*)(d + 0);
				u32 vBase = *(const u32*)(d + 4);
				u32 vCount = *(const u32*)(d + 8);
				u32 ibID = *(const u32*)(d + 12);
				u32 iBase = *(const u32*)(d + 16);
				u32 iCount = *(const u32*)(d + 20);

				if (vbID < world.vbs.size() && ibID < world.ibs.size() &&
					vBase + vCount <= world.vbs[vbID].vCount && iBase + iCount <= world.ibs[ibID].iCount)
				{
					R5StaticMesh m;
					m.vb = vbID; m.vBase = vBase; m.vCount = vCount;
					m.ib = ibID; m.iBase = iBase; m.iCount = iCount;
					m.shader = shaderId;

					// M8: MT_PROGRESSIVE 的 OGF_SWIDATA(6)：IB 内串联多个 LOD，
					// 必须只画选中的 LOD 窗口（否则所有 LOD 叠加 -> 重叠几何 -> z-fighting 闪烁）。
					// 布局：[u32 reserved×4][u32 count][count × {u32 offset; u16 num_tris; u16 num_verts}]
					size_t so = 0;
					while (so + 8 <= size)
					{
						u32 sid = *(const u32*)(p + so);
						u32 ssz = *(const u32*)(p + so + 4);
						if (so + 8 + ssz > size) break;
						if (sid == OGF_SWIDATA && ssz >= 20 + 8)
						{
							const u8* s = p + so + 8;
							u32 cnt = *(const u32*)(s + 16);
							if (cnt > 0 && 20 + (u64)cnt * 8 <= ssz)
							{
								// 仿 FProgressive::Render：LOD=0（最近）-> lod_id = count-1（最精细）
								u32 lodId = cnt - 1;
								const u8* sw = s + 20 + (u64)lodId * 8;
								u32 off = *(const u32*)(sw + 0);
								u16 numTris = *(const u16*)(sw + 4);
								// 只接受落在 IB 范围内的窗口
								if (iBase + off + (u64)numTris * 3 <= world.ibs[ibID].iCount)
								{
									m.lodIndexOffset = off;
									m.lodIndices = (UINT)numTris * 3;
								}
							}
							break;
						}
						so += 8 + ssz;
					}

					world.meshes.push_back(m);
				}
			}
			o += 8 + sz;
		}
	}
}

// ---------------------------------------------------------------------------
// 入口：level_Load(IReader* fs)
// ---------------------------------------------------------------------------

bool r5_level::Load(IReader* fs, R5LevelWorld& world)
{
	if (world.loaded)
		Clear(world);

	// M8: Fbox 默认构造为全 0（非 invalidate），必须显式 invalidate 才能正确 accumulate
	world.aabb.invalidate();
	world.direct = false;

	// 1. level.geom（$level$ 虚拟路径由引擎 FS 提供）
	{
		CStreamReader* geom = FS.rs_open("$level$", "level.geom");
		if (!geom)
		{
			Msg("! R5 level: level.geom open failed");
			return false;
		}

		// 读取整块 VB / IB chunk 到内存再解析（CStreamReader 逐块）
		for (int pass = 0; pass < 2; ++pass)
		{
			CStreamReader* c = geom->open_chunk(pass == 0 ? 9 : 10);	// fsL_VB=9, fsL_IB=10
			if (!c) continue;
			u32 len = c->length();
			xr_vector<u8> buf(len);
			c->r(buf.data(), len);
			c->close();
			if (pass == 0) LoadGeomVB(buf.data(), len, world);
			else LoadGeomIB(buf.data(), len, world);
		}
		geom->close();
	}

	// 2. level 的 SHADERS（fsL_SHADERS=2）
	{
		IReader* sh = fs->open_chunk(fsL_SHADERS);
		if (sh)
		{
			u32 len = sh->length();
			xr_vector<u8> buf(len);
			sh->r(buf.data(), len);
			ParseShaderNames(buf.data(), len, world);
			sh->close();
		}
	}

	// 3. level 的 VISUALS（fsL_VISUALS=3）
	{
		IReader* vis = fs->open_chunk(3);
		if (vis)
		{
			for (u32 i = 0;; ++i)
			{
				IReader* c = vis->open_chunk(i);
				if (!c) break;
				u32 len = c->length();
				xr_vector<u8> buf(len);
				c->r(buf.data(), len);
				ParseVisual(buf.data(), len, world);
				c->close();
			}
			vis->close();
		}
	}

	if (world.vbs.empty() || world.ibs.empty() || world.meshes.empty())
	{
		Msg("! R5 level: load incomplete (vb=%u ib=%u mesh=%u)",
			(u32)world.vbs.size(), (u32)world.ibs.size(), (u32)world.meshes.size());
		Clear(world);
		return false;
	}

	world.loaded = true;
	Msg("* R5 level: loaded (vb=%u ib=%u meshes=%u)",
		(u32)world.vbs.size(), (u32)world.ibs.size(), (u32)world.meshes.size());
	return true;
}

void r5_level::Clear(R5LevelWorld& world)
{
	world.vbs.clear();
	world.ibs.clear();
	world.meshes.clear();
	world.shaderNames.clear();
	world.shaderSRVs.clear();
	world.aabb.invalidate();
	world.direct = false;
	world.loaded = false;
}

// M8: 预加载全部 shader 主贴图（必须在 create 阶段/首帧前调用：
// g_gpuHeap 每帧 Reset，若渲染期再 Load 新纹理的 SRV 分配会覆盖 G-buffer 槽位）
void r5_level::PreloadTextures(R5LevelWorld& world)
{
	world.shaderSRVs.resize(world.shaderNames.size());
	for (size_t i = 0; i < world.shaderNames.size(); ++i)
	{
		const shared_str& nm = world.shaderNames[i];
		const char* s = nm.c_str();
		if (!s || !s[0])
		{
			world.shaderSRVs[i] = {};
			continue;
		}
		// M8: terrain 贴图路径已转换为 'levels\...'（gamedata\ 相对），走 LoadRoot
		if (_strnicmp(s, "levels\\", 7) == 0)
			world.shaderSRVs[i] = r5_texture::LoadRoot(s);
		else
			world.shaderSRVs[i] = r5_texture::Load(s);
	}
	Msg("* R5 level: preloaded %u shader textures", (u32)world.shaderSRVs.size());
}

// ---------------------------------------------------------------------------
// M8: 直接从磁盘加载地图（绕过游戏/存档/UI）
//   gamedata\levels\<mapName>\level.geom + level
// ---------------------------------------------------------------------------

// 读取整个文件到内存（相对 exe 工作目录 = 游戏根目录）
static bool ReadWholeFile(const char* path, xr_vector<u8>& buf)
{
	FILE* f = nullptr;
	if (fopen_s(&f, path, "rb") != 0 || !f)
		return false;
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf.resize(len > 0 ? (size_t)len : 0);
	if (len > 0 && fread(buf.data(), 1, len, f) != (size_t)len)
	{
		fclose(f);
		buf.clear();
		return false;
	}
	fclose(f);
	return true;
}

// 在 chunk 流 [u32 id][u32 size][data] 中查找指定顶层 chunk，返回数据指针+大小
static const u8* FindChunk(const u8* data, size_t size, u32 id, size_t& outSz)
{
	size_t o = 0;
	while (o + 8 <= size)
	{
		u32 cid = *(const u32*)(data + o);
		u32 csz = *(const u32*)(data + o + 4);
		if (o + 8 + (u64)csz > size)
			break;
		if (cid == id)
		{
			outSz = csz;
			return data + o + 8;
		}
		o += 8 + (u64)csz;
	}
	return nullptr;
}

bool r5_level::LoadDirect(const char* mapName, R5LevelWorld& world)
{
	if (world.loaded)
		Clear(world);

	world.aabb.invalidate();
	world.direct = true;

	string_path path;
	xr_strconcat(path, "gamedata\\levels\\", mapName, "\\level.geom");

	// 1. level.geom：VB(9) + IB(10) 池
	xr_vector<u8> geom;
	if (!ReadWholeFile(path, geom) || geom.empty())
	{
		Msg("! R5 level: cannot open %s", path);
		Clear(world);
		return false;
	}
	{
		size_t vbSz = 0, ibSz = 0;
		const u8* vb = FindChunk(geom.data(), geom.size(), fsL_VB, vbSz);
		const u8* ib = FindChunk(geom.data(), geom.size(), fsL_IB, ibSz);
		if (vb) LoadGeomVB(vb, vbSz, world);
		if (ib) LoadGeomIB(ib, ibSz, world);
	}

	// 2. level：SHADERS(2) + VISUALS(3) 内嵌套子 chunk，每个是 OGF 流
	xr_strconcat(path, "gamedata\\levels\\", mapName, "\\level");
	xr_vector<u8> lvl;
	if (!ReadWholeFile(path, lvl) || lvl.empty())
	{
		Msg("! R5 level: cannot open %s", path);
		Clear(world);
		return false;
	}
	{
		size_t shSz = 0;
		const u8* sh = FindChunk(lvl.data(), lvl.size(), fsL_SHADERS, shSz);
		if (sh) ParseShaderNames(sh, shSz, world, mapName);	// M8: 传 mapName 转换 terrain 路径

		size_t visSz = 0;
		const u8* vis = FindChunk(lvl.data(), lvl.size(), fsL_VISUALS, visSz);
		if (vis)
		{
			size_t o = 0;
			while (o + 8 <= visSz)
			{
				u32 idx = *(const u32*)(vis + o);
				u32 csz = *(const u32*)(vis + o + 4);
				if (o + 8 + (u64)csz > visSz)
					break;
				ParseVisual(vis + o + 8, csz, world);
				o += 8 + (u64)csz;
			}
		}
	}

	if (world.vbs.empty() || world.ibs.empty() || world.meshes.empty())
	{
		Msg("! R5 level: direct load incomplete (vb=%u ib=%u mesh=%u)",
			(u32)world.vbs.size(), (u32)world.ibs.size(), (u32)world.meshes.size());
		Clear(world);
		return false;
	}

	world.loaded = true;
	Fvector c;
	world.aabb.getcenter(c);
	Msg("* R5 level: direct loaded '%s' (vb=%u ib=%u meshes=%u) aabb[%g,%g,%g]-[%g,%g,%g] center[%g,%g,%g]",
		mapName, (u32)world.vbs.size(), (u32)world.ibs.size(), (u32)world.meshes.size(),
		world.aabb.min.x, world.aabb.min.y, world.aabb.min.z,
		world.aabb.max.x, world.aabb.max.y, world.aabb.max.z,
		c.x, c.y, c.z);
	return true;
}
