#pragma once

// ============================================================================
// aima/host.h — the reusable host loop + code-hot-reload game-module loader.
//
// The framework owns no game and no renderer, but it DOES own the boilerplate
// that every project repeats: open a window, pump SDL input into a resource,
// load the game module, poll it for rebuilds and hot-swap it live, run
// App::Tick once per frame between the renderer's begin/end_frame, and drain the
// asset/shader watcher. `aima::Host` packages exactly that.
//
// A project's executable is tiny:
//
//     #include "aima/host.h"
//     #include "my_renderer.h"
//
//     int main() {
//         MyRenderer renderer;                       // implements aima::Renderer
//         aima::HostConfig cfg;
//         cfg.title = "My Game";
//         cfg.game_module = "my_game";               // -> libmy_game.dylib / my_game.dll
//         cfg.watch_dirs = { "assets", "shaders" };
//         return aima::Host{}.run(cfg, renderer);    // blocks until quit
//     }
//
// The GAME lives in a separate shared module (`my_game`) the host loads at
// runtime and hot-swaps when rebuilt — see the ABI typedefs below and
// USAGE_FOR_AI.md §"Game-module ABI".
// ============================================================================

#include <functional>
#include <string>
#include <vector>

#include "aima/renderer.h"

namespace Arimu { class App; }

namespace aima {

class HotReload;
struct FileEvent;

// ---- Game-module ABI -------------------------------------------------------
// The game module exports a small C ABI the host resolves by name (GetProcAddress
// / dlsym). Every export is tagged AIMA_GAME_API (src/core/game_api_macro.h) and
// declared extern "C". Only the first three are REQUIRED; the rest are optional
// (a nullptr means "the host skips that hook").
extern "C" {
// REQUIRED. Layout gate: bump whenever any component/resource STRUCT LAYOUT
// changes. The host caches this at first load and compares on every hot-swap; a
// mismatch means the live World is the wrong shape for the new code.
using GameStateVersionFn = int (*)();

// REQUIRED. Register resources + systems. Runs on the INITIAL load AND on every
// hot-reload, on the SAME World. Resource registration must be Ensure-only so
// live state survives; systems are (re)added each swap.
using GameRegisterFn = void (*)(Arimu::App* app);

// REQUIRED. Spawn the initial world (entities/resources content). Runs ONCE, on
// first load only.
using GameSpawnWorldFn = void (*)(Arimu::App* app);

// OPTIONAL. ImGui (or any other static-lib singleton) context handoff. If a
// project statically links ImGui, the module carries its own globals; the host
// hands its context over right after each load. The framework passes opaque
// pointers so it pulls in no imgui headers — the module casts them back. Pass
// nullptr exports to skip.
using GameBindHostFn = void (*)(void* imgui_ctx, void* alloc, void* free_fn, void* user);

// OPTIONAL. Asset hot-reload hook for the project (e.g. a changed model/map). The
// host's file watcher can't call game code directly, so it routes a Model/Data
// FileEvent here. `renderer` is the live Renderer; `path` is the changed file.
using GameOnAssetReloadFn = void (*)(Arimu::App* app, Renderer* renderer, const char* path);

// OPTIONAL. Per-frame service hook the host calls right AFTER App::Tick (a safe
// point — no systems iterating), letting the game do deferred structural work that
// can't run inside a system: e.g. servicing a portal/scene map reload that a game
// system only FLAGGED. Arimu systems can't take World& or do immediate registry
// surgery, and the exe can't link game code, so this game-side per-frame callback is
// the home for "between-Tick" work. (스쿼드 버스터즈 포팅 2026-06-21 — 포털 리로드.)
using GameServiceFrameFn = void (*)(Arimu::App* app);
} // extern "C"

// ---- Host configuration ----------------------------------------------------
struct HostConfig {
    std::string title = "AIMA app";
    int width = 1280;
    int height = 720;
    ClearColor clear_color{0.05f, 0.06f, 0.09f, 1.0f};
    bool vsync = true;

    // Base name of the game module (no lib prefix / extension). The host loads
    // "<game_module>.dll" (Windows) / "lib<game_module>.dylib" (macOS) /
    // "lib<game_module>.so" (Linux) sitting next to the executable, and polls it
    // for rebuilds. Leave EMPTY to run WITHOUT a separate module: in that case
    // pass register/spawn callbacks via the `inproc_*` fields below (no
    // hot-reload, but no DLL split needed — handy for the minimal example).
    std::string game_module;

    // In-process game callbacks, used only when game_module is empty. The host
    // calls inproc_register (every... once, here) + inproc_spawn at startup.
    std::function<void(Arimu::App&)> inproc_register;
    std::function<void(Arimu::App&)> inproc_spawn;

    // Directories the asset/shader watcher recurses (efsw). Missing dirs skipped.
    std::vector<std::string> watch_dirs;

    // Audio is opt-in: if the project wants the SDL3 audio device, set this and
    // (typically) load a bank from a register-callback. Off by default (silent).
    bool enable_audio = false;

    // Headless verification: if AIMA_SHOT=<path> is set in the environment, the
    // host captures a screenshot at frame `shot_frame` (AIMA_SHOTFRAME overrides)
    // and exits cleanly. `max_frames` (0 = unlimited) hard-caps the loop so a CI
    // run can't hang; AIMA_MAXFRAMES overrides it. These make the loop
    // headless-friendly without a `timeout`.
    int shot_frame = 60;
    int max_frames = 0;
};

// ---- The host --------------------------------------------------------------
class Host {
public:
    // Open the window, init the renderer, load + spawn the game, then run the
    // loop until the window closes / a frame cap is hit. Returns a process exit
    // code (0 = clean). Blocks. The Renderer is owned by the caller (stack/heap)
    // and must outlive run().
    int run(const HostConfig& config, Renderer& renderer);

    // One-shot hook the host calls right AFTER the renderer is initialized and the
    // generic InputState resource is inserted, but BEFORE the game module's
    // GameRegister / GameSpawnWorld run. Lets a project seed World resources the
    // game's register/spawn depend on — e.g. a Gfx resource carrying the concrete
    // renderer's device, which the project's host owns but the game module can't
    // see. Optional. Receives the live App (which already has its World).
    using RegisterHook = std::function<void(Arimu::App&)>;
    void set_register_hook(RegisterHook hook) { register_hook_ = std::move(hook); }

    // Per-frame hook the host calls right after marshalling input, before
    // App::Tick — lets a project read SDL state the generic InputState doesn't
    // carry, or tweak resources. Optional. Receives the live App and frame index.
    // `dt` is the frame delta (seconds) the host also passes to App::Tick — the
    // project bridges it into whatever per-frame-time resource its game reads.
    using FrameHook = std::function<void(Arimu::App&, int frame, float dt)>;
    void set_frame_hook(FrameHook hook) { frame_hook_ = std::move(hook); }

    // Per-frame hook returning the scene index App::Tick should advance THIS frame
    // (Arimu runs only the active scene's systems). Called right before Tick, after
    // frame_hook. A project with multiple scenes reads its own active-scene
    // resource here AND applies any pending transition, so menu/title scenes run
    // instead of the world. Optional: if unset, the host always ticks scene 0.
    using SceneHook = std::function<int(Arimu::App&)>;
    void set_scene_hook(SceneHook hook) { scene_hook_ = std::move(hook); }

    // Per-file-event hook, called for every asset/shader change (in addition to
    // the module's GameOnAssetReload). Optional — handy for a renderer that wants
    // to reload shaders itself.
    using AssetHook = std::function<void(const FileEvent&)>;
    void set_asset_hook(AssetHook hook) { asset_hook_ = std::move(hook); }

    // Supplies the four opaque pointers the host passes to the game module's
    // GameBindHost (ImGui context + allocator alloc/free + user). A project that
    // statically links ImGui into both the exe and the game module owns the single
    // live context (e.g. on its renderer) and exposes it here, so the host hands it
    // to the module on the initial load AND every hot-reload. Optional: if unset,
    // the host binds nullptrs (the original behaviour). Set BEFORE run() — it is
    // invoked after renderer.init (so the context already exists).
    using BindProvider = std::function<void(void*&, void*&, void*&, void*&)>;
    void set_bind_provider(BindProvider p) { bind_provider_ = std::move(p); }

private:
    RegisterHook register_hook_;
    FrameHook frame_hook_;
    SceneHook scene_hook_;
    AssetHook asset_hook_;
    BindProvider bind_provider_;
};

// ---- Headless sim-ready gate ------------------------------------------------
// With AIMA_FRAMES_AFTER_READY=1 the host does not count frames against
// AIMA_MAXFRAMES / AIMA_SHOTFRAME until the game calls MarkSimReady() (e.g. on
// entering battle). Lets fixed-dt test runs burn through wall-clock-bound
// loading without eating the frame budget. No-op without the env var.
void MarkSimReady();
bool SimReady();

} // namespace aima
