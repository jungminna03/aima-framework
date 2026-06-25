#pragma once

// The STARTER renderer: ~a screenful of SDL3's built-in 2D render API, clearing
// the window to SOLID BLACK every frame. It proves the aima::Renderer seam with
// no graphics library beyond SDL3 (which the framework already links). This is
// your blank canvas — when you start drawing your game, you either draw on top of
// this clear or replace it with your own backend (DX12 / SDL_GPU / etc.). The host
// code never changes, only this implementation.

#include "aima/renderer.h"

struct SDL_Renderer;
struct SDL_Window;

namespace game {

class BlackRenderer final : public aima::Renderer {
public:
    bool init(SDL_Window* window, int width, int height) override;
    aima::FrameHandle begin_frame(const aima::ClearColor& clear) override;
    void end_frame(bool vsync) override;
    void resize(int width, int height) override;
    void screenshot(const std::string& path) override;
    void shutdown() override;

private:
    SDL_Renderer* renderer_ = nullptr;
    int width_ = 0, height_ = 0;
};

} // namespace game
