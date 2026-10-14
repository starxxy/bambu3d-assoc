cmake_minimum_required(VERSION 3.20)
project(Bambu3DAssoc VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Optional: build the OCCT runtime plugin (bs3d_occ.dll) when OpenCASCADE
# is available. Consumers load it via LoadLibrary at runtime; the manager
# and shell extension never link against it.
option(BUILD_OCC_PLUGIN "Build bs3d_occ.dll plugin (requires OpenCASCADE)" OFF)

if(WIN32)
    add_compile_definitions(
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
    if(MSVC)
        add_compile_options(/utf-8)
        # Anti-crack hardening (v1.3.0): control-flow guard in shipping configs.
        add_compile_options($<$<NOT:$<CONFIG:Debug>>:/guard:cf>)
    endif()
endif()

# ---------------------------------------------------------------------------
# Common library (parsers, rasterizer, cache, thumbnail core, registry, etc.)
# ---------------------------------------------------------------------------
set(LIB_SOURCES
    src/bs3d_core.cpp
    src/winutil.cpp
    src/formats.cpp
    src/zipread.cpp
    src/parsers.cpp
    src/rasterizer.cpp
    src/thumbcache.cpp
    src/thumbnailcore.cpp
    src/cadmesh.cpp
    src/occplugin.cpp
    src/envcheck.cpp
    src/registry.cpp
    src/protect.cpp
    src/branding.cpp
)

add_library(bs3d_lib STATIC ${LIB_SOURCES})
target_include_directories(bs3d_lib PUBLIC include)
target_compile_definitions(bs3d_lib PUBLIC _SILENCE_STDEXT_HASH_DEPRECATION_WARNINGS)

# System libraries
target_link_libraries(bs3d_lib PUBLIC
    advapi32
    shlwapi
    shell32
    ole32
    oleaut32
    gdiplus
    gdi32
    user32
    comdlg32
    oleacc
    winhttp
)

# Optional OCCT plugin target (built only when a toolchain provides OCCT)
if(BUILD_OCC_PLUGIN)
    find_package(OCCT CONFIG QUIET)
    if(NOT OCCT_FOUND)
        find_package(OCC QUIET)
    endif()
    if(NOT OCCT_FOUND AND NOT OCC_FOUND)
        find_package(OpenCASCADE CONFIG QUIET)
    endif()
    # Official OpenCASCADE installer layout (win64/vc14 libs, /MD-built DLLs).
    if(OpenCASCADE_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/bs3d_occ_plugin.cpp")
        add_library(bs3d_occ SHARED src/bs3d_occ_plugin.cpp)
        target_include_directories(bs3d_occ PRIVATE include "${OpenCASCADE_INCLUDE_DIR}")
        set(_bs3d_occt_libs "")
        foreach(_tk IN ITEMS TKernel TKMath TKBRep TKGeomBase TKGeomAlgo TKG3d TKG2d
                             TKShHealing TKTopAlgo TKPrim TKSTEP TKSTEPBase TKSTEPAttr
                             TKSTEP209 TKXSBase TKXDE TKIGES TKMesh TKOffset TKFillet)
            if(EXISTS "${OpenCASCADE_LIBRARY_DIR}/${_tk}.lib")
                list(APPEND _bs3d_occt_libs "${OpenCASCADE_LIBRARY_DIR}/${_tk}.lib")
            endif()
        endforeach()
        target_link_libraries(bs3d_occ PRIVATE ${_bs3d_occt_libs})
        set_target_properties(bs3d_occ PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dist"
            RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/dist"
            RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/dist"
            RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/dist"
            RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/dist"
        )
    endif()
    if((OCCT_FOUND OR OCC_FOUND) AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/bs3d_occ_plugin.cpp")
        add_library(bs3d_occ SHARED src/bs3d_occ_plugin.cpp)
        target_link_libraries(bs3d_occ PRIVATE bs3d_lib)
        if(COMMAND occt_application_param)
            occt_application_param(bs3d_occ)
        else()
            target_link_libraries(bs3d_occ PRIVATE
                TKernel TKMath TKBRep TKGeomBase TKGeomAlgo TKG3d TKG2d
                TKShHealing TKTopAlgo TKPrim TKSTEP TKSTEPBase TKSTEPAttr
                TKXSBase TKIGES TKMesh TKOffset TKFillet
            )
        endif()
        set_target_properties(bs3d_occ PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dist"
            RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/dist"
            RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/dist"
            RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/dist"
            RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/dist"
        )
    else()
        message(WARNING "BUILD_OCC_PLUGIN=ON but OpenCASCADE was not found; skipping bs3d_occ.dll")
    endif()
endif()

# ---------------------------------------------------------------------------
# COM Shell Extension DLL (bs3d_thumb.dll)
# ---------------------------------------------------------------------------
add_library(bs3d_thumb SHARED src/bs3d_thumb_dll.cpp src/bs3d_thumb.def)
target_link_libraries(bs3d_thumb PRIVATE bs3d_lib)
target_compile_definitions(bs3d_thumb PRIVATE BS3D_DLL_BUILD)
if(MSVC)
    target_link_options(bs3d_thumb PRIVATE "/GUARD:CF")
endif()

set_target_properties(bs3d_thumb PROPERTIES
    LIBRARY_OUTPUT_NAME "bs3d_thumb"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dist"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/dist"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/dist"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/dist"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/dist"
)

# ---------------------------------------------------------------------------
# Manager GUI (bs3d_assoc.exe)
# ---------------------------------------------------------------------------
add_executable(bs3d_assoc WIN32 src/bs3d_assoc_main.cpp src/bs3d_assoc.rc)
target_link_libraries(bs3d_assoc PRIVATE bs3d_lib)
if(MSVC)
    target_link_options(bs3d_assoc PRIVATE "/GUARD:CF")
endif()
target_link_options(bs3d_assoc PRIVATE
    "/MANIFEST:EMBED"
    "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/src/bs3d_assoc.exe.manifest")
set_target_properties(bs3d_assoc PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dist"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/dist"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/dist"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/dist"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/dist"
)

# ---------------------------------------------------------------------------
# Headless render diagnostic (console)
# ---------------------------------------------------------------------------
add_executable(thumb_probe tools/thumb_probe.cpp)
target_link_libraries(thumb_probe PRIVATE bs3d_lib)

# FBX parser verification probe (console).
add_executable(fbx_probe tools/fbx_probe.cpp)
target_link_libraries(fbx_probe PRIVATE bs3d_lib)

# v1.3.0: redeem_gen 退役（软件已免费，无兑换码可生成）。

# Post-build: stamp the CRC32 integrity slot into the shipping binaries.
# Runtime verification lives in src/protect.cpp (manager exe enforces).
find_program(BS3D_PYTHON NAMES py python3 python DOC "Python for integrity stamping")
if(BS3D_PYTHON)
    add_custom_command(TARGET bs3d_assoc POST_BUILD
        COMMAND "${BS3D_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/stamp_integrity.py"
                "$<TARGET_FILE:bs3d_assoc>"
        VERBATIM)
    add_custom_command(TARGET bs3d_thumb POST_BUILD
        COMMAND "${BS3D_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/stamp_integrity.py"
                "$<TARGET_FILE:bs3d_thumb>"
        VERBATIM)
else()
    message(WARNING "No Python interpreter found - run tools/stamp_integrity.py "
                    "manually on dist binaries, or the tamper check stays lenient")
endif()

# Both targets already output to ${CMAKE_BINARY_DIR}/dist via RUNTIME_OUTPUT_DIRECTORY
