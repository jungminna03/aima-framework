#pragma once
#include <concepts>

namespace Arimu {

class App;

// A plugin is any type with `static void Build(App&)`.
template <typename T>
concept PluginType = requires(App& app) {
    { T::Build(app) } -> std::same_as<void>;
};

} // namespace Arimu
