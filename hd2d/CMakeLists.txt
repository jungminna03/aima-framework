# ---------------------------------------------------------------------------
# aima-framework/hd2d/CMakeLists.txt — HD2D 공통 렌더러 (DX12/SDL_GPU) + imgui/에셋 글루
#
# 구 HD2D_Project/CMakeLists.txt 의 hd2d_renderer 블록을 프레임워크로 편입한 것.
# 소스/정의/링크는 원본과 동등하게 유지하되, 경로만 hd2d/ 기준으로 조정했다.
# include 경로는 기존 그대로(renderer/..., ui/..., assets/..., core/...,
# <stb_image.h>/<cgltf.h>)가 성립하도록 hd2d/ 와 hd2d/third_party 를 공개한다 —
# HD2D 게임 코드의 include 문은 한 줄도 바뀌지 않는다.
#
# 컨트롤러 조정: 호스트 main.cpp/aima::hd2d_host_main 은 편입하지 않는다
# (main.cpp 는 game/* 헤더 6종에 의존 — HD2D 에 잔류). host/stb_impl.cpp 만 이동.
# ---------------------------------------------------------------------------
set(AIMA_HD2D_BACKEND "sdlgpu" CACHE STRING "d3d12|sdlgpu")

# GRAPHICS dependencies (aima_framework 에는 없는 HD2D 렌더러 전용 deps).
find_package(imgui CONFIG REQUIRED)
find_package(meshoptimizer CONFIG REQUIRED)
if(AIMA_HD2D_BACKEND STREQUAL "d3d12")
    find_package(directxtk12 CONFIG REQUIRED)
    find_package(D3D12MemoryAllocator CONFIG REQUIRED)
endif()

# Renderer backend sources. The common passes (forward/shadow/post/scene_targets/
# gpu_resources) are written against raw ID3D12GraphicsCommandList*, so they compile
# only in the d3d12 build. The sdlgpu build supplies real SDL_GPU passes + a
# header-only null-RHI for the DX12-coupled passes (stub_passes.cpp).
if(AIMA_HD2D_BACKEND STREQUAL "d3d12")
    set(HD2D_RENDERER_SOURCES
        renderer/dx12/device.cpp
        renderer/gpu_resources.cpp
        renderer/forward_pass.cpp
        renderer/scene_targets.cpp
        renderer/post_chain.cpp
        renderer/shadow_pass.cpp
    )
else()  # sdlgpu backend (real SDL_GPU device on Metal)
    set(HD2D_RENDERER_SOURCES
        renderer/sdlgpu/device.cpp          # real SDL_GPU device: clear + screenshot
        renderer/sdlgpu/triangle_pass.cpp   # hello-triangle (MSL) — fallback
        renderer/sdlgpu/geometry_pass.cpp   # real world meshes (MSL) + depth + camera + shadows
        renderer/sdlgpu/post_pass.cpp       # HDR offscreen -> bloom + AgX tonemap (MSL)
        renderer/sdlgpu/stub_passes.cpp     # no-op forward/shadow/post/targets/gpu_resources
    )
endif()

add_library(aima_hd2d_renderer STATIC
    renderer/renderer_impl.cpp      # Hd2dRenderer (implements aima::Renderer)
    renderer/camera.cpp
    ui/imgui_layer.cpp
    assets/gltf_loader.cpp
    assets/sprite_sheet.cpp
    host/stb_impl.cpp               # stb_image impl (used by the asset loaders)
    ${HD2D_RENDERER_SOURCES}
)
add_library(aima::hd2d_renderer ALIAS aima_hd2d_renderer)

# hd2d/ 는 "renderer/...", "ui/...", "assets/...", "core/log_compat.h" 등을 성립시키고
# hd2d/third_party 는 <stb_image.h>/<stb_image_write.h>/<cgltf.h> 를 성립시킨다.
target_include_directories(aima_hd2d_renderer PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
)

target_compile_definitions(aima_hd2d_renderer PUBLIC
    HD2D_SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/shaders")
# HD2D_ASSET_DIR / AIMA_ASSET_DIR 은 게임(소비자)이 정의한다 — 프레임워크는
# 자기 에셋 경로를 모른다.
if(WIN32)
    target_compile_definitions(aima_hd2d_renderer PUBLIC
        UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# Header-used deps stay PUBLIC so consumers (the exe + the game module) get the
# includes AND the out-of-line symbols for templates they instantiate themselves.
# aima::framework already propagates SDL3, spdlog, Arimu+EnTT, DirectXMath, Jolt.
target_link_libraries(aima_hd2d_renderer PUBLIC
    aima::framework
    imgui::imgui
    meshoptimizer::meshoptimizer
)

# --- backend dependency branches (구 HD2D 루트 CMakeLists 원문 그대로) ---
if(AIMA_HD2D_BACKEND STREQUAL "d3d12")
    target_compile_definitions(aima_hd2d_renderer PUBLIC HD2D_RENDERER_D3D12)
    target_link_libraries(aima_hd2d_renderer PUBLIC
        Microsoft::DirectXTK12
        GPUOpen::D3D12MemoryAllocator
        # Windows SDK DirectX 12 import libs
        d3d12
        dxgi
        dxguid
        d3dcompiler
    )
else()
    target_compile_definitions(aima_hd2d_renderer PUBLIC HD2D_RENDERER_SDLGPU)
    # Runtime HLSL -> SPIR-V -> MSL cross-compiler (SDL_shadercross): not on
    # arm64-osx today (its directx-dxc dep has no port), so the SDL_GPU backend
    # ships MSL directly. Link it only if a viable port is present.
    find_package(SDL3_shadercross CONFIG QUIET)
    if(SDL3_shadercross_FOUND)
        target_link_libraries(aima_hd2d_renderer PUBLIC SDL3_shadercross::SDL3_shadercross)
        target_compile_definitions(aima_hd2d_renderer PUBLIC HD2D_HAVE_SHADERCROSS)
        message(STATUS "SDL3_shadercross found — runtime HLSL cross-compile enabled")
    endif()
endif()

# ---------------------------------------------------------------------------
# game_core 표준 플러그인 세트 승격(2026-07-13): render/sprite/physics/sound/ui/
# input_mapper 의 .cpp 를 프레임워크가 소유한다. 단, 이들은 게임 레이어 헤더
# (game/resources.h·scene_enum.h·combat_components.h 등)를 소비자 include 경로로
# 참조하므로(=game_core/clip_rules.h 가 game/components.h 를 참조하는 선례) 프레임워크
# 라이브러리에 직접 컴파일하지 않고, HD2D hd2d_game(SHARED, 핫리로드) 타깃이
# target_sources 로 흡수한다. 절대경로 목록을 최상위 스코프까지 CACHE INTERNAL 로 전달.
# ---------------------------------------------------------------------------
set(AIMA_HD2D_GAME_CORE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/game_core/plugins/render_plugin.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/game_core/plugins/sprite_plugin.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/game_core/plugins/physics_plugin.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/game_core/plugins/sound_plugin.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/game_core/plugins/ui_plugin.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/game_core/plugins/input_mapper_plugin.cpp"
    CACHE INTERNAL "HD2D 표준 플러그인 세트(승격) — hd2d_game 이 소스로 흡수")
