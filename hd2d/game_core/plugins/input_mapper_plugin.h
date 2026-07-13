#pragma once

#include "arimu/App.hpp"

namespace hd2d {

// Generic -> game input mapper.
//
// Post-migration, the aima HOST fills the GENERIC aima::InputState resource
// (wasd / mouse edges+levels / wheel / confirm / cancel / gamepad analog) from
// SDL every frame, but knows nothing about HD2D's verbs. This plugin installs a
// single system, FIRST in the Input phase, that reads aima::InputState and
// rewrites HD2D's game-layer hd2d::InputState resource with the game verbs
// (attack / roll / guard / lockon / interact / menu_* / inventory / ...).
//
// Because Arimu runs systems in REGISTRATION ORDER within a phase
// (Schedule::Run), every other Input/Logic consumer of hd2d::InputState sees a
// freshly mapped snapshot only if this plugin is added BEFORE them in
// GameRegister. Add it first (right after CorePlugin::RegisterResources).
struct InputMapperPlugin {
    static void Build(Arimu::App& app);
};

} // namespace hd2d
