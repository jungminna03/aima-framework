#pragma once

#include "renderer/device.h"

namespace hd2d {

// The HDR scene render target (RGBA16F) + its own depth buffer. The forward
// pass renders here; the post chain reads it (bloom + tonemap) and writes the
// swapchain backbuffer. Lazily (re)created when the swapchain size changes.
class SceneTargets {
public:
    static constexpr DXGI_FORMAT kHdrFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

    // (Re)create for the given size if needed. Returns false on failure.
    bool ensure(Dx12Device& dev, uint32_t width, uint32_t height);
    void shutdown(Dx12Device& dev);

    // Barrier helpers (no-ops when already in the requested state).
    void to_render_target(ID3D12GraphicsCommandList* cmd);
    void to_shader_resource(ID3D12GraphicsCommandList* cmd);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv() const { return rtv_cpu_; }
    D3D12_CPU_DESCRIPTOR_HANDLE dsv() const { return dsv_cpu_; }
    D3D12_GPU_DESCRIPTOR_HANDLE srv() const { return srv_gpu_; }
    // Depth read view (R32_FLOAT over the R32_TYPELESS depth) — DoF CoC가 샘플.
    // to_shader_resource()가 depth도 PSR로 넘기고 to_render_target()이 되돌린다.
    D3D12_GPU_DESCRIPTOR_HANDLE depth_srv() const { return depth_srv_gpu_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    bool valid() const { return hdr_ != nullptr; }

private:
    template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    ComPtr<ID3D12Resource> hdr_;
    ComPtr<ID3D12Resource> depth_;
    ComPtr<ID3D12DescriptorHeap> rtv_heap_;   // 1 slot
    ComPtr<ID3D12DescriptorHeap> dsv_heap_;   // 1 slot
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_cpu_{};
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_cpu_{};
    D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu_{};
    D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu_{};
    D3D12_CPU_DESCRIPTOR_HANDLE depth_srv_cpu_{};
    D3D12_GPU_DESCRIPTOR_HANDLE depth_srv_gpu_{};
    D3D12_RESOURCE_STATES hdr_state_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
    D3D12_RESOURCE_STATES depth_state_ = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool srv_allocated_ = false;
};

} // namespace hd2d
