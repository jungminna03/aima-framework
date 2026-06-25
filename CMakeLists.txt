cmake_minimum_required(VERSION 3.25)

# =============================================================================
# AIMA_framework — a RENDERER-LESS, cross-platform game-app foundation.
#
# Builds the `aima_framework` library + the `aima::framework` ALIAS. It carries
# the reusable, engine-agnostic core: the host loop, code + asset hot-reload, the
# SDL3 platform layer (window/input/audio), core services (log/math/file-watch),
# the Arimu ECS, and the abstract Renderer INTERFACE. It contains NO renderer:
# each project implements aima::Renderer with its own graphics.
#
# Use from a project:
#   add_subdirectory(aima-framework)
#   target_link_libraries(MyHost  PRIVATE aima::framework)   # + #include "aima/aima.h"
#   target_link_libraries(MyGame  PRIVATE aima::framework)   # game module (SHARED)
#
# Deps are resolved by vcpkg in manifest mode (toolchain wired via CMakePresets).
# =============================================================================
project(aima_framework LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# -------- options ------------------------------------------------------------
# Jolt is general-purpose physics (not graphics), so it is INCLUDABLE but OFF by
# default: most foundations don't need it and it is the heaviest dep to build. A
# project that wants it flips this ON (and adds "joltphysics" to its vcpkg.json).
option(AIMA_WITH_JOLT "Link Jolt physics into the framework" OFF)

# -------- dependencies (general-purpose, NON-graphics) -----------------------
find_package(SDL3 CONFIG REQUIRED)          # window / input / audio / gamepad
find_package(spdlog CONFIG REQUIRED)        # logging
find_package(efsw CONFIG REQUIRED)          # file-watch hot-reload
find_package(nlohmann_json CONFIG REQUIRED) # config / data
find_package(tomlplusplus CONFIG REQUIRED)  # config / data
find_package(directxmath CONFIG REQUIRED)   # math (cross-platform, header-only, no GPU)
if(AIMA_WITH_JOLT)
    find_package(Jolt CONFIG REQUIRED)
endif()
# NOTE: deliberately NO imgui / meshoptimizer / DX12 / SDL_GPU here — that is
# GRAPHICS. A project links its own graphics libs next to its Renderer impl.

# -------- Arimu ECS (vendored) -----------------------------------------------
# Arimu is THE structural core: the game is arimu resources/components/systems/
# plugins driven by app.Tick(). Arimu vendors its OWN single-header EnTT and
# exposes it PUBLIC. NEVER also link vcpkg's EnTT: two EnTT copies = two
# incompatible entt::registry ABIs in one link graph = silent corruption across
# the host/game module boundary. The whole project shares arimu's EnTT.
add_subdirectory(arimu-framework arimu-build)

# -------- core linkage: the "SDL3 linked once" trick -------------------------
# Windows: STATIC. SDL3 is a normal DLL there (one copy in the process), and the
#   game module resolves the host's framework symbols at load time against the exe
#   — no duplicate-symbol problem.
# macOS/Linux: SHARED. SDL3 (and the other deps) link STATICALLY into the
#   framework, so a STATIC framework would be baked into BOTH the exe and the game
#   .dylib — two copies of SDL3's Metal renderer, whose Objective-C classes both
#   register at load ("implemented in both ..." objc warnings, "may cause
#   crashes"). Building the framework as ONE shared library means SDL3 exists
#   exactly once in the process; the exe and the game module both link that single
#   libaima_framework.dylib.
if(WIN32)
    set(AIMA_LINKAGE STATIC)
else()
    set(AIMA_LINKAGE SHARED)
endif()

add_library(aima_framework ${AIMA_LINKAGE}
    src/core/log.cpp
    src/core/hot_reload.cpp
    src/core/host.cpp
    src/platform/window.cpp
    src/platform/audio.cpp
)
add_library(aima::framework ALIAS aima_framework)

target_include_directories(aima_framework PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"   # so #include "aima/aima.h" resolves
    "${CMAKE_CURRENT_SOURCE_DIR}/src"       # so #include "core/log.h" etc. resolve
)

if(WIN32)
    target_compile_definitions(aima_framework PUBLIC
        UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# Header-used deps stay PUBLIC: consumers (host exe + game module) instantiate
# their OWN templates from these headers (spdlog/fmt format calls, etc.) and so
# need the out-of-line symbols on their own link lines. None of these register
# Objective-C classes, so a per-image copy is harmless.
target_link_libraries(aima_framework PUBLIC
    arimu                       # ECS core + its vendored EnTT (sole entt in the build)
    spdlog::spdlog
    efsw::efsw
    nlohmann_json::nlohmann_json
    tomlplusplus::tomlplusplus
    Microsoft::DirectXMath
)
if(AIMA_WITH_JOLT)
    target_link_libraries(aima_framework PUBLIC Jolt::Jolt)
    target_compile_definitions(aima_framework PUBLIC AIMA_WITH_JOLT)
endif()

# SDL3 is the ONE dep that must be single-copy in the process (its Metal renderer
# registers Objective-C classes at image load; two copies make the objc runtime
# warn "implemented in both ... may cause crashes"). Only the framework's .cpp
# files touch SDL3 directly. Fold SDL3 into libaima_framework.dylib ONCE on the
# SHARED path; on Windows it stays a normal PUBLIC DLL dependency.
if(AIMA_LINKAGE STREQUAL "SHARED")
    # PRIVATE = the archive is NOT propagated onto consumer link lines.
    # -force_load keeps ALL of SDL3's exported symbols in the dylib (no dead-strip),
    # so a host exe can resolve every SDL symbol it uses against the single copy.
    target_link_libraries(aima_framework PRIVATE
        "-Wl,-force_load,$<TARGET_FILE:SDL3::SDL3>")
    get_target_property(_sdl_inc SDL3::SDL3 INTERFACE_INCLUDE_DIRECTORIES)
    if(_sdl_inc)
        target_include_directories(aima_framework SYSTEM PUBLIC ${_sdl_inc})
    endif()
    # SDL3's transitive system framework deps (Cocoa/Metal/...) must still be on
    # the link line even though we force_load the archive directly.
    get_target_property(_sdl_ifacelibs SDL3::SDL3 INTERFACE_LINK_LIBRARIES)
    if(_sdl_ifacelibs)
        target_link_libraries(aima_framework PRIVATE ${_sdl_ifacelibs})
    endif()
else()
    target_link_libraries(aima_framework PUBLIC SDL3::SDL3)
endif()

# Warnings. spdlog/{fmt} generate harmless warnings; keep non-fatal.
if(MSVC)
    target_compile_options(aima_framework PRIVATE /W4 /permissive- /MP /utf-8 /EHsc)
else()
    target_compile_options(aima_framework PRIVATE -Wall -Wextra)
endif()
