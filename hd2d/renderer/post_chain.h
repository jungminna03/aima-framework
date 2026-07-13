#pragma once

#include "renderer/device.h"
#include "renderer/scene_targets.h"

#include <string>

namespace hd2d {

// Live-tunable post parameters (owned by the game's RenderSettings resource).
struct PostSettings {
    float exposure = 1.0f;
    float bloom_intensity = 0.06f;
    float bloom_threshold = 1.0f;   // soft-knee threshold (scene-linear)
    float bloom_knee = 0.5f;
    bool bloom = true;
    bool agx = true;           // AgX view transform (Blender default) vs sRGB
    // The standard shader outputs scene-linear HDR; the chain owns the
    // display transform. (False would pass the input through unchanged.)
    bool input_linear = true;
    // --- 틸트시프트 DoF (미니어처 룩, 스펙 2026-07-02). 색감 불변 — 블러만. ---
    bool  dof = true;
    float dof_strength = 0.35f;        // 게임(CameraOrbitSystem)이 매 프레임 이징 기입
    float dof_focus_distance = 10.0f;  // 게임이 매 프레임 기입(오빗 타깃 거리)
    float dof_focus_range = 4.0f;      // 완전 선명 깊이 반폭 m
    float dof_blur_range = 18.0f;      // 선명→최대 블러 전이 m
    float dof_max_coc_px = 14.0f;      // 최대 블러 반경(풀해상도 px)
    float dof_band_center = 0.55f;     // 선명 밴드 중심(uv.y 0=상단; 캐릭터가 중앙 살짝 아래)
    float dof_band_half = 0.16f;
    float dof_band_feather = 0.28f;
    float dof_protect_range = 6.0f;    // 밴드 항 깊이 보호 반경 m
    float dof_cam_near = 0.1f;         // 게임이 카메라에서 복사
    float dof_cam_far = 2500.0f;
};

// The HDR post-processing chain: (bloom — milestone B4) + tonemap to the
// swapchain backbuffer. Compiled at runtime from shaders/post.hlsl so it
// hot-reloads with the rest of the pipeline.
class PostChain {
public:
    bool init(Dx12Device& dev, const std::string& shader_path);
    bool reload_shader();
    void shutdown();

    // Reads the scene HDR target, writes `dest_rtv` (the bound backbuffer).
    // Caller guarantees viewport/scissor cover the full window.
    void execute(ID3D12GraphicsCommandList* cmd, SceneTargets& scene,
                 D3D12_CPU_DESCRIPTOR_HANDLE dest_rtv, const PostSettings& ps);

private:
    template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    static constexpr uint32_t kBloomMips = 6;
    static constexpr uint32_t kDofTargets = 2;   // ping-pong: [0]=CoC/최종, [1]=blur 중간

    bool build_root_signature();
    bool build_psos();
    bool ensure_bloom(uint32_t width, uint32_t height);
    bool ensure_dof(uint32_t width, uint32_t height);

    Dx12Device* dev_ = nullptr;
    std::string shader_path_;

    ComPtr<ID3D12RootSignature> root_sig_;
    ComPtr<ID3D12PipelineState> pso_tonemap_;
    ComPtr<ID3D12PipelineState> pso_bloom_down_;
    ComPtr<ID3D12PipelineState> pso_bloom_up_;     // additive blend
    ComPtr<ID3D12PipelineState> pso_dof_coc_;      // scene+depth -> half-res color+CoC
    ComPtr<ID3D12PipelineState> pso_dof_blur_;     // golden-angle disc gather
    ComPtr<ID3D12PipelineState> pso_dof_tent_;     // 3x3 tent cleanup
    ComPtr<ID3DBlob> vs_;
    ComPtr<ID3DBlob> ps_tonemap_;
    ComPtr<ID3DBlob> ps_bloom_down_;
    ComPtr<ID3DBlob> ps_bloom_up_;
    ComPtr<ID3DBlob> ps_dof_coc_;
    ComPtr<ID3DBlob> ps_dof_blur_;
    ComPtr<ID3DBlob> ps_dof_tent_;

    // Bloom mip chain (half res down to 1/64), each with an RTV + SRV.
    ComPtr<ID3D12Resource> bloom_[kBloomMips];
    ComPtr<ID3D12DescriptorHeap> bloom_rtv_heap_;
    D3D12_CPU_DESCRIPTOR_HANDLE bloom_rtv_[kBloomMips]{};
    D3D12_CPU_DESCRIPTOR_HANDLE bloom_srv_cpu_[kBloomMips]{};
    D3D12_GPU_DESCRIPTOR_HANDLE bloom_srv_gpu_[kBloomMips]{};
    D3D12_RESOURCE_STATES bloom_state_[kBloomMips]{};
    uint32_t bloom_w_[kBloomMips]{};
    uint32_t bloom_h_[kBloomMips]{};
    uint32_t bloom_base_w_ = 0;
    uint32_t bloom_base_h_ = 0;
    bool bloom_srv_allocated_ = false;

    // DoF 하프해상도 핑퐁 타깃 (rgb=색, a=CoC).
    ComPtr<ID3D12Resource> dof_[kDofTargets];
    ComPtr<ID3D12DescriptorHeap> dof_rtv_heap_;
    D3D12_CPU_DESCRIPTOR_HANDLE dof_rtv_[kDofTargets]{};
    D3D12_CPU_DESCRIPTOR_HANDLE dof_srv_cpu_[kDofTargets]{};
    D3D12_GPU_DESCRIPTOR_HANDLE dof_srv_gpu_[kDofTargets]{};
    D3D12_RESOURCE_STATES dof_state_[kDofTargets]{};
    uint32_t dof_w_ = 0;
    uint32_t dof_h_ = 0;
    uint32_t dof_base_w_ = 0;
    uint32_t dof_base_h_ = 0;
    bool dof_srv_allocated_ = false;
};

} // namespace hd2d
