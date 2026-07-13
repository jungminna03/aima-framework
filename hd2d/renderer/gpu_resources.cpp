#include "renderer/gpu_resources.h"

#include "core/log_compat.h"

#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace hd2d {

void release_texture(Dx12Device& dev, GpuTexture& tex) {
    if (tex.resource && tex.srv_cpu.ptr) dev.free_srv(tex.srv_cpu, tex.srv_gpu);
    tex = GpuTexture{};
}

void immediate_submit(Dx12Device& dev, const std::function<void(ID3D12GraphicsCommandList*)>& fn) {
    ID3D12Device* d = dev.device();
    ComPtr<ID3D12CommandAllocator> alloc;
    d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    ComPtr<ID3D12GraphicsCommandList> list;
    d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
                         IID_PPV_ARGS(&list));

    fn(list.Get());

    list->Close();
    ID3D12CommandList* lists[] = {list.Get()};
    dev.queue()->ExecuteCommandLists(1, lists);

    ComPtr<ID3D12Fence> fence;
    d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    dev.queue()->Signal(fence.Get(), 1);
    if (fence->GetCompletedValue() < 1) {
        HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        fence->SetEventOnCompletion(1, e);
        WaitForSingleObject(e, INFINITE);
        CloseHandle(e);
    }
}

ComPtr<ID3D12Resource> create_upload_buffer(Dx12Device& dev, const void* data, size_t size) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> buf;
    if (FAILED(dev.device()->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&buf)))) {
        HD2D_ERROR("create_upload_buffer failed ({} bytes)", size);
        return nullptr;
    }
    void* mapped = nullptr;
    D3D12_RANGE none{0, 0};
    buf->Map(0, &none, &mapped);
    std::memcpy(mapped, data, size);
    buf->Unmap(0, nullptr);
    return buf;
}

namespace {

// sRGB <-> linear for CPU mip filtering (exact transfer function).
float srgb_to_linear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
float linear_to_srgb(float c) {
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

// 2x2 box-filter one mip level down. Color channels are filtered in linear
// space when `srgb` (alpha is always linear). Odd sizes clamp the second tap.
std::vector<uint8_t> downsample_rgba8(const std::vector<uint8_t>& src, uint32_t w, uint32_t h,
                                      uint32_t nw, uint32_t nh, bool srgb) {
    std::vector<uint8_t> dst(static_cast<size_t>(nw) * nh * 4);
    auto load = [&](uint32_t x, uint32_t y, int c) {
        const float v = src[(static_cast<size_t>(y) * w + x) * 4 + c] / 255.0f;
        return (srgb && c < 3) ? srgb_to_linear(v) : v;
    };
    for (uint32_t y = 0; y < nh; ++y) {
        const uint32_t y0 = y * 2, y1 = (y0 + 1 < h) ? y0 + 1 : y0;
        for (uint32_t x = 0; x < nw; ++x) {
            const uint32_t x0 = x * 2, x1 = (x0 + 1 < w) ? x0 + 1 : x0;
            for (int c = 0; c < 4; ++c) {
                float v = 0.25f * (load(x0, y0, c) + load(x1, y0, c) +
                                   load(x0, y1, c) + load(x1, y1, c));
                if (srgb && c < 3) v = linear_to_srgb(v);
                dst[(static_cast<size_t>(y) * nw + x) * 4 + c] =
                    static_cast<uint8_t>(v * 255.0f + 0.5f);
            }
        }
    }
    return dst;
}

} // namespace

GpuTexture upload_texture_rgba8_mips(Dx12Device& dev, uint32_t width, uint32_t height,
                                     const uint8_t* rgba, bool srgb) {
    GpuTexture tex;
    tex.width = width;
    tex.height = height;
    ID3D12Device* d = dev.device();

    // Build the CPU mip chain.
    std::vector<std::vector<uint8_t>> mips;
    mips.emplace_back(rgba, rgba + static_cast<size_t>(width) * height * 4);
    {
        uint32_t w = width, h = height;
        while (w > 1 || h > 1) {
            const uint32_t nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
            mips.push_back(downsample_rgba8(mips.back(), w, h, nw, nh, srgb));
            w = nw; h = nh;
        }
    }
    const uint16_t mip_count = static_cast<uint16_t>(mips.size());
    const DXGI_FORMAT fmt = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;

    D3D12_HEAP_PROPERTIES def_heap{};
    def_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = width;
    td.Height = height;
    td.DepthOrArraySize = 1;
    td.MipLevels = mip_count;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (FAILED(d->CreateCommittedResource(&def_heap, D3D12_HEAP_FLAG_NONE, &td,
                                          D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(&tex.resource)))) {
        HD2D_ERROR("upload_texture_mips: create texture failed ({}x{}, {} mips)",
                   width, height, mip_count);
        return tex;
    }

    // One staging buffer holding every padded mip footprint.
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fps(mip_count);
    std::vector<UINT> rows(mip_count);
    std::vector<UINT64> row_sizes(mip_count);
    UINT64 total = 0;
    d->GetCopyableFootprints(&td, 0, mip_count, 0, fps.data(), rows.data(), row_sizes.data(),
                             &total);

    D3D12_HEAP_PROPERTIES up_heap{};
    up_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> staging;
    d->CreateCommittedResource(&up_heap, D3D12_HEAP_FLAG_NONE, &bd,
                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                               IID_PPV_ARGS(&staging));

    uint8_t* mapped = nullptr;
    D3D12_RANGE none{0, 0};
    staging->Map(0, &none, reinterpret_cast<void**>(&mapped));
    for (uint16_t m = 0; m < mip_count; ++m) {
        const uint32_t mw = fps[m].Footprint.Width;
        for (UINT y = 0; y < rows[m]; ++y) {
            std::memcpy(mapped + fps[m].Offset + static_cast<size_t>(y) * fps[m].Footprint.RowPitch,
                        mips[m].data() + static_cast<size_t>(y) * mw * 4,
                        static_cast<size_t>(mw) * 4);
        }
    }
    staging->Unmap(0, nullptr);

    immediate_submit(dev, [&](ID3D12GraphicsCommandList* cmd) {
        for (uint16_t m = 0; m < mip_count; ++m) {
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = tex.resource.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = m;
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = staging.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint = fps[m];
            cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = tex.resource.Get();
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1, &b);
    });

    if (!dev.alloc_srv(&tex.srv_cpu, &tex.srv_gpu)) {
        HD2D_ERROR("upload_texture_mips: no SRV slot");
        return tex;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = fmt;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = mip_count;
    d->CreateShaderResourceView(tex.resource.Get(), &sd, tex.srv_cpu);
    return tex;
}

GpuTexture upload_texture_rgba8(Dx12Device& dev, uint32_t width, uint32_t height,
                                const uint8_t* rgba, bool srgb) {
    GpuTexture tex;
    tex.width = width;
    tex.height = height;

    ID3D12Device* d = dev.device();

    D3D12_HEAP_PROPERTIES def_heap{};
    def_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = width;
    td.Height = height;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    // srgb=true: PNG는 sRGB로 저작 — _SRGB 뷰가 샘플 시 리니어화해 조명 파이프라인에서
    // 맞게 계산된다(월드 스프라이트). srgb=false: UI/ImGui — 백버퍼(UNORM) 직행이라
    // 리니어화하면 재인코드 없이 γ만큼 어두워진다(2026-07-10, tools/ingame_color_check.py).
    td.Format = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (FAILED(d->CreateCommittedResource(&def_heap, D3D12_HEAP_FLAG_NONE, &td,
                                          D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(&tex.resource)))) {
        HD2D_ERROR("upload_texture: create texture failed ({}x{})", width, height);
        return tex;
    }

    // Staging upload buffer sized to the padded footprint.
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    d->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &total);

    D3D12_HEAP_PROPERTIES up_heap{};
    up_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> staging;
    d->CreateCommittedResource(&up_heap, D3D12_HEAP_FLAG_NONE, &bd,
                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                               IID_PPV_ARGS(&staging));

    uint8_t* mapped = nullptr;
    D3D12_RANGE none{0, 0};
    staging->Map(0, &none, reinterpret_cast<void**>(&mapped));
    for (uint32_t y = 0; y < height; ++y) {
        std::memcpy(mapped + fp.Offset + static_cast<size_t>(y) * fp.Footprint.RowPitch,
                    rgba + static_cast<size_t>(y) * width * 4, static_cast<size_t>(width) * 4);
    }
    staging->Unmap(0, nullptr);

    immediate_submit(dev, [&](ID3D12GraphicsCommandList* cmd) {
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = tex.resource.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = staging.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = fp;
        cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = tex.resource.Get();
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1, &b);
    });

    if (!dev.alloc_srv(&tex.srv_cpu, &tex.srv_gpu)) {
        HD2D_ERROR("upload_texture: no SRV slot");
        return tex;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    d->CreateShaderResourceView(tex.resource.Get(), &sd, tex.srv_cpu);
    return tex;
}

// --- Dx12ResourceTable (PAL render keystone, brick 2) -----------------------
rhi::GpuTexture Dx12ResourceTable::add_texture(GpuTexture tex) {
    if (!tex_free_.empty()) {                              // 해제된 슬롯 재사용
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
    release_texture(dev, textures_[static_cast<size_t>(h.id) - 1]);    // SRV 반납 + 클리어
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

} // namespace hd2d
