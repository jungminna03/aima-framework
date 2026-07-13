#include "renderer/forward_pass.h"

#include "core/log_compat.h"

#include <d3dcompiler.h>

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
            HD2D_ERROR("shader compile {} failed: {}", entry,
                       static_cast<const char*>(err->GetBufferPointer()));
        } else {
            HD2D_ERROR("shader compile {} failed (hr=0x{:08X})", entry,
                       static_cast<unsigned>(hr));
        }
        return false;
    }
    return true;
}

} // namespace

bool ForwardPass::init(Dx12Device& dev, const std::string& shader_path) {
    dev_ = &dev;
    shader_path_ = shader_path;

    if (!build_root_signature()) return false;
    if (!reload_shader()) return false;  // compiles + builds PSO

    // Per-frame constant rings (persistently mapped UPLOAD buffers).
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
        if (FAILED(dev_->device()->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&cb_ring_[i])))) {
            HD2D_ERROR("forward pass: create constant ring failed");
            return false;
        }
        D3D12_RANGE none{0, 0};
        cb_ring_[i]->Map(0, &none, reinterpret_cast<void**>(&cb_mapped_[i]));
    }

    HD2D_INFO("forward pass initialized");
    return true;
}

bool ForwardPass::build_root_signature() {
    // t0 base, t1 metallic-roughness, t2 normal, t3 emissive, t4 sun shadow —
    // one table each (the device's SRV heap hands out single slots, so
    // contiguity is not guaranteed and each map gets its own root table).
    D3D12_DESCRIPTOR_RANGE srv_ranges[5]{};
    for (UINT i = 0; i < 5; ++i) {
        srv_ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srv_ranges[i].NumDescriptors = 1;
        srv_ranges[i].BaseShaderRegister = i;
        srv_ranges[i].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    }

    D3D12_ROOT_PARAMETER params[7]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;  // b0 frame
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;  // b1 draw
    params[1].Descriptor.ShaderRegister = 1;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    for (UINT i = 0; i < 5; ++i) {
        params[2 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2 + i].DescriptorTable.NumDescriptorRanges = 1;
        params[2 + i].DescriptorTable.pDescriptorRanges = &srv_ranges[i];
        params[2 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }

    // s0 linear/wrap, s1 linear/clamp, s2 point/wrap, s3 point/clamp — the
    // material picks via gFlags (bit0 nearest, bit1 clamp). s4 = PCF compare.
    D3D12_STATIC_SAMPLER_DESC samplers[5]{};
    for (UINT i = 0; i < 4; ++i) {
        const bool nearest = (i & 2) != 0;
        const bool clamp = (i & 1) != 0;
        samplers[i].Filter = nearest ? D3D12_FILTER_MIN_MAG_MIP_POINT
                                     : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        const D3D12_TEXTURE_ADDRESS_MODE mode =
            clamp ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplers[i].AddressU = samplers[i].AddressV = samplers[i].AddressW = mode;
        samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[i].ShaderRegister = i;
        samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    samplers[4].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    samplers[4].AddressU = samplers[4].AddressV = samplers[4].AddressW =
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[4].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    samplers[4].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[4].ShaderRegister = 4;
    samplers[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 7;
    desc.pParameters = params;
    desc.NumStaticSamplers = 5;
    desc.pStaticSamplers = samplers;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> blob, err;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &err);
    if (FAILED(hr)) {
        HD2D_ERROR("serialize root signature failed: {}",
                   err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        return false;
    }
    if (FAILED(dev_->device()->CreateRootSignature(0, blob->GetBufferPointer(),
                                                   blob->GetBufferSize(),
                                                   IID_PPV_ARGS(&root_sig_)))) {
        HD2D_ERROR("create root signature failed");
        return false;
    }
    return true;
}

bool ForwardPass::reload_shader() {
    ComPtr<ID3DBlob> vs, ps;
    if (!compile_entry(shader_path_, "VSMain", "vs_5_1", vs)) return false;
    if (!compile_entry(shader_path_, "PSMain", "ps_5_1", ps)) return false;
    vs_ = vs;
    ps_ = ps;
    return build_pso();  // keeps old pso_ if this fails (build_pso writes a temp first)
}

bool ForwardPass::build_pso() {
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
    pd.NumRenderTargets = 1;
    // 씬은 HDR 타깃(R16G16B16A16_FLOAT)에 그린다 — PSO RTV 포맷도 거기 맞춰야 함.
    // (이전엔 kBackBufferFormat=R8G8B8A8 8비트로 선언돼 GPU가 씬 출력을 8비트로
    //  양자화→HDR 타깃에 저장→밴딩이 HDR 단계에서 구워져 톤맵/디더로 못 살렸음.)
    pd.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pd.DSVFormat = Dx12Device::kDepthFormat;
    pd.SampleDesc.Count = 1;
    pd.SampleMask = UINT_MAX;

    // Rasterizer: cull none (robust against the glTF->DX winding flip + double-
    // sided billboards). Tighten to back-cull later once winding is verified.
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;

    // Opaque blend; transparency handled by alpha-test (discard) in the shader.
    for (auto& rt : pd.BlendState.RenderTarget) rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    pd.DepthStencilState.DepthEnable = TRUE;
    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

    ComPtr<ID3D12PipelineState> pso;
    if (FAILED(dev_->device()->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)))) {
        HD2D_ERROR("create PSO failed (keeping previous)");
        return false;
    }

    // Translucent variant for the camera-occlusion fade pass: standard SRC_ALPHA
    // over-blend, depth-test LESS_EQUAL so the occluder still hides behind nearer
    // geometry, but NO depth write (it draws after the opaque scene + the player
    // billboard, blending over them without leaving depth that breaks later draws).
    D3D12_GRAPHICS_PIPELINE_STATE_DESC bd = pd;
    D3D12_RENDER_TARGET_BLEND_DESC& rt0 = bd.BlendState.RenderTarget[0];
    rt0.BlendEnable = TRUE;
    rt0.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    rt0.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    rt0.BlendOp = D3D12_BLEND_OP_ADD;
    rt0.SrcBlendAlpha = D3D12_BLEND_ONE;
    rt0.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    rt0.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    bd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    bd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

    ComPtr<ID3D12PipelineState> pso_blend;
    if (FAILED(dev_->device()->CreateGraphicsPipelineState(&bd, IID_PPV_ARGS(&pso_blend)))) {
        HD2D_ERROR("create blend PSO failed (keeping previous)");
        return false;
    }

    pso_ = pso;
    pso_blend_ = pso_blend;
    return true;
}

void ForwardPass::set_translucent(ID3D12GraphicsCommandList* cmd, bool on) {
    cmd->SetPipelineState(on ? pso_blend_.Get() : pso_.Get());
}

void* ForwardPass::alloc_constants(size_t size, D3D12_GPU_VIRTUAL_ADDRESS* out_va) {
    const size_t aligned = (size + 255) & ~size_t(255);
    if (cb_offset_ + aligned > kRingSize) cb_offset_ = 0;  // wrap (1 MB is plenty)
    const size_t off = cb_offset_;
    cb_offset_ += aligned;
    *out_va = cb_ring_[ring_index_]->GetGPUVirtualAddress() + off;
    return cb_mapped_[ring_index_] + off;
}

void ForwardPass::begin(ID3D12GraphicsCommandList* cmd, const FrameConstants& frame,
                        D3D12_GPU_DESCRIPTOR_HANDLE shadow_srv) {
    ring_index_ = (ring_index_ + 1) % Dx12Device::kFrameCount;
    cb_offset_ = 0;

    cmd->SetGraphicsRootSignature(root_sig_.Get());
    cmd->SetPipelineState(pso_.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    D3D12_GPU_VIRTUAL_ADDRESS va{};
    void* dst = alloc_constants(sizeof(FrameConstants), &va);
    memcpy(dst, &frame, sizeof(FrameConstants));
    cmd->SetGraphicsRootConstantBufferView(0, va);
    cmd->SetGraphicsRootDescriptorTable(6, shadow_srv);  // t4 sun shadow map
}

void ForwardPass::draw(ID3D12GraphicsCommandList* cmd, const DrawConstants& dc,
                       const MaterialSrvs& mat,
                       const D3D12_VERTEX_BUFFER_VIEW& vbv,
                       const D3D12_INDEX_BUFFER_VIEW& ibv, uint32_t index_count) {
    D3D12_GPU_VIRTUAL_ADDRESS va{};
    void* dst = alloc_constants(sizeof(DrawConstants), &va);
    memcpy(dst, &dc, sizeof(DrawConstants));
    cmd->SetGraphicsRootConstantBufferView(1, va);
    cmd->SetGraphicsRootDescriptorTable(2, mat.base);
    cmd->SetGraphicsRootDescriptorTable(3, mat.mr);
    cmd->SetGraphicsRootDescriptorTable(4, mat.normal);
    cmd->SetGraphicsRootDescriptorTable(5, mat.emissive);
    cmd->IASetVertexBuffers(0, 1, &vbv);
    cmd->IASetIndexBuffer(&ibv);
    cmd->DrawIndexedInstanced(index_count, 1, 0, 0, 0);
}

void ForwardPass::shutdown() {
    for (uint32_t i = 0; i < Dx12Device::kFrameCount; ++i) {
        if (cb_ring_[i]) cb_ring_[i]->Unmap(0, nullptr);
        cb_ring_[i].Reset();
        cb_mapped_[i] = nullptr;
    }
    pso_.Reset();
    pso_blend_.Reset();
    root_sig_.Reset();
    vs_.Reset();
    ps_.Reset();
}

} // namespace hd2d
