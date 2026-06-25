# =============================================================================
# Arimu ECS framework — a renderer-agnostic ECS library (EnTT-backed).
#
# The framework has NO Siv3D / windowing / main-loop dependency: it only advances
# the simulation one frame at a time via App::Tick(). Your game owns the platform
# loop and calls Tick(). So this builds as a plain STATIC library.
#
# Usage from your game's CMakeLists.txt:
#
#   add_subdirectory(arimu-framework)
#   target_link_libraries(MyGame PRIVATE arimu)   # include dirs + sources + C++20, all transitive
#
# Then in code:  #include "arimu/App.hpp"
#
# Note: link your game and `arimu` with the SAME C++ standard and MSVC runtime
# (CMAKE_MSVC_RUNTIME_LIBRARY) — set those before add_subdirectory() so `arimu`
# inherits them. The example does exactly this.
# =============================================================================
cmake_minimum_required(VERSION 3.24)

add_library(arimu STATIC
    arimu/App.cpp
    arimu/World.cpp
    arimu/Commands.cpp
    arimu/Event.cpp
    arimu/Schedule.cpp
    arimu/FixedTime.cpp
)

target_include_directories(arimu PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"               # so #include "arimu/App.hpp" resolves
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party"   # vendored EnTT (#include <entt/entt.hpp>)
)

target_compile_features(arimu PUBLIC cxx_std_20)

# Vendored EnTT has non-ASCII bytes in comments; force UTF-8 so MSVC doesn't warn
# (C4819) under non-Latin system codepages (e.g. Korean cp949).
if(MSVC)
    target_compile_options(arimu PRIVATE /utf-8)
endif()
