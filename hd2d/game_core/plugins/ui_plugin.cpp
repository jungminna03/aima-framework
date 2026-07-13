#include "game_core/plugins/ui_plugin.h"

#include "game/resources.h"        // FrameTime, RenderContext
#include "game/scene_enum.h"
#include "game/sound_ids.h"
#include "game/input.h"
#include "ui/ui_components.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <utility>
#include <vector>

namespace hd2d {

namespace {

// PRODUCER: mouse deltas -> device-agnostic nav ticks/edges. The accumulator
// fills with relative motion and emits ONE cardinal step per threshold
// crossing (dominant axis first), so a diagonal cell is reached by two steps
// (left then down / down then left) exactly like the mockups. Stale residue
// decays so an old half-flick doesn't fire a surprise step much later.
void UiNavInputSystem(Arimu::Res<InputState> in,
                      Arimu::Res<FrameTime> ft,
                      Arimu::ResMut<UiNav> nav,
                      Arimu::ResMut<NavInput> out) {
    out->dir_x = 0; out->dir_y = 0;
    out->confirm_down = false; out->confirm_up = false; out->cancel = false;
    if (nav->active_group == 0) {           // nav owns the mouse only in a menu
        nav->acc_x = 0.0f; nav->acc_y = 0.0f;
        return;
    }

    const float decay = std::exp(-6.0f * ft->dt);
    nav->acc_x = nav->acc_x * decay + in->mouse_dx;
    nav->acc_y = nav->acc_y * decay + in->mouse_dy;

    if (std::fabs(nav->acc_x) >= std::fabs(nav->acc_y)) {
        if (std::fabs(nav->acc_x) >= nav->step_px) {
            out->dir_x = nav->acc_x > 0.0f ? 1 : -1;
            nav->acc_x -= static_cast<float>(out->dir_x) * nav->step_px;
        }
    } else {
        if (std::fabs(nav->acc_y) >= nav->step_px) {
            out->dir_y = nav->acc_y > 0.0f ? 1 : -1;
            nav->acc_y -= static_cast<float>(out->dir_y) * nav->step_px;
        }
    }

    out->confirm_down = in->lmb_pressed;
    out->confirm_up = in->lmb_released;
    out->cancel = in->rmb_pressed;
}

// CONSUMER: apply nav ticks inside the active group. Selection moves to the
// neighbour cell in the tick direction; no widget there (edge) = stay put.
// LMB-release records the confirmed widget for game code to consume.
void UiNavSystem(Arimu::Query<UiSelectable> sel,
                 Arimu::Res<NavInput> in,
                 Arimu::Res<InputState> kin,
                 Arimu::ResMut<UiNav> nav,
                 Arimu::ResMut<SoundQueue> snd) {
    nav->activated = entt::null;
    if (nav->active_group == 0) { nav->confirm_latch = kin->menu_confirm; return; }

    if ((in->dir_x != 0 || in->dir_y != 0) &&
        nav->selected != entt::null && sel.Contains(nav->selected)) {
        const UiSelectable& cur = sel.Get<UiSelectable>(nav->selected);
        const int wx = cur.cell_x + in->dir_x;
        const int wy = cur.cell_y + in->dir_y;
        for (auto [e, s] : sel.each()) {
            if (s.group == nav->active_group && s.cell_x == wx && s.cell_y == wy) {
                if (e != nav->selected) snd->play(SoundId::UiTick, 0.5f);
                nav->selected = e;
                break;
            }
        }
    }

    // 키보드/패드 Enter 라이징 엣지로 확정(마우스 클릭은 UiMouseHitSystem이 위치-인지로
    // 처리 — 빈 공간 클릭 오발동 방지). confirm_up(LMB 릴리즈)은 더는 확정에 안 쓴다.
    const bool confirm_edge = kin->menu_confirm && !nav->confirm_latch;
    nav->confirm_latch = kin->menu_confirm;
    if (confirm_edge && nav->selected != entt::null)
        nav->activated = nav->selected;
}

// Ease UiNode offset/size toward UiLerp targets (grid-open expansion).
void UiLerpSystem(Arimu::Query<UiNode, UiLerp> nodes, Arimu::Res<FrameTime> ft) {
    for (auto [e, node, lp] : nodes.each()) {
        const float k = 1.0f - std::exp(-lp.speed * ft->dt);
        node.offset.x += (lp.target_offset.x - node.offset.x) * k;
        node.offset.y += (lp.target_offset.y - node.offset.y) * k;
        node.size.x   += (lp.target_size.x   - node.size.x)   * k;
        node.size.y   += (lp.target_size.y   - node.size.y)   * k;
    }
}

// Resolve anchor+offset through the parent chain into absolute px. Chains are
// depth<=2 here, so the naive per-entity walk is plenty.
void UiLayoutSystem(Arimu::Query<UiNode, UiComputed> nodes,
                    Arimu::Query<UiParent> parents,
                    Arimu::Query<UiNode> all_nodes,
                    Arimu::Res<RenderContext> rc) {
    const float W = static_cast<float>(rc->width > 0 ? rc->width : 1600);
    const float H = static_cast<float>(rc->height > 0 ? rc->height : 900);

    auto resolve = [&](entt::entity e, auto&& self) -> UiVec2 {
        const UiNode& n = all_nodes.Get<UiNode>(e);
        UiVec2 origin{0.0f, 0.0f};
        UiVec2 psize{W, H};
        if (parents.Contains(e)) {
            const entt::entity p = parents.Get<UiParent>(e).parent;
            if (p != entt::null && all_nodes.Contains(p)) {
                origin = self(p, self);
                psize = all_nodes.Get<UiNode>(p).size;
            }
        }
        return UiVec2{origin.x + n.anchor.x * psize.x + n.offset.x,
                      origin.y + n.anchor.y * psize.y + n.offset.y};
    };

    for (auto [e, node, out] : nodes.each()) out.abs_pos = resolve(e, resolve);
}

// FREE-CURSOR absolute hit-test for ECS panels. Runs AFTER UiLayout (needs
// abs_pos) and writes the same UiNav.selected/activated the keyboard path uses,
// so panel code consumes clicks through the existing pipeline unchanged. Hover
// moves the selection (only on real cursor motion, so it doesn't fight the
// keyboard); a click = LMB-down + LMB-up over the SAME widget, so clicking empty
// space never confirms. Skipped while the cursor is captured (pure play has no
// panels) — MouseModeSystem frees it whenever a group is active.
void UiMouseHitSystem(Arimu::Query<UiNode, UiComputed, UiSelectable> sel,
                      Arimu::Res<InputState> in,
                      Arimu::Res<DebugState> dbg,
                      Arimu::ResMut<UiNav> nav,
                      Arimu::ResMut<SoundQueue> snd) {
    if (nav->active_group == 0) { nav->press_widget = entt::null; return; }
    if (dbg->mouse_captured) return;   // captured = pure play, no panel to click

    const float mx = in->mouse_x, my = in->mouse_y;
    const bool moved = std::fabs(mx - nav->last_mouse_x) > 1.0f ||
                       std::fabs(my - nav->last_mouse_y) > 1.0f;
    nav->last_mouse_x = mx; nav->last_mouse_y = my;

    entt::entity hovered = entt::null;
    for (auto [e, node, comp, s] : sel.each()) {
        if (s.group != nav->active_group || !node.visible) continue;
        const float ax = comp.abs_pos.x, ay = comp.abs_pos.y;
        if (mx >= ax && mx <= ax + node.size.x && my >= ay && my <= ay + node.size.y) {
            hovered = e; break;   // selectables in a group don't overlap — first hit is enough
        }
    }
    if (moved && hovered != entt::null && hovered != nav->selected) {
        nav->selected = hovered;
        snd->play(SoundId::UiTick, 0.5f);
    }
    if (in->lmb_pressed && hovered != entt::null) nav->press_widget = hovered;
    if (in->lmb_released) {
        if (hovered != entt::null && hovered == nav->press_widget)
            nav->activated = hovered;
        nav->press_widget = entt::null;
    }
}

// Draw every visible widget (bg, border, centred text) on the foreground draw
// list, then the selection highlight on top. The highlight is presentation
// state on UiNav, eased here toward the selected widget's rect — sliding from
// cell to cell is what sells "the pointer moved" without an OS cursor.
void UiRenderSystem(Arimu::Query<UiNode, UiComputed> nodes,
                    Arimu::Query<UiStyle> styles,
                    Arimu::Query<UiText> texts,
                    Arimu::Query<UiImage> images,
                    Arimu::Query<UiParent> parents,
                    Arimu::Query<UiNode> all_nodes,
                    Arimu::Res<FrameTime> ft,
                    Arimu::ResMut<UiNav> nav) {
    ImDrawList* dl = ImGui::GetForegroundDrawList();

    auto visible = [&](entt::entity e, auto&& self) -> bool {
        if (!all_nodes.Contains(e) || !all_nodes.Get<UiNode>(e).visible) return false;
        if (parents.Contains(e)) {
            const entt::entity p = parents.Get<UiParent>(e).parent;
            if (p != entt::null) return self(p, self);
        }
        return true;
    };

    // Draw PARENTS BEFORE CHILDREN. A panel's semi-transparent background must
    // never paint over its own cells/text. The ECS view yields entities in
    // reverse-creation order, so a parent (created first) would be drawn LAST and
    // its bg would wash everything under it out — the bug that made every panel's
    // text/cells/borders collapse to one muddy dark tone. Sort by parent-chain
    // depth first (chains are depth<=2; stable_sort keeps sibling order stable).
    auto depth = [&](entt::entity e, auto&& self) -> int {
        if (parents.Contains(e)) {
            const entt::entity p = parents.Get<UiParent>(e).parent;
            if (p != entt::null && all_nodes.Contains(p)) return 1 + self(p, self);
        }
        return 0;
    };

    std::vector<std::pair<int, entt::entity>> order;
    for (auto [e, node, comp] : nodes.each())
        if (visible(e, visible)) order.emplace_back(depth(e, depth), e);
    std::stable_sort(order.begin(), order.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });

    for (const auto& pr : order) {
        const entt::entity e = pr.second;
        const UiNode& node = nodes.Get<UiNode>(e);
        const UiComputed& comp = nodes.Get<UiComputed>(e);
        const ImVec2 a(comp.abs_pos.x, comp.abs_pos.y);
        const ImVec2 b(a.x + node.size.x, a.y + node.size.y);
        if (styles.Contains(e)) {
            const UiStyle& st = styles.Get<UiStyle>(e);
            if (st.bg)     dl->AddRectFilled(a, b, st.bg, st.rounding);
            if (st.border) dl->AddRect(a, b, st.border, st.rounding, 0, st.border_px);
        }
        if (images.Contains(e)) {
            const UiImage& img = images.Get<UiImage>(e);
            if (img.tex_id)
                dl->AddImage((ImTextureID)img.tex_id, a, b,
                             ImVec2(0, 0), ImVec2(1, 1), img.tint);
        }
        if (texts.Contains(e)) {
            const UiText& tx = texts.Get<UiText>(e);
            const float fs = ImGui::GetFontSize() * tx.scale;
            const float wrap_w = tx.wrap ? node.size.x : 0.0f;
            const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, wrap_w, tx.str.c_str());
            const ImVec2 tp = tx.align == 0
                ? a
                : ImVec2(a.x + (node.size.x - ts.x) * 0.5f,
                         a.y + (node.size.y - ts.y) * 0.5f);
            dl->AddText(ImGui::GetFont(), fs, tp, tx.color, tx.str.c_str(), nullptr, wrap_w);
        }
    }

    // Selection highlight. Retarget only when the selected widget is
    // resolvable (spawn commands flush at the END of the spawning system; the
    // guard makes no assumption about who spawned when) and keep drawing the
    // last rect meanwhile, so a menu transition never blinks the pointer.
    if (nav->active_group == 0) { nav->hl_snap = true; return; }
    if (nav->selected != entt::null && nodes.Contains(nav->selected)) {
        const UiNode& sn = nodes.Get<UiNode>(nav->selected);
        const UiComputed& sc = nodes.Get<UiComputed>(nav->selected);
        const UiVec2 tp{sc.abs_pos.x - nav->hl_pad, sc.abs_pos.y - nav->hl_pad};
        const UiVec2 tsz{sn.size.x + nav->hl_pad * 2.0f, sn.size.y + nav->hl_pad * 2.0f};
        if (nav->hl_snap) {
            nav->hl_pos = tp; nav->hl_size = tsz;
            nav->hl_snap = false;
        } else {
            const float k = 1.0f - std::exp(-nav->hl_speed * ft->dt);
            nav->hl_pos.x  += (tp.x  - nav->hl_pos.x)  * k;
            nav->hl_pos.y  += (tp.y  - nav->hl_pos.y)  * k;
            nav->hl_size.x += (tsz.x - nav->hl_size.x) * k;
            nav->hl_size.y += (tsz.y - nav->hl_size.y) * k;
        }
    }
    if (nav->hl_size.x > 1.0f)
        dl->AddRect(ImVec2(nav->hl_pos.x, nav->hl_pos.y),
                    ImVec2(nav->hl_pos.x + nav->hl_size.x, nav->hl_pos.y + nav->hl_size.y),
                    IM_COL32(225, 30, 30, 255), 12.0f, 0, 4.5f);
}

} // namespace

void UiPlugin::Build(Arimu::App& app) {
    Arimu::World& world = app.GetWorld();
    world.EnsureResource<NavInput>();
    world.EnsureResource<UiNav>();

    const uint8_t scene = AsIndex(GameScene::World);
    app.AddSystem(UiNavInputSystem, scene, Arimu::Phase::Logic, "UiNavInput");
    app.AddSystem(UiNavSystem,      scene, Arimu::Phase::Logic, "UiNav");
    app.AddSystem(UiLerpSystem,     scene, Arimu::Phase::Logic, "UiLerp");
    app.AddSystem(UiLayoutSystem,   scene, Arimu::Phase::Logic, "UiLayout");
    app.AddSystem(UiMouseHitSystem, scene, Arimu::Phase::Logic, "UiMouseHit");
    app.AddSystem(UiRenderSystem,   scene, Arimu::Phase::Logic, "UiRender");
}

} // namespace hd2d
