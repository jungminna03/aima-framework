#include "renderer/dx12/device.h"

#include "core/log_compat.h"

#include <D3D12MemAlloc.h>
#include <stb_image_write.h>

#include <cassert>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace hd2d {
namespace {

#define HR_RETURN(expr, msg)                                                    \
    do {                                                                        \
        HRESULT _hr = (expr);                                                   \
        if (FAILED(_hr)) {                                                      \
            HD2D_ERROR("{} (hr=0x{:08X})", msg, static_cast<unsigned>(_hr));    \
            return false;                                                       \
        }                                                                       \
    } while (0)

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* res,
                                  D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    b.Transition.pResource = res;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return b;
}

} // namespace

bool Dx12Device::init(HWND hwnd, uint32_t width, uint32_t height) {
    width_ = width;
    height_ = height;

    UINT factory_flags = 0;
#ifndef NDEBUG
    {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            factory_flags |= DXGI_CREATE_FACTORY_DEBUG;
            HD2D_INFO("D3D12 debug layer enabled");
        }
    }
#endif

    HR_RETURN(CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&factory_)),
              "CreateDXGIFactory2 failed");

    BOOL allow_tearing = FALSE;
    if (SUCCEEDED(factory_->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                                &allow_tearing, sizeof(allow_tearing)))) {
        tearing_supported_ = allow_tearing == TRUE;
    }

    if (!pick_adapter()) {
        HD2D_ERROR("no DX12-capable adapter found");
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC qdesc{};
    qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qdesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    HR_RETURN(device_->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&queue_)),
              "CreateCommandQueue failed");

    if (!create_swapchain(hwnd, width, height)) return false;

    // RTV heap (one per back buffer).
    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
    rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_desc.NumDescriptors = kFrameCount;
    rtv_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    HR_RETURN(device_->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap_)),
              "create RTV heap failed");
    rtv_descriptor_size_ =
        device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // Shader-visible SRV heap shared by ImGui and future textures.
    D3D12_DESCRIPTOR_HEAP_DESC srv_desc{};
    srv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_desc.NumDescriptors = kSrvHeapCapacity;
    srv_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    HR_RETURN(device_->CreateDescriptorHeap(&srv_desc, IID_PPV_ARGS(&srv_heap_)),
              "create SRV heap failed");
    srv_descriptor_size_ =
        device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    srv_free_list_.reserve(kSrvHeapCapacity);
    for (uint32_t i = kSrvHeapCapacity; i-- > 0;) srv_free_list_.push_back(i);

    create_backbuffer_rtvs();

    // DSV heap (single depth target, recreated on resize).
    D3D12_DESCRIPTOR_HEAP_DESC dsv_desc{};
    dsv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsv_desc.NumDescriptors = 1;
    dsv_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    HR_RETURN(device_->CreateDescriptorHeap(&dsv_desc, IID_PPV_ARGS(&dsv_heap_)),
              "create DSV heap failed");
    if (!create_depth(width, height)) return false;

    for (uint32_t i = 0; i < kFrameCount; ++i) {
        HR_RETURN(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  IID_PPV_ARGS(&cmd_allocators_[i])),
                  "create command allocator failed");
    }
    HR_RETURN(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         cmd_allocators_[frame_index_].Get(), nullptr,
                                         IID_PPV_ARGS(&cmd_list_)),
              "create command list failed");
    cmd_list_->Close();

    HR_RETURN(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)),
              "create fence failed");
    fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fence_event_) {
        HD2D_ERROR("CreateEvent failed");
        return false;
    }

    D3D12MA::ALLOCATOR_DESC alloc_desc{};
    alloc_desc.pDevice = device_.Get();
    alloc_desc.pAdapter = adapter_.Get();
    HR_RETURN(D3D12MA::CreateAllocator(&alloc_desc, &allocator_),
              "D3D12MA::CreateAllocator failed");

    HD2D_INFO("DX12 device initialized ({} back buffers, tearing={})",
              kFrameCount, tearing_supported_);
    return true;
}

bool Dx12Device::pick_adapter() {
    for (UINT i = 0;
         factory_->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                              IID_PPV_ARGS(&adapter_)) != DXGI_ERROR_NOT_FOUND;
         ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter_->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            adapter_.Reset();
            continue;
        }
        if (SUCCEEDED(D3D12CreateDevice(adapter_.Get(), D3D_FEATURE_LEVEL_11_0,
                                        IID_PPV_ARGS(&device_)))) {
            char name[128]{};
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name),
                                nullptr, nullptr);
            HD2D_INFO("selected GPU: {} ({} MB VRAM)", name,
                      desc.DedicatedVideoMemory / (1024 * 1024));
            return true;
        }
        adapter_.Reset();
    }
    return false;
}

bool Dx12Device::create_swapchain(HWND hwnd, uint32_t width, uint32_t height) {
    DXGI_SWAP_CHAIN_DESC1 sc{};
    sc.Width = width;
    sc.Height = height;
    sc.Format = kBackBufferFormat;
    sc.SampleDesc.Count = 1;
    sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc.BufferCount = kFrameCount;
    sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    sc.Flags = tearing_supported_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

    ComPtr<IDXGISwapChain1> sc1;
    HR_RETURN(factory_->CreateSwapChainForHwnd(queue_.Get(), hwnd, &sc, nullptr, nullptr, &sc1),
              "CreateSwapChainForHwnd failed");
    // We handle fullscreen transitions ourselves.
    factory_->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    HR_RETURN(sc1.As(&swapchain_), "query IDXGISwapChain3 failed");
    frame_index_ = swapchain_->GetCurrentBackBufferIndex();
    return true;
}

void Dx12Device::create_backbuffer_rtvs() {
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t i = 0; i < kFrameCount; ++i) {
        swapchain_->GetBuffer(i, IID_PPV_ARGS(&back_buffers_[i]));
        device_->CreateRenderTargetView(back_buffers_[i].Get(), nullptr, rtv);
        rtv.ptr += rtv_descriptor_size_;
    }
}

void Dx12Device::release_backbuffers() {
    for (auto& bb : back_buffers_) bb.Reset();
}

bool Dx12Device::create_depth(uint32_t width, uint32_t height) {
    depth_.Reset();

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = width;
    rd.Height = height;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = kDepthFormat;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clear{};
    clear.Format = kDepthFormat;
    clear.DepthStencil.Depth = 1.0f;

    HR_RETURN(device_->CreateCommittedResource(
                  &heap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                  &clear, IID_PPV_ARGS(&depth_)),
              "create depth buffer failed");

    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = kDepthFormat;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device_->CreateDepthStencilView(depth_.Get(), &dsv,
                                    dsv_heap_->GetCPUDescriptorHandleForHeapStart());
    return true;
}

ID3D12GraphicsCommandList* Dx12Device::begin_frame(const float clear_color[4]) {
    // Wait until this back buffer's previous work is finished.
    if (fence_->GetCompletedValue() < fence_values_[frame_index_]) {
        fence_->SetEventOnCompletion(fence_values_[frame_index_], fence_event_);
        WaitForSingleObject(fence_event_, INFINITE);
    }

    cmd_allocators_[frame_index_]->Reset();
    cmd_list_->Reset(cmd_allocators_[frame_index_].Get(), nullptr);

    auto to_rt = transition(back_buffers_[frame_index_].Get(),
                            D3D12_RESOURCE_STATE_PRESENT,
                            D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmd_list_->ResourceBarrier(1, &to_rt);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(frame_index_) * rtv_descriptor_size_;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsv_heap_->GetCPUDescriptorHandleForHeapStart();
    cmd_list_->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    cmd_list_->ClearRenderTargetView(rtv, clear_color, 0, nullptr);
    cmd_list_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_),
                      0.0f, 1.0f};
    D3D12_RECT scissor{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
    cmd_list_->RSSetViewports(1, &vp);
    cmd_list_->RSSetScissorRects(1, &scissor);

    ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get()};
    cmd_list_->SetDescriptorHeaps(1, heaps);

    return cmd_list_.Get();
}

void Dx12Device::end_frame(bool vsync) {
    ID3D12Resource* current_bb = back_buffers_[frame_index_].Get();

    // Optional screenshot: copy the just-rendered RT into a readback buffer
    // before flipping it to PRESENT.
    if (!capture_path_.empty()) {
        D3D12_RESOURCE_DESC bb_desc = current_bb->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT64 total = 0;
        device_->GetCopyableFootprints(&bb_desc, 0, 1, 0, &fp, nullptr, nullptr, &total);

        D3D12_HEAP_PROPERTIES rb_heap{};
        rb_heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rb_desc{};
        rb_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rb_desc.Width = total;
        rb_desc.Height = 1;
        rb_desc.DepthOrArraySize = 1;
        rb_desc.MipLevels = 1;
        rb_desc.Format = DXGI_FORMAT_UNKNOWN;
        rb_desc.SampleDesc.Count = 1;
        rb_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        capture_readback_.Reset();
        device_->CreateCommittedResource(&rb_heap, D3D12_HEAP_FLAG_NONE, &rb_desc,
                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                         IID_PPV_ARGS(&capture_readback_));

        auto to_src = transition(current_bb, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                 D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd_list_->ResourceBarrier(1, &to_src);

        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = capture_readback_.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = fp;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = current_bb;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        cmd_list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        auto src_to_present = transition(current_bb, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                         D3D12_RESOURCE_STATE_PRESENT);
        cmd_list_->ResourceBarrier(1, &src_to_present);
        capture_pending_write_ = true;
    } else {
        auto to_present = transition(current_bb, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                     D3D12_RESOURCE_STATE_PRESENT);
        cmd_list_->ResourceBarrier(1, &to_present);
    }

    cmd_list_->Close();

    ID3D12CommandList* lists[] = {cmd_list_.Get()};
    queue_->ExecuteCommandLists(1, lists);

    const UINT sync_interval = vsync ? 1 : 0;
    const UINT present_flags = (!vsync && tearing_supported_) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    swapchain_->Present(sync_interval, present_flags);

    const uint64_t signal = ++last_signaled_;
    queue_->Signal(fence_.Get(), signal);
    fence_values_[frame_index_] = signal;

    if (capture_pending_write_) {
        flush();              // ensure the copy is done
        write_screenshot();
        capture_pending_write_ = false;
        capture_path_.clear();
        capture_readback_.Reset();
    }

    frame_index_ = swapchain_->GetCurrentBackBufferIndex();
}

void Dx12Device::write_screenshot() {
    if (!capture_readback_) return;

    D3D12_RESOURCE_DESC bb_desc = back_buffers_[frame_index_]->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    device_->GetCopyableFootprints(&bb_desc, 0, 1, 0, &fp, nullptr, nullptr, nullptr);

    const uint32_t w = static_cast<uint32_t>(bb_desc.Width);
    const uint32_t h = bb_desc.Height;
    const uint32_t row_pitch = fp.Footprint.RowPitch;

    void* mapped = nullptr;
    D3D12_RANGE range{0, row_pitch * h};
    if (FAILED(capture_readback_->Map(0, &range, &mapped))) {
        HD2D_ERROR("screenshot: map failed");
        return;
    }

    // De-pad rows (RowPitch is 256-aligned) into a tight RGBA8 buffer.
    // 백버퍼는 kBackBufferFormat = R10G10B10A2_UNORM(10비트) — raw 바이트를 8888로
    // 그대로 쓰면 PNG 색이 사이키델릭하게 깨진다(2026-07-03 진단: 헤드리스 샷 전면
    // 색 오염, 화면 표시는 정상). 픽셀별로 10→8비트 언팩(R 0-9 / G 10-19 / B 20-29).
    std::vector<uint8_t> tight(static_cast<size_t>(w) * h * 4);
    const uint8_t* base = static_cast<const uint8_t*>(mapped);
    const bool ten_bit = bb_desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM;
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* row = base + static_cast<size_t>(y) * row_pitch;
        uint8_t* out = tight.data() + static_cast<size_t>(y) * w * 4;
        if (!ten_bit) {
            memcpy(out, row, static_cast<size_t>(w) * 4);
            continue;
        }
        for (uint32_t x = 0; x < w; ++x) {
            uint32_t v;
            memcpy(&v, row + static_cast<size_t>(x) * 4, 4);
            out[x * 4 + 0] = static_cast<uint8_t>((v & 0x3FFu) >> 2);
            out[x * 4 + 1] = static_cast<uint8_t>(((v >> 10) & 0x3FFu) >> 2);
            out[x * 4 + 2] = static_cast<uint8_t>(((v >> 20) & 0x3FFu) >> 2);
            out[x * 4 + 3] = 255;
        }
    }
    capture_readback_->Unmap(0, nullptr);

    if (stbi_write_png(capture_path_.c_str(), static_cast<int>(w), static_cast<int>(h), 4,
                       tight.data(), static_cast<int>(w * 4))) {
        HD2D_INFO("screenshot written: {} ({}x{})", capture_path_, w, h);
    } else {
        HD2D_ERROR("screenshot: stbi_write_png failed for {}", capture_path_);
    }
}

void Dx12Device::flush() {
    if (!queue_ || !fence_) return;
    const uint64_t signal = ++last_signaled_;
    queue_->Signal(fence_.Get(), signal);
    if (fence_->GetCompletedValue() < signal) {
        fence_->SetEventOnCompletion(signal, fence_event_);
        WaitForSingleObject(fence_event_, INFINITE);
    }
}

void Dx12Device::resize(uint32_t width, uint32_t height) {
    if (!swapchain_ || width == 0 || height == 0) return;
    if (width == width_ && height == height_) return;

    flush();
    release_backbuffers();

    const UINT flags = tearing_supported_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    HRESULT hr = swapchain_->ResizeBuffers(kFrameCount, width, height, kBackBufferFormat, flags);
    if (FAILED(hr)) {
        HD2D_ERROR("ResizeBuffers failed (hr=0x{:08X})", static_cast<unsigned>(hr));
        return;
    }

    width_ = width;
    height_ = height;
    frame_index_ = swapchain_->GetCurrentBackBufferIndex();
    create_backbuffer_rtvs();
    create_depth(width, height);
    HD2D_TRACE("swapchain resized to {}x{}", width, height);
}

bool Dx12Device::alloc_srv(D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu,
                           D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu) {
    if (srv_free_list_.empty()) {
        HD2D_ERROR("SRV descriptor heap exhausted");
        return false;
    }
    const uint32_t index = srv_free_list_.back();
    srv_free_list_.pop_back();
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = srv_heap_->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = srv_heap_->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(index) * srv_descriptor_size_;
    gpu.ptr += static_cast<UINT64>(index) * srv_descriptor_size_;
    *out_cpu = cpu;
    *out_gpu = gpu;
    return true;
}

void Dx12Device::free_srv(D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE /*gpu*/) {
    const SIZE_T base = srv_heap_->GetCPUDescriptorHandleForHeapStart().ptr;
    const uint32_t index = static_cast<uint32_t>((cpu.ptr - base) / srv_descriptor_size_);
    srv_free_list_.push_back(index);
}

void Dx12Device::shutdown() {
    flush();

    if (fence_event_) {
        CloseHandle(fence_event_);
        fence_event_ = nullptr;
    }
    if (allocator_) {
        allocator_->Release();
        allocator_ = nullptr;
    }
    release_backbuffers();
    depth_.Reset();
    capture_readback_.Reset();
    cmd_list_.Reset();
    for (auto& a : cmd_allocators_) a.Reset();
    srv_heap_.Reset();
    rtv_heap_.Reset();
    dsv_heap_.Reset();
    fence_.Reset();
    swapchain_.Reset();
    queue_.Reset();
    device_.Reset();
    adapter_.Reset();
    factory_.Reset();
}

} // namespace hd2d
