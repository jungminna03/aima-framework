// ============================================================================
// renderer/renderer_impl.cpp — Hd2dRenderer implementation.
//
// Delegates the six aima::Renderer virtuals to the owned hd2d::Dx12Device. The
// only real work this layer does is:
//   * deriving the backend's native window handle from the SDL_Window in init(),
//   * packing aima::ClearColor into the float[4] the device's begin_frame wants,
//   * casting the returned ID3D12GraphicsCommandList* to aima::FrameHandle, and
//   * mapping aima's screenshot(path) onto HD2D's request_screenshot consume
//     model.
// Everything else is a straight forward. No renderer internals are touched.
// ============================================================================

#include "renderer/renderer_impl.h"

#include "core/log_compat.h"
#include "ui/imgui_layer.h"   // hd2d::ImGuiLayer (owns the ImGui context/backends)

#include <imgui.h>            // ImGui::GetCurrentContext / GetAllocatorFunctions
#include <SDL3/SDL.h>         // SDL_Window, SDL_GetWindowProperties, native-handle props

#include "renderer/frame_renderer.h"  // hd2d::FrameRenderer (high-level scene API impl)
#include "renderer/forward_pass.h"
#include "renderer/shadow_pass.h"
#include "renderer/post_chain.h"
#include "renderer/scene_targets.h"

namespace hd2d {

// ----------------------------------------------------------------------------
// init — create the device + swapchain for `window`.
//
// The aima host passes the SDL_Window it opened (Window::sdl()). The DX12
// swapchain needs the raw Win32 HWND; the SDL_GPU backend wants the SDL_Window*
// itself. We branch on HD2D_RENDERER_D3D12 (the same define the device.h
// umbrella uses to pick the concrete Dx12Device), NOT on _WIN32, so a Windows
// machine building the sdlgpu backend still takes the SDL_Window* path.
// ----------------------------------------------------------------------------
bool Hd2dRenderer::init(SDL_Window* window, int width, int height) {
    if (!window) {
        HD2D_ERROR("Hd2dRenderer::init: null SDL_Window");
        return false;
    }
    if (width <= 0 || height <= 0) {
        HD2D_ERROR("Hd2dRenderer::init: invalid size {}x{}", width, height);
        return false;
    }

    const auto w = static_cast<uint32_t>(width);
    const auto h = static_cast<uint32_t>(height);

#if defined(HD2D_RENDERER_D3D12)
    // Windows / DX12: pull the HWND out of the SDL window's property bag (same
    // path platform/window.cpp uses) and hand it to the DX12 device.
    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    HWND hwnd = static_cast<HWND>(
        SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (!hwnd) {
        HD2D_ERROR("Hd2dRenderer::init: SDL window has no Win32 HWND property");
        return false;
    }
    const bool ok = device_.init(hwnd, w, h);
#else
    // macOS/Linux / SDL_GPU: the stub/real SDL_GPU device takes the SDL_Window*
    // (as a void* window handle) directly.
    const bool ok = device_.init(static_cast<void*>(window), w, h);
#endif

    if (!ok) {
        HD2D_ERROR("Hd2dRenderer::init: device.init failed");
        return false;
    }

    // Create the single live ImGui context now that the device exists. Faithful
    // to the old main.cpp, which owned the ImGuiLayer and created the context
    // (+ SDL3/DX12 backends) right after the device. On macOS/SDL_GPU this is
    // SDL3-only (no DX12 device needed); the context it makes is what the host
    // hands to the game module via the bind-provider (see imgui_bind_args).
    if (!imgui_.init(window, device_)) {
        HD2D_ERROR("Hd2dRenderer::init: ImGui layer init failed");
        return false;
    }

    initialized_ = true;
    HD2D_INFO("Hd2dRenderer initialized ({}x{})", width, height);
    return true;
}

// ----------------------------------------------------------------------------
// imgui_bind_args — expose the live ImGui context + allocator functions as
// opaque pointers so the host's bind-provider can marshal them into the game
// module's GameBindHost (ImGui is statically linked into BOTH binaries, so each
// has its own globals; the game module must adopt THIS context + allocators).
// ----------------------------------------------------------------------------
void Hd2dRenderer::imgui_bind_args(void*& ctx, void*& alloc, void*& free, void*& user) const {
    ctx = ImGui::GetCurrentContext();
    ImGuiMemAllocFunc a = nullptr;
    ImGuiMemFreeFunc  f = nullptr;
    void* u = nullptr;
    ImGui::GetAllocatorFunctions(&a, &f, &u);
    alloc = reinterpret_cast<void*>(a);
    free  = reinterpret_cast<void*>(f);
    user  = u;
}

// ----------------------------------------------------------------------------
// begin_frame — open the device's frame and surface its command list as the
// opaque aima::FrameHandle. On DX12 the handle is the open
// ID3D12GraphicsCommandList*; on SDL_GPU it is nullptr (device_.begin_frame
// returns nullptr there, and the game's DX12-coupled render systems early-return
// on a null command list — the SDL_GPU clear/scene happens inside the device).
// ----------------------------------------------------------------------------
aima::FrameHandle Hd2dRenderer::begin_frame(const aima::ClearColor& clear) {
    // ImGui NewFrame BEFORE the device frame, faithful to the old main loop: the
    // game's HUD/UI systems build draw data during App::Tick (between begin/end),
    // so the frame must be live first. On SDL_GPU this is SDL3-only.
    imgui_.begin_frame();

    const float cc[4] = { clear.r, clear.g, clear.b, clear.a };
    ID3D12GraphicsCommandList* cmd = device_.begin_frame(cc);
    cur_cmd_ = cmd;   // remembered for end_frame -> imgui_.end_frame(cmd)
    return static_cast<aima::FrameHandle>(cmd);
}

// ----------------------------------------------------------------------------
// end_frame — submit + present. Also consumes any screenshot queued via
// screenshot()/request_screenshot() (the device copies the just-presented frame
// to a readback buffer and writes the PNG inside this call).
// ----------------------------------------------------------------------------
void Hd2dRenderer::end_frame(bool vsync) {
    // Record ImGui draw data BEFORE the device submits/presents. On DX12 it goes
    // into the still-open command list (cur_cmd_); on SDL_GPU cur_cmd_ is null and
    // the ImGui layer draws onto the swapchain via the device's current command
    // buffer + swapchain texture (still open until device_.end_frame submits).
    imgui_.end_frame(device_, cur_cmd_);
    device_.end_frame(vsync);
    cur_cmd_ = nullptr;
}

void Hd2dRenderer::resize(int width, int height) {
    if (width <= 0 || height <= 0) return;
    device_.resize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
}

// ----------------------------------------------------------------------------
// screenshot — adapt aima's "capture to path" onto HD2D's request+consume model.
//
// HD2D never reads back the swapchain synchronously: request_screenshot(path)
// queues the path, and the NEXT end_frame() does the copy + PNG write. The aima
// host calls screenshot() between frames (after end_frame on the shot frame in
// the headless harness), so queuing here means the FOLLOWING frame's end_frame
// captures it. That matches how main.cpp drove the bare device today
// (request_screenshot just before end_frame on the shot frame), so behaviour is
// preserved. We do NOT spin a frame here: the host owns the begin/end loop and
// driving it from inside this call would desync ImGui/the ECS Render phase.
// ----------------------------------------------------------------------------
void Hd2dRenderer::screenshot(const std::string& path) {
    if (!initialized_) {
        HD2D_WARN("Hd2dRenderer::screenshot before init; ignored ({})", path);
        return;
    }
    device_.request_screenshot(path);
}

void Hd2dRenderer::flush() {
    if (initialized_) device_.flush();
}

void Hd2dRenderer::shutdown() {
    if (!initialized_) return;
    imgui_.shutdown();   // destroy the ImGui context + backends before the device
    device_.shutdown();
    initialized_ = false;
}

// ----------------------------------------------------------------------------
// FrameRenderer — backend-neutral HIGH-LEVEL scene API (see frame_renderer.h).
// DX12 impl: holds the renderer's sub-objects + the frame's command list and
// translates the game's high-level calls into the existing passes. Lives in this
// TU (not its own .cpp) to avoid a CMake reconfigure mid-migration; methods are
// added as each game render system moves off raw ID3D12 (PAL render keystone).
// ----------------------------------------------------------------------------
void FrameRenderer::init(Dx12Device* dev, ForwardPass* fwd, ShadowPass* shadow,
                         PostChain* post, SceneTargets* targets, Dx12ResourceTable* table) {
    dev_ = dev; fwd_ = fwd; shadow_ = shadow; post_ = post; targets_ = targets; table_ = table;
    // Engine fallback textures (untextured slots). Registered in the table so the
    // backend-neutral draw path resolves them like any other handle. White is sRGB
    // (identity at 1.0); the flat normal MUST stay UNORM (128 -> 0.5).
    if (dev_ && table_) {
        const uint8_t white[4]  = {255, 255, 255, 255};
        const uint8_t flat_n[4] = {128, 128, 255, 255};
        fb_white_  = table_->add_texture(upload_texture_rgba8_mips(*dev_, 1, 1, white,  true));
        fb_normal_ = table_->add_texture(upload_texture_rgba8_mips(*dev_, 1, 1, flat_n, false));
    }
}

void FrameRenderer::scene_set_translucent(bool on) {
    if (cmd_ && fwd_) fwd_->set_translucent(cmd_, on);
}

void FrameRenderer::scene_draw_mesh(rhi::GpuMesh mesh, const math::Matrix& model,
                                    const DrawMaterial& mat, float alpha) {
    if (!cmd_ || !fwd_ || !table_) return;
    const GpuSubmesh* smp = table_->resolve(mesh);
    if (!smp) return;
    const GpuSubmesh& sm = *smp;

    DrawConstants dc{};
    math::store(dc.model, model);
    math::store(dc.normal_mat, math::transpose(math::inverse(model)));
    dc.uv_scale[0] = dc.uv_scale[1] = 1.0f;

    MaterialSrvs srvs;
    const GpuTexture* fbw = table_->resolve(fb_white_);
    const GpuTexture* fbn = table_->resolve(fb_normal_);
    const D3D12_GPU_DESCRIPTOR_HANDLE white = fbw ? fbw->srv_gpu : D3D12_GPU_DESCRIPTOR_HANDLE{};
    const D3D12_GPU_DESCRIPTOR_HANDLE flat  = fbn ? fbn->srv_gpu : D3D12_GPU_DESCRIPTOR_HANDLE{};
    srvs.base = srvs.mr = srvs.emissive = white;
    srvs.normal = flat;
    if (mat.has_material) {
        for (int c = 0; c < 4; ++c) dc.base_color[c] = mat.base_color[c];
        dc.metallic = mat.metallic;
        dc.roughness = mat.roughness;
        dc.alpha_cutoff = mat.alpha_cutoff;
        dc.flags = mat.flags;
        for (int c = 0; c < 3; ++c) dc.emissive[c] = mat.emissive_factor[c];
        auto resolve_or = [&](rhi::GpuTexture h, D3D12_GPU_DESCRIPTOR_HANDLE fb) {
            if (h.id) if (const GpuTexture* t = table_->resolve(h)) if (t->resource) return t->srv_gpu;
            return fb;
        };
        srvs.base     = resolve_or(mat.base,     white);
        srvs.mr       = resolve_or(mat.mr,       white);
        srvs.normal   = resolve_or(mat.normal,   flat);
        srvs.emissive = resolve_or(mat.emissive, white);
    } else {
        for (int c = 0; c < 4; ++c) dc.base_color[c] = sm.base_color[c];
        dc.metallic = sm.metallic;
        dc.roughness = sm.roughness;
    }
    if (alpha < 1.0f) {                 // occlusion fade: translucent, no alpha-test
        dc.base_color[3] *= alpha;
        dc.alpha_cutoff = 0.0f;
    }
    fwd_->draw(cmd_, dc, srvs, sm.vbv, sm.ibv, sm.index_count);
}

void FrameRenderer::scene_begin(const FrameConstants& fc, const float sky[4],
                                uint32_t w, uint32_t h) {
    if (!cmd_ || !dev_ || !targets_ || !fwd_ || !shadow_) return;
    if (!targets_->ensure(*dev_, w, h)) return;
    targets_->to_render_target(cmd_);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = targets_->rtv();
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = targets_->dsv();
    cmd_->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    cmd_->ClearRenderTargetView(rtv, sky, 0, nullptr);
    cmd_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    // The shadow pass ran with its own 2048^2 viewport — restore the window's.
    const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(w),
                                  static_cast<float>(h), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
    cmd_->RSSetViewports(1, &viewport);
    cmd_->RSSetScissorRects(1, &scissor);
    fwd_->begin(cmd_, fc, shadow_->srv());
}

void FrameRenderer::scene_draw_sprite(rhi::GpuMesh quad, const math::Matrix& model,
                                      rhi::GpuTexture sheet, const float uv_off[2],
                                      const float uv_scale[2], const float tint[4],
                                      float alpha_cutoff, uint32_t flags) {
    if (!cmd_ || !fwd_ || !table_) return;
    const GpuSubmesh* sm = table_->resolve(quad);
    if (!sm) return;
    DrawConstants dc{};
    math::store(dc.model, model);
    math::store(dc.normal_mat, model);   // billboards: rigid + uniform scale
    for (int c = 0; c < 4; ++c) dc.base_color[c] = tint[c];
    dc.metallic = 0.0f; dc.roughness = 1.0f;
    dc.alpha_cutoff = alpha_cutoff;
    dc.flags = flags;
    dc.uv_offset[0] = uv_off[0];   dc.uv_offset[1] = uv_off[1];
    dc.uv_scale[0]  = uv_scale[0]; dc.uv_scale[1]  = uv_scale[1];
    MaterialSrvs srvs;
    const GpuTexture* fbw = table_->resolve(fb_white_);
    const GpuTexture* fbn = table_->resolve(fb_normal_);
    const D3D12_GPU_DESCRIPTOR_HANDLE white = fbw ? fbw->srv_gpu : D3D12_GPU_DESCRIPTOR_HANDLE{};
    srvs.base = white;
    if (sheet.id) if (const GpuTexture* t = table_->resolve(sheet)) if (t->resource) srvs.base = t->srv_gpu;
    srvs.mr = srvs.emissive = white;
    srvs.normal = fbn ? fbn->srv_gpu : D3D12_GPU_DESCRIPTOR_HANDLE{};
    fwd_->draw(cmd_, dc, srvs, sm->vbv, sm->ibv, sm->index_count);
}

bool FrameRenderer::shadow_ready() const {
    return cmd_ && shadow_ && shadow_->valid();
}

void FrameRenderer::shadow_begin(const math::Mat4x4& sun_view_proj) {
    if (cmd_ && shadow_) shadow_->begin(cmd_, sun_view_proj);
}

void FrameRenderer::shadow_draw(rhi::GpuMesh mesh, const math::Mat4x4& model, float cutoff,
                                rhi::GpuTexture base, const float* uv_off, const float* uv_scale) {
    if (!cmd_ || !shadow_ || !table_) return;
    const GpuSubmesh* sm = table_->resolve(mesh);
    if (!sm) return;
    D3D12_GPU_DESCRIPTOR_HANDLE base_srv{};
    if (const GpuTexture* fbw = table_->resolve(fb_white_)) base_srv = fbw->srv_gpu;
    if (base.id) if (const GpuTexture* t = table_->resolve(base)) if (t->resource) base_srv = t->srv_gpu;
    shadow_->draw(cmd_, model, cutoff, uv_off, uv_scale, base_srv,
                  sm->vbv, sm->ibv, sm->index_count);
}

void FrameRenderer::shadow_end() {
    if (cmd_ && shadow_) shadow_->end(cmd_);
}

void FrameRenderer::post(const PostSettings& ps) {
    if (!cmd_ || !post_ || !dev_ || !targets_ || !targets_->valid()) return;
    post_->execute(cmd_, *targets_, dev_->backbuffer_rtv(), ps);
}

} // namespace hd2d
