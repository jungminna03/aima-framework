# Arimu ECS Framework

A small **Bevy-style ECS framework**, built on [EnTT](https://github.com/skypjack/entt).
Systems are plain free functions; what they receive is decided by their parameter *types*
(`Res`, `ResMut`, `Query`, `EventReader`, `EventWriter`, `Commands`). A frame runs systems in a
fixed phase order, per active scene.

**Renderer/platform agnostic.** The framework does not open a window or own the main loop — it
only advances the simulation one frame at a time (`App::Tick`). *Your* game owns its loop and
calls `Tick()`. The bundled example happens to use [Siv3D](https://siv3d.github.io/), but nothing
in `arimu/` depends on Siv3D.

> Extracted from the *Arimu* game project to be reused across games.
> **For an AI assistant building a game on this framework, read [`USAGE_FOR_AI.md`](USAGE_FOR_AI.md).**

## What's in here

```
arimu-framework/
├── arimu/              # the framework (namespace Arimu, include as "arimu/App.hpp") — NO renderer dependency
├── third_party/entt/   # vendored single-header EnTT
├── examples/minimal/   # bouncing-ball + click-counter demo (uses Siv3D; compiles; read it)
├── CMakeLists.txt      # builds `arimu` as a STATIC library (see below)
├── README.md           # this file
└── USAGE_FOR_AI.md     # the manual (mental model + rules + cookbook)
```

## Requirements

**The framework (`arimu/`):**
- **C++20** — the only language requirement. It is C++20-clean.
- **EnTT** — vendored, nothing to install.
- No renderer, no windowing, no main loop. You drive it from your own loop.

**The example (`examples/minimal/`), which is a Siv3D game:**
- **Siv3D 0.6.16**, and in practice **C++23** — Siv3D 0.6.16 on recent MSVC toolchains pulls
  `std::byteswap` (a C++23 symbol) from its own headers. This is a *Siv3D* requirement, not the
  framework's. The example's `CMakeLists.txt` auto-selects C++23 when available, else C++20.

Verified: `arimu` builds as a standalone `arimu.lib` with MSVC; `examples/minimal` links it and
runs against Siv3D 0.6.16.

## Install (vendoring)

1. Copy the whole `arimu-framework/` folder into your game repo.
2. In your game's `CMakeLists.txt` — just two lines:

   ```cmake
   add_subdirectory(arimu-framework)
   target_link_libraries(MyGame PRIVATE arimu)   # STATIC lib + include dirs + C++20, all transitive
   ```

   `arimu` is a plain STATIC library (no renderer coupling). Set your C++ standard and
   `CMAKE_MSVC_RUNTIME_LIBRARY` *before* `add_subdirectory` so `arimu` is built to match your
   game — see `examples/minimal/CMakeLists.txt`.

3. In code:

   ```cpp
   #include "arimu/App.hpp"   // pulls in World, Schedule, System, Plugin, FixedTime
   ```

## 30-second taste

```cpp
struct Score { int v = 0; };                       // a resource

void Bump(Arimu::ResMut<Score> s) { s->v += 1; }   // a system

struct MyPlugin {                                  // a plugin
    static void Build(Arimu::App& app) {
        app.GetWorld().InsertResource<Score>();
        app.AddSystem(Bump, /*scene*/0, Arimu::Phase::Logic, "Bump");
    }
};

void Main() {                                      // YOUR game owns the loop
    Arimu::App app;
    app.AddPlugin<MyPlugin>();
    while (s3d::System::Update()) {                // any platform loop works
        app.Tick(/*sceneIndex*/0, s3d::Scene::DeltaTime());
    }
}
```

See [`examples/minimal/Main.cpp`](examples/minimal/Main.cpp) for a complete, runnable version
and [`USAGE_FOR_AI.md`](USAGE_FOR_AI.md) for the full guide.
