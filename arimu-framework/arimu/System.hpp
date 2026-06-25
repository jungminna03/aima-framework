#pragma once
#include "arimu/World.hpp"
#include "arimu/Resource.hpp"
#include "arimu/Query.hpp"
#include "arimu/Event.hpp"
#include "arimu/Commands.hpp"
#include "arimu/Schedule.hpp"
#include <type_traits>
#include <tuple>
#include <cstdint>

namespace Arimu {

// Parameter factory — given a desired parameter type T, construct an instance from World.
// This is the core of Bevy-style auto-injection.
template <typename T>
auto MakeParam(World& world, uint64_t systemId) {
    using Bare = std::remove_cv_t<std::remove_reference_t<T>>;
    if constexpr (IsRes<Bare>::value) {
        using Inner = typename IsRes<Bare>::inner;
        return Res<Inner>(world.GetResource<Inner>());
    } else if constexpr (IsResMut<Bare>::value) {
        using Inner = typename IsResMut<Bare>::inner;
        return ResMut<Inner>(world.GetResource<Inner>());
    } else if constexpr (IsQuery<Bare>::value) {
        return Bare(world.Registry());
    } else if constexpr (IsEventReader<Bare>::value) {
        using Inner = typename IsEventReader<Bare>::inner;
        return world.GetEventReader<Inner>(systemId);
    } else if constexpr (IsEventWriter<Bare>::value) {
        using Inner = typename IsEventWriter<Bare>::inner;
        return world.GetEventWriter<Inner>();
    } else if constexpr (IsCommands<Bare>::value) {
        return Commands(world.Registry());
    } else {
        static_assert(sizeof(T*) == 0,
            "Unsupported system parameter type. Allowed: Res<T>, ResMut<T>, Query<...>, EventReader<T>, EventWriter<T>, Commands.");
    }
}

// Wrap a free function (or function pointer) into a type-erased SystemFn.
// Each invocation:
//   1. Constructs parameter tuple via MakeParam<Params>...
//   2. Calls fn via std::apply
//   3. Flushes any Commands parameter in the tuple
template <typename... Params>
SystemFn WrapSystem(void (*fn)(Params...)) {
    return [fn](World& world, uint64_t sysId) {
        auto args = std::tuple<std::remove_cv_t<std::remove_reference_t<Params>>...>{
            MakeParam<Params>(world, sysId)...
        };

        std::apply(fn, args);

        // Flush any Commands present in the argument tuple.
        std::apply([](auto&... a) {
            (void)std::initializer_list<int>{ ([](auto& p) {
                if constexpr (std::is_same_v<std::remove_reference_t<decltype(p)>, Commands>) {
                    p.Flush();
                }
            }(a), 0)... };
        }, args);
    };
}

// Monotonic system id generator.
class SystemIdGen {
public:
    static uint64_t Next() {
        static uint64_t counter = 0;
        return ++counter;
    }
};

} // namespace Arimu
