#pragma once

struct SDL_Window;
union SDL_Event;
struct ID3D12GraphicsCommandList;

namespace hd2d {

class Dx12Device;

// Owns the Dear ImGui context plus its SDL3 + DX12 backends.
class ImGuiLayer {
public:
    bool init(SDL_Window* window, Dx12Device& device);
    void shutdown();

    void process_event(const SDL_Event& event);
    void begin_frame();
    // Records ImGui draw data into the open frame. On DX12 it goes into `gpu_cmd`;
    // on the SDL_GPU backend it is drawn onto the swapchain via the device's
    // current command buffer + swapchain texture (gpu_cmd is null there).
    void end_frame(Dx12Device& device, ID3D12GraphicsCommandList* gpu_cmd);

private:
    bool initialized_ = false;
};

} // namespace hd2d
