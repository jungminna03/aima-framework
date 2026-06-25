#include "core/hot_reload.h"

#include "core/log.h"

#include <efsw/efsw.hpp>

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <vector>

namespace aima {
namespace {

// Best-effort extension -> kind. The framework owns no GPU loaders, so these are
// only routing hints for a project's renderer/data layer; unknown extensions are
// Other and dropped by default (see handleFileAction).
AssetKind classify(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext == ".hlsl" || ext == ".hlsli" || ext == ".glsl" || ext == ".wgsl" ||
        ext == ".metal" || ext == ".vert" || ext == ".frag")
        return AssetKind::Shader;
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".dds")
        return AssetKind::Texture;
    if (ext == ".glb" || ext == ".gltf" || ext == ".obj" || ext == ".fbx")
        return AssetKind::Model;
    if (ext == ".json" || ext == ".toml" || ext == ".csv")
        return AssetKind::Data;
    return AssetKind::Other;
}

} // namespace

struct HotReload::Impl : public efsw::FileWatchListener {
    efsw::FileWatcher watcher;
    std::mutex mutex;
    std::vector<FileEvent> pending;

    void handleFileAction(efsw::WatchID /*id*/, const std::string& dir,
                          const std::string& filename, efsw::Action action,
                          const std::string& /*old_filename*/) override {
        if (action != efsw::Actions::Add && action != efsw::Actions::Modified) return;
        std::filesystem::path full = std::filesystem::path(dir) / filename;
        FileEvent ev{classify(full.string()), full.string()};
        if (ev.kind == AssetKind::Other) return;
        std::lock_guard<std::mutex> lock(mutex);
        // Coalesce: editors often emit several Modified events per save.
        auto it = std::find_if(pending.begin(), pending.end(),
                               [&](const FileEvent& e) { return e.path == ev.path; });
        if (it == pending.end()) pending.push_back(std::move(ev));
    }
};

HotReload::HotReload() = default;
HotReload::~HotReload() { shutdown(); }

bool HotReload::init(const std::vector<std::string>& dirs) {
    impl_ = std::make_unique<Impl>();
    for (const std::string& dir : dirs) {
        if (dir.empty()) continue;
        if (!std::filesystem::exists(dir)) {
            AIMA_WARN("hot-reload: directory does not exist, skipping: {}", dir);
            continue;
        }
        efsw::WatchID id = impl_->watcher.addWatch(dir, impl_.get(), /*recursive=*/true);
        if (id < 0) {
            AIMA_WARN("hot-reload: failed to watch {} (code {})", dir, id);
        } else {
            AIMA_INFO("hot-reload: watching {}", dir);
        }
    }
    impl_->watcher.watch();
    return true;
}

void HotReload::poll(const std::function<void(const FileEvent&)>& on_event) {
    if (!impl_) return;
    std::vector<FileEvent> events;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        events.swap(impl_->pending);
    }
    for (const FileEvent& ev : events) on_event(ev);
}

void HotReload::shutdown() {
    impl_.reset();
}

} // namespace aima
