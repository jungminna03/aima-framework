# =============================================================================
# aima game starter — a blank game on the renderer-less aima framework.
#
# This is the template you copy to start a NEW aima game. It wraps a SELF-CONTAINED
# copy of the framework (aima_framework/) and builds two things:
#   - the game executable (game/main.cpp + game/renderer.cpp): your BlackRenderer
#     (aima::Renderer, SDL clear-to-BLACK) + the aima::Host call.
#   - the game MODULE (game/game.cpp): a hot-reloadable shared lib exporting the
#     aima game ABI. It starts EMPTY -> the window is a black screen.
#
# Layout:
#   game/main.cpp          host entry: construct BlackRenderer + run aima::Host
#   game/renderer.{h,cpp}  aima::Renderer impl (SDL3 2D renderer, clears to black)
#   game/game.cpp          the game module (empty: registers nothing)
#   aima_framework/        the foundation (host loop, ECS, platform, Renderer iface).
#                          It vendors Arimu — the SOLE EnTT in the build.
#   third_party/           stb_image_write.h (PNG screenshots for AIMA_SHOT)
#
# Build (macOS, native arm64 — no Rosetta):
#   cmake --preset mac-arm64-debug && cmake --build --preset mac-arm64-debug
#   AIMA_SHOT=/tmp/shot.png ./build/mac-arm64-debug/bin/aima_game   # black window, exits 0
# =============================================================================
cmake_minimum_required(VERSION 3.25)
project(aima_game LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# All binaries (exe + the framework dylib + the game module) land in bin/ together,
# so the host finds lib<game_module>.{dylib,so} / <game_module>.dll next to itself.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# --- the aima framework (brings the host loop, ECS, platform, Renderer iface) --
# It vendors Arimu (the SOLE EnTT in the build) and folds SDL3 in as a single copy
# on macOS/Linux. We add the self-contained copy that ships inside this starter.
add_subdirectory(aima_framework aima-build)

# The game MODULE: a SHARED library exporting the aima game ABI. -fvisibility=hidden
# so ONLY the AIMA_GAME_API-tagged exports are visible (the loader resolves those).
add_library(game_module SHARED game/game.cpp)
target_link_libraries(game_module PRIVATE aima::framework)
if(NOT MSVC)
    set_target_properties(game_module PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)
endif()
# Keep the file name exactly lib<game_module>.<ext> (no version suffix) so the
# host's resolve_module_path finds it next to the exe.
set_target_properties(game_module PROPERTIES PREFIX "lib")

# The host exe: the project's Renderer (BlackRenderer) + the aima::Host call.
add_executable(aima_game game/main.cpp game/renderer.cpp)
target_include_directories(aima_game PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/game"          # so #include "renderer.h" resolves
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party"   # stb_image_write.h
)
# aima::framework PUBLIC-links SDL3 (single copy on macOS) and exposes its headers,
# so the renderer's direct SDL3 calls resolve against the one framework dylib.
target_link_libraries(aima_game PRIVATE aima::framework)
add_dependencies(aima_game game_module)         # a fresh run wants the module built

if(MSVC)
    target_compile_options(aima_game PRIVATE /utf-8 /EHsc)
endif()

# On Windows, copy SDL3 (and any other runtime DLLs) next to the exe.
if(WIN32)
    add_custom_command(TARGET aima_game POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:aima_game> $<TARGET_FILE_DIR:aima_game>
        COMMAND_EXPAND_LISTS)
endif()

# macOS: build the game as a .app BUNDLE so the IDE's Run (▶) launches it as a
# real, foregrounded app. A plain Unix executable's window opens BEHIND the IDE
# without focus — "build succeeds but nothing appears". The host loads
# lib<game_module>.dylib from NEXT TO the executable; inside a bundle that's
# Contents/MacOS/, so the module is built straight into the bundle there — this
# keeps CODE HOT-RELOAD working (the build writes the module exactly where the
# host watches + dlopens it). $<TARGET_FILE_DIR:aima_game> is the bundle's
# Contents/MacOS dir once MACOSX_BUNDLE is on.
if(APPLE)
    set_target_properties(aima_game PROPERTIES MACOSX_BUNDLE ON)
    set_target_properties(game_module PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:aima_game>")
endif()
