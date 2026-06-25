#pragma once

// SDL3 audio backend (HOST side). This is FRAMEWORK code: it knows nothing about
// any game's sound vocabulary. The game names WHAT happened by pushing events into
// its OWN queue resource; the host drains that queue and calls play() with an
// OPAQUE integer id plus a gain. The host registers the sound BANK by name once at
// startup (load_bank), so this layer only ever deals in ids + file paths.
//
// One physical device; every SFX voice and the music channel is its own
// SDL_OpenAudioDeviceStream logical device — SDL mixes them. No device (headless
// CI) or a never-init'd AudioDevice => every call is a cheap no-op.
//
// Bank format: 16-bit signed, mono, 44.1 kHz WAV (assets/<dir>/<name>.wav). A
// mismatched spec is skipped with a warning (keep your bank uniform).

#include <cstdint>
#include <string>
#include <vector>

struct SDL_AudioStream;

namespace aima {

class AudioDevice;

// Host -> game audio bridge. The host opens an AudioDevice on the stack (opt-in
// via HostConfig::enable_audio) and exposes a pointer to it as this World
// resource so a game sound system can drain its own SoundQueue into the device.
// `dev == nullptr` (headless / AIMA_MUTE / audio-off) => the game system no-ops.
struct AudioContext {
    AudioDevice* dev = nullptr;
};

class AudioDevice {
public:
    bool init();                              // open device + streams; false = stay silent

    // Register + load the game's sound bank. `sfx_names[i]` is the file stem for
    // opaque id i (<dir>/<name>.wav); the bank is sized to sfx_names.size().
    // `music_names` are the looping music tracks (set_music selects by index).
    // The game owns this vocabulary and passes it in — the framework stays agnostic.
    void load_bank(const std::string& dir,
                   const std::vector<std::string>& sfx_names,
                   const std::vector<std::string>& music_names);

    void play(int id, float gain);            // id == the game's opaque sound id
    void set_music(int mode);                 // index into music_names; -1 = stop
    void set_master(float gain);              // settings master volume (0..1); scales music
    void poll();                              // keep the music loop fed (once per frame)
    void shutdown();

private:
    struct Wav {
        uint8_t* buf = nullptr;
        uint32_t len = 0;
        bool ok = false;
    };

    static constexpr int kVoices = 12;
    SDL_AudioStream* voices_[kVoices] = {};
    SDL_AudioStream* music_stream_ = nullptr;
    std::vector<Wav> bank_;     // one entry per registered sfx id
    std::vector<Wav> music_;    // one entry per registered music track
    int music_mode_ = -1;       // track currently playing on music_stream_
    int music_target_ = -1;     // requested track; poll() fades out->switch->in
    float music_gain_ = 0.0f;   // current music gain, ramped 0 <-> full
    float master_ = 1.0f;       // settings master volume (0..1); music *= master_
    bool ok_ = false;
};

} // namespace aima
