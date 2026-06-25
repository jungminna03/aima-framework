#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace aima {

// Coarse classification of a changed file, so callers can route by kind without
// re-parsing the extension. Graphics-specific kinds (shader/texture/model) are
// included as *hints* only — this framework owns no GPU loaders; a PROJECT that
// adds a renderer decides what to do with a Shader/Texture/Model event.
enum class AssetKind { Shader, Texture, Model, Data, Other };

struct FileEvent {
    AssetKind kind;
    std::string path;
};

// Watches one or more directories on a background thread (efsw) and queues
// change events. Drain them on the main thread via poll() so reloads happen
// where the game/renderer state lives.
class HotReload {
public:
    HotReload();
    ~HotReload();

    // Watch every directory in `dirs` recursively. Missing dirs are skipped with
    // a warning (so a project that has no shader dir, say, still boots).
    bool init(const std::vector<std::string>& dirs);
    void shutdown();

    // Invokes the callback for every change since the last poll (main thread).
    void poll(const std::function<void(const FileEvent&)>& on_event);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace aima
