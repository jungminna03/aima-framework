# Arimu ECS Framework — Manual for AI Assistants

You are an AI assistant about to build (or extend) a **Siv3D game** on the **Arimu ECS
framework**. This document is your contract. Read §1 once, keep §2 and §3 open while you
write code, and copy the patterns in §4.

The framework lives in `namespace Arimu` and is included as `#include "arimu/App.hpp"`.

---

## 0. TL;DR

- **ECS on EnTT, Bevy-style.** State lives in a `World`. Behaviour lives in **systems** —
  plain free functions whose **parameter types** declare what they touch.
- **The frame is a fixed pipeline of phases**, run once per active **scene** per frame:
  `PreLogic → Input → FixedLogic → Logic → SceneTransition → Render → Cleanup`.
- **You almost never call framework internals.** You write components (structs), resources
  (structs), events (structs), systems (functions), and bundle them in a **plugin**
  (`static void Build(App&)`).
- **The framework is renderer/platform agnostic.** It does NOT own the window or the main loop.
  YOUR game owns its loop and calls `app.Tick(sceneIndex, frameDt)` once per frame. Rendering is
  done by your `Render`-phase systems with whatever library you like (the example uses Siv3D).
- **Dependencies:** C++20 + EnTT (vendored). Nothing else. (The example additionally uses Siv3D,
  but that's the *game's* choice, not the framework's.)

---

## 1. Mental model (read once, ~5 min)

### World — the one container for all state
Everything mutable lives in the `World`:
- **Resources** — global singletons, one instance per type (e.g. `Score`, `SceneState`, `Fonts`).
- **Entities + Components** — an entity is an id; components are structs attached to it.
- **Events** — short-lived typed messages, one channel per type.

Systems receive *handles* into the World; they never reach across to each other directly.
If system A must tell system B something, A writes a resource or sends an event, and B reads it.
That indirection is the whole point — it keeps systems independently understandable and ordered
only by **phase**, not by hidden call chains.

### System — a free function that *requests* what it needs by type
```cpp
void DamageEnemies(Arimu::Query<EnemyTag, Health> enemies,   // entities with BOTH components
                   Arimu::Res<BattleState> battle,           // read-only resource
                   Arimu::ResMut<Score> score,               // mutable resource
                   Arimu::EventReader<Hit> hits,             // inbox of Hit events
                   Arimu::Commands cmd) { ... }              // deferred spawn/despawn
```
The framework inspects each parameter type and injects the right thing (this is `MakeParam`
in `arimu/System.hpp`). **The legal parameter types are exactly these six** — see §5.

A system is registered into a **(scene, phase)** slot. Within a phase, systems run in the
**order they were registered**.

### Phase — fixed per-frame order
| Phase | What goes here |
|-------|----------------|
| `PreLogic` | derive per-frame state (timers, layout, beat phase) |
| `Input` | read Siv3D input → **send events** |
| `FixedLogic` | fixed-timestep logic (runs 0..N times/frame via accumulator); optional |
| `Logic` | the game: consume events, mutate resources/components, send events |
| `SceneTransition` | turn "advance scene" events into a pending scene change |
| `Render` | read-only: draw the World with Siv3D |
| `Cleanup` | end-of-frame bookkeeping |

`App::Tick(sceneIndex, frameDt)` executes them in this order, then flushes event channels. Your
game calls it once per frame from its own loop and applies any pending scene change afterward.
(Source: `arimu/App.cpp`.)

### Scene — a schedule slot
Each scene index `0..15` (`MaxSceneCount = 16`) has its **own** set of registered systems. Each
frame YOU decide which scene is active and pass that index to `app.Tick(sceneIndex, dt)`; only
that scene's systems run. Scene policy is **not** baked into the framework — you own the scene
enum, the "current scene" resource, and the transition rule (typically: read a `SceneState`
resource to get the index, and after `Tick` apply any pending change).

### Plugin — the install unit
A plugin is any type with `static void Build(App&)`. Inside `Build` you register resources and
systems. `app.AddPlugin<MyPlugin>()` just calls `MyPlugin::Build(app)`. Group related systems
into one plugin (e.g. all battle logic in `BattlePlugin`).

---

## 2. Hard rules / anti-patterns

Violating these compiles-but-breaks or won't compile. The framework deliberately makes the
right thing easy.

1. **Systems are free functions, not lambdas or methods.** Registration takes a
   `void (*)(Params...)` function pointer. A *capturing* lambda cannot be a system. Use a
   file-scope `static` function. (Mirrors every system in the reference game.)

2. **Never make a structural change (create/destroy entity, add/remove component) while
   iterating — use `Commands`.** `Commands` records the intent and applies it *after* your
   system returns (auto-flushed by `WrapSystem`). Reading/writing existing component *values*
   during iteration is fine; changing the *set* of components/entities is not.

3. **No global mutable singletons. Use a resource.** Anything you'd reach for a `static`
   variable for is a resource: `world.InsertResource<T>()`, then `Res<T>`/`ResMut<T>` in systems.
   *(The reference game keeps one `static World* g_world` as an escape hatch for raw-registry
   access in init code — treat that as a smell to avoid, not a pattern to copy.)*

4. **Systems communicate only through the World.** No system calls another. Producer writes a
   resource or sends an event; consumer reads it next phase/frame. If you find yourself wanting
   to "call" another system, you want an **event**.

5. **Don't assume an entity exists.** Iterate with a `Query` (it yields only matching entities)
   or check `registry.valid(e)`. `Commands` ops already null-check before applying.

6. **Scope event-driven init to the right scene.** A system that consumes a one-shot event
   (e.g. "New Game pressed") and is registered **globally** will let *every* scene re-consume that
   event from its own cursor and re-run the init — a real bug. Register such systems with
   `AddSystem(fn, sceneIndex, ...)`, not `AddGlobalSystem`. (See the comment in
   `CorePlugin.cpp` around `NewGameInit`.)

7. **Render phase is read-only.** Draw from resources/components; don't mutate game state there.
   Put state changes in `Logic`.

8. **Resource type identity = the slot.** One instance per type in the World. Two different
   pieces of state must be two different struct types (even if both are `int`).

---

## 3. Cookbook — "to do X, write Y"

Each recipe: a minimal snippet + a pointer to the **reference implementation** in the original
*Arimu* game. Those `game/...` files are **not** shipped in this framework package — they live in
the Arimu game repo (`Arimu_Siv3D_forProto/Arimu/Arimu/`) and are cited only as worked examples.

### A. Add a component
A component is a plain struct. Tag components are empty.
```cpp
struct UnitTag {};                       // tag (presence is the data)
struct Health { int32_t current, max; }; // data
```
Attach at spawn time (via `Commands` in a system, or `registry.emplace<T>` in `Build`).
▶ Reference: `game/Components.hpp`.

### B. Add a system
Free function + register it into a (scene, phase) slot.
```cpp
static void RegenSystem(Arimu::Query<UnitTag, Health> units) {
    for (auto [e, tag, hp] : units.each())
        hp.current = s3d::Min(hp.current + 1, hp.max);
}
// in some plugin's Build():
app.AddSystem(RegenSystem, AsIndex(Scene::Battle), Arimu::Phase::Logic, "Regen");
```
Registration variants:
- `app.AddSystem(fn, sceneIndex, phase, name)` — one scene.
- `app.AddGlobalSystem(fn, phase, name)` — all 16 scene slots (use for truly global things like a time-accumulator).
- `app.AddSystemInScenes(fn, {Idx1, Idx2}, phase, name)` — a chosen set.

▶ Reference: `game/plugins/BattlePlugin.cpp` (`PlayerActionSystem` shows multi-query +
`EventReader` + `ResMut` together), `game/plugins/RenderPlugin.cpp` (render systems).

### C. Add a resource
A resource is a struct; register one instance in a plugin's `Build`, then request it in systems.
```cpp
struct Score { int32_t value = 0; };
// Build():
world.InsertResource<Score>();              // or InsertResource<Score>(initialArgs...)
// system:
void AddPoint(Arimu::ResMut<Score> s) { s->value += 1; }   // ResMut = write, Res = read
```
▶ Reference: `game/Resources.hpp` (definitions) + `game/plugins/CorePlugin.cpp`
(`CorePlugin::Build` registers them all).

### D. Add an event
A struct + a writer somewhere + a reader somewhere. Channels are created on first use.
```cpp
struct ButtonPressed { int32_t index; };
// producer (e.g. Input phase):
void Input(Arimu::EventWriter<ButtonPressed> out) { out.Send({ 2 }); }
// consumer (e.g. Logic phase):
void Handle(Arimu::EventReader<ButtonPressed> in) {
    for (auto& ev : in.Read()) { /* ... */ }   // Read() drains THIS system's cursor
}
```
Each reader has an independent cursor, so two systems can both read the same event. An event
survives until every reader has read it (in practice one frame). Don't store the event for later;
react to it in the frame you read it.
▶ Reference: `game/Events.hpp` (defs), writers in `game/plugins/InputPlugin.cpp`, readers in
`game/plugins/BattlePlugin.cpp`.

### E. Write a plugin
Header declares the struct; `.cpp` implements `Build`.
```cpp
// MyPlugin.hpp
#include "arimu/App.hpp"
namespace mygame { struct MyPlugin { static void Build(Arimu::App&); }; }

// MyPlugin.cpp
void MyPlugin::Build(Arimu::App& app) {
    app.GetWorld().InsertResource<Score>();
    app.AddSystem(AddPoint, 0, Arimu::Phase::Logic, "AddPoint");
}
```
Install in `Main()`: `app.AddPlugin<MyPlugin>()`. Plugin order matters when one plugin's `Build`
depends on a resource another registered — register the provider first. ▶ Reference:
`game/plugins/CorePlugin.hpp` + `Main.cpp` (the `AddPlugin` chain and its ordering comment).

### F. Add a scene
1. Add an enum value (`0..15`):
   ```cpp
   enum class Scene : uint8_t { Title = 0, Battle = 1, /* ... */ };
   constexpr uint8_t AsIndex(Scene s) { return static_cast<uint8_t>(s); }
   ```
2. Keep a "current scene" resource (`struct SceneState { Scene current; std::optional<Scene> pending; };`).
3. In your game loop, read the index for `Tick` and apply pending changes after it:
   ```cpp
   while (s3d::System::Update()) {
       const uint8_t s = AsIndex(app.GetWorld().GetResource<SceneState>().current);
       app.Tick(s, s3d::Scene::DeltaTime());
       auto& st = app.GetWorld().GetResource<SceneState>();
       if (st.pending) { st.current = *st.pending; st.pending.reset(); /* OnEnter hooks */ }
   }
   ```
4. Drive transitions with an event + a `SceneTransition`-phase system that sets `pending`.

▶ Reference: `game/SceneEnum.hpp`, `game/plugins/ScenePlugin.cpp`
(`SceneTransitionSystem`, `GetCurrentSceneIndex`, `ApplyPendingSceneChange`), `Main.cpp`.

### G. Spawn / despawn entities
- **Inside a system** (may be mid-iteration) → `Commands`:
  ```cpp
  void Spawn(Arimu::Commands cmd) {
      auto e = cmd.Create();                       // id is real immediately
      cmd.AddComponent<Health>(e, Health{10,10});  // deferred to end of system
  }
  ```
- **In a plugin's `Build`** (no system running, no iteration) → raw registry is fine:
  ```cpp
  auto& reg = world.Registry();
  auto e = reg.create(); reg.emplace<Health>(e, Health{10,10});
  ```
▶ Reference: `game/plugins/BattlePlugin.cpp` (`BattleEntitySetupSystem` uses `Commands`),
`game/plugins/CorePlugin.cpp` (`InitializeRun` uses raw registry at Build/init time).

### H. (Optional) Fixed-timestep logic
Register systems in `Phase::FixedLogic`; the framework runs them 0..N times per frame to drain a
`1/60s` accumulator (`arimu/FixedTime.hpp`). Read `Arimu::Res<Arimu::FixedTime>` for the tick.
The reference game leaves this phase empty — use it only when you need physics-grade determinism.

---

## 4. Full walkthrough — `examples/minimal/Main.cpp`

The shipped example (`examples/minimal/Main.cpp`) is a complete, compiling game that exercises
every primitive once: a bouncing-ball + click-counter. Read it top to bottom. Its shape is the
shape of every Arimu game:

1. **Components** (`Position`, `Velocity`) — plain structs.
2. **Resources** (`ClickCount`, `DemoFont`, `SceneCtl`) — `SceneCtl` exists only to answer the
   framework's "which scene?" question for this single-scene game.
3. **Event** (`Clicked`) — produced in `Input`, consumed twice in `Logic` (two independent readers:
   one counts, one spawns).
4. **Systems** in phase order: `CollectInput` (Input) → `CountClicks` / `SpawnOnClick` / `MoveBalls`
   (Logic) → `DrawBalls` / `DrawHud` (Render).
5. **`Commands`** in `SpawnOnClick` — structural change deferred safely.
6. **`DemoPlugin::Build`** registers the resources, spawns 3 starting balls via raw registry, and
   wires the systems into scene slot 0.
7. **`Main`** (the game) does Siv3D window setup, builds the `App`, adds the plugin, then runs its
   own `while (s3d::System::Update())` loop calling `app.Tick(scene, Scene::DeltaTime())` + `ApplyScene`.

To build a *new* game, replace the demo's components/resources/events/systems with yours, split
them across plugins per subsystem, and add a real scene enum (recipe F). For a multi-scene,
multi-plugin reference at production scale, study the original Arimu game's `game/plugins/`.

---

## 5. Appendix — legal system parameter types

These are the **only** types a system parameter may be (enforced by a `static_assert` in
`arimu/System.hpp`). Anything else fails to compile.

| Parameter type | Meaning | Injected from |
|----------------|---------|---------------|
| `Res<T>` | read-only resource `T` | `World::GetResource<T>()` |
| `ResMut<T>` | mutable resource `T` | `World::GetResource<T>()` |
| `Query<A, B, ...>` | view of entities having **all** of `A, B, ...` | `registry.view<A,B,...>()` |
| `EventReader<T>` | inbox draining this system's cursor for event `T` | per-system event cursor |
| `EventWriter<T>` | outbox to send event `T` | event channel for `T` |
| `Commands` | deferred entity/component mutations | `World::MakeCommands()`, auto-flushed |

Handle API quick reference:
- `Res<T>` / `ResMut<T>`: `r->field`, `*r`, `r.Get()`. `Res` is `const`.
- `Query`: `for (auto [e, a, b] : q.each())`, `q.size()`, `q.Get<C>(e)`, `q.Contains(e)`.
- `EventReader<T>`: `std::vector<T> Read()` (drains + advances cursor).
- `EventWriter<T>`: `Send(T)`.
- `Commands`: `Create() -> entity`, `AddComponent<C>(e, c)`, `RemoveComponent<C>(e)`, `Destroy(e)`.

App / World API you call directly (mostly in `Build` and `Main`):
- `App()` (no window — the framework doesn't own one), `app.AddPlugin<P>()`,
  `app.AddSystem(fn, scene, phase, name)`, `app.AddGlobalSystem(fn, phase, name)`,
  `app.AddSystemInScenes(fn, {…}, phase, name)`, `app.GetWorld()`,
  `app.Tick(sceneIndex, frameDt)` — advance one frame; call once per frame from YOUR loop.
- `world.InsertResource<T>(args...)`, `world.GetResource<T>()`, `world.HasResource<T>()`,
  `world.Registry()` (raw EnTT, for Build-time spawning).
