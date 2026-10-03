project(xrRender_R5)
set(XRR5_SRC_FILES)

file(GLOB_RECURSE XRR5_SOURCE_ALL_FILES CONFIGURE_DEPENDS
    "*.cpp"
    "*.h"
)
file(GLOB_RECURSE XRR5_SOURCE_INCL_FILES CONFIGURE_DEPENDS "../../Include/*.h")

source_group("render/src" FILES ${XRR5_SOURCE_ALL_FILES})
source_group("render/include" FILES ${XRR5_SOURCE_INCL_FILES})

list(APPEND XRR5_SRC_FILES ${XRR5_SOURCE_ALL_FILES})
list(APPEND XRR5_SRC_FILES ${XRR5_SOURCE_INCL_FILES})

add_library(xrRender_R5 SHARED ${XRR5_SRC_FILES})
target_precompile_headers(xrRender_R5 PRIVATE "stdafx.h")

target_include_directories(xrRender_R5 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(xrRender_R5 PUBLIC "${IXRAY_SDK_INC}")

target_compile_definitions(xrRender_R5 PRIVATE XRRENDER_R5_EXPORTS)
target_compile_definitions(xrRender_R5 PRIVATE USE_DX12)
target_compile_definitions(xrRender_R5 PRIVATE _USRDLL)
target_compile_definitions(xrRender_R5 PRIVATE "$<$<CONFIG:Debug>:DEBUG>")

target_link_libraries(xrRender_R5 PUBLIC xrAbstractions)
target_link_libraries(xrRender_R5 PUBLIC xrParticles)
target_link_libraries(xrRender_R5 PUBLIC "dxgi.lib")
target_link_libraries(xrRender_R5 PUBLIC "d3d12.lib")
target_link_libraries(xrRender_R5 PUBLIC "d3dcompiler.lib")
target_link_libraries(xrRender_R5 PUBLIC "dxguid.lib")

target_include_directories(xrRender_R5 PUBLIC "${RENDERDOC_API}")

## DirectXTex（M4c: DDS 纹理加载，与 R4 同包）
target_include_directories(xrRender_R5 PUBLIC "${CMAKE_BINARY_DIR}/packages/directxtex_desktop_2019.2024.6.5.1/include/")
target_link_libraries(xrRender_R5 PUBLIC ${CMAKE_BINARY_DIR}/packages/directxtex_desktop_2019.2024.6.5.1/native/lib/${NUGET_PACKAGE_PLATFORM}/release/DirectXTex.lib)
