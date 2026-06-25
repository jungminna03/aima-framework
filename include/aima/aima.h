#pragma once

// ============================================================================
// aima/aima.h — the AIMA_framework public umbrella header.
//
// One include that re-exports the RENDERER-LESS framework surface a project (host
// exe + game module) builds against. The framework provides the engine-agnostic
// foundation — ECS, host loop, code + asset hot-reload, platform (window/input/
// audio), core services, and the abstract Renderer INTERFACE — but NO concrete
// renderer. Each project implements aima::Renderer with its own graphics and
// registers it with the host.
//
// What is NOT here (by design — it is GRAPHICS, the project supplies it):
//   - any concrete renderer / GPU device / passes / shaders
//   - the GPU-bound asset loaders (glTF -> GPU mesh, sprite -> GPU texture)
//   - imgui, meshoptimizer, and any other graphics library
// The framework defines the Renderer seam (aima/renderer.h); the project fills it.
//
// What is NOT here (it is GAME, the project supplies it): the game's components,
// resources, scene enum, sound vocabulary, plugins. Those live in the game module.
// ============================================================================

// --- ECS core (arimu, vendored) — App / World / Schedule / System / Plugin ---
// A project expresses itself as arimu resources/components/systems/plugins and
// the host drives it via app.Tick(). This is the heart of the framework.
#include "arimu/App.hpp"

// --- The reusable host loop + the game-module ABI typedefs -------------------
#include "aima/host.h"

// --- The abstract Renderer interface a project implements --------------------
#include "aima/renderer.h"

// --- Game-module ABI export macro --------------------------------------------
// AIMA_GAME_API: tag the C ABI exports (GameStateVersion / GameRegister /
// GameSpawnWorld / optional GameBindHost / GameOnAssetReload). dllexport on
// Windows, default visibility (paired with -fvisibility=hidden) elsewhere.
#include "core/game_api_macro.h"

// --- Core services -----------------------------------------------------------
#include "core/log.h"          // AIMA_TRACE/INFO/WARN/ERROR (spdlog-backed)
#include "core/math.h"         // Float2/3/4 + DirectXMath SIMD bridge (cross-platform)
#include "core/hot_reload.h"   // efsw asset/shader file watcher

// --- Platform ----------------------------------------------------------------
#include "platform/window.h"   // SDL3 window (the host owns one; renderer draws into it)
#include "platform/input.h"    // generic per-frame InputState mailbox resource
#include "platform/audio.h"    // SDL3 opaque-id audio device (opt-in)

// --- Asset scaffolding (general, non-graphics) -------------------------------
#include "assets/res_path.h"   // name -> path convention (GPU loaders are project-side)
