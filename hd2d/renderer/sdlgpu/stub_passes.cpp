// ============================================================================
// sdlgpu/stub_passes.cpp — no-op definitions of the renderer passes + the
// gpu_resources free functions, for the macOS/Linux walking skeleton.
//
// The real implementations (forward_pass.cpp / shadow_pass.cpp / post_chain.cpp
// / scene_targets.cpp / gpu_resources.cpp) are raw DX12 and compile ONLY in the
// d3d12 build. Here we provide the SAME symbol surface with empty bodies so:
//   - the host (main.cpp) can construct + init/shutdown the passes,
//   - the game render systems (all guarded by `!rc->gpu_cmd`, always true here)
//     link against ForwardPass::draw / ShadowPass::* / PostChain::execute / ...,
//   - core_plugin / gltf_loader / sprite_sheet link against create_upload_buffer
//     / upload_texture_* (which just return empty handles — the map renders
//     nothing, which is the expected skeleton behaviour).
//
// init() returns true so the host's bring-up sequence proceeds to the main loop.
// ============================================================================

#include "renderer/forward_pass.h"
#include "renderer/gpu_resources.h"
#include "renderer/post_chain.h"
#include "renderer/scene_targets.h"
#include "renderer/shadow_pass.h"

namespace hd2d {

// ---------------------------------------------------------------------------
// gpu_resources.h free functions — return empty handles (no real GPU memory).
// ---------------------------------------------------------------------------
void immediate_submit(Dx12Device&,
                      const std::function<void(ID3D12GraphicsCommandList*)>&) {}

Microsoft::WRL::ComPtr<ID3D12Resource>
create_upload_buffer(Dx12Device&, const void*, size_t) {
    return {};
}

GpuTexture upload_texture_rgba8(Dx12Device& dev, uint32_t width, uint32_t height,
                                const uint8_t*) {
    GpuTexture tex;
    tex.width = width;
    tex.height = height;
    dev.alloc_srv(&tex.srv_cpu, &tex.srv_gpu);  // dummy handle so callers see "loaded"
    return tex;
}

GpuTexture upload_texture_rgba8_mips(Dx12Device& dev, uint32_t width, uint32_t height,
                                     const uint8_t*, bool) {
    GpuTexture tex;
    tex.width = width;
    tex.height = height;
    dev.alloc_srv(&tex.srv_cpu, &tex.srv_gpu);
    return tex;
}

void release_texture(Dx12Device&, GpuTexture& tex) {
    tex = GpuTexture{};
}

// ---------------------------------------------------------------------------
// Dx12ResourceTable (PAL render keystone, brick 2/4) — pure CPU handle
// bookkeeping (std::vector slots). The only DX12-coupled call is the SRV release
// inside free_texture, which routes through release_texture() (stubbed above), so
// the real gpu_resources.cpp logic compiles UNCHANGED on the SDL_GPU backend.
// Mirrors src/renderer/gpu_resources.cpp so handles stay valid + resolvable here.
// ---------------------------------------------------------------------------
rhi::GpuTexture Dx12ResourceTable::add_texture(GpuTexture tex) {
    if (!tex_free_.empty()) {                              // reuse a freed slot
        const uint32_t idx = tex_free_.back();
        tex_free_.pop_back();
        textures_[idx] = std::move(tex);
        return rhi::GpuTexture{static_cast<uint64_t>(idx) + 1};
    }
    textures_.push_back(std::move(tex));
    return rhi::GpuTexture{static_cast<uint64_t>(textures_.size())};   // id = index+1
}
void Dx12ResourceTable::free_texture(Dx12Device& dev, rhi::GpuTexture h) {
    if (h.id == 0 || h.id > textures_.size()) return;
    release_texture(dev, textures_[static_cast<size_t>(h.id) - 1]);
    tex_free_.push_back(static_cast<uint32_t>(h.id) - 1);
}
rhi::GpuMesh Dx12ResourceTable::add_mesh(GpuSubmesh mesh) {
    meshes_.push_back(std::move(mesh));
    return rhi::GpuMesh{static_cast<uint64_t>(meshes_.size())};
}
GpuTexture* Dx12ResourceTable::resolve(rhi::GpuTexture h) {
    if (h.id == 0 || h.id > textures_.size()) return nullptr;
    return &textures_[static_cast<size_t>(h.id) - 1];
}
GpuSubmesh* Dx12ResourceTable::resolve(rhi::GpuMesh h) {
    if (h.id == 0 || h.id > meshes_.size()) return nullptr;
    return &meshes_[static_cast<size_t>(h.id) - 1];
}

// ---------------------------------------------------------------------------
// ForwardPass
// ---------------------------------------------------------------------------
bool ForwardPass::init(Dx12Device& dev, const std::string& shader_path) {
    dev_ = &dev;
    shader_path_ = shader_path;
    return true;
}
void ForwardPass::shutdown() {}
bool ForwardPass::reload_shader() { return true; }
void ForwardPass::begin(ID3D12GraphicsCommandList*, const FrameConstants&,
                        D3D12_GPU_DESCRIPTOR_HANDLE) {}
void ForwardPass::draw(ID3D12GraphicsCommandList*, const DrawConstants&,
                       const MaterialSrvs&, const D3D12_VERTEX_BUFFER_VIEW&,
                       const D3D12_INDEX_BUFFER_VIEW&, uint32_t) {}
void ForwardPass::set_translucent(ID3D12GraphicsCommandList*, bool) {}

// ---------------------------------------------------------------------------
// ShadowPass
// ---------------------------------------------------------------------------
bool ShadowPass::init(Dx12Device& dev, const std::string& shader_path) {
    dev_ = &dev;
    shader_path_ = shader_path;
    return true;
}
bool ShadowPass::reload_shader() { return true; }
void ShadowPass::shutdown(Dx12Device&) {}
void ShadowPass::begin(ID3D12GraphicsCommandList*, const dx::XMFLOAT4X4&) {}
void ShadowPass::draw(ID3D12GraphicsCommandList*, const dx::XMFLOAT4X4&, float,
                      const float[2], const float[2], D3D12_GPU_DESCRIPTOR_HANDLE,
                      const D3D12_VERTEX_BUFFER_VIEW&, const D3D12_INDEX_BUFFER_VIEW&,
                      uint32_t) {}
void ShadowPass::end(ID3D12GraphicsCommandList*) {}

// ---------------------------------------------------------------------------
// SceneTargets
// ---------------------------------------------------------------------------
bool SceneTargets::ensure(Dx12Device&, uint32_t width, uint32_t height) {
    width_ = width;
    height_ = height;
    return false;  // never "valid" — render systems that check val() bail out
}
void SceneTargets::shutdown(Dx12Device&) {}
void SceneTargets::to_render_target(ID3D12GraphicsCommandList*) {}
void SceneTargets::to_shader_resource(ID3D12GraphicsCommandList*) {}

// ---------------------------------------------------------------------------
// PostChain
// ---------------------------------------------------------------------------
bool PostChain::init(Dx12Device& dev, const std::string& shader_path) {
    dev_ = &dev;
    shader_path_ = shader_path;
    return true;
}
bool PostChain::reload_shader() { return true; }
void PostChain::shutdown() {}
void PostChain::execute(ID3D12GraphicsCommandList*, SceneTargets&,
                        D3D12_CPU_DESCRIPTOR_HANDLE, const PostSettings&) {}

} // namespace hd2d
