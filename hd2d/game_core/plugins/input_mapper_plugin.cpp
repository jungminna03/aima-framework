#include "game_core/plugins/input_mapper_plugin.h"

#include "arimu/App.hpp"

// Generic engine input surface (aima::InputState). The migrated host fills this
// from SDL every frame and drops it into the World as a resource; we read it
// here and translate to HD2D's game verbs. Public umbrella header keeps the
// game module decoupled from the framework's internal layout. NOTE: aima.h also
// brings in "platform/input.h" (the GENERIC one) — to avoid the include-name
// clash with HD2D's own platform/input.h, this TU pulls the game InputState in
// via the game/resources.h aggregate below (which owns the hd2d:: one) and the
// generic one through the aima umbrella.
#include "aima/aima.h"

#include "game/resources.h"            // hd2d::InputState (game-layer resource),
                                       // DebugState (mouse_captured mirror)
#include "game/combat_components.h"    // BattleMode (gates direct mouse combat)
#include "game/scene_enum.h"           // GameScene / AsIndex

#include <imgui.h>                     // ImGui::GetIO().WantCapture* (the game
                                       // module statically links ImGui; HUD uses it too)
#include <cmath>                       // std::fabs
#include <cstdlib>                     // std::getenv (debug autopilots, preserved)

namespace hd2d {

namespace {

// --- tuning constants (match the values that lived inline in main.cpp) -------
constexpr float kPadMoveDeadzone   = 0.35f;  // stick magnitude that counts as a move
constexpr float kPadLookDeadzone   = 0.18f;  // stick magnitude that feeds camera delta
constexpr float kPadLookYawScale   = 14.0f;  // right-stick X -> mouse_dx scale
constexpr float kPadLookPitchScale = 10.0f;  // right-stick Y -> mouse_dy scale
constexpr float kPadTriggerOn      = 0.5f;   // analog trigger -> digital threshold

// ---------------------------------------------------------------------------
// THE MAPPER. Runs FIRST in the Input phase (World scene): reads the generic
// aima::InputState resource the host marshalled this frame and OVERWRITES the
// game-layer hd2d::InputState resource so every downstream consumer (combat,
// player, flow, dialogue, UI, command-menu, ...) sees game verbs only.
//
// Semantics preserved verbatim from the pre-migration main.cpp event loop:
//   * movement / sprint / wheel / mouse deltas pass straight through;
//   * attack  = LMB *press edge* only (never the held level) so a held click
//     doesn't carry an attack reservation across the engage transition;
//   * guard   = K equivalent (mapped from RMB *held*), gated off in battle;
//   * lockon  = Tab (generic has no Tab key, so mapped from gamepad RB; see note);
//   * the battle command menu owns LMB/RMB *edges* (lmb_pressed/released,
//     rmb_pressed) regardless of battle state;
//   * menu_confirm = Enter (held; menus edge-detect), menu_back = ESC press edge;
//   * ImGui keyboard/mouse capture is respected exactly as before (held keys are
//     dropped while a text field has focus; mouse combat is dropped while ImGui
//     wants the mouse), with the same "relative mode makes WantCaptureMouse
//     stale" carve-out via DebugState.mouse_captured.
//
// Keyboard verbs that the GENERIC surface cannot express (K-guard, Tab-lockon,
// Q/E rotate, R interact, X delete, V skills, P possess, E inventory/party)
// are NOT carried by aima::InputState today. They are mapped from the gamepad
// where an equivalent exists; the keyboard side is a KNOWN GAP that the host's
// generic surface must grow (or the project must read raw SDL keys in the host
// frame-hook) — flagged in the return summary. Everything the generic surface
// DOES carry is mapped faithfully.
void InputMapSystem(Arimu::Res<aima::InputState> gen,
                    Arimu::ResMut<InputState> out,
                    Arimu::Res<BattleMode> battle,
                    Arimu::Res<DebugState> dbg,
                    Arimu::Res<RawKeys> raw) {
    const aima::InputState& g = *gen;
    InputState in{};   // start from defaults every frame (same as main.cpp did)

    // mouse_captured mirrors SDL relative-mouse mode (F1 toggles it, host-side).
    // In relative mode the OS cursor is frozen/hidden, so ImGui's WantCaptureMouse
    // is stale — treat mouse edges/levels as unconditionally ours there.
    const bool relative_now  = dbg->mouse_captured;
    const bool battle_active = battle->active;
    const ImGuiIO& io = ImGui::GetIO();
    const bool mouse_ours = relative_now || !io.WantCaptureMouse;
    const bool keys_ours  = !io.WantCaptureKeyboard;

    // --- held movement intent -------------------------------------------------
    if (keys_ours) {
        in.move_fwd   = g.move_fwd;
        in.move_back  = g.move_back;
        in.move_left  = g.move_left;
        in.move_right = g.move_right;
        in.sprint     = g.sprint;
        // Generic surface has no Q/E/R/Tab/V/X/E/P keys (see header note); those
        // game verbs (rot_left/right, interact, lockon, skills_menu, menu_delete,
        // inventory, party_menu, possess) come from the gamepad below or stay
        // false until the host grows a richer key surface.
        in.menu_confirm = g.confirm;   // Enter (host fills g.confirm); menus edge-detect
    }

    // --- mouse deltas + wheel (camera yaw / UI nav / zoom) --------------------
    if (relative_now) {
        in.mouse_dx += g.mouse_dx;
        in.mouse_dy += g.mouse_dy;
    }
    if (mouse_ours) in.wheel += g.wheel;

    // --- mouse button edges + held level -------------------------------------
    // The command/box menus consume these edges; combat consumes the levels.
    if (mouse_ours) {
        in.lmb_pressed  = g.lmb_pressed;
        in.lmb_released = g.lmb_released;
        in.rmb_pressed  = g.rmb_pressed;
        in.rmb_down     = g.rmb_down;
    }

    // --- ESC -> menu_back (press edge) ---------------------------------------
    // The host maps ESC to the generic `cancel` edge; flow_plugin owns what
    // "back" means per scene (pause in World, back-out in Title/SaveSelect).
    in.menu_back = g.cancel;

    // --- mouse combat (LMB attack press-edge, RMB guard held) ----------------
    // Suppressed while ImGui wants the mouse or an encounter runs (there the
    // command menu owns both buttons). Same stale-flag carve-out as above.
    if (mouse_ours && !battle_active) {
        if (g.lmb_pressed)            in.attack = true;   // PRESS EDGE only
        if (g.rmb_down)               in.guard  = true;   // held (K-equivalent)
    }

    // --- gamepad held-state overlay (OR on top of keyboard/mouse) ------------
    // Mapping mirrors main.cpp: left stick / d-pad move, right stick camera,
    // A 상호작용·확인, B 취소, X 공격, Y 인벤토리, LB 가드, RB 락온, LT 가드,
    // RT 공격, L3 질주, Select 파티, R3 스킬, Start 일시정지. The generic surface
    // exposes the analog sticks (pad_lx/ly/rx/ry) and a connected flag; the
    // digital BUTTONS are NOT in aima::InputState yet, so only the analog-derived
    // verbs (move / camera) are reconstructable here. Button verbs are flagged
    // as a host-surface gap (see return summary) — once the host adds gamepad
    // button edges/levels to aima::InputState, extend the block below.
    if (g.pad_connected) {
        const float lx = g.pad_lx, ly = g.pad_ly;
        in.move_left  |= lx < -kPadMoveDeadzone;
        in.move_right |= lx >  kPadMoveDeadzone;
        in.move_fwd   |= ly < -kPadMoveDeadzone;
        in.move_back  |= ly >  kPadMoveDeadzone;

        const float rx = g.pad_rx, ry = g.pad_ry;
        if (std::fabs(rx) > kPadLookDeadzone) in.mouse_dx += rx * kPadLookYawScale;
        if (std::fabs(ry) > kPadLookDeadzone) in.mouse_dy += ry * kPadLookPitchScale;

        // 디지털 버튼 → 게임 동사 (CLAUDE.md 매핑). held는 OR, 공격/취소/일시정지는
        // press edge. (호스트가 pad_btn_*를 채우면서 위 스틱 매핑도 함께 살아난다.)
        using enum aima::PadButton;   // 열거자(PadA…)를 블록 스코프로
        in.interact     = in.interact     || aima::pad_down(g, PadA);     // A 상호작용
        in.menu_confirm = in.menu_confirm || aima::pad_down(g, PadA);     // A 확정(메뉴 edge-detect)
        if (aima::pad_pressed(g, PadB))     in.menu_back = true;          // B 취소
        if (aima::pad_pressed(g, PadStart)) in.menu_back = true;          // Start 일시정지
        if (aima::pad_pressed(g, PadX) || aima::pad_pressed(g, PadRT))
            in.attack = true;                                             // X / RT 공격(edge)
        in.inventory   = in.inventory   || aima::pad_down(g, PadY);       // Y 인벤토리
        in.guard       = in.guard       || aima::pad_down(g, PadLB) || aima::pad_down(g, PadLT); // LB/LT 가드
        in.lockon      = in.lockon      || aima::pad_down(g, PadRB);      // RB 락온
        in.sprint      = in.sprint      || aima::pad_down(g, PadL3);      // L3 질주
        in.skills_menu = in.skills_menu || aima::pad_down(g, PadR3);      // R3 스킬창
        in.party_menu  = in.party_menu  || aima::pad_down(g, PadBack);    // Select 파티
        in.move_left   = in.move_left   || aima::pad_down(g, PadDpadLeft);
        in.move_right  = in.move_right  || aima::pad_down(g, PadDpadRight);
        in.move_fwd    = in.move_fwd    || aima::pad_down(g, PadDpadUp);
        in.move_back   = in.move_back   || aima::pad_down(g, PadDpadDown);
    }

    // 키보드 게임-동사(R/E/Tab/V/X/P/Q): 제네릭 표면이 안 나르므로 호스트 frame-hook이
    // SDL로 읽어 채운 RawKeys에서 가져온다(마이그레이션 갭 메움 2026-06-21). 게임패드/마우스
    // 경로는 위에서 이미 OR됐을 수 있어 보존.
    if (keys_ours) {
        in.interact    = in.interact    || raw->interact;
        in.inventory   = in.inventory   || raw->inventory;
        in.party_menu  = in.party_menu  || raw->party_menu;
        in.skills_menu = in.skills_menu || raw->skills_menu;
        in.menu_delete = in.menu_delete || raw->menu_delete;
        in.possess     = in.possess     || raw->possess;
        in.lockon      = in.lockon      || raw->lockon;
        in.rot_left    = in.rot_left    || raw->rot_left;
        in.rot_right   = in.rot_right   || raw->rot_right;
    }

    // 절대 커서 좌표는 캡처 여부와 무관하게 항상 전달(자유 커서 메뉴 히트테스트용).
    in.mouse_x = raw->mouse_x;
    in.mouse_y = raw->mouse_y;

    *out = in;   // publish the mapped snapshot for every downstream consumer
}

} // namespace

void InputMapperPlugin::Build(Arimu::App& app) {
    // hd2d::InputState is Ensure-only (CorePlugin already EnsureResource<InputState>();
    // re-ensuring is harmless and keeps this plugin self-contained for hot-reload).
    app.GetWorld().EnsureResource<InputState>();
    app.GetWorld().EnsureResource<RawKeys>();   // host frame-hook fills 키보드 게임-동사

    // World scene only: Title/SaveSelect run flow_plugin systems that read the
    // same hd2d::InputState resource, so they need the mapper too. Register in
    // ALL three game scenes so menu nav (confirm/back) works pre-World as well.
    const Arimu::Phase phase = Arimu::Phase::Input;
    app.AddSystemInScenes(InputMapSystem,
                          { AsIndex(GameScene::World),
                            AsIndex(GameScene::Title),
                            AsIndex(GameScene::SaveSelect),
                            AsIndex(GameScene::Story),
                            AsIndex(GameScene::CharSelect),
                            AsIndex(GameScene::NameEntry),
                            AsIndex(GameScene::Loading) },
                          phase, "InputMap");
}

} // namespace hd2d
