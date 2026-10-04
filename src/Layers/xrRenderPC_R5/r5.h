#pragma once

// M0 最小 CRender：不继承共享层 R_dsgraph_structure，
// 直接实现 IRender_interface + IRenderDeviceRender 纯虚接口。
// 目标：DLL 可加载、设备创建、每帧清屏。

class CRenderTarget;

class CRender : public IRender_interface, public IRenderDeviceRender
{
public:
	CRenderTarget* Target = nullptr;

	// IRender_interface
	virtual GenerationLevel	get_generation() override;
	virtual LPCSTR			getShaderPath() override;
	virtual IRender_Target* getTarget() override;

	virtual void			create() override;
	virtual void			destroy() override;
	virtual void			reset_begin() override;
	virtual void			reset_end() override;

	virtual void			Calculate() override;
	virtual void			Render() override;
	virtual void			flush() override;

	virtual IRender_ObjectSpecific*	ros_create(IRenderable* parent) override;
	virtual void					ros_destroy(IRender_ObjectSpecific* &) override;
	virtual IRender_Light*			light_create() override;
	virtual void					light_destroy(IRender_Light* p_) override;
	virtual IRender_Glow*			glow_create() override;
	virtual void					glow_destroy(IRender_Glow* p_) override;

	// M5: 静态物体
	virtual IRenderVisual*			model_Create(LPCSTR name, IReader* data = 0) override;
	virtual void					model_Delete(IRenderVisual* & V, BOOL bDiscard) override;

	// M8: 世界加载
	virtual void					level_Load(IReader* fs) override;
	virtual void					level_Unload() override;

	// IRenderDeviceRender
	virtual void	Copy(IRenderDeviceRender& _in) override;

	virtual void	setGamma(float fGamma) override;
	virtual void	setBrightness(float fGamma) override;
	virtual void	setContrast(float fGamma) override;
	virtual void	updateGamma() override;

	virtual void	OnDeviceDestroy(BOOL bKeepTextures) override;
	virtual void	ValidateHW() override;
	virtual void	DestroyHW() override;
	virtual void	Reset(SDL_Window* window, u32& dwWidth, u32& dwHeight, float& fWidth_2, float& fHeight_2) override;

	virtual void	SetupStates() override;
	virtual void	OnDeviceCreate(LPCSTR shName) override;
	virtual void	Create(SDL_Window* window, u32& dwWidth, u32& dwHeight, float& fWidth_2, float& fHeight_2, bool) override;
	virtual void	SetupGPU(BOOL bForceGPU_SW, BOOL bForceGPU_NonPure, BOOL bForceGPU_REF) override;

	virtual void	overdrawBegin() override;
	virtual void	overdrawEnd() override;

	virtual void	DeferredLoad(BOOL E) override;
	virtual void	ResourcesDeferredUpload() override;
	virtual void	ResourcesDeferredUnload() override;
	virtual void	ResourcesGetMemoryUsage(u32& m_base, u32& c_base, u32& m_lmaps, u32& c_lmaps) override;
	virtual void	ResourcesDestroyNecessaryTextures() override;
	virtual void	ResourcesStoreNecessaryTextures() override;
	virtual void	ResourcesDumpMemoryUsage() override;

	virtual bool	HWSupportsShaderYUV2RGB() override;

	virtual DeviceState GetDeviceState() override;
	virtual BOOL	GetForceGPU_REF() override;
	virtual u32		GetCacheStatPolys() override;
	virtual void	Begin() override;
	virtual void	Clear() override;
	virtual void	End() override;
	virtual void	ClearTarget() override;
	virtual void	SetupDefaultTarget() override;
	virtual void	SetCacheXform(Fmatrix& mView, Fmatrix& mProject) override;
	virtual void	SetCacheXformOld(Fmatrix& mView, Fmatrix& mProject) override;
	virtual void	OnAssetsChanged() override;
};

extern CRender RImplementation;
