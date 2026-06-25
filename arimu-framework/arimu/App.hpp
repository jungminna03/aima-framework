#pragma once
#include "arimu/World.hpp"
#include "arimu/Schedule.hpp"
#include "arimu/System.hpp"
#include "arimu/Plugin.hpp"
#include "arimu/FixedTime.hpp"
#include <array>
#include <initializer_list>
#include <cstdint>

namespace Arimu {

// Number of scene slots the framework reserves. Game Scene enum values must fit in [0, MaxSceneCount).
constexpr int MaxSceneCount = 16;

// The ECS application: owns the World and one Schedule per scene slot.
//
// The framework is renderer/platform agnostic — it does NOT open a window or own the
// main loop. The GAME owns its platform loop (e.g. Siv3D's `while (System::Update())`)
// and calls Tick() once per frame, supplying the active scene index and the frame delta.
class App {
public:
    App() = default;

    // Plugin installation.
    template <PluginType T>
    App& AddPlugin() {
        T::Build(*this);
        return *this;
    }

    // Register system in a single scene slot.
    template <typename... Params>
    App& AddSystem(void (*fn)(Params...), uint8_t sceneIndex, Phase phase, const char* name = "unnamed") {
        uint64_t id = SystemIdGen::Next();
        m_schedules[sceneIndex].Register(phase, id, name, WrapSystem(fn));
        return *this;
    }

    // Register system in ALL scene slots (used by globally-active systems like Cleanup/NewGameInit).
    template <typename... Params>
    App& AddGlobalSystem(void (*fn)(Params...), Phase phase, const char* name = "unnamed") {
        for (int i = 0; i < MaxSceneCount; ++i) {
            uint64_t id = SystemIdGen::Next();
            m_schedules[i].Register(phase, id, name, WrapSystem(fn));
        }
        return *this;
    }

    // Register system across a specific set of scene indices.
    template <typename... Params>
    App& AddSystemInScenes(void (*fn)(Params...), std::initializer_list<uint8_t> sceneIndices,
                           Phase phase, const char* name = "unnamed") {
        for (auto idx : sceneIndices) {
            uint64_t id = SystemIdGen::Next();
            m_schedules[idx].Register(phase, id, name, WrapSystem(fn));
        }
        return *this;
    }

    World& GetWorld() { return m_world; }

    // Code hot-reload support: drop every schedule's systems (they hold function
    // pointers into the game module about to be unloaded). World state survives.
    void ClearSystems() {
        for (auto& s : m_schedules) s.Clear();
    }

    size_t SystemCount() const {
        size_t n = 0;
        for (const auto& s : m_schedules) n += s.Count();
        return n;
    }

    // Advance the simulation by exactly one frame for the given active scene.
    //   sceneIndex : which scene's systems to run this frame (game decides; see SceneState pattern).
    //   frameDt    : seconds elapsed since last frame (game supplies, e.g. s3d::Scene::DeltaTime()).
    // Runs the phases in order, drains the fixed-timestep accumulator, and flushes event channels.
    // Apply any pending scene change in the GAME loop, after this returns.
    void Tick(uint8_t sceneIndex, double frameDt);

private:
    World m_world;
    std::array<Schedule, MaxSceneCount> m_schedules;
};

} // namespace Arimu
