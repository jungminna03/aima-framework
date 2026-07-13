#pragma once

// ============================================================================
// d3d12_stubs.h — null-RHI DirectX 12 placeholder types for the sdlgpu skeleton.
//
// THE KEYSTONE of the cross-platform walking-skeleton port. GpuTexture /
// GpuSubmesh (renderer/gpu_resources.h) embed ComPtr<ID3D12Resource> and
// D3D12_*_VIEW *by value*, and those structs are baked into the game's core
// data model (components.h, resources.h, gltf_loader.h, sprite_sheet.h,
// physics_components.h). So every game translation unit that includes those
// headers needs ID3D12Resource / D3D12_* / ComPtr to be TYPE-COMPLETE — even
// pure-gameplay plugins. They cannot be #ifdef'd away.
//
// This header defines those names as trivial, layout-only placeholders so the
// whole game + renderer header graph compiles on arm64-osx. No method here does
// anything real: the host never opens a real command list in the skeleton
// (Dx12Device::begin_frame returns nullptr), so every render-system body
// early-returns on `!rc->gpu_cmd` and these stubs are never executed — they only
// need to COMPILE.
//
// This file is ONLY compiled in the sdlgpu build. On Windows (HD2D_RENDERER_D3D12)
// the real <directx/d3d12.h> / <dxgi1_6.h> / <wrl/client.h> are used instead and
// this header is never included — the Windows path stays byte-identical.
// ============================================================================

#if !defined(HD2D_RENDERER_D3D12)

#include <cstddef>
#include <cstdint>
#include <memory>   // std::addressof

// ---------------------------------------------------------------------------
// Microsoft::WRL::ComPtr — a minimal smart-pointer shim with the surface the
// renderer/game touch: .Get(), operator->, operator bool, .Reset(),
// .GetAddressOf() / & (for IID_PPV_ARGS-style out params), comparison vs
// nullptr. It does NOT do COM refcounting (the stub objects are never created),
// it just owns a raw pointer so the by-value members in GpuTexture/GpuSubmesh/
// ClothBody are well-formed and default/move-constructible.
// ---------------------------------------------------------------------------
namespace Microsoft {
namespace WRL {

template <class T>
class ComPtr {
public:
    ComPtr() noexcept = default;
    ComPtr(std::nullptr_t) noexcept {}
    ComPtr(const ComPtr&) noexcept = default;
    ComPtr(ComPtr&& other) noexcept : ptr_(other.ptr_) { other.ptr_ = nullptr; }
    ComPtr& operator=(const ComPtr&) noexcept = default;
    ComPtr& operator=(ComPtr&& other) noexcept {
        // NB: operator& is overloaded below, so use std::addressof for identity.
        if (std::addressof(other) != this) { ptr_ = other.ptr_; other.ptr_ = nullptr; }
        return *this;
    }
    ComPtr& operator=(std::nullptr_t) noexcept { ptr_ = nullptr; return *this; }

    T* Get() const noexcept { return ptr_; }
    T* operator->() const noexcept { return ptr_; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }

    T* const* GetAddressOf() const noexcept { return &ptr_; }
    T** GetAddressOf() noexcept { return &ptr_; }
    T** operator&() noexcept { return &ptr_; }

    void Reset() noexcept { ptr_ = nullptr; }
    T* Detach() noexcept { T* p = ptr_; ptr_ = nullptr; return p; }

    bool operator==(std::nullptr_t) const noexcept { return ptr_ == nullptr; }
    bool operator!=(std::nullptr_t) const noexcept { return ptr_ != nullptr; }

private:
    T* ptr_ = nullptr;
};

} // namespace WRL
} // namespace Microsoft

// ---------------------------------------------------------------------------
// Win32 scalar/macro stand-ins used in the renderer headers (LONG/SIZE_T,
// FALSE, the GPU virtual-address type).
// ---------------------------------------------------------------------------
using LONG = long;
using SIZE_T = size_t;
using UINT = unsigned int;
using UINT64 = unsigned long long;
using D3D12_GPU_VIRTUAL_ADDRESS = unsigned long long;

#ifndef FALSE
#define FALSE 0
#endif
#ifndef TRUE
#define TRUE 1
#endif

// ---------------------------------------------------------------------------
// DXGI_FORMAT — only the enum values the game/renderer reference need to exist.
// ---------------------------------------------------------------------------
enum DXGI_FORMAT {
    DXGI_FORMAT_UNKNOWN = 0,
    DXGI_FORMAT_R32_UINT = 42,
    DXGI_FORMAT_R16_UINT = 57,
    DXGI_FORMAT_R8G8B8A8_UNORM = 28,
    DXGI_FORMAT_R8G8B8A8_UNORM_SRGB = 29,
    DXGI_FORMAT_R16G16B16A16_FLOAT = 10,
    DXGI_FORMAT_D32_FLOAT = 40,
};

// ---------------------------------------------------------------------------
// Descriptor handles + buffer/index views + viewport/scissor/range. These are
// embedded by value in GpuTexture/GpuSubmesh/MaterialSrvs and declared as locals
// in the (never-executed) render system bodies, so they must be layout-complete.
// ---------------------------------------------------------------------------
struct D3D12_CPU_DESCRIPTOR_HANDLE {
    SIZE_T ptr = 0;
};

struct D3D12_GPU_DESCRIPTOR_HANDLE {
    UINT64 ptr = 0;
};

struct D3D12_VERTEX_BUFFER_VIEW {
    D3D12_GPU_VIRTUAL_ADDRESS BufferLocation = 0;
    UINT SizeInBytes = 0;
    UINT StrideInBytes = 0;
};

struct D3D12_INDEX_BUFFER_VIEW {
    D3D12_GPU_VIRTUAL_ADDRESS BufferLocation = 0;
    UINT SizeInBytes = 0;
    DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
};

struct D3D12_VIEWPORT {
    float TopLeftX = 0.0f;
    float TopLeftY = 0.0f;
    float Width = 0.0f;
    float Height = 0.0f;
    float MinDepth = 0.0f;
    float MaxDepth = 0.0f;
};

struct D3D12_RECT {
    LONG left = 0;
    LONG top = 0;
    LONG right = 0;
    LONG bottom = 0;
};

struct D3D12_RANGE {
    SIZE_T Begin = 0;
    SIZE_T End = 0;
};

// Clear flags (the begin-scene-pass system names DEPTH; never executed).
enum D3D12_CLEAR_FLAGS {
    D3D12_CLEAR_FLAG_DEPTH = 0x1,
    D3D12_CLEAR_FLAG_STENCIL = 0x2,
};

// Resource-state enum: the pass headers store a current-state member initialized
// to one of these. Only the referenced values need to exist.
enum D3D12_RESOURCE_STATES {
    D3D12_RESOURCE_STATE_COMMON = 0,
    D3D12_RESOURCE_STATE_RENDER_TARGET = 0x4,
    D3D12_RESOURCE_STATE_DEPTH_WRITE = 0x10,
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE = 0x80,
    D3D12_RESOURCE_STATE_GENERIC_READ = 0xAC3,
};

// ---------------------------------------------------------------------------
// Interface placeholders. The methods listed are the exact surface the game /
// renderer call against a raw command-list / resource pointer in the
// !rc->gpu_cmd-guarded (never-run) bodies; bodies are no-ops here.
// ---------------------------------------------------------------------------
struct ID3D12Resource {
    D3D12_GPU_VIRTUAL_ADDRESS GetGPUVirtualAddress() const { return 0; }
    long Map(UINT /*subresource*/, const D3D12_RANGE* /*read*/, void** out) {
        if (out) *out = nullptr;
        return 0;
    }
    void Unmap(UINT /*subresource*/, const D3D12_RANGE* /*written*/) {}
};

struct ID3D12GraphicsCommandList {
    void OMSetRenderTargets(UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, int,
                            const D3D12_CPU_DESCRIPTOR_HANDLE*) {}
    void ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE, const float*, UINT,
                               const D3D12_RECT*) {}
    void ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE, UINT, float, uint8_t,
                               UINT, const D3D12_RECT*) {}
    void RSSetViewports(UINT, const D3D12_VIEWPORT*) {}
    void RSSetScissorRects(UINT, const D3D12_RECT*) {}
};

struct ID3D12Device {};
struct ID3D12CommandQueue {};
struct ID3D12DescriptorHeap {};

// Pipeline objects held by value (ComPtr) in the pass classes' private members.
// Empty placeholders — the passes' real bodies don't compile in this backend.
struct ID3D12RootSignature {};
struct ID3D12PipelineState {};
struct ID3DBlob {};

#endif // !HD2D_RENDERER_D3D12
