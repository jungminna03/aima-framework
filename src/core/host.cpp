// aima::Host — the reusable host loop + code-hot-reload game-module loader.
//
// This is the renderer-LESS extraction of the source engine's main.cpp: it owns
// the window + the loop + the game-module ABI + BOTH hot-reload mechanisms, but
// it draws nothing — every pixel goes through the project's aima::Renderer. The
// loader (dlopen/LoadLibrary, per-generation copy, leak-old, mtime poll, macOS
// ad-hoc codesign) is carried over verbatim in spirit; only the renderer calls
// and the game-specific resource reads are gone.

#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>   // SDL_SetMainReady

#include "aima/host.h"
#include "aima/renderer.h"

#include "arimu/App.hpp"

#include "core/hot_reload.h"
#include "core/log.h"
#include "platform/audio.h"
#include "platform/input.h"
#include "platform/window.h"

// ---- code-hot-reload loader platform headers -------------------------------
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#if defined(__APPLE__)
#include <mach-o/dyld.h>     // _NSGetExecutablePath
#elif defined(__linux__)
#include <unistd.h>          // readlink
#endif
#endif

#include <cmath>
#include <cstdlib>
#include <string>

namespace aima {
namespace {

// --------------------------------------------------------------------------
// CODE hot-reload module loader. The game lives in a shared module
// (<name>.dll / lib<name>.dylib / lib<name>.so). The host loads a COPY (so the
// linker can overwrite the original), and when the original's timestamp changes
// — a build finished — it clears the schedules (their SystemFns point into the
// old module), leaks the old module on purpose, loads the new copy, and
// re-registers on the SAME World. Live state survives; layout changes don't
// (GameStateVersion gates those).
//
// CRITICAL (all platforms): each load copies the module to a UNIQUE path before
// loading. On Windows the original is locked while loaded; on POSIX dlopen()
// returns the SAME handle for an already-loaded path and does NOT re-read the
// file, so a rebuild at the original path would keep running STALE code. Copying
// every rebuild to a fresh path forces a genuine fresh load.
// --------------------------------------------------------------------------
struct GameModule {
    GameStateVersionFn version = nullptr;
    GameRegisterFn     reg     = nullptr;
    GameSpawnWorldFn   spawn   = nullptr;
    GameBindHostFn     bind    = nullptr;   // optional
    GameOnAssetReloadFn on_asset = nullptr; // optional
    GameServiceFrameFn  service  = nullptr; // optional — per-frame post-Tick service

#ifdef _WIN32
    HMODULE h = nullptr;
    std::wstring dll, loaded;
    FILETIME mtime{};

    static bool file_time(const std::wstring& p, FILETIME& out) {
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa)) return false;
        out = fa.ftLastWriteTime;
        return true;
    }

    bool load() {
        static int gen = 0;
        loaded = dll;
        loaded.replace(loaded.find(L".dll"), 4, L"_loaded" + std::to_wstring(++gen) + L".dll");
        if (!CopyFileW(dll.c_str(), loaded.c_str(), FALSE)) {
            AIMA_ERROR("[code-reload] copy failed ({})", GetLastError());
            return false;
        }
        h = LoadLibraryW(loaded.c_str());
        if (!h) { AIMA_ERROR("[code-reload] LoadLibrary failed ({})", GetLastError()); return false; }
        return resolve();
    }

    void* sym(const char* name) const { return reinterpret_cast<void*>(GetProcAddress(h, name)); }
    void unload_handle() { if (h) { /* leak on purpose */ h = nullptr; } }

    bool newer_on_disk() const {
        FILETIME t{};
        if (!file_time(dll, t)) return false;
        if (CompareFileTime(&t, &mtime) <= 0) return false;
        FILETIME now{};
        GetSystemTimeAsFileTime(&now);
        ULARGE_INTEGER a{t.dwLowDateTime, t.dwHighDateTime}, b{now.dwLowDateTime, now.dwHighDateTime};
        return (b.QuadPart - a.QuadPart) > 4000000ull;   // 0.4s in 100ns ticks
    }
    void stamp() { file_time(dll, mtime); }
#else  // ----------------------------- POSIX (macOS / Linux) ------------------
    void* h = nullptr;
    std::string dll, loaded;
    std::filesystem::file_time_type mtime{};

    static bool file_time(const std::string& p, std::filesystem::file_time_type& out) {
        std::error_code ec;
        out = std::filesystem::last_write_time(p, ec);
        return !ec;
    }

    bool load() {
        static int gen = 0;
        std::filesystem::path src(dll);
        std::filesystem::path dst = src;
        dst.replace_filename(src.stem().string() + "_loaded" + std::to_string(++gen) +
                             src.extension().string());
        loaded = dst.string();

        std::error_code ec;
        std::filesystem::copy_file(src, dst,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) { AIMA_ERROR("[code-reload] copy failed ({})", ec.message()); return false; }

#if defined(__APPLE__)
        // macOS refuses to dlopen a freshly-copied .dylib whose code signature no
        // longer matches the bytes on disk. Re-sign the copy ad-hoc (requires the
        // Command Line Tools `codesign`).
        {
            std::string cmd = "codesign -s - -f \"" + loaded + "\" 2>/dev/null";
            if (std::system(cmd.c_str()) != 0)
                AIMA_WARN("[code-reload] ad-hoc codesign failed for {} — dlopen may reject it",
                          loaded);
        }
#endif
        h = dlopen(loaded.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!h) { AIMA_ERROR("[code-reload] dlopen failed ({})", dlerror()); return false; }
        dlerror();
        return resolve();
    }

    void* sym(const char* name) const { return dlsym(h, name); }
    void unload_handle() { if (h) { /* leak on purpose */ h = nullptr; } }

    bool newer_on_disk() const {
        std::filesystem::file_time_type t{};
        if (!file_time(dll, t)) return false;
        if (t <= mtime) return false;
        auto age = std::filesystem::file_time_type::clock::now() - t;
        return age > std::chrono::milliseconds(400);
    }
    void stamp() { file_time(dll, mtime); }
#endif

    // Resolve the C ABI; the three REQUIRED exports must be present.
    bool resolve() {
        version  = reinterpret_cast<GameStateVersionFn>(sym("GameStateVersion"));
        reg      = reinterpret_cast<GameRegisterFn>(sym("GameRegister"));
        spawn    = reinterpret_cast<GameSpawnWorldFn>(sym("GameSpawnWorld"));
        bind     = reinterpret_cast<GameBindHostFn>(sym("GameBindHost"));     // optional
        on_asset = reinterpret_cast<GameOnAssetReloadFn>(sym("GameOnAssetReload")); // optional
        service  = reinterpret_cast<GameServiceFrameFn>(sym("GameServiceFrame"));    // optional
        if (!version || !reg || !spawn) {
            AIMA_ERROR("[code-reload] required exports missing "
                       "(GameStateVersion/GameRegister/GameSpawnWorld)");
            return false;
        }
        stamp();
        return true;
    }
};

#ifndef _WIN32
// POSIX equivalent of GetModuleFileName(nullptr,...): absolute path of the running
// executable, used to resolve the game module sitting next to it.
std::string executable_path() {
#if defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
    std::error_code ec;
    std::filesystem::path canon = std::filesystem::canonical(buf.c_str(), ec);
    return ec ? std::string(buf.c_str()) : canon.string();
#elif defined(__linux__)
    std::error_code ec;
    std::filesystem::path self = std::filesystem::canonical("/proc/self/exe", ec);
    if (!ec) return self.string();
    char b[4096];
    ssize_t n = readlink("/proc/self/exe", b, sizeof(b) - 1);
    if (n <= 0) return {};
    b[n] = '\0';
    return std::string(b);
#else
    return {};
#endif
}
#endif

// Resolve the platform module file name next to the executable.
bool resolve_module_path(GameModule& game, const std::string& base) {
#ifdef _WIN32
    wchar_t exe_path[MAX_PATH];
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    std::wstring dir(exe_path);
    dir.erase(dir.find_last_of(L"\\/") + 1);
    std::wstring wbase(base.begin(), base.end());
    game.dll = dir + wbase + L".dll";
    return true;
#else
    std::filesystem::path exe(executable_path());
    if (exe.empty()) return false;
    std::filesystem::path dir = exe.parent_path();
#if defined(__APPLE__)
    const std::string fname = "lib" + base + ".dylib";
#else
    const std::string fname = "lib" + base + ".so";
#endif
    game.dll = (dir / fname).string();
    return true;
#endif
}

} // namespace

int Host::run(const HostConfig& cfg, Renderer& renderer) {
    init_logging();
    SDL_SetMainReady();

    Window window;
    if (!window.init(cfg.title, static_cast<uint32_t>(cfg.width),
                     static_cast<uint32_t>(cfg.height)))
        return 1;

    if (!renderer.init(window.sdl(), static_cast<int>(window.width()),
                       static_cast<int>(window.height()))) {
        AIMA_ERROR("renderer init failed");
        window.shutdown();
        return 1;
    }

    Arimu::App app;
    // The framework's generic per-frame input mailbox. The project's Input-phase
    // systems read it and map it to their own actions.
    app.GetWorld().InsertResource<InputState>();

    // Project register-hook: seed resources the game's GameRegister/GameSpawnWorld
    // depend on (e.g. a Gfx resource carrying the renderer's device) BEFORE they
    // run. The renderer is already initialized at this point.
    if (register_hook_) register_hook_(app);

    // ---- load + register the game --------------------------------------------
    GameModule game;
    int state_version = 0;
    const bool use_module = !cfg.game_module.empty();
    if (use_module) {
        if (!resolve_module_path(game, cfg.game_module) || !game.load()) {
            renderer.shutdown();
            window.shutdown();
            return 1;
        }
        if (game.bind) {
            // Hand the project's static-lib singleton context (e.g. ImGui) to the
            // module. The bind-provider pulls it from wherever the project owns it
            // (here: the renderer's ImGui context); nullptrs if unset (legacy).
            void* bc = nullptr; void* ba = nullptr; void* bf = nullptr; void* bu = nullptr;
            if (bind_provider_) bind_provider_(bc, ba, bf, bu);
            game.bind(bc, ba, bf, bu);
        }
        state_version = game.version();
        game.reg(&app);
        game.spawn(&app);
        AIMA_INFO("[code-reload] game module '{}' loaded (state v{}, {} systems)",
                  cfg.game_module, state_version, app.SystemCount());
    } else {
        if (cfg.inproc_register) cfg.inproc_register(app);
        if (cfg.inproc_spawn) cfg.inproc_spawn(app);
        AIMA_INFO("[host] in-process game registered ({} systems)", app.SystemCount());
    }

    // ---- asset/shader hot-reload watcher -------------------------------------
    HotReload hot;
    hot.init(cfg.watch_dirs);

    // ---- audio (opt-in) ------------------------------------------------------
    AudioDevice audio;
    const bool audio_on = cfg.enable_audio && std::getenv("AIMA_MUTE") == nullptr && audio.init();

    // Expose the (possibly null) device to the game via a World resource, so a
    // game sound system can drain its own queue into it. Null when audio is off.
    app.GetWorld().InsertResource<AudioContext>(
        AudioContext{audio_on ? &audio : nullptr});

    // ---- headless verification -----------------------------------------------
    const char* shot_path = std::getenv("AIMA_SHOT");
    int shot_frame = cfg.shot_frame;
    if (const char* sf = std::getenv("AIMA_SHOTFRAME")) shot_frame = std::atoi(sf);
    int max_frames = cfg.max_frames;
    if (const char* mf = std::getenv("AIMA_MAXFRAMES")) max_frames = std::atoi(mf);

    uint64_t prev = SDL_GetPerformanceCounter();
    const double freq = static_cast<double>(SDL_GetPerformanceFrequency());
    int frame_no = 0;

    AIMA_INFO("entering main loop");
    SDL_Gamepad* gamepad = nullptr;   // first connected pad (additive input source)
    uint32_t prev_pad_btn = 0;        // previous frame's held bitmask (rising-edge calc)
    bool running = true;
    while (running) {
        InputState input;

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
                case SDL_EVENT_QUIT:
                    running = false; break;
                case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                    if (ev.window.windowID == SDL_GetWindowID(window.sdl())) running = false;
                    break;
                case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: {
                    int w = 0, h = 0;
                    SDL_GetWindowSizeInPixels(window.sdl(), &w, &h);
                    window.set_size(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
                    renderer.resize(w, h);
                    break;
                }
                case SDL_EVENT_MOUSE_WHEEL:
                    input.wheel += ev.wheel.y; break;
                case SDL_EVENT_MOUSE_MOTION:
                    input.mouse_dx += ev.motion.xrel;
                    input.mouse_dy += ev.motion.yrel;
                    break;
                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                    if (ev.button.button == SDL_BUTTON_LEFT)  input.lmb_pressed = true;
                    if (ev.button.button == SDL_BUTTON_RIGHT) input.rmb_pressed = true;
                    break;
                case SDL_EVENT_MOUSE_BUTTON_UP:
                    if (ev.button.button == SDL_BUTTON_LEFT)  input.lmb_released = true;
                    if (ev.button.button == SDL_BUTTON_RIGHT) input.rmb_released = true;
                    break;
                case SDL_EVENT_KEY_DOWN:
                    if (ev.key.scancode == SDL_SCANCODE_RETURN) input.confirm = true;
                    if (ev.key.scancode == SDL_SCANCODE_ESCAPE) input.cancel = true;
                    break;
                case SDL_EVENT_GAMEPAD_ADDED:
                    if (!gamepad) {
                        gamepad = SDL_OpenGamepad(ev.gdevice.which);
                        if (gamepad) AIMA_INFO("[pad] gamepad connected");
                    }
                    break;
                case SDL_EVENT_GAMEPAD_REMOVED:
                    if (gamepad && ev.gdevice.which == SDL_GetGamepadID(gamepad)) {
                        SDL_CloseGamepad(gamepad);
                        gamepad = nullptr;
                        AIMA_INFO("[pad] gamepad disconnected");
                    }
                    break;
                default: break;
            }
        }

        // Held keyboard state -> generic movement intent.
        const bool* keys = SDL_GetKeyboardState(nullptr);
        input.move_fwd   = keys[SDL_SCANCODE_W];
        input.move_back  = keys[SDL_SCANCODE_S];
        input.move_left  = keys[SDL_SCANCODE_A];
        input.move_right = keys[SDL_SCANCODE_D];
        input.sprint     = keys[SDL_SCANCODE_LSHIFT];
        const SDL_MouseButtonFlags mb = SDL_GetMouseState(nullptr, nullptr);
        input.lmb_down = (mb & SDL_BUTTON_LMASK) != 0;
        input.rmb_down = (mb & SDL_BUTTON_RMASK) != 0;

        // Gamepad: first connected pad drives sticks + digital buttons. ADDITIVE —
        // the keyboard/mouse fills above are untouched. Setting pad_connected here
        // activates the project's existing stick mapping (move/camera); the digital
        // buttons are exposed via pad_btn_* bitmasks for the project to map.
        if (gamepad) {
            input.pad_connected = true;
            input.pad_lx = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX)  / 32767.0f;
            input.pad_ly = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY)  / 32767.0f;
            input.pad_rx = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTX) / 32767.0f;
            input.pad_ry = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTY) / 32767.0f;
            uint32_t b = 0;
            auto set = [&](PadButton bit, bool on) { if (on) b |= (1u << bit); };
            set(PadA, SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_SOUTH));
            set(PadB, SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_EAST));
            set(PadX, SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_WEST));
            set(PadY, SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_NORTH));
            set(PadBack,  SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_BACK));
            set(PadStart, SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_START));
            set(PadLB, SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER));
            set(PadRB, SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER));
            set(PadL3, SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_LEFT_STICK));
            set(PadR3, SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_RIGHT_STICK));
            set(PadDpadUp,    SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_UP));
            set(PadDpadDown,  SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_DOWN));
            set(PadDpadLeft,  SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_LEFT));
            set(PadDpadRight, SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT));
            const Sint16 TRIG = 16384;  // ~50% trigger pull = digital press
            set(PadLT, SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER)  > TRIG);
            set(PadRT, SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > TRIG);
            input.pad_btn_down = b;
            input.pad_btn_pressed = b & ~prev_pad_btn;
            prev_pad_btn = b;
        } else {
            prev_pad_btn = 0;
        }

        // dt
        uint64_t now = SDL_GetPerformanceCounter();
        float dt = static_cast<float>((now - prev) / freq);
        prev = now;
        if (dt > 0.1f) dt = 0.1f;   // clamp huge stalls
        // Determinism (PAL step 8): AIMA_FIXEDSTEP/HD2D_FIXEDSTEP feeds a CONSTANT dt so
        // the sim is machine-speed-independent — reproducible headless runs (TDD) and
        // identical behaviour across machines. Default keeps wall-clock dt for smooth
        // live play. (A full real-time accumulator needs App to split sim/render phases;
        // Tick currently couples them, so this opt-in constant step is the safe first cut.)
        static const bool s_fixed_step =
            std::getenv("AIMA_FIXEDSTEP") != nullptr || std::getenv("HD2D_FIXEDSTEP") != nullptr;
        if (s_fixed_step) dt = 1.0f / 60.0f;

        // Asset/shader hot-reload: drain the watcher and route to project hooks.
        hot.poll([&](const FileEvent& fe) {
            AIMA_INFO("[hot-reload] file changed: {}", fe.path);
            if (asset_hook_) asset_hook_(fe);
            if (use_module && game.on_asset)
                game.on_asset(&app, &renderer, fe.path.c_str());
        });

        // CODE hot-reload: a finished module build swaps in live.
        if (use_module) {
            static int reload_check = 0;
            if (++reload_check >= 30) {
                reload_check = 0;
                if (game.newer_on_disk()) {
                    GameModule next = game;        // carries the path; load() refreshes the rest
                    app.ClearSystems();
                    AIMA_INFO("[code-reload] schedules cleared; loading the new module...");
                    if (next.load()) {
                        if (next.version() != state_version) {
                            AIMA_WARN("[code-reload] STATE LAYOUT CHANGED (v{} -> v{}) — "
                                      "restart required; re-registering anyway.",
                                      state_version, next.version());
                        }
                        game.unload_handle();      // leak old on purpose (live vtables point in)
                        game = next;
                        if (game.bind) {
                            // Re-hand the host's static-lib context to the freshly
                            // swapped module (its globals are fresh/null again).
                            void* bc = nullptr; void* ba = nullptr; void* bf = nullptr; void* bu = nullptr;
                            if (bind_provider_) bind_provider_(bc, ba, bf, bu);
                            game.bind(bc, ba, bf, bu);
                        }
                        game.reg(&app);
                        AIMA_INFO("[code-reload] swapped in new game code ({} systems)",
                                  app.SystemCount());
                    } else {
                        AIMA_ERROR("[code-reload] swap failed — exiting (no game code loaded)");
                        break;
                    }
                }
            }
        }

        // Marshal generic input into the World for the project's systems.
        app.GetWorld().GetResource<InputState>() = input;

#if defined(__APPLE__)
        // macOS: a non-.app-bundle binary launched under Xcode/lldb opens its window
        // BEHIND the debugger and never becomes the key app, so neither keyboard nor
        // mouse input reaches the game. The single SDL_RaiseWindow at window creation
        // (window.cpp) fires before the Cocoa run loop settles, so the debugger re-takes
        // the foreground. Re-raise for the first frames (run loop now live) until the
        // window actually holds input focus. Self-limiting (stops once focused or after
        // the grace window); skipped for headless screenshot runs (nothing to focus,
        // and we must not yank the user's foreground app during a capture/CI run).
        if (!shot_path && frame_no < 30 &&
            !(SDL_GetWindowFlags(window.sdl()) & SDL_WINDOW_INPUT_FOCUS)) {
            SDL_RaiseWindow(window.sdl());
        }
#endif

        if (frame_hook_) frame_hook_(app, frame_no, dt);

        // ---- the renderer-agnostic frame ----
        FrameHandle frame = renderer.begin_frame(cfg.clear_color);
        (void)frame;   // the project's render systems read it from their own resource

        // Advance the simulation one frame. The project decides the active scene
        // via scene_hook_ (reads its own SceneState + applies pending transitions);
        // unset => the generic host runs scene 0.
        const int scene_index = scene_hook_ ? scene_hook_(app) : 0;
        app.Tick(scene_index, dt);

        renderer.end_frame(cfg.vsync);

        // Per-frame game service: deferred structural work a system could only FLAG
        // (Tick done, no systems iterating) — e.g. a portal map reload. Runs AFTER
        // end_frame: a reload frees the OLD map's GPU buffers, but THIS frame's
        // command list (recorded in Tick) still references them. They're only safe
        // to free once that list has been SUBMITTED (end_frame), so the reload's
        // dev.flush() can drain it. Freeing before submit faults the GPU -> the
        // fence never signals -> the next frame hangs forever (portal-exit freeze).
        if (use_module && game.service) game.service(&app);

        ++frame_no;

        if (audio_on) audio.poll();

        // Headless self-exit (no `timeout` needed).
        if (shot_path && frame_no == shot_frame) renderer.screenshot(shot_path);
        if (shot_path && frame_no >= shot_frame + 2) running = false;
        if (max_frames > 0 && frame_no >= max_frames) running = false;
    }

    AIMA_INFO("shutting down");
    renderer.flush();
    if (audio_on) audio.shutdown();
    hot.shutdown();
    renderer.shutdown();
    window.shutdown();
    return 0;
}

} // namespace aima
