#include "renderer/scene_targets.h"

#include "core/log_compat.h"

namespace hd2d {

namespace {
D3D12_RESOURCE_BARRIER transition(ID3D12Resource* res, D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return b;
}
} // namespace

bool SceneTargets::ensure(Dx12Device& dev, uint32_t width, uint32_t height) {
    if (hdr_ && width == width_ && height == height_) return true;
    if (width == 0 || height == 0) return false;

    ID3D12Device* d = dev.device();

    // Resizing: in-flight frames may still reference the old targets.
    if (hdr_) dev.flush();

    if (!rtv_heap_) {
        D3D12_DESCRIPTOR_HEAP_DESC rd{};
        rd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rd.NumDescriptors = 1;
        if (FAILED(d->CreateDescriptorHeap(&rd, IID_PPV_ARGS(&rtv_heap_)))) return false;
        rtv_cpu_ = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
        D3D12_DESCRIPTOR_HEAP_DESC dd{};
        dd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        dd.NumDescriptors = 1;
        if (FAILED(d->CreateDescriptorHeap(&dd, IID_PPV_ARGS(&dsv_heap_)))) return false;
        dsv_cpu_ = dsv_heap_->GetCPUDescriptorHandleForHeapStart();
    }
    if (!srv_allocated_) {
        if (!dev.alloc_srv(&srv_cpu_, &srv_gpu_)) return false;
        if (!dev.alloc_srv(&depth_srv_cpu_, &depth_srv_gpu_)) return false;
        srv_allocated_ = true;
    }

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    // HDR color.
    D3D12_RESOURCE_DESC cd{};
    cd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    cd.Width = width;
    cd.Height = height;
    cd.DepthOrArraySize = 1;
    cd.MipLevels = 1;
    cd.Format = kHdrFormat;
    cd.SampleDesc.Count = 1;
    cd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cclear{};
    cclear.Format = kHdrFormat;
    hdr_.Reset();
    if (FAILED(d->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &cd,
                                          D3D12_RESOURCE_STATE_RENDER_TARGET, &cclear,
                                          IID_PPV_ARGS(&hdr_)))) {
        HD2D_ERROR("scene targets: create HDR target failed ({}x{})", width, height);
        return false;
    }
    hdr_state_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
    d->CreateRenderTargetView(hdr_.Get(), nullptr, rtv_cpu_);

    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = kHdrFormat;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    d->CreateShaderResourceView(hdr_.Get(), &sd, srv_cpu_);

    // Depth (the scene pass owns its own depth; the device's depth stays with
    // the backbuffer path).
    D3D12_RESOURCE_DESC dd{};
    dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    dd.Width = width;
    dd.Height = height;
    dd.DepthOrArraySize = 1;
    dd.MipLevels = 1;
    // TYPELESS + typed views: DSV는 D32_FLOAT, SRV는 R32_FLOAT — 포스트(DoF CoC)가
    // 깊이를 샘플할 수 있게. (D32_FLOAT 리소스는 SRV를 못 만든다.)
    dd.Format = DXGI_FORMAT_R32_TYPELESS;
    dd.SampleDesc.Count = 1;
    dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE dclear{};
    dclear.Format = Dx12Device::kDepthFormat;
    dclear.DepthStencil.Depth = 1.0f;
    depth_.Reset();
    if (FAILED(d->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &dd,
                                          D3D12_RESOURCE_STATE_DEPTH_WRITE, &dclear,
                                          IID_PPV_ARGS(&depth_)))) {
        HD2D_ERROR("scene targets: create depth failed ({}x{})", width, height);
        return false;
    }
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = Dx12Device::kDepthFormat;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    d->CreateDepthStencilView(depth_.Get(), &dsv, dsv_cpu_);

    D3D12_SHADER_RESOURCE_VIEW_DESC dsr{};
    dsr.Format = DXGI_FORMAT_R32_FLOAT;
    dsr.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    dsr.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    dsr.Texture2D.MipLevels = 1;
    d->CreateShaderResourceView(depth_.Get(), &dsr, depth_srv_cpu_);
    depth_state_ = D3D12_RESOURCE_STATE_DEPTH_WRITE;

    width_ = width;
    height_ = height;
    HD2D_INFO("scene targets: {}x{} HDR ready", width, height);
    return true;
}

void SceneTargets::to_render_target(ID3D12GraphicsCommandList* cmd) {
    D3D12_RESOURCE_BARRIER b[2];
    UINT n = 0;
    if (hdr_state_ != D3D12_RESOURCE_STATE_RENDER_TARGET) {
        b[n++] = transition(hdr_.Get(), hdr_state_, D3D12_RESOURCE_STATE_RENDER_TARGET);
        hdr_state_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    if (depth_state_ != D3D12_RESOURCE_STATE_DEPTH_WRITE) {
        b[n++] = transition(depth_.Get(), depth_state_, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        depth_state_ = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    }
    if (n) cmd->ResourceBarrier(n, b);
}

void SceneTargets::to_shader_resource(ID3D12GraphicsCommandList* cmd) {
    D3D12_RESOURCE_BARRIER b[2];
    UINT n = 0;
    if (hdr_state_ != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
        b[n++] = transition(hdr_.Get(), hdr_state_,
                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        hdr_state_ = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }
    if (depth_state_ != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
        b[n++] = transition(depth_.Get(), depth_state_,
                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        depth_state_ = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }
    if (n) cmd->ResourceBarrier(n, b);
}

void SceneTargets::shutdown(Dx12Device& dev) {
    if (srv_allocated_) {
        dev.free_srv(srv_cpu_, srv_gpu_);
        dev.free_srv(depth_srv_cpu_, depth_srv_gpu_);
        srv_allocated_ = false;
    }
    hdr_.Reset();
    depth_.Reset();
    rtv_heap_.Reset();
    dsv_heap_.Reset();
    width_ = height_ = 0;
}

} // namespace hd2d
