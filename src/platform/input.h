#pragma once

#include <cstdint>

namespace aima {

// Per-frame input snapshot, filled from SDL by the host and consumed by game
// systems via a World resource. Kept POD and decoupled from SDL so systems never
// depend on the platform layer.
//
// This is the GENERIC, game-agnostic engine surface: raw movement intent, mouse
// state, and a few common edges/buttons. It deliberately does NOT encode any
// specific game's verbs (attack/guard/inventory/etc.) — a project layers its own
// action mapping on top (read these fields in an Input-phase system and write the
// game's own InputState/Action resources). The host's event pump fills whatever
// of these the project wires up.
struct InputState {
    // --- held movement intent (WASD by default) ---
    bool move_fwd = false;    // W
    bool move_back = false;   // S
    bool move_left = false;   // A
    bool move_right = false;  // D
    bool sprint = false;      // Left Shift

    // --- relative-mouse deltas + wheel, accumulated this frame ---
    float mouse_dx = 0.0f;    // horizontal delta (relative mode)
    float mouse_dy = 0.0f;    // vertical delta (relative mode)
    float wheel = 0.0f;       // accumulated wheel ticks this frame

    // --- mouse button edges + held levels this frame ---
    bool lmb_pressed = false;
    bool lmb_released = false;
    bool lmb_down = false;
    bool rmb_pressed = false;
    bool rmb_released = false;
    bool rmb_down = false;

    // --- common menu/confirm edges (host fills from keyboard/gamepad) ---
    bool confirm = false;     // Enter / gamepad A
    bool cancel = false;      // ESC / gamepad B (press edge)

    // --- gamepad analog passthrough (left/right stick), -1..1 ---
    float pad_lx = 0.0f, pad_ly = 0.0f;
    float pad_rx = 0.0f, pad_ry = 0.0f;
    bool pad_connected = false;

    // --- gamepad digital buttons (host fills; bit index = PadButton) ---
    uint32_t pad_btn_down = 0;     // currently held this frame
    uint32_t pad_btn_pressed = 0;  // rising edge this frame
};

// Common gamepad buttons (bit indices into InputState::pad_btn_*). The host maps
// SDL_GamepadButton + triggers into these; a project maps these into its own verbs.
enum PadButton {
    PadA = 0, PadB, PadX, PadY,        // SDL3 SOUTH/EAST/WEST/NORTH
    PadBack, PadStart,
    PadLB, PadRB, PadL3, PadR3,
    PadDpadUp, PadDpadDown, PadDpadLeft, PadDpadRight,
    PadLT, PadRT,                      // triggers as digital (past threshold)
};
inline bool pad_down(const InputState& s, PadButton b)    { return (s.pad_btn_down    >> b) & 1u; }
inline bool pad_pressed(const InputState& s, PadButton b) { return (s.pad_btn_pressed >> b) & 1u; }

} // namespace aima
