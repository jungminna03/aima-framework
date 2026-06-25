#pragma once

// Export visibility for the game module's hot-reload entry points
// (GameStateVersion / GameBindHost / GameRegister / GameSpawnWorld /
// GameRespawnMap). On Windows the module is a .dll and needs __declspec(dllexport);
// on macOS/Linux it is a .dylib/.so loaded via dlopen, where the equivalent is
// default ELF/Mach-O symbol visibility (paired with -fvisibility=hidden on the
// module so ONLY these entry points are exported).
//
// See include/aima/host.h for the ABI typedefs and USAGE_FOR_AI.md §"Game-module
// ABI" for the contract.
#ifdef _WIN32
#define AIMA_GAME_API __declspec(dllexport)
#else
#define AIMA_GAME_API __attribute__((visibility("default")))
#endif
