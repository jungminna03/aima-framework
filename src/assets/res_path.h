#pragma once
// Convention-based resource paths: name -> file under the project's asset root.
// This is the GENERAL (non-graphics) part of the source engine's asset layer.
// The GPU-bound loaders (glTF -> GPU mesh, sprite-sheet -> GPU texture) are
// GRAPHICS and are intentionally NOT in this framework — each project supplies
// its own loaders next to its Renderer. What stays here is the engine-agnostic
// "name -> path" lookup + the file-watch reload (core/hot_reload.*).
//
// Two ways to set the asset root, in priority order:
//   1. aima::res::set_root("/abs/or/rel/dir")  at runtime (recommended — no
//      hard-coded paths bake into the framework).
//   2. AIMA_ASSET_DIR compile definition (a convenient default the build can set
//      to the project's assets/ dir).
// If neither is set the root is "assets" relative to the cwd.

#include "core/log.h"

#include <cstdlib>
#include <filesystem>
#include <string>

namespace aima::res {

namespace detail {
inline std::string& root_storage() {
    // Priority: AIMA_ASSET_DIR env var (a portable/distributed build ships
    // assets/ next to the exe and exports this at startup) > the
    // AIMA_ASSET_DIR compile definition (source-tree path, dev hot-reload) >
    // "assets" relative to cwd.
    static std::string root = [] {
        if (const char* e = std::getenv("AIMA_ASSET_DIR"); e && *e) return std::string(e);
#ifdef AIMA_ASSET_DIR
        return std::string(AIMA_ASSET_DIR);
#else
        return std::string("assets");
#endif
    }();
    return root;
}
} // namespace detail

// Override the asset root at runtime (e.g. resolved next to the executable).
inline void set_root(const std::string& dir) { detail::root_storage() = dir; }
inline const std::string& root() { return detail::root_storage(); }

// <root>/<sub>/<name><ext>, no existence check. Build any project path.
inline std::string path(const std::string& sub, const std::string& name,
                        const std::string& ext = "") {
    return (std::filesystem::path(root()) / sub / (name + ext)).string();
}

// Resolve <root>/<sub>/<name>{exts...}, returning the first that exists (so a
// loader can prefer .glb over .gltf, .png over .jpg, etc.). Empty + a warning if
// none exist — callers skip the load instead of crashing.
inline std::string resolve(const std::string& sub, const std::string& name,
                           std::initializer_list<const char*> exts) {
    const std::filesystem::path dir = std::filesystem::path(root()) / sub;
    for (const char* ext : exts) {
        std::filesystem::path p = dir / (name + ext);
        if (std::filesystem::exists(p)) return p.string();
    }
    AIMA_WARN("res: '{}' not found under {} (tried {} extensions)", name, dir.string(),
              static_cast<int>(exts.size()));
    return {};
}

// Convention: level/map name -> assets/map/<name>.glb (.gltf fallback, tried
// deterministically when both exist). Missing map: warn + empty string (via
// resolve()) so the caller can skip the load instead of crashing.
inline std::string map_path(const std::string& name) {
    return resolve("map", name, {".glb", ".gltf"});
}

// Sprite sheets load lazily elsewhere, so this only builds the path — no
// existence check, no warning here (the loader logs if the file is missing).
inline std::string sprite_path(const std::string& name) {
    return path("sprite", name, ".png");
}

} // namespace aima::res
