#include "platform/audio.h"

#include "core/log.h"

#include <SDL3/SDL.h>

#include <climits>

namespace aima {

namespace {
// Keep every voice's converter static: one fixed source spec for the whole bank.
// (s16 / mono / 44100 — the framework's bank convention.)
constexpr SDL_AudioSpec kSrcSpec{SDL_AUDIO_S16, 1, 44100};
}

bool AudioDevice::init() {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        AIMA_WARN("[audio] SDL audio init failed ({}) — silent mode", SDL_GetError());
        return false;
    }
    auto open_stream = [&]() -> SDL_AudioStream* {
        SDL_AudioStream* s = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                                       &kSrcSpec, nullptr, nullptr);
        if (s) SDL_ResumeAudioStreamDevice(s);   // OpenAudioDeviceStream starts paused
        return s;
    };
    for (int i = 0; i < kVoices; ++i) {
        voices_[i] = open_stream();
        if (voices_[i] == nullptr) {
            AIMA_WARN("[audio] voice stream {} failed ({}) — silent mode", i, SDL_GetError());
            shutdown();
            return false;
        }
    }
    music_stream_ = open_stream();
    ok_ = true;
    AIMA_INFO("[audio] device opened ({} voices + music)", kVoices);
    return true;
}

void AudioDevice::load_bank(const std::string& dir,
                            const std::vector<std::string>& sfx_names,
                            const std::vector<std::string>& music_names) {
    if (!ok_) return;
    auto load = [&](Wav& w, const std::string& name) {
        const std::string path = dir + "/" + name + ".wav";
        SDL_AudioSpec spec{};
        if (!SDL_LoadWAV(path.c_str(), &spec, &w.buf, &w.len)) {
            AIMA_WARN("[audio] missing wav: {} ({})", path, SDL_GetError());
            return;
        }
        if (spec.format != kSrcSpec.format || spec.channels != kSrcSpec.channels ||
            spec.freq != kSrcSpec.freq) {
            AIMA_WARN("[audio] {}: spec mismatch (want s16/mono/44100) — skipped", path);
            SDL_free(w.buf);
            w.buf = nullptr;
            w.len = 0;
            return;
        }
        w.ok = true;
    };
    bank_.assign(sfx_names.size(), Wav{});
    int n = 0;
    for (size_t i = 0; i < sfx_names.size(); ++i) {
        load(bank_[i], sfx_names[i]);
        n += bank_[i].ok ? 1 : 0;
    }
    music_.assign(music_names.size(), Wav{});
    int m = 0;
    for (size_t i = 0; i < music_names.size(); ++i) {
        load(music_[i], music_names[i]);
        m += music_[i].ok ? 1 : 0;
    }
    AIMA_INFO("[audio] bank loaded ({}/{} sfx, {}/{} music)", n,
              static_cast<int>(sfx_names.size()), m, static_cast<int>(music_names.size()));
}

void AudioDevice::play(int id, float gain) {
    if (!ok_ || id < 0 || id >= static_cast<int>(bank_.size())) return;
    const Wav& w = bank_[id];
    if (!w.ok) return;
    // Prefer an idle voice; otherwise steal the one closest to finishing.
    SDL_AudioStream* pick = nullptr;
    int best = INT_MAX;
    for (SDL_AudioStream* s : voices_) {
        const int queued = SDL_GetAudioStreamQueued(s);
        if (queued <= 0) { pick = s; break; }
        if (queued < best) { best = queued; pick = s; }
    }
    SDL_ClearAudioStream(pick);
    SDL_SetAudioStreamGain(pick, gain);
    SDL_PutAudioStreamData(pick, w.buf, static_cast<int>(w.len));
}

void AudioDevice::set_music(int mode) {
    // Just record the request; poll() does the fade-out -> switch -> fade-in so a
    // track change is a smooth dissolve, not an abrupt cut.
    if (!ok_ || music_stream_ == nullptr) return;
    music_target_ = mode;
}

void AudioDevice::set_master(float gain) {
    master_ = gain < 0.0f ? 0.0f : (gain > 1.0f ? 1.0f : gain);
}

void AudioDevice::poll() {
    if (!ok_ || music_stream_ == nullptr) return;

    constexpr float kMusicGain = 0.5f;     // full music level
    constexpr float kFadeStep = 0.03f;     // per-frame ramp (~0.3s out, ~0.3s in)

    if (music_target_ != music_mode_) {
        // Fade the current track down; once silent, swap to the target and let the
        // fade-in below bring it up. (fade OUT then fade IN — user 2026-06-22.)
        music_gain_ -= kFadeStep;
        if (music_gain_ <= 0.0f) {
            music_gain_ = 0.0f;
            music_mode_ = music_target_;
            SDL_ClearAudioStream(music_stream_);
            if (music_mode_ >= 0 && music_mode_ < static_cast<int>(music_.size()) &&
                music_[music_mode_].ok) {
                SDL_PutAudioStreamData(music_stream_, music_[music_mode_].buf,
                                       static_cast<int>(music_[music_mode_].len));
                AIMA_INFO("[audio] music -> track {}", music_mode_);
            }
        }
    } else if (music_mode_ >= 0 && music_gain_ < kMusicGain) {
        music_gain_ += kFadeStep;          // fade the new track in
        if (music_gain_ > kMusicGain) music_gain_ = kMusicGain;
    }
    SDL_SetAudioStreamGain(music_stream_, music_gain_ * master_);

    // Keep the active loop fed; topping up BEFORE it drains = seamless.
    if (music_mode_ < 0 || music_mode_ >= static_cast<int>(music_.size())) return;
    const Wav& w = music_[music_mode_];
    if (!w.ok) return;
    if (SDL_GetAudioStreamQueued(music_stream_) < static_cast<int>(w.len))
        SDL_PutAudioStreamData(music_stream_, w.buf, static_cast<int>(w.len));
}

void AudioDevice::shutdown() {
    for (SDL_AudioStream*& s : voices_) {
        if (s) SDL_DestroyAudioStream(s);   // also closes its logical device
        s = nullptr;
    }
    if (music_stream_) { SDL_DestroyAudioStream(music_stream_); music_stream_ = nullptr; }
    for (Wav& w : bank_) {
        if (w.buf) SDL_free(w.buf);
    }
    bank_.clear();
    for (Wav& w : music_) {
        if (w.buf) SDL_free(w.buf);
    }
    music_.clear();
    ok_ = false;
}

} // namespace aima
