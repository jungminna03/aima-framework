#include "ui/imgui_layer.h"

#include "core/log_compat.h"
#include "assets/res_path.h"   // aima::res::root() — 번들 폰트(assets/fonts) 해석
#include "renderer/device.h"   // Dx12Device (real DX12 on Windows, SDL_GPU elsewhere)

#include <imgui.h>
#if defined(HD2D_RENDERER_D3D12)
#include <imgui_impl_dx12.h>
#else
#include <imgui_impl_sdlgpu3.h>
#endif
#include <imgui_impl_sdl3.h>

#include <SDL3/SDL.h>

namespace hd2d {
#if defined(HD2D_RENDERER_D3D12)
namespace {

// ImGui 1.92's DX12 backend allocates SRV descriptors on demand; route those
// requests to our shared descriptor heap via the device stored in UserData.
void srv_alloc(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* cpu,
               D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
    static_cast<Dx12Device*>(info->UserData)->alloc_srv(cpu, gpu);
}

void srv_free(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpu,
              D3D12_GPU_DESCRIPTOR_HANDLE gpu) {
    static_cast<Dx12Device*>(info->UserData)->free_srv(cpu, gpu);
}

} // namespace
#endif

bool ImGuiLayer::init(SDL_Window* window, [[maybe_unused]] Dx12Device& device) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    // Load a Korean-capable system font so in-game UI (e.g. 공격 / 구르기 battle
    // buttons) renders Hangul; the default ImGui font is ASCII-only. Falls back to
    // the default font if the platform font isn't present.
    {
        static ImVector<ImWchar> ranges;
        ImFontGlyphRangesBuilder builder;
        builder.AddRanges(io.Fonts->GetGlyphRangesDefault());
        builder.AddRanges(io.Fonts->GetGlyphRangesKorean());
        builder.BuildRanges(&ranges);
        // 한글 가능 폰트 후보 체인 (PAL fonts, 2026-06-23): 한 경로만 하드코딩하면
        // 그 OS에서 폰트가 없을 때 조용히 ASCII 폴백(두부 □)으로 떨어진다. 여러 후보를
        // 순회해 첫 성공을 쓴다. Windows는 malgun이 1순위라 기존 동작 불변, mac/linux는
        // 후보가 늘어 견고해진다(.ttc 컬렉션은 ImGui 기본 FontNo=0 로드).
        // 1순위: 번들 폰트 도스샘물(DOSSaemmul, MIT — 라이선스 전문은
        // assets/fonts/LICENSE-hurss-fonts.txt). 비트맵 유래 폰트라 16px
        // 정수배에서 또렷하다. 없으면 기존 시스템 폰트 체인으로 폴백.
        const std::string bundled =
            (std::filesystem::path(aima::res::root()) / "fonts" / "DOSSaemmul.ttf").string();
        const char* candidates[] = {
            bundled.c_str(),
#if defined(_WIN32)
            "C:\\Windows\\Fonts\\malgun.ttf",
            "C:\\Windows\\Fonts\\malgunbd.ttf",
            "C:\\Windows\\Fonts\\gulim.ttc",
            "C:\\Windows\\Fonts\\batang.ttc",
#elif defined(__APPLE__)
            "/System/Library/Fonts/AppleSDGothicNeo.ttc",
            "/System/Library/Fonts/Supplemental/AppleGothic.ttf",
            "/Library/Fonts/AppleGothic.ttf",
#else
            "/usr/share/fonts/truetype/nanum/NanumGothic.ttf",
            "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
            "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
#endif
        };
        ImFont* kr = nullptr;
        const char* loaded_from = nullptr;
        for (const char* path : candidates) {
            const float size = (path == bundled.c_str()) ? 16.0f : 18.0f;
            kr = io.Fonts->AddFontFromFileTTF(path, size, nullptr, ranges.Data);
            if (kr) { loaded_from = path; break; }
        }
        if (kr) {
            io.FontDefault = kr;
            HD2D_INFO("loaded Korean UI font ({})", loaded_from);
        } else {
            io.Fonts->AddFontDefault();
            HD2D_ERROR("Korean UI font not found (tried {} candidates); UI falls back to ASCII",
                       static_cast<int>(sizeof(candidates) / sizeof(candidates[0])));
        }
    }

#if defined(HD2D_RENDERER_D3D12)
    if (!ImGui_ImplSDL3_InitForD3D(window)) {
        HD2D_ERROR("ImGui_ImplSDL3_InitForD3D failed");
        return false;
    }

    ImGui_ImplDX12_InitInfo init{};
    init.Device = device.device();
    init.CommandQueue = device.queue();
    init.NumFramesInFlight = static_cast<int>(Dx12Device::kFrameCount);
    init.RTVFormat = Dx12Device::kBackBufferFormat;
    init.DSVFormat = DXGI_FORMAT_UNKNOWN;
    init.SrvDescriptorHeap = device.srv_heap();
    init.UserData = &device;
    init.SrvDescriptorAllocFn = &srv_alloc;
    init.SrvDescriptorFreeFn = &srv_free;
    if (!ImGui_ImplDX12_Init(&init)) {
        HD2D_ERROR("ImGui_ImplDX12_Init failed");
        return false;
    }

    initialized_ = true;
    HD2D_INFO("ImGui {} initialized (SDL3 + DX12)", IMGUI_VERSION);
    return true;
#else
    // SDL_GPU (Metal on macOS): the official imgui_impl_sdlgpu3 renderer backend
    // draws ImGui onto the swapchain. The platform backend must be init'd for
    // SDL_GPU so the SDL3 NewFrame path matches.
    if (!ImGui_ImplSDL3_InitForSDLGPU(window)) {
        HD2D_ERROR("ImGui_ImplSDL3_InitForSDLGPU failed");
        return false;
    }

    ImGui_ImplSDLGPU3_InitInfo init{};
    init.Device = device.gpu();
    // ImGui renders straight onto the swapchain (see end_frame), so its pipeline's
    // color target format must match the swapchain's.
    init.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(device.gpu(), window);
    init.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
    if (!ImGui_ImplSDLGPU3_Init(&init)) {
        HD2D_ERROR("ImGui_ImplSDLGPU3_Init failed");
        return false;
    }
    initialized_ = true;
    HD2D_INFO("ImGui {} initialized (SDL3 + SDL_GPU)", IMGUI_VERSION);
    return true;
#endif
}

void ImGuiLayer::process_event(const SDL_Event& event) {
    if (initialized_) ImGui_ImplSDL3_ProcessEvent(&event);
}

void ImGuiLayer::begin_frame() {
#if defined(HD2D_RENDERER_D3D12)
    ImGui_ImplDX12_NewFrame();
#else
    ImGui_ImplSDLGPU3_NewFrame();
#endif
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
}

void ImGuiLayer::end_frame([[maybe_unused]] Dx12Device& device,
                           [[maybe_unused]] ID3D12GraphicsCommandList* gpu_cmd) {
    ImGui::Render();
#if defined(HD2D_RENDERER_D3D12)
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), gpu_cmd);
#else
    // Draw the UI onto the swapchain via the device's current frame handles. The
    // scene (geometry + post resolve) was already recorded onto swap_tex this
    // frame; we LOAD it and composite ImGui on top, before the device submits.
    ImDrawData* draw_data = ImGui::GetDrawData();
    SDL_GPUCommandBuffer* cmd = device.gpu_cmd();
    SDL_GPUTexture* swap = device.swap_texture();
    if (draw_data && cmd && swap && draw_data->TotalVtxCount > 0) {
        // Upload vertex/index/texture data (must be OUTSIDE a render pass).
        ImGui_ImplSDLGPU3_PrepareDrawData(draw_data, cmd);

        SDL_GPUColorTargetInfo target{};
        target.texture = swap;
        target.load_op = SDL_GPU_LOADOP_LOAD;     // keep the rendered scene
        target.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &target, 1, nullptr);
        if (pass) {
            ImGui_ImplSDLGPU3_RenderDrawData(draw_data, cmd, pass);
            SDL_EndGPURenderPass(pass);
        }
    }
#endif
}

void ImGuiLayer::shutdown() {
    if (!initialized_) return;
#if defined(HD2D_RENDERER_D3D12)
    ImGui_ImplDX12_Shutdown();
#else
    ImGui_ImplSDLGPU3_Shutdown();
#endif
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    initialized_ = false;
}

} // namespace hd2d
