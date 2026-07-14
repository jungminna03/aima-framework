#pragma once
#include <entt/entt.hpp>
#include <vector>
#include <functional>
#include <memory>
#include <type_traits>

namespace Arimu {

// Deferred registry mutation buffer. Systems record intents; Flush() applies.
//
// Create() returns a real entity id synchronously (safe when caller isn't iterating views of
// the affected component types). AddComponent/RemoveComponent/Destroy are deferred until Flush(),
// which happens at the end of each system invocation (see arimu/System.hpp WrapSystem).
//
// Value-semantic with shared internal state: copies of a Commands instance share the same
// deferred-op buffer, so passing `Commands cmd` (by value) into a system still routes its
// queued ops back to the WrapSystem-owned instance that gets Flush()ed. Without the shared
// buffer, system-local mutations would be silently lost on return.
class Commands {
public:
    explicit Commands(entt::registry& reg)
        : m_registry(reg),
          m_deferred(std::make_shared<std::vector<std::function<void()>>>()) {}

    entt::entity Create() { return m_registry.create(); }

    template <typename C>
    void AddComponent(entt::entity e, C comp) {
        m_deferred->emplace_back([reg = &m_registry, e, c = std::move(comp)]() mutable {
            if (reg->valid(e)) reg->emplace<C>(e, std::move(c));
        });
    }

    template <typename C>
    void RemoveComponent(entt::entity e) {
        m_deferred->emplace_back([reg = &m_registry, e]() {
            if (reg->valid(e) && reg->all_of<C>(e)) reg->remove<C>(e);
        });
    }

    void Destroy(entt::entity e) {
        m_deferred->emplace_back([reg = &m_registry, e]() {
            if (reg->valid(e)) reg->destroy(e);
        });
    }

    void Flush() {
        for (auto& op : *m_deferred) op();
        m_deferred->clear();
    }

    // Direct registry access for spawn helpers that create+emplace synchronously
    // (e.g. hd2d::fx::spawn). ⚠ The returned registry mutates immediately — call ONLY
    // when the caller is NOT iterating a view of the affected component pools (collect
    // during iteration, spawn after the loop). Deferred ops still go through AddComponent.
    entt::registry& registry() { return m_registry; }

private:
    entt::registry& m_registry;
    std::shared_ptr<std::vector<std::function<void()>>> m_deferred;
};

template <typename T> struct IsCommands : std::false_type {};
template <> struct IsCommands<Commands> : std::true_type {};

} // namespace Arimu
