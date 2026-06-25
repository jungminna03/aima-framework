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

#include <filesystem>
#include <string>

namespace aima::res {

namespace detail {
inline std::string& root_storage() {
    static std::string root =
#ifdef AIMA_ASSET_DIR
        AIMA_ASSET_DIR;
#else
        "assets";
#endif
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

} // namespace aima::res
