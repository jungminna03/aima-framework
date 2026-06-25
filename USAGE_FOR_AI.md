# AIMA_framework — Manual for AI Assistants

You are an AI assistant about to build (or extend) a game **on top of**
**AIMA_framework** — a small, **renderer-less**, cross-platform game-app
foundation. This document is your contract. Read §1 once, keep §2–§4 open while
you write code, and copy the example in §7.

> **The one thing to internalize: AIMA_framework draws NOTHING.** It is the
> reusable, engine-agnostic foundation — the cross-platform build, general-purpose
> libraries, the host loop, code + asset hot-reload, the SDL platform layer, the
> ECS, and an abstract **Renderer interface**. Every pixel is drawn by an
> `aima::Renderer` implementation **your project supplies** with its own graphics
> (DX12, SDL_GPU/Metal, Siv3D, a bare SDL clear — whatever fits). AIMA's HD-2D game
> is 3D; a Siv3D game is 2D; the framework is identical underneath both because the
> renderer is **out of scope** for it.

This folder is meant to be **copy-pasted** into a new project. It is
self-contained: it vendors the Arimu ECS, documents its deps in `vcpkg.json`, and
hard-codes no absolute paths.

---

## 0. TL;DR

- **Three layers.** `HOST` (`aima::Host`: window, loop, hot-reload, input pump)
  drives the `FRAMEWORK` (AIMA: ECS + platform + core + the Renderer *interface*)
  which runs the `GAME` (a hot-swappable shared module of plugins/systems). **The
  renderer is a 4th thing your project plugs in** — the host calls it but never
  owns a concrete one.
- **You implement `aima::Renderer`** (§4) and register it: `init / begin_frame /
  end_frame / resize / screenshot / flush / shutdown`. The host calls these around
  `App::Tick` every frame. Your render-phase ECS systems draw into the opaque frame
  handle `begin_frame` returns.
- **The game is an Arimu ECS module.** State lives in a `World`; behaviour lives in
  **systems** (free functions whose parameter *types* declare what they touch),
  grouped in **plugins**. The host calls `app.Tick(scene, dt)` once per frame. (For
  the ECS itself, read `arimu-framework/USAGE_FOR_AI.md` — that is the substrate.)
- **The game ships as a shared module the host loads at runtime** (`.dll` / `.dylib`
  / `.so`). A finished rebuild hot-swaps into the **running** process; the live
  World survives. The module exports a small C ABI (§3).
- **One codebase, three OSes.** The build carries the cross-platform wisdom (WIN32
  gates, the "SDL3 linked once" trick, the visibility macro, presets for Win/mac/
  Linux). Code + asset hot-reload work cross-platform.
- **Dependencies (general-purpose, NON-graphics):** C++20 + SDL3 (window/input/
  audio/gamepad) + EnTT (vendored via Arimu) + spdlog + efsw + nlohmann_json +
  tomlplusplus + DirectXMath (cross-platform), and **optionally** Jolt (physics —
  off by default). **No graphics libs** (no imgui, meshoptimizer, DX12, SDL_GPU) —
  those belong to your renderer.

---

## 1. Mental model (read once)

```
┌──────────────────────────────────────────────────────────────────┐
│ HOST  — aima::Host  (your tiny exe + the framework's host loop)    │
│   owns: the SDL window, the main loop, input marshalling, BOTH     │
│   hot-reload loops (code module + asset/shader watcher), audio.    │
│   Calls the RENDERER (below) + app.Tick(scene, dt) each frame.     │
└───────┬───────────────────────────────────────────┬──────────────┘
        │ loads + hot-swaps the game ABI (§3)        │ calls each frame
        │ drives app.Tick(scene, dt)                 │
┌───────▼──────────────────────────────┐  ┌──────────▼───────────────┐
│ FRAMEWORK — AIMA (aima::framework)    │  │ RENDERER — YOUR project   │
│   Arimu ECS (World/System/Plugin)     │  │   implements aima::Renderer│
│   platform: window, InputState, audio │  │   with its OWN graphics:   │
│   core: log, math, file-watch         │  │   DX12 / SDL_GPU / Siv3D / │
│   hot-reload: efsw + dlopen/LoadLib   │  │   bare SDL clear / ...      │
│   aima::Renderer  (INTERFACE ONLY)    │◄─┤   (NOT in the framework)   │
└───────▲──────────────────────────────┘  └────────────────────────────┘
        │ the game is registered into the framework's World
┌───────┴──────────────────────────────────────────────────────────┐
│ GAME  — YOUR shared module  (libyourgame.dylib / yourgame.dll)     │
│   your plugins/systems/components/resources + the C ABI (§3).      │
│   Your Render-phase systems draw into the renderer's frame handle. │
└──────────────────────────────────────────────────────────────────┘
```

### What is IN the framework vs EXCLUDED

**IN (this folder):**

1. **The cross-platform build** — `CMakeLists.txt` (WIN32 gates, the SHARED-core
   "SDL3 linked once" trick, the `AIMA_GAME_API` visibility macro, the host/module
   split), `CMakePresets.json` (Windows/mac/Linux), `vcpkg.json` (general deps).
2. **General-purpose libraries** — SDL3, spdlog, efsw, nlohmann_json, tomlplusplus,
   DirectXMath, EnTT (via Arimu); Jolt optional.
3. **The host loop** — `aima::Host` (`include/aima/host.h`, `src/core/host.cpp`):
   window + loop + input pump + `begin_frame → Tick → end_frame`.
4. **Code hot-reload + the game-module ABI** — the dlopen/LoadLibrary loader, the
   per-generation copy, the macOS ad-hoc codesign, the mtime poll, the versioned
   ABI (§3, §5).
5. **The platform layer** — `Window` (SDL3), `InputState` mailbox, `AudioDevice`
   (opaque ids).
6. **The ECS** — Arimu (vendored, with its single-header EnTT).
7. **Asset scaffolding (general)** — `res_path.h` (name→path convention) + the
   efsw file-watch reload. **NOT** the GPU loaders.
8. **The Renderer INTERFACE** — `include/aima/renderer.h` (abstract; you implement).
9. **Core services** — `log` (spdlog), `math` (DirectXMath bridge).

**EXCLUDED (your project supplies these — they are GRAPHICS):**

- Any concrete renderer / GPU device / render passes / shaders.
- The GPU-bound asset loaders (glTF → GPU mesh, sprite-sheet → GPU texture). The
  framework's `res_path` resolves names to file paths; *loading the bytes onto the
  GPU is yours.*
- imgui, meshoptimizer, and any other graphics library.

The framework gives you the `aima::Renderer` seam; you fill it.

### Frame anatomy (what `aima::Host` does each tick)

1. Pump SDL events → fill a generic `InputState` (movement, mouse, common edges).
2. Compute `dt` (clamped); poll the **asset/shader** watcher (efsw); poll for a
   newer **game module** on disk and hot-swap if found.
3. Marshal `InputState` into the World resource; call your optional `frame_hook`.
4. `renderer.begin_frame(clear_color)` → opaque frame handle.
5. `app.Tick(scene, dt)` — runs the active scene's systems in phase order; your
   Render-phase systems draw into the frame handle (read it from a resource you set).
6. `renderer.end_frame(vsync)` (present).
7. Headless self-exit checks (`AIMA_SHOT` screenshot, frame caps).

---

## 2. Hard rules / anti-patterns

1. **Never link vcpkg's EnTT.** Arimu vendors its own single-header EnTT and exposes
   it PUBLIC. Two EnTT copies = two incompatible `entt::registry` ABIs in one link
   graph = silent corruption across the host/game module boundary. The whole project
   shares Arimu's EnTT transitively. (See the comment in `CMakeLists.txt`.)
2. **No graphics libs in the framework.** If you find yourself adding imgui / a GPU
   API / a mesh optimizer to `aima-framework/`, stop: that belongs next to your
   renderer in *your* project. The framework stays renderer-less.
3. **The game module never opens a window or owns the renderer.** Those are HOST
   concerns. The game writes to resources; the host services them around `Tick`.
4. **Bump `GameStateVersion()` whenever a component/resource LAYOUT changes.** Live
   registry memory can't be re-shaped, so a hot-swap of differently-shaped state
   would corrupt the running World. The host compares versions and warns (§3).
5. **Hot-reload entry points must carry `AIMA_GAME_API` and be `extern "C"`.**
   Otherwise `GetProcAddress`/`dlsym` can't find them, or the module won't export
   them (visibility). The module builds `-fvisibility=hidden`; only the ABI exports
   are visible.
6. **A code hot-reload re-runs `GameRegister` on the SAME World.** Resource
   registration must be **Ensure-only** (`world.EnsureResource<T>()`, insert-if-
   absent), never clobber, so live state survives. Systems are cleared and
   re-registered every swap; resources/entities persist.
7. **Render-phase systems are read-only w.r.t. game state** (Arimu rule). They
   record draw calls against the renderer's frame handle; they don't mutate the sim.

---

## 3. The game-module ABI

Your game ships as one shared module the host loads at runtime. The module exports
these C symbols, each tagged `AIMA_GAME_API` and `extern "C"`. The typedefs are in
`include/aima/host.h`; the macro is `src/core/game_api_macro.h`; a reference impl is
`examples/minimal/game.cpp`.

```cpp
extern "C" {

// REQUIRED. Layout gate. Bump whenever any component/resource STRUCT LAYOUT
// changes. The host caches this at first load, compares on every swap; a mismatch
// means the live World's memory is the wrong shape for the new code.
AIMA_GAME_API int  GameStateVersion();

// REQUIRED. Register resources + systems. Runs on the INITIAL load AND every
// hot-reload, on the SAME World. Resource registration is Ensure-only (state
// survives); systems are (re)added in your canonical plugin order.
AIMA_GAME_API void GameRegister(Arimu::App* app);

// REQUIRED. Spawn the initial world (entities/resources content). Runs ONCE.
AIMA_GAME_API void GameSpawnWorld(Arimu::App* app);

// OPTIONAL. Context handoff for a static-lib singleton (e.g. ImGui) — the host
// hands opaque pointers (context + allocators) over after each load. Omit if you
// don't statically link such a lib.
AIMA_GAME_API void GameBindHost(void* imgui_ctx, void* alloc, void* free_fn, void* user);

// OPTIONAL. Asset hot-reload hook: the host routes a changed model/data file here
// (its watcher can't call game code directly). `renderer` is the live Renderer.
AIMA_GAME_API void GameOnAssetReload(Arimu::App* app, aima::Renderer* renderer, const char* path);

}
```

The host resolves these by name (`GameModule::resolve` in `src/core/host.cpp`),
requires the three REQUIRED ones, then calls (optional) `GameBindHost`,
`GameRegister`, and (first time) `GameSpawnWorld`.

**`GameStateVersion` gating.** The host caches `state_version` at first load. On
every swap it reloads and compares: equal → swap proceeds; different → it **warns**
that the layout changed and a restart is needed (running new-shape code on the
old-shape World would corrupt it). Discipline: **bump the version in the same commit
that changes any component/resource layout.**

### Running WITHOUT a separate module (in-process)

For tiny tools / examples you can skip the DLL split: leave `HostConfig.game_module`
empty and pass `inproc_register` / `inproc_spawn` lambdas instead. You lose code
hot-reload but keep everything else. (The minimal example uses a real module to
exercise the full ABI — see §7.)

---

## 4. The Renderer interface (the seam YOU implement)

`include/aima/renderer.h` is the load-bearing seam. The host owns no concrete
renderer; you subclass `aima::Renderer` and pass an instance to `Host::run`.

```cpp
namespace aima {
struct ClearColor { float r=0, g=0, b=0, a=1; };
using FrameHandle = void*;               // opaque per-frame object (yours)

class Renderer {
public:
    virtual ~Renderer() = default;
    virtual bool        init(SDL_Window* window, int w, int h) = 0;   // device + swapchain
    virtual FrameHandle begin_frame(const ClearColor& clear) = 0;     // acquire + clear -> handle
    virtual void        end_frame(bool vsync = true) = 0;             // submit + present
    virtual void        resize(int w, int h) = 0;                     // swapchain resize
    virtual void        screenshot(const std::string& path) {}        // optional (PNG/BMP)
    virtual void        flush() {}                                    // optional (GPU idle)
    virtual void        shutdown() = 0;                               // teardown
};
}
```

**The contract:**

- `init` gets the `SDL_Window*` the framework opened. A backend that needs the raw
  OS handle (a DX12 swapchain wants the Win32 HWND) reaches `Window::native_handle()`
  — the host passes the window so you choose. Return `false` to abort startup.
- `begin_frame` acquires the swapchain image, starts a pass that clears to `clear`,
  and returns an **opaque** `FrameHandle`. The framework never dereferences it — it
  only hands it to your game's render systems (put it in a World resource your
  systems read). The real type is yours: a command list, an `SDL_GPURenderPass*`,
  `nullptr`, a sentinel — whatever your render systems expect.
- `end_frame` finishes + presents. After it returns the handle is invalid.
- `resize` is called between frames from the window-resize event.
- `screenshot` / `flush` have default no-op implementations — implement them only if
  your backend can (the headless `AIMA_SHOT` harness calls `screenshot`).

The minimal example's `ClearRenderer` (`examples/minimal/clear_renderer.cpp`,
~50 lines incl. boilerplate) implements this with SDL3's built-in 2D renderer — no
graphics library beyond SDL3. Your 3D project replaces it with a real GPU backend;
**the host code never changes.**

---

## 5. Hot-reload (two independent mechanisms)

Both are driven by the host. Orthogonal: code reload swaps compiled game logic;
asset/shader reload swaps data on disk.

### 5a. CODE hot-reload (the game module)

A finished module build swaps into the **running** process. The host:

1. Polls the module's on-disk mtime every ~30 frames; a newer-AND-settled file
   (write older than ~0.4s, so the linker is done) triggers a swap.
2. `app.ClearSystems()` — schedules hold callables into the module about to be freed.
3. Loads the new module, checks `GameStateVersion`, then `GameRegister` on the
   **same** World. Live entities + resources survive (Ensure-only registration;
   EnTT's type keys are name-hashed, stable across modules).
4. **Leaks the old module on purpose** — live objects in the surviving World may hold
   vtables / code pointers into it. A few MB per swap is the price of state surviving.

**Per-generation copy (ALL platforms).** Every load copies the module to a **unique**
path (`..._loaded<N>`) before loading:
- Windows: the original is *locked* while loaded — a rebuild can't overwrite the
  running copy unless it lives elsewhere.
- POSIX: `dlopen()` returns the **same handle** for an already-loaded path and does
  **not** re-read the file — a rebuild at the original path would keep running
  **stale** code. A fresh path forces a genuine reload.

**macOS code-signing (CRITICAL).** macOS refuses to `dlopen` a freshly-copied
`.dylib` whose code signature no longer matches the bytes. The host re-signs each
copy ad-hoc before loading (`codesign -s - -f <copy>`), requiring the Command Line
Tools `codesign`. Without it the swap fails.

| | Windows | POSIX (macOS / Linux) |
|---|---|---|
| load | `LoadLibraryW` | `dlopen(..., RTLD_NOW \| RTLD_LOCAL)` |
| resolve | `GetProcAddress` | `dlsym` |
| mtime | `GetFileAttributesExW` | `std::filesystem::last_write_time` |
| exe path | `GetModuleFileNameW` | `_NSGetExecutablePath` / `/proc/self/exe` |

### 5b. ASSET / SHADER hot-reload (efsw watcher)

The host watches `HostConfig.watch_dirs` recursively (efsw). Each frame it delivers
`FileEvent{kind, path}` to your `set_asset_hook(...)` callback and to the module's
optional `GameOnAssetReload`. `AssetKind` (Shader/Texture/Model/Data) is a routing
**hint** — the framework owns no loaders, so *you* decide what a Shader/Model event
does (reload a pipeline, respawn a map, etc.).

---

## 6. The cross-platform build (the wisdom carried over)

- **`AIMA_GAME_API` visibility macro** (`src/core/game_api_macro.h`): `dllexport` on
  Windows; `__attribute__((visibility("default")))` elsewhere, paired with the
  module's `-fvisibility=hidden` so ONLY the ABI exports are visible.
- **The "SDL3 linked once" trick** (`CMakeLists.txt`). The framework library is
  **STATIC on Windows** (SDL3 is a normal DLL there — one copy; the module resolves
  host symbols at load) but **SHARED on macOS/Linux**. Why SHARED off-Windows: SDL3
  links *statically* into the framework; a STATIC framework would be baked into BOTH
  the exe and the game `.dylib` → two copies of SDL3's Metal renderer, whose
  Objective-C classes both register at load ("implemented in both … may cause
  crashes"). One SHARED `libaima_framework.dylib` means SDL3 exists exactly once; the
  exe and module both link that single copy. SDL3 is folded in with
  `-Wl,-force_load` (PRIVATE, no dead-strip) so the exe resolves every SDL symbol
  against the one copy.
- **WIN32 gates.** `UNICODE`/`NOMINMAX`/`WIN32_LEAN_AND_MEAN` defs; the runtime-DLL
  copy step is Windows-only (`$<TARGET_RUNTIME_DLLS>` is empty elsewhere).
- **Presets** (`CMakePresets.json`): `windows-debug` (cl, `x64-windows`),
  `mac-arm64-debug` (clang++, `arm64-osx`), `linux-x64-debug` (`x64-linux`), all
  wiring the vcpkg toolchain via `$env{VCPKG_ROOT}` — **no hard-coded paths**.

### Building (macOS, what the verification harness runs)

```bash
export PATH="/opt/homebrew/bin:$PATH"
VCPKG_ROOT="$HOME/vcpkg" cmake -S aima-framework/examples/minimal -B /tmp/aima_build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_TARGET_TRIPLET=arm64-osx -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++
cmake --build /tmp/aima_build
# Headless verify (no `timeout` needed — the host self-exits):
AIMA_SHOT=/tmp/shot.bmp AIMA_SHOTFRAME=30 /tmp/aima_build/bin/aima_minimal   # exits 0 + writes the image
```

Or with the presets: `cmake --preset mac-arm64-debug && cmake --build --preset mac-arm64-debug`.

### Headless verification hooks (in `aima::Host`)

| Env var | Effect |
|---|---|
| `AIMA_SHOT=<path>` | `renderer.screenshot(path)` at `shot_frame`, then exit. |
| `AIMA_SHOTFRAME=<n>` | Override the screenshot/exit frame. |
| `AIMA_MAXFRAMES=<n>` | Hard frame cap (a CI run can't hang). |
| `AIMA_MUTE=1` | Skip the audio device (if `enable_audio`). |

`HostConfig.shot_frame` / `HostConfig.max_frames` set defaults in code.

---

## 7. Starting a new project on AIMA (the recipe)

1. **Copy `aima-framework/`** into your repo (it's self-contained; vendors Arimu).
2. **Write your renderer** — one class implementing `aima::Renderer` (§4) with your
   graphics. Link your graphics libs to *it*, not to the framework.
3. **Write your game module** — a SHARED library exporting the ABI (§3), registering
   your Arimu plugins/systems in `GameRegister`. Build it `-fvisibility=hidden` with
   the `PREFIX "lib"` so the host finds `lib<name>.{dylib,so}` next to the exe.
4. **Write your host exe** — a few lines: construct your renderer, fill a
   `HostConfig` (title, size, `game_module` name, `watch_dirs`), call
   `aima::Host{}.run(cfg, renderer)`. See `examples/minimal/main.cpp`.
5. **Wire CMake** — `add_subdirectory(aima-framework)`, link `aima::framework` to
   both your host and game module, add your renderer's graphics deps to the host.
   See `examples/minimal/CMakeLists.txt` (the three framework lines are marked
   `# <-- AIMA`). Add your renderer's graphics libs to your project's `vcpkg.json`.

**2D vs 3D is purely your renderer's concern.** A Siv3D 2D game and an HD-2D 3D game
share this entire folder; only the `aima::Renderer` implementation + the render-phase
systems differ. The build, hot-reload, platform, ECS, and ABI are identical.

For the ECS substrate itself (legal system parameter types, the phase pipeline, the
plugin/scene/event model), read **`arimu-framework/USAGE_FOR_AI.md`** — AIMA is the
engine *around* that ECS.

---

## 8. Honest status / what's deliberately minimal

- **Verified on macOS (arm64).** The minimal example builds + runs: a window opens,
  the Renderer clears to color, the game module loads via the ABI, the ECS runs a
  system, a screenshot is captured, exit 0. Windows + Linux codepaths are present
  (the loader, the WIN32 gates, the presets) but **not built/run in this pass** —
  treat them as plausible-but-unverified, same as the source engine's Linux path.
- **Input is generic.** `InputState` carries movement / mouse / common edges, not
  any game's verbs. Map it to your actions in an Input-phase system. (The source
  engine's combat/menu-specific input fields were game-specific and were dropped.)
- **Audio is opt-in + game-agnostic** (opaque ids + file names). Your game owns the
  sound vocabulary and registers a bank by name.
- **The host runs scene 0 by default.** A project needing scene policy drives the
  active scene from a `frame_hook` reading its own `SceneState` resource (the source
  engine did exactly this); a future version can take a scene-selector callback.
- **Jolt physics is included but OFF by default** (`AIMA_WITH_JOLT`) — it's
  general-purpose, not graphics, but heavy; enable it + add `joltphysics` to your
  `vcpkg.json` if you want it.
```
