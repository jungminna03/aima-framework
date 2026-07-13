#pragma once

#include "core/math_compat.h"
#include "renderer/device.h"

#include <string>

namespace hd2d {

// Depth-only pass rendering the sun's shadow map (orthographic, fitted to the
// map bounds by the caller). Alpha-tested so billboard sprites cast cutout
// silhouettes. Hot-reloads from shaders/shadow.hlsl like every other pass.
class ShadowPass {
public:
    static constexpr uint32_t kSize = 4096;   // 2048→4096: 텍셀 절반 → 그림자 엣지 계단 완화

    bool init(Dx12Device& dev, const std::string& shader_path);
    bool reload_shader();
    void shutdown(Dx12Device& dev);

    // Transition to DEPTH_WRITE, clear, bind the shadow viewport + root state.
    void begin(ID3D12GraphicsCommandList* cmd, const dx::XMFLOAT4X4& sun_view_proj);
    // One alpha-tested depth draw (cutoff 0 = opaque, never discards).
    void draw(ID3D12GraphicsCommandList* cmd, const dx::XMFLOAT4X4& model,
              float alpha_cutoff, const float uv_offset[2], const float uv_scale[2],
              D3D12_GPU_DESCRIPTOR_HANDLE base_tex,
              const D3D12_VERTEX_BUFFER_VIEW& vbv,
              const D3D12_INDEX_BUFFER_VIEW& ibv, uint32_t index_count);
    // Transition to PIXEL_SHADER_RESOURCE for the scene pass.
    void end(ID3D12GraphicsCommandList* cmd);

    D3D12_GPU_DESCRIPTOR_HANDLE srv() const { return srv_gpu_; }
    bool valid() const { return depth_ != nullptr; }

private:
    template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    bool build_root_signature();
    bool build_pso();
    bool ensure_target(Dx12Device& dev);
    void* alloc_constants(size_t size, D3D12_GPU_VIRTUAL_ADDRESS* out_va);

    Dx12Device* dev_ = nullptr;
    std::string shader_path_;

    ComPtr<ID3D12RootSignature> root_sig_;
    ComPtr<ID3D12PipelineState> pso_;
    ComPtr<ID3DBlob> vs_;
    ComPtr<ID3DBlob> ps_;

    ComPtr<ID3D12Resource> depth_;
    ComPtr<ID3D12DescriptorHeap> dsv_heap_;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_cpu_{};
    D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu_{};
    D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu_{};
    bool srv_allocated_ = false;
    D3D12_RESOURCE_STATES state_ = D3D12_RESOURCE_STATE_DEPTH_WRITE;

    static constexpr size_t kRingSize = 256u << 10;  // 256 KB per frame
    ComPtr<ID3D12Resource> cb_ring_[Dx12Device::kFrameCount];
    uint8_t* cb_mapped_[Dx12Device::kFrameCount] = {};
    size_t cb_offset_ = 0;
    uint32_t ring_index_ = 0;
};

} // namespace hd2d
