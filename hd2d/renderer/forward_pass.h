#pragma once

#include "core/math_compat.h"
#include "renderer/device.h"
#include "renderer/render_constants.h"   // FrameConstants / DrawConstants (D3D12-free, 게임 공유)

#include <string>

namespace hd2d {

// FrameConstants + DrawConstants (plain cbuffer mirrors of shaders/standard.hlsl)
// moved to renderer/render_constants.h so the game can build them without <d3d12.h>
// (PAL render keystone). MaterialSrvs (D3D12 descriptors) stays renderer-side here.

// The four material SRVs every draw binds (use the engine fallbacks for
// slots a material doesn't provide).
struct MaterialSrvs {
    D3D12_GPU_DESCRIPTOR_HANDLE base{};
    D3D12_GPU_DESCRIPTOR_HANDLE mr{};
    D3D12_GPU_DESCRIPTOR_HANDLE normal{};
    D3D12_GPU_DESCRIPTOR_HANDLE emissive{};
};

// The single forward pass every drawable goes through (meshes + billboards).
// Owns the root signature, PSO, and a per-frame constant upload ring. The shader
// is compiled at runtime (D3DCompile) so it hot-reloads.
class ForwardPass {
public:
    bool init(Dx12Device& dev, const std::string& shader_path);
    void shutdown();

    // Recompile the shader and rebuild the PSO. On failure the previous PSO is
    // kept so a bad edit never takes the renderer down (Day-1 hot-reload spec).
    bool reload_shader();

    // Bind root sig + PSO + per-frame constants + the sun shadow map SRV.
    // Call once per frame after the device's begin_frame (binds the SRV heap).
    void begin(ID3D12GraphicsCommandList* cmd, const FrameConstants& frame,
               D3D12_GPU_DESCRIPTOR_HANDLE shadow_srv);

    // Draw one primitive with its material textures (handles into the device's
    // shared SRV heap; pass fallbacks for missing maps).
    void draw(ID3D12GraphicsCommandList* cmd, const DrawConstants& dc,
              const MaterialSrvs& mat,
              const D3D12_VERTEX_BUFFER_VIEW& vbv,
              const D3D12_INDEX_BUFFER_VIEW& ibv, uint32_t index_count);

    // Switch the bound pipeline between the opaque PSO (begin() default) and the
    // alpha-blend PSO used for the camera-occlusion fade pass (SRC_ALPHA over,
    // depth-test LESS_EQUAL, depth-write OFF). Call before/after the faded draws;
    // draw() itself is PSO-agnostic (it only sets constants + buffers).
    void set_translucent(ID3D12GraphicsCommandList* cmd, bool on);

private:
    template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    bool build_root_signature();
    bool build_pso();
    void* alloc_constants(size_t size, D3D12_GPU_VIRTUAL_ADDRESS* out_va);

    Dx12Device* dev_ = nullptr;
    std::string shader_path_;

    ComPtr<ID3D12RootSignature> root_sig_;
    ComPtr<ID3D12PipelineState> pso_;
    ComPtr<ID3D12PipelineState> pso_blend_;   // translucent occluder-fade variant
    ComPtr<ID3DBlob> vs_;
    ComPtr<ID3DBlob> ps_;

    // Per-frame-in-flight constant upload ring (linear bump allocator).
    static constexpr size_t kRingSize = 1u << 20;  // 1 MB per frame
    ComPtr<ID3D12Resource> cb_ring_[Dx12Device::kFrameCount];
    uint8_t* cb_mapped_[Dx12Device::kFrameCount] = {};
    size_t cb_offset_ = 0;
    uint32_t ring_index_ = 0;
};

} // namespace hd2d
