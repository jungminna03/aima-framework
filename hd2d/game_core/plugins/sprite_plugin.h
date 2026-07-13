#pragma once

#include "arimu/App.hpp"

namespace hd2d {

// Billboard sprite lifecycle: deferred sheet load, frame animation, and the
// 8-direction facing pick (camera-relative). Facing lives here in Logic — NOT in
// Render — so the Render phase stays read-only (USAGE_FOR_AI rule 7).
struct SpritePlugin {
    static void Build(Arimu::App& app);
};

} // namespace hd2d
