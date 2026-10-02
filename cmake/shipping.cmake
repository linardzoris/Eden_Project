## Mixed - maximum performance optimization with debug information
SET(CMAKE_CXX_FLAGS_MIXED "/MD /O2 /Zi /DNDEBUG" CACHE STRING "Flags used by the CXX compiler during Mixed builds." FORCE )
SET(CMAKE_C_FLAGS_MIXED "/MD /O2 /Zi /DNDEBUG" CACHE STRING "Flags used by the C compiler during Mixed builds." FORCE )
SET(CMAKE_EXE_LINKER_FLAGS_MIXED "/DEBUG /OPT:REF /OPT:ICF" CACHE STRING "Flags used by the linker during Mixed builds." FORCE )
SET(CMAKE_SHARED_LINKER_FLAGS_MIXED "/DEBUG /OPT:REF /OPT:ICF" CACHE STRING "Flags used by the linker during Mixed builds." FORCE )
MARK_AS_ADVANCED(CMAKE_CXX_FLAGS_MIXED CMAKE_C_FLAGS_MIXED CMAKE_EXE_LINKER_FLAGS_MIXED CMAKE_SHARED_LINKER_FLAGS_MIXED)

## MixedAVX - maximum performance optimization with AVX2 and debug information
SET(CMAKE_CXX_FLAGS_MIXEDAVX "/MD /O2 /arch:AVX2 /Zi /DNDEBUG" CACHE STRING "Flags used by the CXX compiler during MixedAVX builds." FORCE )
SET(CMAKE_C_FLAGS_MIXEDAVX "/MD /O2 /arch:AVX2 /Zi /DNDEBUG" CACHE STRING "Flags used by the C compiler during MixedAVX builds." FORCE )
SET(CMAKE_EXE_LINKER_FLAGS_MIXEDAVX "/DEBUG /OPT:REF /OPT:ICF" CACHE STRING "Flags used by the linker during MixedAVX builds." FORCE )
SET(CMAKE_SHARED_LINKER_FLAGS_MIXEDAVX "/DEBUG /OPT:REF /OPT:ICF" CACHE STRING "Flags used by the linker during MixedAVX builds." FORCE )
MARK_AS_ADVANCED(CMAKE_CXX_FLAGS_MIXEDAVX CMAKE_C_FLAGS_MIXEDAVX CMAKE_EXE_LINKER_FLAGS_MIXEDAVX CMAKE_SHARED_LINKER_FLAGS_MIXEDAVX)

## Shipping
if (DEVIXRAY_ENABLE_SHIPPING)
    SET(CMAKE_CXX_FLAGS_SHIPPING "${CMAKE_CXX_FLAGS}" CACHE STRING "Flags used by the CXX compiler during coverage builds." FORCE )
    SET(CMAKE_C_FLAGS_SHIPPING "${CMAKE_C_FLAGS}" CACHE STRING "Flags used by the C compiler during coverage builds." FORCE )
    SET(CMAKE_EXE_LINKER_FLAGS_SHIPPING "" CACHE STRING "Flags used by the linker during coverage builds." FORCE )
    SET(CMAKE_SHARED_LINKER_FLAGS_SHIPPING "" CACHE STRING "Flags used by the linker during coverage builds." FORCE )
    MARK_AS_ADVANCED(CMAKE_CXX_FLAGS_SHIPPING CMAKE_C_FLAGS_SHIPPING CMAKE_EXE_LINKER_FLAGS_SHIPPING CMAKE_SHARED_LINKER_FLAGS_SHIPPING)

    SET(IXR_CONFIGURATIONS_STR "Debug;Mixed;MixedAVX;RelWithDebInfo;Release;Shipping" CACHE STRING "" FORCE)
else()
    SET(IXR_CONFIGURATIONS_STR "Debug;Mixed;MixedAVX;RelWithDebInfo;Release" CACHE STRING "" FORCE)
endif()

# Build config
if (DEVIXRAY_ENABLE_SHIPPING)
    add_compile_options("$<$<CONFIG:Shipping>:/wd4530>" "$<$<CONFIG:Shipping>:/wd4251>"
                        "$<$<CONFIG:Shipping>:/wd4275>" "$<$<CONFIG:Shipping>:/wd4577>"
                        "$<$<CONFIG:Shipping>:/Ob2>" "$<$<CONFIG:Shipping>:/WX>")
endif()
