#pragma once
#include <entt/entt.hpp>
#include <type_traits>

namespace Arimu {

// System parameter: typed view over components. Thin wrapper around entt::view.
//
// Usage in systems:
//   void MySystem(Query<UnitTag, Stats> units) {
//       for (auto [e, tag, stats] : units.each()) { ... }
//   }
template <typename... Comps>
class Query {
public:
    explicit Query(entt::registry& reg)
        : m_view(reg.view<Comps...>()) {}

    auto each() { return m_view.each(); }
    auto begin() { return m_view.begin(); }
    auto end() { return m_view.end(); }

    // Note: size_hint() for multi-component views; size() for single-component.
    // EnTT view has .size_hint() method regardless of count >= 1.
    std::size_t size() const {
        if constexpr (sizeof...(Comps) == 1) {
            return m_view.size();
        } else {
            return m_view.size_hint();
        }
    }

    template <typename C>
    decltype(auto) Get(entt::entity e) { return m_view.template get<C>(e); }

    bool Contains(entt::entity e) const { return m_view.contains(e); }

    auto& Raw() { return m_view; }

private:
    decltype(std::declval<entt::registry&>().view<Comps...>()) m_view;
};

template <typename T> struct IsQuery : std::false_type {};
template <typename... Cs> struct IsQuery<Query<Cs...>> : std::true_type {};

} // namespace Arimu
