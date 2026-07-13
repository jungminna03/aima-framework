#pragma once

// ============================================================================
// rhi.h — thin, backend-agnostic Render Hardware Interface.
//
// Abstracts the GPU so the renderer can run on DX12 (Windows) or SDL_GPU
// (macOS/Linux/Windows) from one set of call sites. This is THE contract:
// nothing above this header may name ID3D12*/DXGI_*/D3D12_*/ComPtr or any
// SDL_GPU type. Passes, asset uploads, and game render systems talk only to
// GpuDevice / CommandList / the opaque handles below.
//
// Design choices match the SDL_GPU resource model (and are trivially mappable
// onto the existing DX12 code):
//   * PUSH UNIFORMS, not GPU-VA constant buffers. Per-draw/per-pass uniform
//     data is memcpy'd into a slot by VALUE (CommandList::push_*_uniform).
//     DX12 maps this onto its bump-allocated upload ring + root CBV; SDL_GPU
//     maps it onto SDL_PushGPU{Vertex,Fragment}UniformData. No GetGPUVirtualAddress.
//   * POSITIONAL texture+sampler binding, not descriptor tables / root sigs.
//     A draw binds an array of {GpuTexture, GpuSampler} pairs to fragment
//     slots 0..N. DX12 maps this onto SRV-heap slots + (dynamic) samplers;
//     SDL_GPU maps it onto SDL_BindGPUFragmentSamplers.
//   * EXPLICIT sampler objects (GpuSampler), created up front, bound per draw —
//     no static samplers baked into a root signature.
//   * Pipelines carry their full render-state + a fixed vertex layout + an
//     attachment-format signature (so they are validated against the pass).
//   * Render passes are explicit begin/end with load/clear ops, matching both
//     SDL_GPU render passes and DX12's manual barrier+OMSetRenderTargets model.
//
// Handles are opaque, trivially-copyable, 64-bit. A null handle (id == 0) is
// "none". The backend owns the lifetime tables behind them.
// ============================================================================

#include <cstdint>
#include <cstddef>

namespace hd2d {

// Interleaved vertex shared by glTF meshes and billboard quads (64 bytes).
// Relocated here from core/math.h during the aima_framework migration: aima's
// math layer is renderer-less and intentionally drops this GRAPHICS type. The
// shader input layout assumes this exact 64-byte stride. tangent.w is the
// bitangent handedness (glTF convention, ±1); color is a linear vertex color
// that multiplies base color (white = no-op).
struct Vertex {
    float px, py, pz;
    float nx, ny, nz;
    float tx = 1.0f, ty = 0.0f, tz = 0.0f, tw = 1.0f;
    float u = 0.0f, v = 0.0f;
    float cr = 1.0f, cg = 1.0f, cb = 1.0f, ca = 1.0f;
};
static_assert(sizeof(Vertex) == 64, "shader input layout assumes 64-byte Vertex");

} // namespace hd2d

namespace hd2d::rhi {

// ----------------------------------------------------------------------------
// Opaque handles. POD; pass by value; id==0 == invalid.
// ----------------------------------------------------------------------------
struct GpuBuffer   { uint64_t id = 0; explicit operator bool() const { return id != 0; } };
struct GpuTexture  { uint64_t id = 0; explicit operator bool() const { return id != 0; } };
struct GpuPipeline { uint64_t id = 0; explicit operator bool() const { return id != 0; } };
struct GpuSampler  { uint64_t id = 0; explicit operator bool() const { return id != 0; } };

inline bool operator==(GpuBuffer a, GpuBuffer b)   { return a.id == b.id; }
inline bool operator==(GpuTexture a, GpuTexture b) { return a.id == b.id; }

// ----------------------------------------------------------------------------
// Formats. Only the set this renderer actually uses, plus the depth formats.
// (DX12 names in comments; SDL_GPU has 1:1 equivalents.)
// ----------------------------------------------------------------------------
enum class GpuFormat : uint8_t {
    Unknown = 0,
    RGBA8Unorm,        // DXGI_FORMAT_R8G8B8A8_UNORM        — backbuffer, data textures
    RGBA8UnormSrgb,    // DXGI_FORMAT_R8G8B8A8_UNORM_SRGB   — sprite/base color
    RGBA16Float,       // DXGI_FORMAT_R16G16B16A16_FLOAT    — HDR scene + bloom
    R32Float,          // DXGI_FORMAT_R32_FLOAT             — shadow depth SRV view
    Depth32Float,      // DXGI_FORMAT_D32_FLOAT             — depth targets
};

// Vertex attribute element formats (input layout).
enum class VertexFormat : uint8_t {
    Float2,            // DXGI_FORMAT_R32G32_FLOAT
    Float3,            // DXGI_FORMAT_R32G32B32_FLOAT
    Float4,            // DXGI_FORMAT_R32G32B32A32_FLOAT
};

enum class IndexFormat : uint8_t { U16, U32 };

// ----------------------------------------------------------------------------
// Frame-packet convenience types (PAL render keystone, 2026-06-23). GpuMesh is an
// opaque mesh handle (the backend's resource table resolves it to vb+ib+count for
// the draw); Material carries per-draw factors + texture handles. These let the
// game's LiveScene/RenderFrame carry backend-neutral handles instead of DX12
// GpuSubmesh/ComPtr. ADDED ADDITIVELY — no consumer yet (wired in later keystone
// bricks); zero behavior change.
// ----------------------------------------------------------------------------
struct GpuMesh { uint64_t id = 0; explicit operator bool() const { return id != 0; } };

struct Material {
    float      base_color[4] = {1, 1, 1, 1};
    float      metallic      = 0.0f;
    float      roughness     = 1.0f;
    float      emissive[3]   = {0, 0, 0};
    float      alpha_cutoff  = 0.0f;   // >0 => alpha-test
    uint32_t   flags         = 0;      // backend sampler / normal-map-present flags
    GpuTexture tex_base;               // srgb base color
    GpuTexture tex_mr;                 // metallic-roughness
    GpuTexture tex_normal;
    GpuTexture tex_emissive;
};

// ----------------------------------------------------------------------------
// Resource creation descriptions.
// ----------------------------------------------------------------------------
enum class BufferUsage : uint8_t {
    Vertex,            // bound via CommandList::bind_vertex_buffer
    Index,             // bound via CommandList::bind_index_buffer
};

// CPU access intent. Static = upload once at create; Dynamic = persistently
// host-writable each frame (cloth re-upload). The backend chooses heaps/staging.
enum class BufferAccess : uint8_t { GpuStatic, HostDynamic };

struct BufferDesc {
    uint32_t      size_bytes = 0;
    BufferUsage   usage      = BufferUsage::Vertex;
    BufferAccess  access     = BufferAccess::GpuStatic;
    const void*   initial_data = nullptr;   // optional; size_bytes if present
    const char*   debug_name = nullptr;
};

enum class TextureUsage : uint8_t {
    Sampled,           // shader-read only (uploaded asset textures)
    ColorTarget,       // render target + sampled (HDR scene, bloom mips)
    DepthTarget,       // depth-stencil target + sampled (shadow map, scene depth)
};

struct TextureDesc {
    uint32_t     width      = 0;
    uint32_t     height     = 0;
    uint32_t     mip_levels = 1;            // >1 => caller supplies a mip chain
    GpuFormat    format     = GpuFormat::RGBA8Unorm;
    TextureUsage usage      = TextureUsage::Sampled;
    const char*  debug_name = nullptr;
};

enum class Filter      : uint8_t { Nearest, Linear };
enum class AddressMode : uint8_t { Wrap, Clamp };
enum class CompareOp   : uint8_t { Never, Less, LessEqual, Always };

struct SamplerDesc {
    Filter      min_filter   = Filter::Linear;
    Filter      mag_filter   = Filter::Linear;
    Filter      mip_filter   = Filter::Linear;
    AddressMode address_u    = AddressMode::Wrap;
    AddressMode address_v    = AddressMode::Wrap;
    AddressMode address_w    = AddressMode::Wrap;
    bool        compare      = false;        // true => PCF shadow comparison sampler
    CompareOp   compare_op   = CompareOp::LessEqual;
    float       max_lod      = 1e30f;         // FLT_MAX equivalent
};

// ----------------------------------------------------------------------------
// Pipeline description. Shaders are passed as backend-native blobs by the
// backend's own shader-loading path (HLSL bytecode for DX12, SPIR-V/MSL for
// SDL_GPU); the RHI takes opaque pointers + sizes so the cross-platform shader
// build pipeline lives outside this header.
// ----------------------------------------------------------------------------
struct ShaderBytecode {
    const void* data = nullptr;
    size_t      size = 0;
};

struct VertexAttribute {
    uint32_t      location;       // shader input index (== input-element order)
    uint32_t      offset_bytes;   // offset within the vertex
    VertexFormat  format;
};

struct VertexLayout {
    const VertexAttribute* attributes      = nullptr;
    uint32_t               attribute_count = 0;
    uint32_t               stride_bytes    = 0;
};

enum class CullMode      : uint8_t { None, Back, Front };
enum class PrimitiveType : uint8_t { TriangleList };

enum class BlendMode : uint8_t {
    Opaque,            // write RGBA, no blend
    Additive,          // ONE + ONE (bloom upsample composite)
};

struct DepthState {
    bool      test_enable  = false;
    bool      write_enable = false;
    CompareOp compare      = CompareOp::Less;
    // Slope-scaled bias baked into the pipeline (shadow pass).
    int32_t   depth_bias        = 0;
    float     slope_scaled_bias = 0.0f;
};

// The attachment-format signature this pipeline renders into. Must match the
// render pass it is used in (the backend validates / specializes on this).
struct PipelineDesc {
    ShaderBytecode vs;
    ShaderBytecode ps;                       // size==0 => depth-only pass

    VertexLayout   vertex_layout;
    PrimitiveType  topology    = PrimitiveType::TriangleList;
    CullMode       cull        = CullMode::None;
    BlendMode      blend       = BlendMode::Opaque;
    DepthState     depth;

    // Attachment formats (0 color targets == depth-only).
    GpuFormat      color_formats[4] = {};
    uint32_t       color_target_count = 0;
    GpuFormat      depth_format = GpuFormat::Unknown;

    const char*    debug_name = nullptr;
};

// ----------------------------------------------------------------------------
// Render-pass description (begin_render_pass). Color + optional depth, with
// load/clear ops. Targets are GpuTextures (ColorTarget/DepthTarget usage) or
// the special swapchain texture acquired from the device.
// ----------------------------------------------------------------------------
enum class LoadOp : uint8_t { Load, Clear, DontCare };

struct ColorAttachment {
    GpuTexture target;
    LoadOp     load_op   = LoadOp::Clear;
    float      clear_color[4] = {0, 0, 0, 1};
};

struct DepthAttachment {
    GpuTexture target;
    LoadOp     load_op     = LoadOp::Clear;
    float      clear_depth = 1.0f;
};

struct RenderPassDesc {
    const ColorAttachment* color = nullptr;
    uint32_t               color_count = 0;
    const DepthAttachment* depth = nullptr;  // optional
};

struct Viewport { float x, y, w, h, min_depth = 0.0f, max_depth = 1.0f; };
struct Rect     { int32_t x, y; uint32_t w, h; };

// One positional texture+sampler binding for a draw (fragment slot index ==
// array position passed to bind_textures).
struct TextureBinding {
    GpuTexture texture;
    GpuSampler sampler;
};

// ----------------------------------------------------------------------------
// CommandList — records one render pass's worth of work. Obtained from the
// device for the current frame; not owned by the caller.
// ----------------------------------------------------------------------------
class CommandList {
public:
    virtual ~CommandList() = default;

    // A render pass groups draws against a fixed set of attachments. The backend
    // performs the necessary layout/barrier transitions on the targets at begin
    // and end (textures are left in a sampleable state on end).
    virtual void begin_render_pass(const RenderPassDesc& desc) = 0;
    virtual void end_render_pass() = 0;

    virtual void set_viewport(const Viewport& vp) = 0;
    virtual void set_scissor(const Rect& rect) = 0;

    virtual void bind_pipeline(GpuPipeline pipeline) = 0;

    // PUSH UNIFORMS by value. `slot` is the uniform buffer binding index in the
    // shader (b0, b1, ...). Data is copied; valid only for the recorded draws.
    virtual void push_vertex_uniform(uint32_t slot, const void* data, size_t size) = 0;
    virtual void push_fragment_uniform(uint32_t slot, const void* data, size_t size) = 0;

    // POSITIONAL texture+sampler binding: bindings[i] -> fragment sampler slot i.
    virtual void bind_textures(const TextureBinding* bindings, uint32_t count) = 0;
    // Convenience for the single-texture passes (shadow / fullscreen).
    void bind_texture(GpuTexture t, GpuSampler s) {
        TextureBinding b{t, s};
        bind_textures(&b, 1);
    }

    virtual void bind_vertex_buffer(GpuBuffer vb) = 0;
    virtual void bind_index_buffer(GpuBuffer ib, IndexFormat fmt) = 0;

    virtual void draw(uint32_t vertex_count, uint32_t first_vertex = 0) = 0;
    virtual void draw_indexed(uint32_t index_count,
                              uint32_t first_index = 0,
                              int32_t  base_vertex = 0) = 0;
};

// ----------------------------------------------------------------------------
// GpuDevice — owns the GPU, swapchain, and resource lifetimes.
// ----------------------------------------------------------------------------
class GpuDevice {
public:
    virtual ~GpuDevice() = default;

    // --- lifetime ---
    // window_handle is the platform native handle (HWND on Windows, SDL_Window*
    // for SDL_GPU); the backend interprets it.
    virtual bool init(void* window_handle, uint32_t width, uint32_t height) = 0;
    virtual void shutdown() = 0;
    virtual void resize(uint32_t width, uint32_t height) = 0;
    virtual void wait_idle() = 0;            // block until GPU drained (teardown/resize)

    virtual uint32_t width() const = 0;
    virtual uint32_t height() const = 0;
    // Format/size of the swapchain image acquire_swapchain_texture() returns.
    virtual GpuFormat swapchain_format() const = 0;

    // --- resource create/destroy ---
    // For mip chains / multi-subresource textures the caller uploads each level
    // via update_texture; create with the final mip_levels.
    virtual GpuTexture create_texture(const TextureDesc& desc) = 0;
    // Upload tightly-packed pixels into one mip level (backend handles padding).
    virtual void update_texture(GpuTexture tex, uint32_t mip_level,
                                const void* pixels, size_t size) = 0;

    virtual GpuBuffer  create_buffer(const BufferDesc& desc) = 0;
    // Host-writable mapping for HostDynamic buffers (cloth ring); nullptr for
    // GpuStatic. Persistently mapped for the buffer's lifetime.
    virtual void*      map(GpuBuffer buf) = 0;

    virtual GpuPipeline create_pipeline(const PipelineDesc& desc) = 0;
    virtual GpuSampler  create_sampler(const SamplerDesc& desc) = 0;

    virtual void destroy_texture(GpuTexture)   = 0;
    virtual void destroy_buffer(GpuBuffer)     = 0;
    virtual void destroy_pipeline(GpuPipeline) = 0;
    virtual void destroy_sampler(GpuSampler)   = 0;

    // --- frame loop ---
    // Begin recording. Returns the per-frame command list (waits for this
    // frame's prior GPU work). Returns nullptr if the frame can't begin.
    virtual CommandList* begin_frame() = 0;
    // Acquire the swapchain backbuffer as an RHI texture (ColorTarget). Valid
    // only between begin_frame and submit; left in PRESENT-ready state by submit.
    virtual GpuTexture acquire_swapchain_texture() = 0;
    // End recording, submit the command list, and present. vsync toggles
    // present sync interval.
    virtual void submit(bool vsync) = 0;

    // --- screenshot (autonomous verification hook) ---
    // Queue a PNG of the next presented frame; consumed inside submit().
    virtual void request_screenshot(const char* path) = 0;

    // The backend's ImGui integration (backend-specific impl behind the RHI).
    // Returns an opaque token the ImGui platform layer needs; see imgui glue.
    // (Kept here so imgui_layer never names a graphics API directly.)
    virtual void* imgui_init_info() = 0;
};

// Backend factory (one definition per backend; the platform picks at link/build).
GpuDevice* create_device();

} // namespace hd2d::rhi
