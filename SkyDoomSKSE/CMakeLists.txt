cmake_minimum_required(VERSION 3.21)

project(
    SkyDoomSKSE
    VERSION 0.1.0
    DESCRIPTION "SkyDoom SKSE bridge"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)


# ============================================================
# COMMONLIBSSE-NG
# ============================================================

if(
    NOT EXISTS
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/commonlibsse-ng/CMakeLists.txt"
)
    message(
        FATAL_ERROR
        "lib/commonlibsse-ng is missing. Run: git submodule update --init --recursive"
    )
endif()


find_package(
    spdlog
    CONFIG
    REQUIRED
)


set(
    ENABLE_SKYRIM_SE
    ON
    CACHE BOOL ""
    FORCE
)

set(
    ENABLE_SKYRIM_AE
    ON
    CACHE BOOL ""
    FORCE
)

set(
    ENABLE_SKYRIM_VR
    ON
    CACHE BOOL ""
    FORCE
)

set(
    BUILD_TESTS
    OFF
    CACHE BOOL ""
    FORCE
)

set(
    SKSE_SUPPORT_XBYAK
    OFF
    CACHE BOOL ""
    FORCE
)


set(
    _github_actions
    "$ENV{GITHUB_ACTIONS}"
)

unset(
    ENV{GITHUB_ACTIONS}
)


add_subdirectory(
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/commonlibsse-ng"
    "${CMAKE_CURRENT_BINARY_DIR}/commonlibsse-ng"
    EXCLUDE_FROM_ALL
)


set(
    ENV{GITHUB_ACTIONS}
    "${_github_actions}"
)


include(
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/commonlibsse-ng/cmake/CommonLibSSE.cmake"
)


# ============================================================
# VERSION RESOURCE
# ============================================================

file(
    MAKE_DIRECTORY
    "${CMAKE_CURRENT_BINARY_DIR}/src"
)


configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.rc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/src/version.rc"
    @ONLY
)


# ============================================================
# SKYDOOM PLUGIN
# ============================================================

add_commonlibsse_plugin(
    ${PROJECT_NAME}

    NAME
    "SkyDoomSKSE"

    AUTHOR
    "Caffs"

    SOURCES
    src/Plugin.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/src/version.rc"

    USE_ADDRESS_LIBRARY
)


target_precompile_headers(
    ${PROJECT_NAME}

    PRIVATE

    src/PCH.h
)


target_include_directories(
    ${PROJECT_NAME}

    PRIVATE

    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/src"
)


# ============================================================
# NATIVE D3D11 SKYDOOM RENDERER
# ============================================================
#
# No DirectXTK.
#
# SkyDoom creates its own:
#
#   vertex shader
#   pixel shader
#   alpha blend state
#   point sampler
#   dynamic RGBA texture
#
# using the Windows D3D11 APIs directly.
# ============================================================

target_link_libraries(
    ${PROJECT_NAME}

    PRIVATE

    d3d11.lib
    d3dcompiler.lib
    dxgi.lib
    dxguid.lib
    uuid.lib
)


set_target_properties(
    ${PROJECT_NAME}

    PROPERTIES

    OUTPUT_NAME
    "skydoomskse"

    PREFIX
    ""

    SUFFIX
    ".dll"
)


if(MSVC)

    target_compile_options(
        ${PROJECT_NAME}

        PRIVATE

        /Zi
    )


    target_link_options(
        ${PROJECT_NAME}

        PRIVATE

        /INCREMENTAL:NO
        /OPT:REF
        /OPT:ICF
        /DEBUG:FULL
    )

endif()