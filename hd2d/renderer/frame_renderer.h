#pragma once

// FrameRenderer — the backend-neutral, HIGH-LEVEL scene-drawing API.
//
// The PAL render keystone's "renderer owns drawing" step. The game's render systems
// call THIS (rhi handles + math + plain floats) instead of touching ID3D12 directly:
// the backend owns every GPU command. This header is deliberately D3D12-FREE (only
// forward declarations + plain types) so the game module can include it without
// pulling in <d3d12.h>. The DX12 implementation lives in renderer_impl.cpp; an
// sdlgpu implementation follows so BOTH backends share one game-side render path
// (killing the per-OS render divergence that caused mac-only / win-only bugs).
//
// Migration is incremental: methods are added here as each game render system moves
// off raw ID3D12. The host hands the open command list in begin_frame() each frame;
// the game never sees it.

#include <cstdint>

#include "renderer/rhi.h"        // rhi::GpuMesh / GpuTexture handles (backend-neutral)
#include "core/math_compat.h"    // math::Matrix (DirectXMath facade — not D3D12)

struct ID3D12GraphicsCommandList;   // host-only (begin_frame); game never dereferences it

namespace hd2d {

class Dx12Device;
class ForwardPass;
class ShadowPass;
class PostChain;
struct SceneTargets;
class Dx12ResourceTable;
struct PostSettings;
struct FrameConstants;   // plain constant-buffer data the game fills (no D3D12 in its fields)

// One mesh primitive's material, backend-neutral: rhi texture handles ({0} = use the
// engine fallback) + PBR factors. has_material=false → the renderer uses the resolved
// submesh's own base_color/metallic/roughness (untextured glTF primitives).
struct DrawMaterial {
    bool has_material = false;
    rhi::GpuTexture base{}, mr{}, normal{}, emissive{};
    float base_color[4] = {1, 1, 1, 1};
    float emissive_factor[3] = {0, 0, 0};
    float metallic = 0.0f, roughness = 1.0f, alpha_cutoff = 0.0f;
    uint32_t flags = 0;
};

class FrameRenderer {
public:
    // Wired once at startup (host) with the renderer's sub-objects.
    void init(Dx12Device* dev, ForwardPass* fwd, ShadowPass* shadow,
              PostChain* post, SceneTargets* targets, Dx12ResourceTable* table);

    // Per-frame: the HOST hands the open command list (null on headless / sdlgpu).
    void begin_frame(ID3D12GraphicsCommandList* cmd) { cmd_ = cmd; }
    bool ready() const { return cmd_ != nullptr; }

    // Begin the HDR scene pass: ensure targets at w×h, barrier to render-target,
    // clear colour to `sky` + depth, restore the window viewport, and prime the
    // forward pass with the game-built frame constants. The game fills `fc`/`sky`
    // (plain data); all GPU command recording happens here.
    void scene_begin(const FrameConstants& fc, const float sky[4], uint32_t w, uint32_t h);

    // Bind the opaque (false) or alpha-blend (true) scene PSO — the camera-occlusion
    // fade draws faded meshes translucent between set_translucent(true) / (false).
    void scene_set_translucent(bool on);

    // Draw one mesh primitive at `model` with `mat`. alpha<1 = occlusion fade
    // (translucent, alpha-test off). Resolves rhi handles -> GPU resources internally.
    void scene_draw_mesh(rhi::GpuMesh mesh, const math::Matrix& model,
                         const DrawMaterial& mat, float alpha);

    // Draw one camera-facing sprite quad (billboards + fireflies): tint in `tint`,
    // sprite-atlas uv_off/uv_scale, `sheet` texture ({0} = fallback white).
    void scene_draw_sprite(rhi::GpuMesh quad, const math::Matrix& model, rhi::GpuTexture sheet,
                           const float uv_off[2], const float uv_scale[2],
                           const float tint[4], float alpha_cutoff, uint32_t flags);

    // --- Sun shadow pre-pass (the game computes the fit; these record the GPU work) ---
    bool shadow_ready() const;
    void shadow_begin(const math::Mat4x4& sun_view_proj);
    void shadow_draw(rhi::GpuMesh mesh, const math::Mat4x4& model, float cutoff,
                     rhi::GpuTexture base, const float* uv_off, const float* uv_scale);
    void shadow_end();

    // Resolve the HDR scene target to the backbuffer (bloom + AgX tonemap). The last
    // scene step before ImGui draws over the backbuffer.
    void post(const PostSettings& ps);

private:
    Dx12Device* dev_ = nullptr;
    ForwardPass* fwd_ = nullptr;
    ShadowPass* shadow_ = nullptr;
    PostChain* post_ = nullptr;
    SceneTargets* targets_ = nullptr;
    Dx12ResourceTable* table_ = nullptr;
    ID3D12GraphicsCommandList* cmd_ = nullptr;
    rhi::GpuTexture fb_white_{};    // engine fallback textures, registered in the table at init
    rhi::GpuTexture fb_normal_{};
};

} // namespace hd2d
