#include "renderer/post_chain.h"

#include "core/log_compat.h"

#include <d3dcompiler.h>

#include <algorithm>
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
            HD2D_ERROR("post shader compile {} failed: {}", entry,
                       static_cast<const char*>(err->GetBufferPointer()));
        } else {
            HD2D_ERROR("post shader compile {} failed (hr=0x{:08X})", entry,
                       static_cast<unsigned>(hr));
        }
        return false;
    }
    return true;
}

// Root constants pushed to every post draw (b0). shaders/post.hlsl cbuffer와 1:1.
struct PostConstants {
    float exposure;
    float bloom_intensity;
    uint32_t flags;      // bit0 AgX, bit1 input-linear, bit2 first bloom down, bit3 DoF
    float threshold;
    float knee;
    float texel_x;       // texel size of the SOURCE being sampled
    float texel_y;
    float _pad;
    // --- 틸트시프트 DoF (dof_math.h / post.hlsl과 1:1) ---
    float dof_focus_dist;
    float dof_focus_range;
    float dof_blur_range;
    float dof_strength;
    float dof_band_center;
    float dof_band_half;
    float dof_band_feather;
    float dof_protect;
    float dof_max_coc;
    float cam_near;
    float cam_far;
    float _pad2;
};

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

bool PostChain::init(Dx12Device& dev, const std::string& shader_path) {
    dev_ = &dev;
    shader_path_ = shader_path;
    if (!build_root_signature()) return false;
    if (!reload_shader()) return false;
    HD2D_INFO("post chain initialized");
    return true;
}

bool PostChain::build_root_signature() {
    D3D12_DESCRIPTOR_RANGE scene_range{};
    scene_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    scene_range.NumDescriptors = 1;
    scene_range.BaseShaderRegister = 0;  // t0 scene HDR / bloom source
    D3D12_DESCRIPTOR_RANGE bloom_range = scene_range;
    bloom_range.BaseShaderRegister = 1;  // t1 bloom composite input
    D3D12_DESCRIPTOR_RANGE depth_range = scene_range;
    depth_range.BaseShaderRegister = 2;  // t2 scene depth (DoF CoC)
    D3D12_DESCRIPTOR_RANGE dof_range = scene_range;
    dof_range.BaseShaderRegister = 3;    // t3 DoF result (tonemap composite)

    D3D12_ROOT_PARAMETER params[5]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;  // b0
    params[0].Constants.Num32BitValues = sizeof(PostConstants) / 4;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &scene_range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &bloom_range;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3].DescriptorTable.NumDescriptorRanges = 1;
    params[3].DescriptorTable.pDescriptorRanges = &depth_range;
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[4].DescriptorTable.NumDescriptorRanges = 1;
    params[4].DescriptorTable.pDescriptorRanges = &dof_range;
    params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samplers[2]{};
    samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[0].ShaderRegister = 0;
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    samplers[1] = samplers[0];
    samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;  // s1: depth는 보간 없이
    samplers[1].ShaderRegister = 1;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 5;
    desc.pParameters = params;
    desc.NumStaticSamplers = 2;
    desc.pStaticSamplers = samplers;

    ComPtr<ID3DBlob> blob, err;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob,
                                           &err))) {
        HD2D_ERROR("post: serialize root signature failed: {}",
                   err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        return false;
    }
    if (FAILED(dev_->device()->CreateRootSignature(0, blob->GetBufferPointer(),
                                                   blob->GetBufferSize(),
                                                   IID_PPV_ARGS(&root_sig_)))) {
        HD2D_ERROR("post: create root signature failed");
        return false;
    }
    return true;
}

bool PostChain::reload_shader() {
    ComPtr<ID3DBlob> vs, ps_tone, ps_down, ps_up, ps_coc, ps_blur, ps_tent;
    if (!compile_entry(shader_path_, "VSFullscreen", "vs_5_1", vs)) return false;
    if (!compile_entry(shader_path_, "PSTonemap", "ps_5_1", ps_tone)) return false;
    if (!compile_entry(shader_path_, "PSBloomDown", "ps_5_1", ps_down)) return false;
    if (!compile_entry(shader_path_, "PSBloomUp", "ps_5_1", ps_up)) return false;
    if (!compile_entry(shader_path_, "PSDofCoc", "ps_5_1", ps_coc)) return false;
    if (!compile_entry(shader_path_, "PSDofBlur", "ps_5_1", ps_blur)) return false;
    if (!compile_entry(shader_path_, "PSDofTent", "ps_5_1", ps_tent)) return false;
    vs_ = vs;
    ps_tonemap_ = ps_tone;
    ps_bloom_down_ = ps_down;
    ps_bloom_up_ = ps_up;
    ps_dof_coc_ = ps_coc;
    ps_dof_blur_ = ps_blur;
    ps_dof_tent_ = ps_tent;
    return build_psos();
}

bool PostChain::build_psos() {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root_sig_.Get();
    pd.VS = {vs_->GetBufferPointer(), vs_->GetBufferSize()};
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.SampleDesc.Count = 1;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    for (auto& rt : pd.BlendState.RenderTarget)
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.DepthStencilState.DepthEnable = FALSE;

    // Tonemap -> backbuffer.
    pd.PS = {ps_tonemap_->GetBufferPointer(), ps_tonemap_->GetBufferSize()};
    pd.RTVFormats[0] = Dx12Device::kBackBufferFormat;
    ComPtr<ID3D12PipelineState> tonemap;
    if (FAILED(dev_->device()->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&tonemap)))) {
        HD2D_ERROR("post: create tonemap PSO failed (keeping previous)");
        return false;
    }

    // Bloom downsample -> HDR mip.
    pd.PS = {ps_bloom_down_->GetBufferPointer(), ps_bloom_down_->GetBufferSize()};
    pd.RTVFormats[0] = SceneTargets::kHdrFormat;
    ComPtr<ID3D12PipelineState> down;
    if (FAILED(dev_->device()->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&down)))) {
        HD2D_ERROR("post: create bloom down PSO failed (keeping previous)");
        return false;
    }

    // DoF 3종 — 불투명 풀스크린, HDR 포맷(하프해상도 타깃).
    ComPtr<ID3D12PipelineState> dof_coc, dof_blur, dof_tent;
    pd.PS = {ps_dof_coc_->GetBufferPointer(), ps_dof_coc_->GetBufferSize()};
    if (FAILED(dev_->device()->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&dof_coc)))) {
        HD2D_ERROR("post: create dof coc PSO failed (keeping previous)");
        return false;
    }
    pd.PS = {ps_dof_blur_->GetBufferPointer(), ps_dof_blur_->GetBufferSize()};
    if (FAILED(dev_->device()->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&dof_blur)))) {
        HD2D_ERROR("post: create dof blur PSO failed (keeping previous)");
        return false;
    }
    pd.PS = {ps_dof_tent_->GetBufferPointer(), ps_dof_tent_->GetBufferSize()};
    if (FAILED(dev_->device()->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&dof_tent)))) {
        HD2D_ERROR("post: create dof tent PSO failed (keeping previous)");
        return false;
    }

    // Bloom upsample: additive tent onto the lower mip.
    pd.PS = {ps_bloom_up_->GetBufferPointer(), ps_bloom_up_->GetBufferSize()};
    auto& rt0 = pd.BlendState.RenderTarget[0];
    rt0.BlendEnable = TRUE;
    rt0.SrcBlend = D3D12_BLEND_ONE;
    rt0.DestBlend = D3D12_BLEND_ONE;
    rt0.BlendOp = D3D12_BLEND_OP_ADD;
    rt0.SrcBlendAlpha = D3D12_BLEND_ONE;
    rt0.DestBlendAlpha = D3D12_BLEND_ONE;
    rt0.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    ComPtr<ID3D12PipelineState> up;
    if (FAILED(dev_->device()->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&up)))) {
        HD2D_ERROR("post: create bloom up PSO failed (keeping previous)");
        return false;
    }

    pso_tonemap_ = tonemap;
    pso_bloom_down_ = down;
    pso_bloom_up_ = up;
    pso_dof_coc_ = dof_coc;
    pso_dof_blur_ = dof_blur;
    pso_dof_tent_ = dof_tent;
    return true;
}

bool PostChain::ensure_bloom(uint32_t width, uint32_t height) {
    if (bloom_[0] && width == bloom_base_w_ && height == bloom_base_h_) return true;
    ID3D12Device* d = dev_->device();
    if (bloom_[0]) dev_->flush();

    if (!bloom_rtv_heap_) {
        D3D12_DESCRIPTOR_HEAP_DESC rd{};
        rd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rd.NumDescriptors = kBloomMips;
        if (FAILED(d->CreateDescriptorHeap(&rd, IID_PPV_ARGS(&bloom_rtv_heap_))))
            return false;
        const uint32_t inc =
            d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE h = bloom_rtv_heap_->GetCPUDescriptorHandleForHeapStart();
        for (uint32_t i = 0; i < kBloomMips; ++i) {
            bloom_rtv_[i] = h;
            h.ptr += inc;
        }
    }
    if (!bloom_srv_allocated_) {
        for (uint32_t i = 0; i < kBloomMips; ++i)
            if (!dev_->alloc_srv(&bloom_srv_cpu_[i], &bloom_srv_gpu_[i])) return false;
        bloom_srv_allocated_ = true;
    }

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    uint32_t w = std::max(1u, width / 2), h = std::max(1u, height / 2);
    for (uint32_t i = 0; i < kBloomMips; ++i) {
        bloom_w_[i] = w;
        bloom_h_[i] = h;
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = w;
        td.Height = h;
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.Format = SceneTargets::kHdrFormat;
        td.SampleDesc.Count = 1;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = SceneTargets::kHdrFormat;
        bloom_[i].Reset();
        if (FAILED(d->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td,
                                              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                              &clear, IID_PPV_ARGS(&bloom_[i])))) {
            HD2D_ERROR("post: create bloom mip {} failed ({}x{})", i, w, h);
            return false;
        }
        bloom_state_[i] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        d->CreateRenderTargetView(bloom_[i].Get(), nullptr, bloom_rtv_[i]);
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = SceneTargets::kHdrFormat;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        d->CreateShaderResourceView(bloom_[i].Get(), &sd, bloom_srv_cpu_[i]);
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
    }
    bloom_base_w_ = width;
    bloom_base_h_ = height;
    HD2D_INFO("post: bloom chain {}x{} .. {}x{}", bloom_w_[0], bloom_h_[0],
              bloom_w_[kBloomMips - 1], bloom_h_[kBloomMips - 1]);
    return true;
}

bool PostChain::ensure_dof(uint32_t width, uint32_t height) {
    if (dof_[0] && width == dof_base_w_ && height == dof_base_h_) return true;
    ID3D12Device* d = dev_->device();
    if (dof_[0]) dev_->flush();

    if (!dof_rtv_heap_) {
        D3D12_DESCRIPTOR_HEAP_DESC rd{};
        rd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rd.NumDescriptors = kDofTargets;
        if (FAILED(d->CreateDescriptorHeap(&rd, IID_PPV_ARGS(&dof_rtv_heap_))))
            return false;
        const uint32_t inc =
            d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE h = dof_rtv_heap_->GetCPUDescriptorHandleForHeapStart();
        for (uint32_t i = 0; i < kDofTargets; ++i) {
            dof_rtv_[i] = h;
            h.ptr += inc;
        }
    }
    if (!dof_srv_allocated_) {
        for (uint32_t i = 0; i < kDofTargets; ++i)
            if (!dev_->alloc_srv(&dof_srv_cpu_[i], &dof_srv_gpu_[i])) return false;
        dof_srv_allocated_ = true;
    }

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    dof_w_ = std::max(1u, width / 2);
    dof_h_ = std::max(1u, height / 2);
    for (uint32_t i = 0; i < kDofTargets; ++i) {
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = dof_w_;
        td.Height = dof_h_;
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.Format = SceneTargets::kHdrFormat;
        td.SampleDesc.Count = 1;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = SceneTargets::kHdrFormat;
        dof_[i].Reset();
        if (FAILED(d->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td,
                                              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                              &clear, IID_PPV_ARGS(&dof_[i])))) {
            HD2D_ERROR("post: create dof target {} failed ({}x{})", i, dof_w_, dof_h_);
            return false;
        }
        dof_state_[i] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        d->CreateRenderTargetView(dof_[i].Get(), nullptr, dof_rtv_[i]);
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = SceneTargets::kHdrFormat;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        d->CreateShaderResourceView(dof_[i].Get(), &sd, dof_srv_cpu_[i]);
    }
    dof_base_w_ = width;
    dof_base_h_ = height;
    HD2D_INFO("post: dof targets {}x{}", dof_w_, dof_h_);
    return true;
}

void PostChain::execute(ID3D12GraphicsCommandList* cmd, SceneTargets& scene,
                        D3D12_CPU_DESCRIPTOR_HANDLE dest_rtv, const PostSettings& ps) {
    if (!pso_tonemap_ || !scene.valid()) return;

    const bool bloom_on = ps.bloom && ensure_bloom(scene.width(), scene.height());
    const bool dof_on = ps.dof && ps.dof_strength > 0.001f &&
                        ensure_dof(scene.width(), scene.height());
    scene.to_shader_resource(cmd);

    cmd->SetGraphicsRootSignature(root_sig_.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    PostConstants pc{};
    pc.exposure = ps.exposure;
    pc.bloom_intensity = ps.bloom ? ps.bloom_intensity : 0.0f;
    pc.threshold = ps.bloom_threshold;
    pc.knee = ps.bloom_knee;
    pc.dof_focus_dist = ps.dof_focus_distance;
    pc.dof_focus_range = ps.dof_focus_range;
    pc.dof_blur_range = ps.dof_blur_range;
    pc.dof_strength = ps.dof_strength;
    pc.dof_band_center = ps.dof_band_center;
    pc.dof_band_half = ps.dof_band_half;
    pc.dof_band_feather = ps.dof_band_feather;
    pc.dof_protect = ps.dof_protect_range;
    pc.dof_max_coc = ps.dof_max_coc_px;
    pc.cam_near = ps.dof_cam_near;
    pc.cam_far = ps.dof_cam_far;

    auto set_viewport = [&](uint32_t w, uint32_t h) {
        D3D12_VIEWPORT vp{0, 0, float(w), float(h), 0.0f, 1.0f};
        D3D12_RECT sc{0, 0, LONG(w), LONG(h)};
        cmd->RSSetViewports(1, &vp);
        cmd->RSSetScissorRects(1, &sc);
    };

    // --- 틸트시프트 DoF: 하프해상도 CoC→디스크 개더→텐트 (스펙 2026-07-02) ---
    if (dof_on) {
        auto dof_pass = [&](uint32_t dst, ID3D12PipelineState* pso,
                            D3D12_GPU_DESCRIPTOR_HANDLE src, float src_w, float src_h) {
            if (dof_state_[dst] != D3D12_RESOURCE_STATE_RENDER_TARGET) {
                auto b = transition(dof_[dst].Get(), dof_state_[dst],
                                    D3D12_RESOURCE_STATE_RENDER_TARGET);
                cmd->ResourceBarrier(1, &b);
                dof_state_[dst] = D3D12_RESOURCE_STATE_RENDER_TARGET;
            }
            cmd->SetPipelineState(pso);
            cmd->OMSetRenderTargets(1, &dof_rtv_[dst], FALSE, nullptr);
            set_viewport(dof_w_, dof_h_);
            pc.flags = (ps.agx ? 1u : 0u) | (ps.input_linear ? 2u : 0u);
            pc.texel_x = 1.0f / src_w;
            pc.texel_y = 1.0f / src_h;
            cmd->SetGraphicsRoot32BitConstants(0, sizeof(PostConstants) / 4, &pc, 0);
            cmd->SetGraphicsRootDescriptorTable(1, src);
            cmd->SetGraphicsRootDescriptorTable(2, scene.srv());
            cmd->SetGraphicsRootDescriptorTable(3, scene.depth_srv());
            cmd->SetGraphicsRootDescriptorTable(4, scene.srv());
            cmd->DrawInstanced(3, 1, 0, 0);
            auto b = transition(dof_[dst].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            cmd->ResourceBarrier(1, &b);
            dof_state_[dst] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        };
        // [0] = CoC+색(소스: 풀해상도 씬) → [1] = 디스크 블러 → [0] = 텐트 정리(최종)
        dof_pass(0, pso_dof_coc_.Get(), scene.srv(),
                 float(scene.width()), float(scene.height()));
        dof_pass(1, pso_dof_blur_.Get(), dof_srv_gpu_[0], float(dof_w_), float(dof_h_));
        dof_pass(0, pso_dof_tent_.Get(), dof_srv_gpu_[1], float(dof_w_), float(dof_h_));
    }

    if (bloom_on) {
        // Downsample chain: scene -> mip0 (thresholded) -> mip1 ... mip5.
        cmd->SetPipelineState(pso_bloom_down_.Get());
        for (uint32_t i = 0; i < kBloomMips; ++i) {
            if (bloom_state_[i] != D3D12_RESOURCE_STATE_RENDER_TARGET) {
                auto b = transition(bloom_[i].Get(), bloom_state_[i],
                                    D3D12_RESOURCE_STATE_RENDER_TARGET);
                cmd->ResourceBarrier(1, &b);
                bloom_state_[i] = D3D12_RESOURCE_STATE_RENDER_TARGET;
            }
            cmd->OMSetRenderTargets(1, &bloom_rtv_[i], FALSE, nullptr);
            set_viewport(bloom_w_[i], bloom_h_[i]);
            const uint32_t src_w = i == 0 ? scene.width() : bloom_w_[i - 1];
            const uint32_t src_h = i == 0 ? scene.height() : bloom_h_[i - 1];
            pc.flags = (ps.agx ? 1u : 0u) | (ps.input_linear ? 2u : 0u) |
                       (i == 0 ? 4u : 0u);
            pc.texel_x = 1.0f / static_cast<float>(src_w);
            pc.texel_y = 1.0f / static_cast<float>(src_h);
            cmd->SetGraphicsRoot32BitConstants(0, sizeof(PostConstants) / 4, &pc, 0);
            cmd->SetGraphicsRootDescriptorTable(1, i == 0 ? scene.srv()
                                                          : bloom_srv_gpu_[i - 1]);
            cmd->SetGraphicsRootDescriptorTable(2, scene.srv());
            cmd->SetGraphicsRootDescriptorTable(3, scene.depth_srv());
            cmd->SetGraphicsRootDescriptorTable(4, scene.srv());
            cmd->DrawInstanced(3, 1, 0, 0);
            auto b = transition(bloom_[i].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            cmd->ResourceBarrier(1, &b);
            bloom_state_[i] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }

        // Upsample chain: tent-blur mip i+1 additively onto mip i.
        cmd->SetPipelineState(pso_bloom_up_.Get());
        for (int i = static_cast<int>(kBloomMips) - 2; i >= 0; --i) {
            auto b = transition(bloom_[i].Get(), bloom_state_[i],
                                D3D12_RESOURCE_STATE_RENDER_TARGET);
            cmd->ResourceBarrier(1, &b);
            bloom_state_[i] = D3D12_RESOURCE_STATE_RENDER_TARGET;
            cmd->OMSetRenderTargets(1, &bloom_rtv_[i], FALSE, nullptr);
            set_viewport(bloom_w_[i], bloom_h_[i]);
            pc.texel_x = 1.0f / static_cast<float>(bloom_w_[i + 1]);
            pc.texel_y = 1.0f / static_cast<float>(bloom_h_[i + 1]);
            cmd->SetGraphicsRoot32BitConstants(0, sizeof(PostConstants) / 4, &pc, 0);
            cmd->SetGraphicsRootDescriptorTable(1, bloom_srv_gpu_[i + 1]);
            cmd->SetGraphicsRootDescriptorTable(2, scene.srv());
            cmd->SetGraphicsRootDescriptorTable(3, scene.depth_srv());
            cmd->SetGraphicsRootDescriptorTable(4, scene.srv());
            cmd->DrawInstanced(3, 1, 0, 0);
            auto b2 = transition(bloom_[i].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            cmd->ResourceBarrier(1, &b2);
            bloom_state_[i] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }
    }

    // Tonemap to the backbuffer (full window viewport).
    cmd->SetPipelineState(pso_tonemap_.Get());
    cmd->OMSetRenderTargets(1, &dest_rtv, FALSE, nullptr);
    set_viewport(scene.width(), scene.height());
    pc.flags = (ps.agx ? 1u : 0u) | (ps.input_linear ? 2u : 0u) | (dof_on ? 8u : 0u);
    pc.texel_x = 1.0f / static_cast<float>(scene.width());
    pc.texel_y = 1.0f / static_cast<float>(scene.height());
    cmd->SetGraphicsRoot32BitConstants(0, sizeof(PostConstants) / 4, &pc, 0);
    cmd->SetGraphicsRootDescriptorTable(1, scene.srv());
    cmd->SetGraphicsRootDescriptorTable(2, bloom_on ? bloom_srv_gpu_[0] : scene.srv());
    cmd->SetGraphicsRootDescriptorTable(3, scene.depth_srv());
    cmd->SetGraphicsRootDescriptorTable(4, dof_on ? dof_srv_gpu_[0] : scene.srv());
    cmd->DrawInstanced(3, 1, 0, 0);
}

void PostChain::shutdown() {
    if (bloom_srv_allocated_ && dev_) {
        for (uint32_t i = 0; i < kBloomMips; ++i)
            dev_->free_srv(bloom_srv_cpu_[i], bloom_srv_gpu_[i]);
        bloom_srv_allocated_ = false;
    }
    if (dof_srv_allocated_ && dev_) {
        for (uint32_t i = 0; i < kDofTargets; ++i)
            dev_->free_srv(dof_srv_cpu_[i], dof_srv_gpu_[i]);
        dof_srv_allocated_ = false;
    }
    for (auto& b : bloom_) b.Reset();
    bloom_rtv_heap_.Reset();
    for (auto& t : dof_) t.Reset();
    dof_rtv_heap_.Reset();
    pso_tonemap_.Reset();
    pso_bloom_down_.Reset();
    pso_bloom_up_.Reset();
    pso_dof_coc_.Reset();
    pso_dof_blur_.Reset();
    pso_dof_tent_.Reset();
    root_sig_.Reset();
    vs_.Reset();
    ps_tonemap_.Reset();
    ps_bloom_down_.Reset();
    ps_bloom_up_.Reset();
    ps_dof_coc_.Reset();
    ps_dof_blur_.Reset();
    ps_dof_tent_.Reset();
}

} // namespace hd2d
