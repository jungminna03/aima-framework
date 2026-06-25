// The STARTER host executable. ~20 lines of real code: construct the project's
// Renderer (BlackRenderer), fill a HostConfig, and hand both to aima::Host. The
// host owns the window, the loop, code + asset hot-reload, and the game-module
// ABI; this file only chooses the renderer + the game module name + the clear
// color. This is the canonical shape of a project's entry point on the aima
// framework — start here and grow your game.

#include "aima/host.h"

#include "renderer.h"

int main() {
    game::BlackRenderer renderer;   // implements aima::Renderer (SDL clear-to-color)

    aima::HostConfig cfg;
    cfg.title = "aima game";
    cfg.width = 1280;
    cfg.height = 720;
    cfg.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};   // SOLID BLACK = an empty game
    cfg.game_module = "game_module";              // -> libgame_module.{dylib,so} / .dll
    cfg.watch_dirs = {};                          // nothing to watch yet
    cfg.shot_frame = 30;                          // AIMA_SHOT captures here
    cfg.max_frames = 120;                         // hard cap so a CI run can't hang

    return aima::Host{}.run(cfg, renderer);       // blocks until quit / cap
}
