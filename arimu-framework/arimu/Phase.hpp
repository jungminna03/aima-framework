#pragma once
#include <cstdint>

namespace Arimu {

enum class Phase : uint8_t {
    PreLogic,         // compute derived per-frame state (timers, layout, etc.)
    Input,            // read platform input -> send Events
    FixedLogic,       // fixed-timestep logic; runs 0..N times/frame via accumulator (optional)
    Logic,            // event consumption, state changes
    SceneTransition,  // turn "advance scene" events into a pending scene change
    Render,           // read-only: draw the World with your renderer
    Cleanup,          // end-of-frame bookkeeping (event flush handled by the framework)
};

inline constexpr int PhaseCount = 7;

} // namespace Arimu
