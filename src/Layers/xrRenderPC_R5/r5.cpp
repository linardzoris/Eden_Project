#include "stdafx.h"
#include "r5.h"
#include "r5_rendertarget.h"
#include "r5_stubs.h"
#include "r5_pipeline.h"
#include "r5_resources.h"

// ---------------------------------------------------------------------------
// FactoryPtr<IUIShader> 显式实例化（共享层 RenderFactory.cpp 提供，R5 自包含需自行提供）
// ---------------------------------------------------------------------------

#include "../../Include/xrRender/FactoryPtr.h"

template<> void FactoryPtr<IUIShader>::CreateObject()
{
	m_pObject = RenderFactory->CreateUIShader();
}

template<> void FactoryPtr<IUIShader>::DestroyObject()
{
	RenderFactory->DestroyUIShader(m_pObject);
	m_pObject = nullptr;
}

CRender RImplementation;

// ---------------------------------------------------------------------------
// Stub 资源类型：工厂需要返回合法指针，但 M0 不使用。
// 接口签名已按 Include/xrRender/*.h 逐一核实。
// ---------------------------------------------------------------------------

class dx5UIShader : public IUIShader
{
public:
	virtual void Copy(IUIShader& _in) override {}
	virtual void create(LPCSTR sh, LPCSTR tex = 0) override {}
	virtual bool inited() override { return false; }
	virtual void destroy() override {}
};

class dx5UISequenceVideoItem : public IUISequenceVideoItem
{
public:
	virtual void Copy(IUISequenceVideoItem& _in) override {}
	virtual bool HasTexture() override { return false; }
	virtual void CaptureTexture() override {}
	virtual void ResetTexture() override {}
	virtual BOOL video_IsPlaying() override { return FALSE; }
	virtual void video_Sync(u32 _time) override {}
	virtual void video_Play(BOOL looped, u32 _time = 0xFFFFFFFF) override {}
	virtual void video_Stop() override {}
};

class dx5StatGraphRender : public IStatGraphRender
{
public:
	virtual void Copy(IStatGraphRender& _in) override {}
	virtual void OnDeviceCreate() override {}
	virtual void OnDeviceDestroy() override {}
	virtual void OnRender(CStatGraph& owner) override {}
};

class dx5RenderDeviceRender : public IRenderDeviceRender
{
public:
	virtual void Copy(IRenderDeviceRender& _in) override {}
	virtual void setGamma(float) override {}
	virtual void setBrightness(float) override {}
	virtual void setContrast(float) override {}
	virtual void updateGamma() override {}
	virtual void OnDeviceDestroy(BOOL) override {}
	virtual void ValidateHW() override {}
	virtual void DestroyHW() override {}
	virtual void Reset(SDL_Window*, u32&, u32&, float&, float&) override {}
	virtual void SetupStates() override {}
	virtual void OnDeviceCreate(LPCSTR) override {}
	virtual void Create(SDL_Window*, u32&, u32&, float&, float&, bool) override {}
	virtual void SetupGPU(BOOL, BOOL, BOOL) override {}
	virtual void overdrawBegin() override {}
	virtual void overdrawEnd() override {}
	virtual void DeferredLoad(BOOL) override {}
	virtual void ResourcesDeferredUpload() override {}
	virtual void ResourcesDeferredUnload() override {}
	virtual void ResourcesGetMemoryUsage(u32&, u32&, u32&, u32&) override {}
	virtual void ResourcesDestroyNecessaryTextures() override {}
	virtual void ResourcesStoreNecessaryTextures() override {}
	virtual void ResourcesDumpMemoryUsage() override {}
	virtual bool HWSupportsShaderYUV2RGB() override { return false; }
	virtual DeviceState GetDeviceState() override { return dsOK; }
	virtual BOOL GetForceGPU_REF() override { return FALSE; }
	virtual u32 GetCacheStatPolys() override { return 0; }
	virtual void Begin() override {}
	virtual void Clear() override {}
	virtual void End() override {}
	virtual void ClearTarget() override {}
	virtual void SetupDefaultTarget() override {}
	virtual void SetCacheXform(Fmatrix&, Fmatrix&) override {}
	virtual void SetCacheXformOld(Fmatrix&, Fmatrix&) override {}
	virtual void OnAssetsChanged() override {}
};

// 设备渲染路径（Device.m_pRender）必须指向 RImplementation，
// 否则 CRender 的 OnDeviceCreate/Clear/End 等不会被调用。
#define RENDER_FACTORY_RENDER_DEVICE_RENDER \
	virtual IRenderDeviceRender* CreateRenderDeviceRender() override { return &RImplementation; } \
	virtual void DestroyRenderDeviceRender(IRenderDeviceRender*) override {}

#ifdef DEBUG
class dx5ObjectSpaceRender : public IObjectSpaceRender
{
public:
	virtual void Copy(IObjectSpaceRender& _in) override {}
	virtual void dbgRender() override {}
	virtual void dbgAddSphere(const Fsphere& sphere, u32 colour) override {}
	virtual void SetShader() override {}
};
#endif

class dx5WallMarkArray : public IWallMarkArray
{
public:
	virtual void Copy(IWallMarkArray& _in) override {}
	virtual void AppendMark(LPCSTR s_textures) override {}
	virtual void clear() override {}
	virtual bool empty() override { return true; }
	virtual wm_shader GenerateWallmark() override
	{
		// M0: 不创建实际 shader，返回空 FactoryPtr
		return wm_shader();
	}
};

class dx5StatsRender : public IStatsRender
{
public:
	virtual void Copy(IStatsRender& _in) override {}
	virtual void OutData1(CGameFont& F) override {}
	virtual void OutData2(CGameFont& F) override {}
	virtual void OutData3(CGameFont& F) override {}
	virtual void OutData4(CGameFont& F) override {}
	virtual void GuardVerts(CGameFont& F) override {}
	virtual void GuardDrawCalls(CGameFont& F) override {}
	virtual void SetDrawParams(IRenderDeviceRender* pRender) override {}
};

class dx5FlareRender : public IFlareRender
{
public:
	virtual void Copy(IFlareRender& _in) override {}
	virtual void CreateShader(LPCSTR sh_name, LPCSTR tex_name) override {}
	virtual void DestroyShader() override {}
};

class dx5ThunderboltRender : public IThunderboltRender
{
public:
	virtual void Copy(IThunderboltRender& _in) override {}
	virtual void Render(CEffect_Thunderbolt& owner) override {}
};

class dx5ThunderboltDescRender : public IThunderboltDescRender
{
public:
	virtual void Copy(IThunderboltDescRender& _in) override {}
	virtual void CreateModel(LPCSTR m_name) override {}
	virtual void DestroyModel() override {}
};

class dx5RainRender : public IRainRender
{
	Fsphere m_Bounds = {};
public:
	virtual void Copy(IRainRender& _in) override {}
	virtual void Render(CEffect_Rain& owner) override {}
	virtual const Fsphere& GetDropBounds() const override { return m_Bounds; }
};

class dx5LensFlareRender : public ILensFlareRender
{
public:
	virtual void Copy(ILensFlareRender& _in) override {}
	virtual void Render(CLensFlare& owner, BOOL bSun, BOOL bFlares, BOOL bGradient) override {}
	virtual void OnDeviceCreate() override {}
	virtual void OnDeviceDestroy() override {}
};

class dx5EnvironmentRender : public IEnvironmentRender
{
public:
	virtual void Copy(IEnvironmentRender& _in) override {}
	virtual void OnFrame(CEnvironment& env) override {}
	virtual void OnLoad() override {}
	virtual void OnUnload() override {}
	virtual void RenderSky(CEnvironment& env) override {}
	virtual void RenderClouds(CEnvironment& env) override {}
	virtual void OnDeviceCreate() override {}
	virtual void OnDeviceDestroy() override {}
	virtual particles_systems::library_interface const& particles_systems_library() override
	{
		static particles_systems::library_interface* s_stub = nullptr;
		return *s_stub;
	}
};

class dx5EnvDescriptorMixerRender : public IEnvDescriptorMixerRender
{
public:
	virtual void Copy(IEnvDescriptorMixerRender& _in) override {}
	virtual void Destroy() override {}
	virtual void Clear() override {}
	virtual void lerp(IEnvDescriptorRender* inA, IEnvDescriptorRender* inB) override {}
};

class dx5EnvDescriptorRender : public IEnvDescriptorRender
{
public:
	virtual void Copy(IEnvDescriptorRender& _in) override {}
	virtual void OnDeviceCreate(CEnvDescriptor& owner) override {}
	virtual void OnDeviceDestroy() override {}
};

class dx5FontRender : public IFontRender
{
public:
	virtual void Initialize(LPCSTR cShader, LPCSTR cTexture) override {}
	virtual void OnRender(CGameFont& owner) override {}
	virtual void CreateFontAtlas(u32 width, u32 height, const char* name, void* bitmap) override {}
};

// ---------------------------------------------------------------------------
// ROS / Light / Glow stub：IRender_interface 默认返回 nullptr 会导致
// CEffect_Rain / CLensFlare 等引擎代码空指针崩溃，M0 提供空实现。
// ---------------------------------------------------------------------------

class dx5ObjectSpecific : public IRender_ObjectSpecific
{
	float m_cube[6] = {};
public:
	virtual void force_mode(u32 mode) override {}
	virtual float get_luminocity() override { return 0.f; }
	virtual float get_luminocity_hemi() override { return 0.f; }
	virtual float* get_luminocity_hemi_cube() override { return m_cube; }
};

class dx5Light : public IRender_Light
{
	vis_data m_hom = {};
public:
	virtual void set_type(LT type) override {}
	virtual void set_active(bool) override {}
	virtual bool get_active() override { return false; }
	virtual void set_shadow(bool) override {}
	virtual void set_volumetric(bool) override {}
	virtual void set_volumetric_quality(float) override {}
	virtual void set_volumetric_intensity(float) override {}
	virtual void set_volumetric_distance(float) override {}
	virtual void set_position(const Fvector& P) override {}
	virtual void set_rotation(const Fvector& D, const Fvector& R) override {}
	virtual void set_cone(float angle) override {}
	virtual void set_range(float R) override {}
	virtual void set_virtual_size(float R) override {}
	virtual void set_texture(LPCSTR name) override {}
	virtual void set_color(const Fcolor& C) override {}
	virtual void set_color(float r, float g, float b) override {}
	virtual void set_hud_mode(bool b) override {}
	virtual bool get_hud_mode() override { return false; }
	virtual vis_data& get_homdata() override { return m_hom; }
	virtual void set_occq_mode(bool b) override {}
	virtual bool get_occq_mode() override { return false; }
	virtual void set_ignore_object(CObject* O) override {}
	virtual CObject* get_ignore_object() override { return nullptr; }
	virtual void set_decor_object(CObject* O, int index = 0) override {}
	virtual CObject* get_decor_object(int index = 0) override { return nullptr; }
	virtual void destroy(bool deffered = true) override { xr_delete(this); }
};

class dx5Glow : public IRender_Glow
{
public:
	virtual void set_active(bool) override {}
	virtual bool get_active() override { return false; }
	virtual void set_position(const Fvector& P) override {}
	virtual void set_direction(const Fvector& P) override {}
	virtual void set_radius(float R) override {}
	virtual void set_texture(LPCSTR name) override {}
	virtual void set_color(const Fcolor& C) override {}
	virtual void set_color(float r, float g, float b) override {}
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

class dx5RenderFactory : public IRenderFactory
{
#define RENDER_FACTORY_IMPLEMENT_STUB(Class) \
	virtual I##Class* Create##Class() override { return new dx5##Class(); } \
	virtual void Destroy##Class(I##Class* pObject) override { xr_delete((dx5##Class*&)pObject); }

	RENDER_FACTORY_IMPLEMENT_STUB(UISequenceVideoItem)
	RENDER_FACTORY_IMPLEMENT_STUB(UIShader)
	RENDER_FACTORY_IMPLEMENT_STUB(StatGraphRender)
	RENDER_FACTORY_RENDER_DEVICE_RENDER
#	ifdef DEBUG
	RENDER_FACTORY_IMPLEMENT_STUB(ObjectSpaceRender)
#	endif
	RENDER_FACTORY_IMPLEMENT_STUB(WallMarkArray)
	RENDER_FACTORY_IMPLEMENT_STUB(StatsRender)
	RENDER_FACTORY_IMPLEMENT_STUB(FlareRender)
	RENDER_FACTORY_IMPLEMENT_STUB(ThunderboltRender)
	RENDER_FACTORY_IMPLEMENT_STUB(ThunderboltDescRender)
	RENDER_FACTORY_IMPLEMENT_STUB(RainRender)
	RENDER_FACTORY_IMPLEMENT_STUB(LensFlareRender)
	RENDER_FACTORY_IMPLEMENT_STUB(EnvironmentRender)
	RENDER_FACTORY_IMPLEMENT_STUB(EnvDescriptorMixerRender)
	RENDER_FACTORY_IMPLEMENT_STUB(EnvDescriptorRender)
	RENDER_FACTORY_IMPLEMENT_STUB(FontRender)
};

dx5RenderFactory RenderFactoryImpl;

// ---------------------------------------------------------------------------
// UIRender stub
// ---------------------------------------------------------------------------

class dx5UIRender : public IUIRender
{
public:
	virtual void CreateUIGeom() override {}
	virtual void DestroyUIGeom() override {}
	virtual void SetShader(IUIShader& shader) override {}
	virtual void SetAlphaRef(int aref) override {}
	virtual void SetScissor(Irect* rect = NULL) override {}
	virtual void GetActiveTextureResolution(Fvector2& res) override { res.set(0.f, 0.f); }
	virtual void PushPoint(float x, float y, float z, u32 C, float u, float v) override {}
	virtual void StartPrimitive(u32 iMaxVerts, ePrimitiveType primType, ePointType pointType) override {}
	virtual void FlushPrimitive() override {}
	virtual LPCSTR UpdateShaderName(LPCSTR tex_name, LPCSTR sh_name) override { return sh_name; }
	virtual void CacheSetXformWorld(const Fmatrix& M) override {}
	virtual void CacheSetCullMode(CullMode) override {}
};

dx5UIRender UIRenderImpl;

// ---------------------------------------------------------------------------
// DU (DrawUtils) stub
// ---------------------------------------------------------------------------

class dx5DUInterface : public CDUInterface
{
public:
	virtual void DrawCross(const Fvector& p, float szx1, float szy1, float szz1, float szx2, float szy2, float szz2, u32 clr, BOOL bRot45 = false) override {}
	virtual void DrawCross(const Fvector& p, float sz, u32 clr, BOOL bRot45 = false) override {}
	virtual void DrawFlag(const Fvector& p, float heading, float height, float sz, float sz_fl, u32 clr, BOOL bDrawEntity) override {}
	virtual void DrawRomboid(const Fvector& p, float radius, u32 clr) override {}
	virtual void DrawJoint(const Fvector& p, float radius, u32 clr) override {}
	virtual void DrawSpotLight(const Fvector& p, const Fvector& d, float range, float phi, u32 clr) override {}
	virtual void DrawDirectionalLight(const Fvector& p, const Fvector& d, float radius, float range, u32 clr) override {}
	virtual void DrawPointLight(const Fvector& p, float radius, u32 clr) override {}
	virtual void DrawSound(const Fvector& p, float radius, u32 clr) override {}
	virtual void DrawLineSphere(const Fvector& p, float radius, u32 clr, BOOL bCross) override {}
	virtual void dbgDrawPlacement(const Fvector& p, int sz, u32 clr, LPCSTR caption = 0, u32 clr_font = 0xffffffff) override {}
	virtual void dbgDrawVert(const Fvector& p0, u32 clr, LPCSTR caption = 0) override {}
	virtual void dbgDrawEdge(const Fvector& p0, const Fvector& p1, u32 clr, LPCSTR caption = 0) override {}
	virtual void dbgDrawFace(const Fvector& p0, const Fvector& p1, const Fvector& p2, u32 clr, LPCSTR caption = 0) override {}
	virtual void DrawFace(const Fvector& p0, const Fvector& p1, const Fvector& p2, u32 clr_s, u32 clr_w, BOOL bSolid, BOOL bWire) override {}
	virtual void DrawLine(const Fvector& p0, const Fvector& p1, u32 clr) override {}
	virtual void DrawLink(const Fvector& p0, const Fvector& p1, float sz, u32 clr) override {}
	virtual void DrawFaceNormal(const Fvector& p0, const Fvector& p1, const Fvector& p2, float size, u32 clr) override {}
	virtual void DrawFaceNormal(const Fvector* p, float size, u32 clr) override {}
	virtual void DrawFaceNormal(const Fvector& C, const Fvector& N, float size, u32 clr) override {}
	virtual void DrawSelectionBox(const Fvector& center, const Fvector& size, u32* c = 0) override {}
	virtual void DrawSelectionBoxB(const Fbox& box, u32* c = 0) override {}
	virtual void DrawIdentSphere(BOOL bSolid, BOOL bWire, u32 clr_s, u32 clr_w) override {}
	virtual void DrawIdentSpherePart(BOOL bSolid, BOOL bWire, u32 clr_s, u32 clr_w) override {}
	virtual void DrawIdentCone(BOOL bSolid, BOOL bWire, u32 clr_s, u32 clr_w) override {}
	virtual void DrawIdentCylinder(BOOL bSolid, BOOL bWire, u32 clr_s, u32 clr_w) override {}
	virtual void DrawIdentBox(BOOL bSolid, BOOL bWire, u32 clr_s, u32 clr_w) override {}
	virtual void DrawBox(const Fvector& offs, const Fvector& Size, BOOL bSolid, BOOL bWire, u32 clr_s, u32 clr_w) override {}
	virtual void DrawAABB(const Fvector& p0, const Fvector& p1, u32 clr_s, u32 clr_w, BOOL bSolid, BOOL bWire) override {}
	virtual void DrawAABB(const Fmatrix& parent, const Fvector& center, const Fvector& size, u32 clr_s, u32 clr_w, BOOL bSolid, BOOL bWire) override {}
	virtual void DrawOBB(const Fmatrix& parent, const Fobb& box, u32 clr_s, u32 clr_w) override {}
	virtual void DrawSphere(const Fmatrix& parent, const Fvector& center, float radius, u32 clr_s, u32 clr_w, BOOL bSolid, BOOL bWire) override {}
	virtual void DrawSphere(const Fmatrix& parent, const Fsphere& S, u32 clr_s, u32 clr_w, BOOL bSolid, BOOL bWire) override {}
	virtual void DrawCylinder(const Fmatrix& parent, const Fvector& center, const Fvector& dir, float height, float radius, u32 clr_s, u32 clr_w, BOOL bSolid, BOOL bWire) override {}
	virtual void DrawCone(const Fmatrix& parent, const Fvector& apex, const Fvector& dir, float height, float radius, u32 clr_s, u32 clr_w, BOOL bSolid, BOOL bWire) override {}
	virtual void DrawPlane(const Fvector& center, const Fvector2& scale, const Fvector& rotate, u32 clr_s, u32 clr_w, BOOL bCull, BOOL bSolid, BOOL bWire) override {}
	virtual void DrawPlane(const Fvector& p, const Fvector& n, const Fvector2& scale, u32 clr_s, u32 clr_w, BOOL bCull, BOOL bSolid, BOOL bWire) override {}
	virtual void DrawRectangle(const Fvector& o, const Fvector& u, const Fvector& v, u32 clr_s, u32 clr_w, BOOL bSolid, BOOL bWire) override {}
	virtual void DrawGrid() override {}
	virtual void DrawPivot(const Fvector& pos, float sz = 5.f) override {}
	virtual void DrawAxis(const Fmatrix& T) override {}
	virtual void DrawObjectAxis(const Fmatrix& T, float sz, BOOL sel) override {}
	virtual void DrawSelectionRect(const Ivector2& m_SelStart, const Ivector2& m_SelEnd) override {}
	virtual void DrawIndexedPrimitive(int prim_type, u32 pc, const Fvector& pos, const Fvector* vb, const u32& vb_size, const u32* ib, const u32& ib_size, const u32& clr_argb, float scale = 1.0f) override {}
	virtual void OutText(const Fvector& pos, LPCSTR text, u32 color = 0xFF000000, u32 shadow_color = 0xFF909090) override {}
	virtual void OnDeviceDestroy() override {}
};

dx5DUInterface DUImpl;

// ---------------------------------------------------------------------------
// DebugRender stub
// ---------------------------------------------------------------------------

#ifdef DEBUG_DRAW
class dx5DebugRender : public IDebugRender
{
public:
	virtual void Render() override {}
	virtual void add_lines(Fvector const* vertices, u32 const& vertex_count, u32 const* pairs, u32 const& pair_count, u32 const& color) override {}
	virtual void NextSceneMode() override {}
	virtual void ZEnable(bool bEnable) override {}
	virtual void OnFrameEnd() override {}
	virtual void SetShader(const debug_shader& shader) override {}
	virtual void CacheSetXformWorld(const Fmatrix& M) override {}
	virtual void CacheSetCullMode(CullMode) override {}
	virtual void SetAmbient(u32 colour) override {}
	virtual void SetDebugShader(dbgShaderHandle shdHandle) override {}
	virtual void DestroyDebugShader(dbgShaderHandle shdHandle) override {}
#ifdef DEBUG
	virtual void dbg_DrawTRI(Fmatrix& T, Fvector& p1, Fvector& p2, Fvector& p3, u32 C) override {}
#endif
};

dx5DebugRender DebugRenderImpl;
#endif

// ---------------------------------------------------------------------------
// CRender 实现
// ---------------------------------------------------------------------------

CRender::GenerationLevel CRender::get_generation()
{
	return IRender_interface::GenerationLevel::GENERATION_R2;
}

LPCSTR CRender::getShaderPath()
{
	return "d3d11\\";
}

IRender_Target* CRender::getTarget()
{
	return Target;
}

void CRender::create()
{
	Target = new CRenderTarget();

	// M3a: 资源基础设施（描述符堆 + 上传环形缓冲）
	if (!r5_res::Init())
	{
		Msg("! R5: resources init failed");
	}

	// M1: 初始化渲染管线（根签名 + PSO）
	if (!r5_pipeline::Init())
	{
		Msg("! R5: pipeline init failed");
	}
}

void CRender::destroy()
{
	r5_pipeline::Shutdown();
	r5_res::Shutdown();
	xr_delete(Target);
}

void CRender::reset_begin()
{
	destroy();
}

void CRender::reset_end()
{
	create();
}

void CRender::Calculate()
{
}

void CRender::Render()
{
	// M1: 帧管理 + M2: 全屏三角形 + M3c: 立方体
	r5_pipeline::BeginFrame();
	r5_res::BeginFrame();

	// 清屏（M0 验证用，后续由具体 pass 替代）
	ID3D12GraphicsCommandList* cmd = dx12::GetCmdList();
	D3D12_CPU_DESCRIPTOR_HANDLE rtv = dx12::GetCurrentRTV();
	const float clearColor[4] = { 0.0f, 0.0f, 0.5f, 1.0f };
	cmd->ClearRenderTargetView(rtv, clearColor, 0, nullptr);

	// M2: 全屏三角形
	r5_pipeline::DrawFullscreenTriangle();

	// M3c: 旋转立方体（时间从引擎全局时钟取）
	r5_pipeline::DrawCube(Device.fTimeGlobal);

	r5_pipeline::EndFrame();
}

void CRender::flush()
{
}

IRender_ObjectSpecific* CRender::ros_create(IRenderable* parent)
{
	return new dx5ObjectSpecific();
}

void CRender::ros_destroy(IRender_ObjectSpecific* &p)
{
	xr_delete(p);
}

IRender_Light* CRender::light_create()
{
	return new dx5Light();
}

void CRender::light_destroy(IRender_Light* p_)
{
	if (p_) p_->destroy();
}

IRender_Glow* CRender::glow_create()
{
	return new dx5Glow();
}

void CRender::glow_destroy(IRender_Glow* p_)
{
	xr_delete(p_);
}

void CRender::Copy(IRenderDeviceRender& _in)
{
}

void CRender::setGamma(float fGamma)
{
}

void CRender::setBrightness(float fGamma)
{
}

void CRender::setContrast(float fGamma)
{
}

void CRender::updateGamma()
{
}

void CRender::OnDeviceDestroy(BOOL bKeepTextures)
{
}

void CRender::ValidateHW()
{
}

void CRender::DestroyHW()
{
}

void CRender::Reset(SDL_Window* window, u32& dwWidth, u32& dwHeight, float& fWidth_2, float& fHeight_2)
{
	// SDL 窗口以 0x0 创建，实际尺寸在此补齐（对齐共享层 Reset 行为）。
	Device.ResizeWindow(psCurrentVidMode[0], psCurrentVidMode[1]);
	dwWidth = Device.GetSwapchainWidth();
	dwHeight = Device.GetSwapchainHeight();
	fWidth_2 = Device.HalfTargetWidth;
	fHeight_2 = Device.HalfTargetHeight;
}

void CRender::SetupStates()
{
}

void CRender::OnDeviceCreate(LPCSTR shName)
{
	// M0: 跳过共享层 shader 加载链，但必须初始化统计字体，
	// 否则 CStats::Show() 中 pFont 为空导致崩溃。
	Device.Statistic->OnDeviceCreate();

	// M1: 触发渲染器资源创建（含 r5_pipeline::Init）
	create();
}

void CRender::Create(SDL_Window* window, u32& dwWidth, u32& dwHeight, float& fWidth_2, float& fHeight_2, bool)
{
	dwWidth = Device.GetSwapchainWidth();
	dwHeight = Device.GetSwapchainHeight();
	fWidth_2 = Device.HalfTargetWidth;
	fHeight_2 = Device.HalfTargetHeight;
}

void CRender::SetupGPU(BOOL bForceGPU_SW, BOOL bForceGPU_NonPure, BOOL bForceGPU_REF)
{
}

void CRender::overdrawBegin()
{
}

void CRender::overdrawEnd()
{
}

void CRender::DeferredLoad(BOOL E)
{
}

void CRender::ResourcesDeferredUpload()
{
}

void CRender::ResourcesDeferredUnload()
{
}

void CRender::ResourcesGetMemoryUsage(u32& m_base, u32& c_base, u32& m_lmaps, u32& c_lmaps)
{
	m_base = c_base = m_lmaps = c_lmaps = 0;
}

void CRender::ResourcesDestroyNecessaryTextures()
{
}

void CRender::ResourcesStoreNecessaryTextures()
{
}

void CRender::ResourcesDumpMemoryUsage()
{
}

bool CRender::HWSupportsShaderYUV2RGB()
{
	return false;
}

CRender::DeviceState CRender::GetDeviceState()
{
	return dsOK;
}

BOOL CRender::GetForceGPU_REF()
{
	return FALSE;
}

u32 CRender::GetCacheStatPolys()
{
	return 0;
}

void CRender::Begin()
{
}

void CRender::Clear()
{
}

void CRender::End()
{
	dx12::FramePresent(psDeviceFlags.test(rsVSync));
}

void CRender::ClearTarget()
{
}

void CRender::SetupDefaultTarget()
{
}

void CRender::SetCacheXform(Fmatrix& mView, Fmatrix& mProject)
{
}

void CRender::SetCacheXformOld(Fmatrix& mView, Fmatrix& mProject)
{
}

void CRender::OnAssetsChanged()
{
}
