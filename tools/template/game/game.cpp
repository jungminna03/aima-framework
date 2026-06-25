// The STARTER game module: a hot-reloadable shared library exporting the aima
// game-module ABI. It is intentionally EMPTY — it registers nothing and spawns
// nothing, so the window stays a black screen. This is your blank slate: add
// resources/components/systems/plugins in GameRegister + GameSpawnWorld and the
// running host hot-swaps your changes (~0.5s) without a restart.
//
// Built as lib<game_module>.{dylib,so} / <game_module>.dll next to the exe; the
// host loads it, calls GameRegister + GameSpawnWorld, and hot-swaps it on rebuild.

#include "arimu/App.hpp"

#include "core/game_api_macro.h"   // AIMA_GAME_API
#include "core/log.h"

extern "C" {

// REQUIRED. Bump whenever any component/resource STRUCT LAYOUT changes (the host
// compares this on hot-swap to refuse a mismatched live World). Empty game = 1.
AIMA_GAME_API int GameStateVersion() { return 1; }

// REQUIRED. Register resources (Ensure-only so live state survives a hot-swap) +
// systems. Runs on the initial load AND every hot-reload, on the SAME World.
// EMPTY: an empty game registers nothing. Add your systems/resources here.
AIMA_GAME_API void GameRegister(Arimu::App* /*app*/) {
    AIMA_INFO("[game] GameRegister — empty (black screen). Add systems/resources here.");
}

// REQUIRED. First load only: spawn the initial world (entities/resources content).
// EMPTY: nothing to spawn. Add your entities here.
AIMA_GAME_API void GameSpawnWorld(Arimu::App* /*app*/) {
    AIMA_INFO("[game] GameSpawnWorld — empty (nothing to spawn yet).");
}

} // extern "C"
