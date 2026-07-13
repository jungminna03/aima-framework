#include "renderer/shadow_pass.h"

#include "core/log_compat.h"

#include <d3dcompiler.h>

#include <cstring>
#include <filesystem>

using Microsoft::WRL::ComPtr;

namespace hd2d {

namespace {

bool compile_entry(const std::string& path, const char* entry, const char* target,
                   ComPtr<ID3DBlob>& out) {
    const std::wstring wpath = std::filesystem::path(path).wstring();
    UINT flags = 0;
#ifndef NDEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    ComPtr<ID3DBlob> err;
    HRESULT hr = D3DCompileFromFile(wpath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                    entry, target, flags, 0, &out, &err);
    if (FAILED(hr)) {
        if (err) {
            HD2D_ERROR("shadow shader compile {} failed: {}", entry,
                       static_cast<const char*>(err->GetBufferPointer()));
        } else {
            HD2D_ERROR("shadow shader compile {} failed (hr=0x{:08X})", entry,
                       static_cast<unsigned>(hr));
        }
        return false;
    }
    return true;
}

struct alignas(256) ShadowFrame {
    dx::XMFLOAT4X4 view_proj;
};

struct alignas(256) ShadowDraw {
    dx::XMFLOAT4X4 model;
    float alpha_cutoff;
    float _pad[3];
    float uv_offset[2];
    float uv_scale[2];
};

} // namespace

bool ShadowPass::init(Dx12Device& dev, const std::string& shader_path) {
    dev_ = &dev;
    shader_path_ = shader_path;
    if (!build_root_signature()) return false;
    if (!reload_shader()) return false;
    if (!ensure_target(dev)) return false;

    for (uint32_t i = 0; i < Dx12Device::kFrameCount; ++i) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = kRingSize;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(dev.device()->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&cb_ring_[i])))) {
            HD2D_ERROR("shadow pass: create constant ring failed");
            return false;
        }
        D3D12_RANGE none{0, 0};
        cb_ring_[i]->Map(0, &none, reinterpret_cast<void**>(&cb_mapped_[i]));
    }
    HD2D_INFO("shadow pass initialized ({}x{})", kSize, kSize);
    return true;
}

bool ShadowPass::ensure_target(Dx12Device& dev) {
    ID3D12Device* d = dev.device();
    D3D12_DESCRIPTOR_HEAP_DESC dd{};
    dd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dd.NumDescriptors = 1;
    if (FAILED(d->CreateDescriptorHeap(&dd, IID_PPV_ARGS(&dsv_heap_)))) return false;
    dsv_cpu_ = dsv_heap_->GetCPUDescriptorHandleForHeapStart();

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = kSize;
    rd.Height = kSize;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R32_TYPELESS;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_D32_FLOAT;
    clear.DepthStencil.Depth = 1.0f;
    if (FAILED(d->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd,
                                          D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
                                          IID_PPV_ARGS(&depth_)))) {
        HD2D_ERROR("shadow pass: create depth target failed");
        return false;
    }
    state_ = D3D12_RESOURCE_STATE_DEPTH_WRITE;

    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = DXGI_FORMAT_D32_FLOAT;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    d->CreateDepthStencilView(depth_.Get(), &dsv, dsv_cpu_);

    if (!srv_allocated_) {
        if (!dev.alloc_srv(&srv_cpu_, &srv_gpu_)) return false;
        srv_allocated_ = true;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R32_FLOAT;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    d->CreateShaderResourceView(depth_.Get(), &sd, srv_cpu_);
    return true;
}

bool ShadowPass::build_root_signature() {
    D3D12_DESCRIPTOR_RANGE srv_range{};
    srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv_range.NumDescriptors = 1;
    srv_range.BaseShaderRegister = 0;  // t0 base color (alpha test)

    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;  // b0 frame
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;  // b1 draw
    params[1].Descriptor.ShaderRegister = 1;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &srv_range;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // Point/clamp: crisp sprite-sheet cutouts without neighbor-frame bleed.
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 3;
    desc.pParameters = params;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers = &sampler;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> blob, err;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob,
                                           &err))) {
        HD2D_ERROR("shadow: serialize root signature failed: {}",
                   err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        return false;
    }
    if (FAILED(dev_->device()->CreateRootSignature(0, blob->GetBufferPointer(),
                                                   blob->GetBufferSize(),
                                                   IID_PPV_ARGS(&root_sig_)))) {
        HD2D_ERROR("shadow: create root signature failed");
        return false;
    }
    return true;
}

bool ShadowPass::reload_shader() {
    ComPtr<ID3DBlob> vs, ps;
    if (!compile_entry(shader_path_, "VSMain", "vs_5_1", vs)) return false;
    if (!compile_entry(shader_path_, "PSMain", "ps_5_1", ps)) return false;
    vs_ = vs;
    ps_ = ps;
    return build_pso();
}

bool ShadowPass::build_pso() {
    D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TANGENT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 48, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root_sig_.Get();
    pd.VS = {vs_->GetBufferPointer(), vs_->GetBufferSize()};
    pd.PS = {ps_->GetBufferPointer(), ps_->GetBufferSize()};
    pd.InputLayout = {layout, _countof(layout)};
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 0;
    pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pd.SampleDesc.Count = 1;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    // Slope-scaled bias carved into the PSO (cheap, stable for an ortho sun).
    pd.RasterizerState.DepthBias = 16;
    pd.RasterizerState.SlopeScaledDepthBias = 2.0f;
    pd.DepthStencilState.DepthEnable = TRUE;
    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

    ComPtr<ID3D12PipelineState> pso;
    if (FAILED(dev_->device()->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)))) {
        HD2D_ERROR("shadow: create PSO failed (keeping previous)");
        return false;
    }
    pso_ = pso;
    return true;
}

void* ShadowPass::alloc_constants(size_t size, D3D12_GPU_VIRTUAL_ADDRESS* out_va) {
    const size_t aligned = (size + 255) & ~size_t(255);
    if (cb_offset_ + aligned > kRingSize) cb_offset_ = 0;
    const size_t off = cb_offset_;
    cb_offset_ += aligned;
    *out_va = cb_ring_[ring_index_]->GetGPUVirtualAddress() + off;
    return cb_mapped_[ring_index_] + off;
}

void ShadowPass::begin(ID3D12GraphicsCommandList* cmd, const dx::XMFLOAT4X4& sun_view_proj) {
    ring_index_ = (ring_index_ + 1) % Dx12Device::kFrameCount;
    cb_offset_ = 0;

    if (state_ != D3D12_RESOURCE_STATE_DEPTH_WRITE) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = depth_.Get();
        b.Transition.StateBefore = state_;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1, &b);
        state_ = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    }

    cmd->OMSetRenderTargets(0, nullptr, FALSE, &dsv_cpu_);
    cmd->ClearDepthStencilView(dsv_cpu_, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    D3D12_VIEWPORT vp{0, 0, float(kSize), float(kSize), 0.0f, 1.0f};
    D3D12_RECT sc{0, 0, LONG(kSize), LONG(kSize)};
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);

    cmd->SetGraphicsRootSignature(root_sig_.Get());
    cmd->SetPipelineState(pso_.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ShadowFrame sf{};
    sf.view_proj = sun_view_proj;
    D3D12_GPU_VIRTUAL_ADDRESS va{};
    void* dst = alloc_constants(sizeof(ShadowFrame), &va);
    std::memcpy(dst, &sf, sizeof(ShadowFrame));
    cmd->SetGraphicsRootConstantBufferView(0, va);
}

void ShadowPass::draw(ID3D12GraphicsCommandList* cmd, const dx::XMFLOAT4X4& model,
                      float alpha_cutoff, const float uv_offset[2], const float uv_scale[2],
                      D3D12_GPU_DESCRIPTOR_HANDLE base_tex,
                      const D3D12_VERTEX_BUFFER_VIEW& vbv,
                      const D3D12_INDEX_BUFFER_VIEW& ibv, uint32_t index_count) {
    ShadowDraw sd{};
    sd.model = model;
    sd.alpha_cutoff = alpha_cutoff;
    sd.uv_offset[0] = uv_offset ? uv_offset[0] : 0.0f;
    sd.uv_offset[1] = uv_offset ? uv_offset[1] : 0.0f;
    sd.uv_scale[0] = uv_scale ? uv_scale[0] : 1.0f;
    sd.uv_scale[1] = uv_scale ? uv_scale[1] : 1.0f;
    D3D12_GPU_VIRTUAL_ADDRESS va{};
    void* dst = alloc_constants(sizeof(ShadowDraw), &va);
    std::memcpy(dst, &sd, sizeof(ShadowDraw));
    cmd->SetGraphicsRootConstantBufferView(1, va);
    cmd->SetGraphicsRootDescriptorTable(2, base_tex);
    cmd->IASetVertexBuffers(0, 1, &vbv);
    cmd->IASetIndexBuffer(&ibv);
    cmd->DrawIndexedInstanced(index_count, 1, 0, 0, 0);
}

void ShadowPass::end(ID3D12GraphicsCommandList* cmd) {
    if (state_ != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = depth_.Get();
        b.Transition.StateBefore = state_;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1, &b);
        state_ = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }
}

void ShadowPass::shutdown(Dx12Device& dev) {
    for (uint32_t i = 0; i < Dx12Device::kFrameCount; ++i) {
        if (cb_ring_[i]) cb_ring_[i]->Unmap(0, nullptr);
        cb_ring_[i].Reset();
        cb_mapped_[i] = nullptr;
    }
    if (srv_allocated_) {
        dev.free_srv(srv_cpu_, srv_gpu_);
        srv_allocated_ = false;
    }
    depth_.Reset();
    dsv_heap_.Reset();
    pso_.Reset();
    root_sig_.Reset();
    vs_.Reset();
    ps_.Reset();
}

} // namespace hd2d
