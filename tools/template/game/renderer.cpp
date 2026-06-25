#include "renderer.h"

#include "core/log.h"

#include <SDL3/SDL.h>

// stb_image_write for PNG screenshots (vendored in third_party/). Defining the
// implementation here keeps it in exactly one translation unit.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace game {

bool BlackRenderer::init(SDL_Window* window, int width, int height) {
    width_ = width;
    height_ = height;
    renderer_ = SDL_CreateRenderer(window, nullptr);   // pick any backend
    if (!renderer_) {
        AIMA_ERROR("[black-renderer] SDL_CreateRenderer failed: {}", SDL_GetError());
        return false;
    }
    AIMA_INFO("[black-renderer] ready ({}) {}x{}", SDL_GetRendererName(renderer_), width, height);
    return true;
}

aima::FrameHandle BlackRenderer::begin_frame(const aima::ClearColor& c) {
    // The host passes HostConfig::clear_color here (black in main.cpp). We honor it,
    // so an EMPTY game = a black window. The opaque frame handle IS the SDL_Renderer*;
    // a game render system that wanted to draw would cast it back and draw on top.
    SDL_SetRenderDrawColorFloat(renderer_, c.r, c.g, c.b, c.a);
    SDL_RenderClear(renderer_);
    return renderer_;
}

void BlackRenderer::end_frame(bool /*vsync*/) {
    SDL_RenderPresent(renderer_);
}

void BlackRenderer::resize(int width, int height) {
    width_ = width;
    height_ = height;
}

void BlackRenderer::screenshot(const std::string& path) {
    SDL_Surface* surf = SDL_RenderReadPixels(renderer_, nullptr);
    if (!surf) {
        AIMA_WARN("[black-renderer] readback failed: {}", SDL_GetError());
        return;
    }
    // Convert to a known 32-bit RGBA layout for stb_image_write (a real PNG, so
    // the AIMA_SHOT verification harness can decode it).
    SDL_Surface* rgba = SDL_ConvertSurface(surf, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(surf);
    if (!rgba) {
        AIMA_WARN("[black-renderer] surface convert failed: {}", SDL_GetError());
        return;
    }
    const int ok = stbi_write_png(path.c_str(), rgba->w, rgba->h, 4,
                                  rgba->pixels, rgba->pitch);
    if (ok) AIMA_INFO("[black-renderer] screenshot -> {} ({}x{})", path, rgba->w, rgba->h);
    else    AIMA_WARN("[black-renderer] PNG write failed for {}", path);
    SDL_DestroySurface(rgba);
}

void BlackRenderer::shutdown() {
    if (renderer_) { SDL_DestroyRenderer(renderer_); renderer_ = nullptr; }
}

} // namespace game
