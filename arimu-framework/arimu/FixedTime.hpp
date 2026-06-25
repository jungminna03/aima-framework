#pragma once
#include <cstdint>

namespace Arimu {

// Fixed-timestep accumulator resource. Res<FixedTime> in systems.
struct FixedTime {
    double dt = 1.0 / 60.0;   // 16.67 ms target tick rate
    double accumulator = 0.0;
    uint64_t tick = 0;

    // Add frame delta to accumulator (called from App::Run per frame).
    void Accumulate(double frameDt) { accumulator += frameDt; }

    // Try to consume one tick's worth. Returns true if a tick should fire this call.
    // Usage: while (ft.Consume()) { RunFixedLogic(); }
    bool Consume() {
        if (accumulator >= dt) {
            accumulator -= dt;
            ++tick;
            return true;
        }
        return false;
    }
};

} // namespace Arimu
