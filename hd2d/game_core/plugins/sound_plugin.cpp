#include "game_core/plugins/sound_plugin.h"

#include "game/combat_components.h"   // BattleMode
#include "game/components.h"          // Controlled, Transform, Locomotion
#include "game_core/plugins/render_plugin.h" // g_tod + tod_is_night (마을 낮/밤 편곡 경계)
#include "game/res_path.h"            // res::sound_dir()
#include "game/resources.h"           // MapScene (current level — village vs field)
#include "game/scene_enum.h"
#include "game/sound_ids.h"

#include "platform/audio.h"           // aima::AudioDevice / aima::AudioContext (the host
                                      // opens the device + exposes it as a resource)

#include <cmath>
#include <string>
#include <vector>

namespace hd2d {

// (g_tod/tod_is_night는 render_plugin.h — 게임 시계의 하루 분율(자정=0), 밤 = 20:00~06:00.)

namespace {

// Music track selection. The host plays the track index in SoundQueue.music; the
// index->wav contract lives in music_names (SoundQueueDrainSystem): 0=overworld,
// 1=battle, 2=village day, 3=village night.
void MusicMoodSystem(Arimu::Res<BattleMode> battle, Arimu::Res<MapScene> map,
                     Arimu::ResMut<SoundQueue> snd) {
    if (battle->active) { snd->music = 1; return; }     // battle overrides
    // In a village, mirror the world clock: 밤 편곡 = 20:00~06:00(tod_is_night —
    // 비주얼 밤 도달 시각과 동일 경계). Elsewhere, the overworld loop.
    if (map->glb_path.find("village") != std::string::npos)
        snd->music = tod_is_night(g_tod) ? 3 : 2;
    else
        snd->music = 0;
}

// Footsteps pace by DISTANCE WALKED, so sprint naturally steps faster. Local
// statics (not a resource): purely cosmetic cadence state — a code hot-swap
// resetting it costs at most one early footstep.
void FootstepSystem(Arimu::Query<Controlled, Transform, Locomotion> players,
                    Arimu::ResMut<SoundQueue> snd) {
    static entt::entity tracked = entt::null;
    static Float3 prev{};
    static float acc = 0.0f;
    for (auto [e, tf, loco] : players.each()) {
        if (e != tracked) { tracked = e; prev = tf.position; acc = 0.0f; }
        const float dx = tf.position.x - prev.x, dz = tf.position.z - prev.z;
        prev = tf.position;
        if (loco.move == Locomotion::Move::Walk) {
            acc += std::sqrt(dx * dx + dz * dz);
            if (acc >= 1.6f) {           // ~stride length at 32px/m scale
                acc = 0.0f;
                snd->play(SoundId::Step, 0.4f);
            }
        } else {
            acc = 0.0f;
        }
        break;
    }
}

// Drain the game's SoundQueue into the host's SDL3 audio device. Moved out of the
// old main.cpp loop into the game module (the device is HOST-side but exposed as
// the aima::AudioContext resource; null when audio is off / AIMA_MUTE / headless,
// in which case this is a cheap no-op). The bank — the SoundId->name vocabulary —
// is GAME knowledge, so the game loads it here once on the first frame the device
// is live. The host stays sound-vocabulary-agnostic (opaque ids + file paths).
void SoundQueueDrainSystem(Arimu::ResMut<SoundQueue> snd,
                           Arimu::Res<aima::AudioContext> audio) {
    aima::AudioDevice* dev = audio->dev;
    if (dev == nullptr) {                 // audio off / AIMA_MUTE / headless
        snd->events.clear();
        return;
    }

    // Lazy one-time bank load: SoundId enum -> file-stem list (game vocabulary).
    static bool bank_loaded = false;
    if (!bank_loaded) {
        bank_loaded = true;
        std::vector<std::string> sfx_names;
        sfx_names.reserve(static_cast<size_t>(SoundId::Count));
        for (int i = 0; i < static_cast<int>(SoundId::Count); ++i)
            sfx_names.emplace_back(sound_name(static_cast<SoundId>(i)));
        // Music track order is the game's contract (SoundQueue.music): 0 = overworld
        // loop, 1 = battle loop, 2 = village day, 3 = village night. Keep in sync
        // with MusicMoodSystem (the selector).
        const std::vector<std::string> music_names{
            "music_adventure", "music_battle",
            "music_village_day", "music_village_night"};
        dev->load_bank(res::sound_dir(), sfx_names, music_names);
    }

    for (const SoundEvent& se : snd->events)
        dev->play(static_cast<int>(se.id), se.gain);   // opaque id into the framework device
    snd->events.clear();
    dev->set_master(snd->master_gain);   // settings volume scales music too (SFX is pre-scaled)
    dev->set_music(snd->music);
    // 음악 펌프(루프 피드/페이드)는 호스트가 매 프레임 1회 poll한다(host.cpp).
    // 여기서 또 부르면 프레임당 2회가 되어 페이드가 2배로 빨라짐 → 단일화(PAL audio).
}

} // namespace

void SoundPlugin::Build(Arimu::App& app) {
    app.GetWorld().EnsureResource<SoundQueue>();
    const uint8_t scene = AsIndex(GameScene::World);
    app.AddSystem(MusicMoodSystem, scene, Arimu::Phase::Logic, "MusicMood");
    app.AddSystem(FootstepSystem, scene, Arimu::Phase::Logic, "Footstep");
    // Drain runs in Cleanup (after every emitter in Logic/Render has pushed).
    app.AddSystem(SoundQueueDrainSystem, scene, Arimu::Phase::Cleanup, "SoundDrain");
}

} // namespace hd2d
