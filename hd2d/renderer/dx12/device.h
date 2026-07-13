#pragma once

#include <cstdint>
#include <vector>

// Use the DirectX-Headers (Agility) d3d12.h to stay consistent with
// D3D12MemoryAllocator (built with D3D12MA_USING_DIRECTX_HEADERS). It shares the
// __d3d12_h__ guard, so it also suppresses the Windows SDK d3d12.h that ImGui's
// DX12 backend includes — as long as this header is included first.
#include <directx/d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <string>

namespace D3D12MA {
class Allocator;
}

namespace hd2d {

// Minimal DX12 device: device, command queue, flip-model swapchain, RTVs,
// per-frame command allocators, fence sync, a shader-visible SRV heap (for
// ImGui / future textures), and a D3D12MemoryAllocator instance.
class Dx12Device {
public:
    // 10비트 출력(채널당 1024단계) — 8비트(256) 밴딩 완화. 백버퍼 소비처(swapchain/
    // ImGui PSO/PostChain tonemap PSO)가 전부 이 상수를 참조해 함께 따라간다.
    static constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
    static constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;
    static constexpr uint32_t kFrameCount = 3;

    bool init(HWND hwnd, uint32_t width, uint32_t height);
    void shutdown();
    void resize(uint32_t width, uint32_t height);

    // Wait for the GPU on this frame, reset the command list, transition the
    // current back buffer to RENDER_TARGET, clear color + depth, bind RTV/DSV +
    // the SRV heap. Returns the open command list for additional recording.
    ID3D12GraphicsCommandList* begin_frame(const float clear_color[4]);
    // Transition back buffer to PRESENT, execute, present, and signal the fence.
    // If a screenshot was requested, copies the frame to a readback buffer first.
    void end_frame(bool vsync);

    // Queue a PNG screenshot of the next rendered frame (consumed in end_frame).
    void request_screenshot(const std::string& path) { capture_path_ = path; }

    void flush(); // block until the GPU is idle (use before teardown/resize)

    ID3D12Device*         device()    const { return device_.Get(); }
    ID3D12CommandQueue*   queue()     const { return queue_.Get(); }
    ID3D12DescriptorHeap* srv_heap()  const { return srv_heap_.Get(); }
    uint32_t              srv_increment() const { return srv_descriptor_size_; }
    uint32_t              width()     const { return width_; }
    uint32_t              height()    const { return height_; }

    // RTV of the backbuffer currently being recorded (valid between
    // begin_frame and end_frame; the resource is in RENDER_TARGET state).
    D3D12_CPU_DESCRIPTOR_HANDLE backbuffer_rtv() const {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>(frame_index_) * rtv_descriptor_size_;
        return rtv;
    }

    // Descriptor sub-allocation from the shared shader-visible SRV heap.
    bool alloc_srv(D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu, D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu);
    void free_srv(D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE gpu);

private:
    template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    bool pick_adapter();
    bool create_swapchain(HWND hwnd, uint32_t width, uint32_t height);
    void create_backbuffer_rtvs();
    void release_backbuffers();
    bool create_depth(uint32_t width, uint32_t height);
    void write_screenshot();

    static constexpr uint32_t kSrvHeapCapacity = 256;

    ComPtr<IDXGIFactory6>            factory_;
    ComPtr<IDXGIAdapter1>           adapter_;
    ComPtr<ID3D12Device>            device_;
    ComPtr<ID3D12CommandQueue>      queue_;
    ComPtr<IDXGISwapChain3>         swapchain_;

    ComPtr<ID3D12DescriptorHeap>    rtv_heap_;
    uint32_t                        rtv_descriptor_size_ = 0;
    ComPtr<ID3D12Resource>          back_buffers_[kFrameCount];

    ComPtr<ID3D12DescriptorHeap>    dsv_heap_;
    ComPtr<ID3D12Resource>          depth_;

    std::string                     capture_path_;          // pending screenshot
    ComPtr<ID3D12Resource>          capture_readback_;
    bool                            capture_pending_write_ = false;

    ComPtr<ID3D12CommandAllocator>  cmd_allocators_[kFrameCount];
    ComPtr<ID3D12GraphicsCommandList> cmd_list_;

    ComPtr<ID3D12Fence>             fence_;
    uint64_t                        fence_values_[kFrameCount] = {}; // value GPU reaches when frame i is done
    uint64_t                        last_signaled_ = 0;              // monotonic fence counter
    HANDLE                          fence_event_ = nullptr;

    ComPtr<ID3D12DescriptorHeap>    srv_heap_;
    uint32_t                        srv_descriptor_size_ = 0;
    std::vector<uint32_t>           srv_free_list_;

    D3D12MA::Allocator*             allocator_ = nullptr;

    uint32_t frame_index_ = 0;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool     tearing_supported_ = false;
};

} // namespace hd2d
