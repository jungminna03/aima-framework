// ============================================================================
// sdlgpu/device.cpp — REAL SDL_GPU device (Phase 2).
//
// Replaces the Phase-0 SDL_Renderer clear with the SDL3 GPU API:
//   init        : SDL_CreateGPUDevice (SPIRV+MSL) + SDL_ClaimWindowForGPUDevice
//   begin_frame : acquire a command buffer + swapchain texture, begin a render
//                 pass that clears to the requested color
//   (rung 3)    : the TrianglePass draws one triangle into that pass
//   end_frame   : end the pass, (optionally) read the swapchain back to CPU and
//                 write a PNG, then submit
//
// begin_frame still returns nullptr: the DX12-coupled game render systems guard
// on `!rc->gpu_cmd` and no-op, so this device's own draw (clear + triangle) is
// the only thing on screen — exactly the Phase-2 milestone.
// ============================================================================

#include "renderer/sdlgpu/device.h"

#include "renderer/sdlgpu/geometry_pass.h"
#include "renderer/sdlgpu/post_pass.h"
#include "renderer/sdlgpu/triangle_pass.h"

#include "assets/res_path.h"
#include "core/log_compat.h"

#include <SDL3/SDL.h>

#include <stb_image_write.h>

#include <cstdlib>
#include <vector>

namespace hd2d {

bool Dx12Device::init(void* window_handle, uint32_t width, uint32_t height) {
    window_ = static_cast<SDL_Window*>(window_handle);
    width_ = width;
    height_ = height;

    if (!window_) {
        HD2D_ERROR("sdlgpu device: null SDL_Window handle");
        return false;
    }

    // Request both SPIR-V (Vulkan) and MSL (Metal) shader formats so the same
    // device works whether SDL picks the Metal or Vulkan driver. SDL_shadercross
    // (rung 3) emits whichever the chosen driver accepts. nullptr name = let SDL
    // choose the best backend (Metal on macOS).
    const bool debug_mode =
#ifdef NDEBUG
        false;
#else
        true;
#endif
    gpu_ = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL,
                               debug_mode, nullptr);
    if (!gpu_) {
        HD2D_ERROR("sdlgpu device: SDL_CreateGPUDevice failed: {}", SDL_GetError());
        return false;
    }

    if (!SDL_ClaimWindowForGPUDevice(gpu_, window_)) {
        HD2D_ERROR("sdlgpu device: SDL_ClaimWindowForGPUDevice failed: {}", SDL_GetError());
        SDL_DestroyGPUDevice(gpu_);
        gpu_ = nullptr;
        return false;
    }

    const char* driver = SDL_GetGPUDeviceDriver(gpu_);
    HD2D_INFO("sdlgpu device initialized ({}x{}, driver='{}') — real SDL_GPU path",
              width, height, driver ? driver : "?");

    // Real world geometry (rung 4). Loads the world .glb directly (CPU meshes
    // come free) and draws it lit, with depth, framed by the orbit camera. The
    // map matches the game's default level resolution (HD2D_MAP env, else the
    // game's village_c default). Non-fatal: falls back to the triangle.
    {
        const char* map_env = std::getenv("HD2D_MAP");
        const std::string level = map_env ? map_env : "village_c";
        const std::string glb = aima::res::map_path(level);
        if (!glb.empty()) {
            geometry_ = new GeometryPass();
            if (!geometry_->init(*this, window_, glb)) {
                HD2D_WARN("sdlgpu device: geometry pass init failed for '{}'", glb);
                geometry_->shutdown(gpu_);
                delete geometry_;
                geometry_ = nullptr;
            }
        }
    }

    // Post chain (rung 2): bloom + AgX tonemap from an HDR offscreen target to
    // the swapchain. Only engaged for the live geometry path; non-fatal (if it
    // fails to init, the geometry pass renders straight to the swapchain LDR).
    post_ = new PostPass();
    if (!post_->init(gpu_, window_)) {
        post_->shutdown(gpu_);
        delete post_;
        post_ = nullptr;
    }

    // Hello-triangle pipeline (rung 3) — the fallback when no world geometry is
    // available. Non-fatal: degrades to the rung-2 clear.
    triangle_ = new TrianglePass();
    if (!triangle_->init(gpu_, window_)) {
        delete triangle_;
        triangle_ = nullptr;
    }
    return true;
}

void Dx12Device::shutdown() {
    if (gpu_) {
        SDL_WaitForGPUIdle(gpu_);
    }
    if (geometry_) {
        geometry_->shutdown(gpu_);
        delete geometry_;
        geometry_ = nullptr;
    }
    if (post_) {
        post_->shutdown(gpu_);
        delete post_;
        post_ = nullptr;
    }
    if (triangle_) {
        triangle_->shutdown(gpu_);
        delete triangle_;
        triangle_ = nullptr;
    }
    if (gpu_ && window_) {
        SDL_ReleaseWindowFromGPUDevice(gpu_, window_);
    }
    if (gpu_) {
        SDL_DestroyGPUDevice(gpu_);
        gpu_ = nullptr;
    }
    window_ = nullptr;
}

void Dx12Device::resize(uint32_t width, uint32_t height) {
    width_ = width;
    height_ = height;
    // The SDL_GPU swapchain tracks the window's drawable size automatically;
    // SDL_AcquireGPUSwapchainTexture returns the current size each frame.
}

ID3D12GraphicsCommandList* Dx12Device::begin_frame(const float clear_color[4]) {
    cmd_ = nullptr;
    swap_tex_ = nullptr;
    if (!gpu_) return nullptr;

    cmd_ = SDL_AcquireGPUCommandBuffer(gpu_);
    if (!cmd_) {
        HD2D_ERROR("sdlgpu: SDL_AcquireGPUCommandBuffer failed: {}", SDL_GetError());
        return nullptr;
    }

    // Block until a swapchain texture is available (simple, correct framing for
    // a single-threaded loop). On a minimized window this returns true with a
    // null texture — we skip the pass that frame.
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd_, window_, &swap_tex_, &swap_w_, &swap_h_)) {
        HD2D_ERROR("sdlgpu: acquire swapchain texture failed: {}", SDL_GetError());
        SDL_SubmitGPUCommandBuffer(cmd_);
        cmd_ = nullptr;
        return nullptr;
    }
    if (!swap_tex_) {
        // Window not presentable this frame (e.g. minimized): submit the empty
        // command buffer so it isn't leaked.
        SDL_SubmitGPUCommandBuffer(cmd_);
        cmd_ = nullptr;
        return nullptr;
    }

    // World geometry needs a depth buffer; the triangle/clear path doesn't.
    // Prefer the LIVE ECS scene the game's sdlgpu render system filled this
    // frame (live world + live camera + billboards); fall back to geometry's
    // own standalone glb load only when the game handed over nothing.
    const bool have_live = live_.valid &&
                           (!live_.meshes.empty() || !live_.billboards.empty());
    const bool draw_geometry = geometry_ && geometry_->valid();

    // Post chain (rung 2): when post is available AND there is world geometry to
    // draw, the lit scene renders into an HDR offscreen target and the post pass
    // (bloom + AgX tonemap) resolves it to the swapchain. The triangle/clear-only
    // fallback still renders straight to the swapchain (LDR).
    SDL_GPUTexture* hdr_tex = nullptr;
    if (post_ && post_->valid() && draw_geometry) {
        hdr_tex = post_->hdr_target(gpu_, swap_w_, swap_h_);
    }
    const bool use_post = (hdr_tex != nullptr);

    SDL_GPUColorTargetInfo color{};
    color.texture = use_post ? hdr_tex : swap_tex_;
    color.clear_color = SDL_FColor{clear_color[0], clear_color[1], clear_color[2],
                                   clear_color[3]};
    color.load_op = SDL_GPU_LOADOP_CLEAR;
    color.store_op = SDL_GPU_STOREOP_STORE;

    // --- sun shadow pass (rung 1) -----------------------------------------
    // Render the live meshes into the sun shadow map from the light's POV in a
    // dedicated depth-only render pass BEFORE the color pass, so the geometry
    // fragment shader can sample it. Runs only for the live ECS path.
    if (draw_geometry && have_live && live_.shadow_active) {
        SDL_GPUTexture* shadow_tex = geometry_->shadow_target(gpu_);
        if (shadow_tex) {
            SDL_GPUDepthStencilTargetInfo sd{};
            sd.texture = shadow_tex;
            sd.clear_depth = 1.0f;
            sd.load_op = SDL_GPU_LOADOP_CLEAR;
            sd.store_op = SDL_GPU_STOREOP_STORE;   // kept for the color pass to read
            sd.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
            sd.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
            SDL_GPURenderPass* spass = SDL_BeginGPURenderPass(cmd_, nullptr, 0, &sd);
            if (spass) {
                geometry_->render_shadow_map(gpu_, cmd_, spass, live_);
                SDL_EndGPURenderPass(spass);
            }
        }
    }

    SDL_GPUDepthStencilTargetInfo depth{};
    SDL_GPUDepthStencilTargetInfo* depth_ptr = nullptr;
    if (draw_geometry) {
        SDL_GPUTexture* dt = geometry_->depth_target(gpu_, swap_w_, swap_h_);
        if (dt) {
            depth.texture = dt;
            depth.clear_depth = 1.0f;
            depth.load_op = SDL_GPU_LOADOP_CLEAR;
            depth.store_op = SDL_GPU_STOREOP_DONT_CARE;
            depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
            depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
            depth_ptr = &depth;
        }
    }

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd_, &color, 1, depth_ptr);
    if (pass) {
        if (draw_geometry && depth_ptr && have_live) {
            geometry_->draw_live(gpu_, cmd_, pass, live_, swap_w_, swap_h_);
        } else if (draw_geometry && depth_ptr) {
            geometry_->draw(cmd_, pass, swap_w_, swap_h_);  // standalone fallback
        } else if (triangle_) {
            triangle_->draw(cmd_, pass, swap_w_, swap_h_);
        }
        SDL_EndGPURenderPass(pass);
    }

    // Resolve the HDR scene to the swapchain (bloom + AgX tonemap). The post pass
    // opens its own render passes on cmd_, so this must be OUTSIDE the geometry
    // pass above.
    if (use_post) {
        PostParams pp{};  // defaults mirror the DX12 PostSettings
        post_->execute(gpu_, cmd_, swap_tex_, swap_w_, swap_h_, pp);
    }

    // The live scene was consumed this frame; the game refills it next frame.
    live_.clear();

    // No DX12 command list: the game render systems early-return on !rc->gpu_cmd.
    return nullptr;
}

void Dx12Device::end_frame(bool /*vsync*/) {
    if (!cmd_) {
        capture_path_.clear();
        return;
    }

    if (!capture_path_.empty() && swap_tex_) {
        // Read the rendered swapchain back to a transfer (download) buffer and
        // write a PNG. We must submit + wait so the GPU has finished writing the
        // texture before mapping the download buffer.
        const uint32_t w = swap_w_, h = swap_h_;
        const uint32_t bpp = 4;
        const uint32_t bytes = w * h * bpp;

        SDL_GPUTransferBufferCreateInfo tci{};
        tci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
        tci.size = bytes;
        SDL_GPUTransferBuffer* xfer = SDL_CreateGPUTransferBuffer(gpu_, &tci);
        if (xfer) {
            SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd_);
            SDL_GPUTextureRegion region{};
            region.texture = swap_tex_;
            region.w = w;
            region.h = h;
            region.d = 1;
            SDL_GPUTextureTransferInfo dst{};
            dst.transfer_buffer = xfer;
            dst.pixels_per_row = w;
            dst.rows_per_layer = h;
            SDL_DownloadFromGPUTexture(copy, &region, &dst);
            SDL_EndGPUCopyPass(copy);

            // Submit + fence so the download completes before we map.
            SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd_);
            cmd_ = nullptr;  // consumed by submit
            if (fence) {
                SDL_WaitForGPUFences(gpu_, true, &fence, 1);
                SDL_ReleaseGPUFence(gpu_, fence);
            }

            void* mapped = SDL_MapGPUTransferBuffer(gpu_, xfer, false);
            if (mapped) {
                // The swapchain format may be BGRA or RGBA depending on driver;
                // normalize to RGBA for the PNG.
                const SDL_GPUTextureFormat fmt =
                    SDL_GetGPUSwapchainTextureFormat(gpu_, window_);
                std::vector<uint8_t> rgba(static_cast<size_t>(bytes));
                const uint8_t* src = static_cast<const uint8_t*>(mapped);
                const bool bgra = (fmt == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM ||
                                   fmt == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB);
                for (uint32_t i = 0; i < w * h; ++i) {
                    const uint8_t* p = src + i * 4;
                    uint8_t* o = rgba.data() + i * 4;
                    if (bgra) { o[0] = p[2]; o[1] = p[1]; o[2] = p[0]; o[3] = p[3]; }
                    else      { o[0] = p[0]; o[1] = p[1]; o[2] = p[2]; o[3] = p[3]; }
                }
                SDL_UnmapGPUTransferBuffer(gpu_, xfer);
                write_screenshot(rgba.data(), w, h);
            } else {
                HD2D_ERROR("sdlgpu screenshot: map transfer buffer failed: {}",
                           SDL_GetError());
            }
            SDL_ReleaseGPUTransferBuffer(gpu_, xfer);
        } else {
            HD2D_ERROR("sdlgpu screenshot: create transfer buffer failed: {}",
                       SDL_GetError());
        }
        capture_path_.clear();
    }

    if (cmd_) {
        SDL_SubmitGPUCommandBuffer(cmd_);
        cmd_ = nullptr;
    }
    swap_tex_ = nullptr;
}

void Dx12Device::write_screenshot(const void* rgba, uint32_t w, uint32_t h) {
    if (stbi_write_png(capture_path_.c_str(), static_cast<int>(w), static_cast<int>(h),
                       4, rgba, static_cast<int>(w * 4)) != 0) {
        HD2D_INFO("screenshot written: {} ({}x{})", capture_path_, w, h);
    } else {
        HD2D_ERROR("screenshot: stbi_write_png failed for {}", capture_path_);
    }
}

void Dx12Device::flush() {
    if (gpu_) SDL_WaitForGPUIdle(gpu_);
}

bool Dx12Device::alloc_srv(D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu,
                           D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu) {
    // Hand out distinct non-zero dummy ids so callers that test `srv_cpu.ptr`
    // treat the (stub) texture as "allocated". No real descriptor heap exists.
    const uint64_t id = next_srv_++;
    if (out_cpu) out_cpu->ptr = static_cast<SIZE_T>(id);
    if (out_gpu) out_gpu->ptr = id;
    return true;
}

void Dx12Device::free_srv(D3D12_CPU_DESCRIPTOR_HANDLE /*cpu*/,
                          D3D12_GPU_DESCRIPTOR_HANDLE /*gpu*/) {
    // No-op: dummy ids are never recycled in the skeleton.
}

} // namespace hd2d
