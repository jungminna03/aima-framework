#pragma once

// ECS-native in-game UI framework v1 (Bevy-UI style): a widget is an ENTITY
// carrying a combination of these components; all behaviour lives in UiPlugin
// systems (nav-input -> nav -> lerp -> layout -> render). The battle command
// menu (CommandMenuPlugin) is the first consumer.
// See docs/superpowers/specs/2026-06-05-ecs-ui-framework-command-menu-design.md.
//
// v1 scope is deliberately tiny (YAGNI): rect+text widgets, parent/child
// anchoring, grid-cell navigation, one selection highlight. No scroll views,
// no text input, no focus chains.

#include <entt/entt.hpp>

#include <cstdint>
#include <string>

namespace hd2d {

struct UiVec2 {
    float x = 0.0f, y = 0.0f;
};

// Hierarchy: children are positioned inside their parent's rect and inherit
// its visibility. v1 has no auto-despawn — owners track and destroy children.
struct UiParent {
    entt::entity parent = entt::null;
};

// Placement: top-left = parent_abs + anchor * parent_size + offset (px).
// Entities without UiParent anchor against the screen rect, so UI stays put
// across window resizes (no hardcoded absolute positions).
struct UiNode {
    UiVec2 anchor;         // 0..1 fraction of the parent rect
    UiVec2 offset;         // px from the anchor point
    UiVec2 size;           // px
    bool visible = true;
};

// Visuals: packed IM_COL32 colours; an alpha-zero value (0) means "skip".
struct UiStyle {
    uint32_t bg = 0;
    uint32_t border = 0;
    float border_px = 2.5f;
    float rounding = 4.0f;
};

// Label, scaled from the default ImGui font size. Default = centred single
// line (기존 버튼/HUD). wrap=true면 node.size.x 폭으로 자동 줄바꿈(다이얼로그 대사),
// align=0이면 좌상단 정렬.
struct UiText {
    std::string str;
    uint32_t color = 0xFFFFFFFFu;
    float scale = 1.0f;
    bool wrap = false;      // true = node.size.x 를 wrap 폭으로 자동 줄바꿈
    uint8_t align = 1;      // 0 = 좌상단, 1 = 중앙(기존)
};

// 이미지 위젯(2026-07-04 초상화) — tex_id는 호스트가 UiImageCache로 채워준
// ImTextureID(uint64) 값. 0 = 아직 로드 안 됨(그리지 않음). tint로 비화자 어둡게.
struct UiImage {
    uint64_t tex_id = 0;
    uint32_t tint = 0xFFFFFFFFu;
};

// Navigation: selectables form a grid per GROUP; moving = "step to the
// neighbour cell in direction D, stay at edges". The 공격/회피 bar is a 2x1
// group and the custom grid a 3x3 group — same code path for both.
struct UiSelectable {
    uint32_t group = 0;
    int cell_x = 0, cell_y = 0;
};

// Eased motion toward a target rect (exponential smoothing applied by
// UiLerpSystem). Used for the grid-open expansion.
struct UiLerp {
    UiVec2 target_offset;
    UiVec2 target_size;
    float speed = 14.0f;
};

// Output of UiLayoutSystem: resolved absolute top-left in px.
struct UiComputed {
    UiVec2 abs_pos;
};

// ---------------------------------------------------------------------------
// Resources
// ---------------------------------------------------------------------------

// Device-agnostic nav intent, REBUILT every frame by UiNavInputSystem from
// mouse deltas. Keyboard/gamepad later = more producers, same consumer.
struct NavInput {
    int dir_x = 0, dir_y = 0;        // -1/0/+1 step tick (one cardinal step max per frame)
    bool confirm_down = false;       // LMB pressed edge
    bool confirm_up = false;         // LMB released edge
    bool cancel = false;             // RMB pressed edge
};

// Selection state + the highlight visual ("the red border IS the pointer").
struct UiNav {
    uint32_t active_group = 0;             // 0 = nav inactive (free roam)
    entt::entity selected = entt::null;
    entt::entity activated = entt::null;   // confirmed this frame (consumer clears)
    float acc_x = 0.0f, acc_y = 0.0f;      // mouse-delta accumulators
    float step_px = 60.0f;                 // delta needed for one selection step

    // Free-cursor absolute hit-testing (UiMouseHitSystem). press_widget = the
    // widget an LMB-down started over (a click = press + release over the SAME
    // widget). last_mouse_* gates hover-selection to actual cursor motion so the
    // mouse doesn't fight keyboard/pad nav. confirm_latch edge-detects Enter.
    entt::entity press_widget = entt::null;
    float last_mouse_x = -1.0f, last_mouse_y = -1.0f;
    bool confirm_latch = false;

    // Highlight presentation (eased + drawn by UiRenderSystem).
    UiVec2 hl_pos, hl_size;                // current rect
    bool hl_snap = true;                   // jump instead of slide next frame
    float hl_pad = 7.0f;                   // grows the highlight past the widget
    float hl_speed = 18.0f;                // exp ease rate
};

} // namespace hd2d
