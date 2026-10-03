#include "stdafx.h"
#include "r5.h"
#include "r5_stubs.h"

BOOL APIENTRY DllMain(HANDLE hModule, DWORD  ul_reason_for_call, LPVOID lpReserved)
{
	switch (ul_reason_for_call)
	{
	case DLL_PROCESS_ATTACH:
		::Render = &RImplementation;
		::RenderFactory = reinterpret_cast<IRenderFactory*>(&RenderFactoryImpl);
		::DU = reinterpret_cast<CDUInterface*>(&DUImpl);
		UIRender = reinterpret_cast<IUIRender*>(&UIRenderImpl);

#ifdef DEBUG_DRAW
		DRender = reinterpret_cast<IDebugRender*>(&DebugRenderImpl);
#endif
		break;
	case DLL_THREAD_ATTACH:
	case DLL_THREAD_DETACH:
	case DLL_PROCESS_DETACH:
		break;
	}
	return TRUE;
}
