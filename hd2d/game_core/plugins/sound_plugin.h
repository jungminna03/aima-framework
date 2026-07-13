#pragma once

#include "arimu/App.hpp"

namespace hd2d {

// Sound emission glue that has no other home: the music mood (BattleMode ->
// SoundQueue.music) and the player's footsteps (distance-paced). Every other
// emission lives at its event's source site (combat/battle/menu/dialogue).
// The HOST drains the queue — see game/sound_ids.h for the contract.
struct SoundPlugin {
    static void Build(Arimu::App& app);
};

} // namespace hd2d
