#pragma once

#include "arimu/App.hpp"

namespace hd2d {

// ECS-native UI framework v1: nav-input -> nav -> lerp -> layout -> render
// (all Logic). Inserts the NavInput + UiNav resources. Widgets are spawned by
// consumers (e.g. CommandMenuPlugin); with no widgets and active_group == 0
// every system is a cheap no-op. Add AFTER CommandMenuPlugin: menu spawns and
// mode flips settle first each frame, then nav reacts and rendering draws the
// settled state (costs one frame of input latency, avoids visual flicker).
struct UiPlugin {
    static void Build(Arimu::App& app);
};

} // namespace hd2d
