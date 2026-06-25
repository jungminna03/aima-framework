#pragma once
#include "arimu/Phase.hpp"
#include <vector>
#include <functional>
#include <array>
#include <cstdint>

namespace Arimu {

class World;

// Type-erased system callable. Takes World + per-system id (for event reader cursors).
using SystemFn = std::function<void(World&, uint64_t)>;

struct SystemEntry {
    uint64_t id;
    const char* name;
    SystemFn fn;
};

// Per-scene schedule: phase -> ordered list of systems.
class Schedule {
public:
    void Register(Phase phase, uint64_t id, const char* name, SystemFn fn) {
        m_phases[static_cast<int>(phase)].push_back({ id, name, std::move(fn) });
    }

    // Run all systems in a given phase in registration order.
    void Run(World& world, Phase phase) {
        for (auto& sys : m_phases[static_cast<int>(phase)]) {
            sys.fn(world, sys.id);
        }
    }

    // Drop every registered system. Code hot-reload uses this BEFORE unloading the
    // game module: the stored SystemFns wrap function pointers into that module and
    // would dangle past FreeLibrary. The World (registry/resources) is untouched.
    void Clear() {
        for (auto& phase : m_phases) phase.clear();
    }

    size_t Count() const {
        size_t n = 0;
        for (const auto& phase : m_phases) n += phase.size();
        return n;
    }

private:
    std::array<std::vector<SystemEntry>, PhaseCount> m_phases;
};

} // namespace Arimu
