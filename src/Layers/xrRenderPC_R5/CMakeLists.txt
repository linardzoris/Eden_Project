project(xrRender_R5)
set(XRR5_SRC_FILES)

# R5 自身只保留 PCH（渲染器主体复用 R4 源码，以 DX12 编译）
file(GLOB XRR5_SOURCE_ALL_FILES CONFIGURE_DEPENDS
    "stdafx.cpp"
    "stdafx.h"
)

# 共享层（API 无关）
file(GLOB_RECURSE XRR5_SOURCE_COMP_FILES CONFIGURE_DEPENDS
    "../xrRender/*.cpp"
    "../xrRender/*.h"
)

# R4 渲染器外壳（r4.cpp / r4_rendertarget*.cpp / blender_*.cpp ...）以 DX12 编译。
# 排除：stdafx.cpp(PCH 重复)、imgui DX11 后端、FSR/DLSS/XESS 后处理。
file(GLOB_RECURSE XRR5_SOURCE_R4_FILES CONFIGURE_DEPENDS
    "../xrRenderPC_R4/*.cpp"
    "../xrRenderPC_R4/*.h"
)
list(FILTER XRR5_SOURCE_R4_FILES EXCLUDE REGEX "xrRenderPC_R4/(OverlayAPI/.*|stdafx\\.cpp|stdafx\\.h|imgui_impl_dx11\\.cpp|imgui_impl_dx11\\.h|r4_rendertarget_phase_fsr\\.cpp|r4_rendertarget_phase_dlss\\.cpp|r4_rendertarget_phase_xess\\.cpp)$")

# D3D12 后端层
file(GLOB_RECURSE XRR5_SOURCE_DX12_FILES CONFIGURE_DEPENDS
    "../xrRenderDX12/*.cpp"
    "../xrRenderDX12/*.h"
)

file(GLOB_RECURSE XRR5_SOURCE_INCL_FILES CONFIGURE_DEPENDS "../../Include/*.h")

source_group("render/src" FILES ${XRR5_SOURCE_ALL_FILES})
source_group("layers" FILES ${XRR5_SOURCE_COMP_FILES})
source_group("r4_shell" FILES ${XRR5_SOURCE_R4_FILES})
source_group("general_dx12" FILES ${XRR5_SOURCE_DX12_FILES})
source_group("render/include" FILES ${XRR5_SOURCE_INCL_FILES})

list(APPEND XRR5_SRC_FILES ${XRR5_SOURCE_ALL_FILES})
list(APPEND XRR5_SRC_FILES ${XRR5_SOURCE_COMP_FILES})
list(APPEND XRR5_SRC_FILES ${XRR5_SOURCE_R4_FILES})
list(APPEND XRR5_SRC_FILES ${XRR5_SOURCE_DX12_FILES})
list(APPEND XRR5_SRC_FILES ${XRR5_SOURCE_INCL_FILES})

add_library(xrRender_R5 SHARED ${XRR5_SRC_FILES})
target_precompile_headers(xrRender_R5 PRIVATE "stdafx.h")

target_include_directories(xrRender_R5 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(xrRender_R5 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../xrRenderPC_R4")
target_include_directories(xrRender_R5 PUBLIC "${IXRAY_SDK_INC}")

target_compile_definitions(xrRender_R5 PRIVATE XRRENDER_R5_EXPORTS)
target_compile_definitions(xrRender_R5 PRIVATE USE_DX12)
# 共享层采用 DX10 风格代码路径（SDeclaration / CB / 着色器阶段）；
# 后端实现由 xrRenderDX12 提供（各 #ifdef USE_DX12 分支优先于 USE_DX11）。
target_compile_definitions(xrRender_R5 PRIVATE USE_DX11)
target_compile_definitions(xrRender_R5 PRIVATE _USRDLL)
target_compile_definitions(xrRender_R5 PRIVATE "$<$<CONFIG:Debug>:DEBUG>")

target_link_libraries(xrRender_R5 PUBLIC xrAbstractions)
target_link_libraries(xrRender_R5 PUBLIC xrParticles)
target_link_libraries(xrRender_R5 PUBLIC "dxgi.lib")
target_link_libraries(xrRender_R5 PUBLIC "d3d12.lib")
target_link_libraries(xrRender_R5 PUBLIC "d3dcompiler.lib")
target_link_libraries(xrRender_R5 PUBLIC "dxguid.lib")

target_include_directories(xrRender_R5 PUBLIC "${RENDERDOC_API}")

## DirectXTex（DDS 纹理加载，与 R4 同包）
target_include_directories(xrRender_R5 PUBLIC "${CMAKE_BINARY_DIR}/packages/directxtex_desktop_2019.2024.6.5.1/include/")
target_link_libraries(xrRender_R5 PUBLIC ${CMAKE_BINARY_DIR}/packages/directxtex_desktop_2019.2024.6.5.1/native/lib/${NUGET_PACKAGE_PLATFORM}/release/DirectXTex.lib)

## DirectXMesh（FlexibleVertexFormat.h，共享层 FVisual 等需要）
target_include_directories(xrRender_R5 PUBLIC "${CMAKE_BINARY_DIR}/packages/directxmesh_desktop_2019.2024.6.5.1/include/")
