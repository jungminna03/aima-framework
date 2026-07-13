#pragma once

#include "renderer/device.h"
#include "renderer/rhi.h"        // rhi::GpuTexture / GpuMesh handles (frame-packet)

#include <cstdint>
#include <functional>
#include <vector>

namespace hd2d {

// A GPU texture: the resource + its SRV slot in the device's shared SRV heap.
struct GpuTexture {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu{};
    uint32_t width = 0;
    uint32_t height = 0;
};

// One drawable primitive: GPU vertex/index buffers + its material factors.
struct GpuSubmesh {
    Microsoft::WRL::ComPtr<ID3D12Resource> vb;
    Microsoft::WRL::ComPtr<ID3D12Resource> ib;
    D3D12_VERTEX_BUFFER_VIEW vbv{};
    D3D12_INDEX_BUFFER_VIEW ibv{};
    uint32_t index_count = 0;
    float base_color[4] = {1, 1, 1, 1};
    float metallic = 0.0f;
    float roughness = 1.0f;
};

// Maps opaque rhi handles (frame-packet) -> the real DX12 resources, so the game
// / LiveScene can carry backend-neutral rhi::GpuTexture/GpuMesh instead of ComPtr
// (PAL render keystone, brick 2). Additive — populated/consumed in later bricks.
// rhi id == vector index + 1 (0 == none). Owned device-side (host), not the game.
class Dx12ResourceTable {
public:
    rhi::GpuTexture add_texture(GpuTexture tex);   // 해제된 슬롯(tex_free_) 재사용
    rhi::GpuMesh    add_mesh(GpuSubmesh mesh);
    GpuTexture* resolve(rhi::GpuTexture h);   // 비-const: cloth가 vbv를 갱신함
    GpuSubmesh* resolve(rhi::GpuMesh h);
    // 텍스처 핸들 해제(포털 이동 시 맵 텍스처) — SRV 반납 + 슬롯을 free-list로.
    void free_texture(Dx12Device& dev, rhi::GpuTexture h);
    void clear() { textures_.clear(); meshes_.clear(); tex_free_.clear(); }
private:
    std::vector<GpuTexture> textures_;   // rhi id = index + 1
    std::vector<GpuSubmesh> meshes_;
    std::vector<uint32_t>   tex_free_;   // 해제된 텍스처 슬롯 인덱스(재사용)
};

// Record one-off GPU work (uploads) and block until it finishes.
void immediate_submit(Dx12Device& dev, const std::function<void(ID3D12GraphicsCommandList*)>& fn);

// Create an UPLOAD-heap buffer initialized with `data` (used for static vertex/
// index buffers — small + static, so reading from upload memory is fine here).
Microsoft::WRL::ComPtr<ID3D12Resource>
create_upload_buffer(Dx12Device& dev, const void* data, size_t size);

// Upload tightly-packed RGBA8 pixels to a DEFAULT-heap texture and register an
// SRV in the device's shared heap.
GpuTexture upload_texture_rgba8(Dx12Device& dev, uint32_t width, uint32_t height,
                                const uint8_t* rgba);

// Same, but with a full CPU-generated mip chain (box filter in linear space).
// srgb=true creates an _SRGB view (base color / emissive maps); false keeps
// UNORM (normal / metallic-roughness / data maps).
GpuTexture upload_texture_rgba8_mips(Dx12Device& dev, uint32_t width, uint32_t height,
                                     const uint8_t* rgba, bool srgb);

// Return the texture's SRV slot to the device free-list and drop the resource.
// Caller guarantees the GPU is idle / no longer references it (flush first).
void release_texture(Dx12Device& dev, GpuTexture& tex);

} // namespace hd2d
