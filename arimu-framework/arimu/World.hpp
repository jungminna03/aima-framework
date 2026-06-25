#pragma once
#include <entt/entt.hpp>
#include "arimu/Event.hpp"
#include "arimu/Commands.hpp"
#include "arimu/Resource.hpp"
#include <typeindex>
#include <unordered_map>
#include <memory>

namespace Arimu {

class World {
public:
    World() = default;
    ~World() = default;
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    // Raw registry access — framework internals (Query construction, plugin init).
    entt::registry& Registry() { return m_registry; }
    const entt::registry& Registry() const { return m_registry; }

    // === Resources (stored in registry's ctx singleton slot per type) ===

    template <typename T, typename... Args>
    T& InsertResource(Args&&... args) {
        return m_registry.ctx().emplace<T>(std::forward<Args>(args)...);
    }

    // Insert only if absent — keeps live state across a code hot-reload, where
    // plugin registration re-runs against a World that already holds the game.
    template <typename T, typename... Args>
    T& EnsureResource(Args&&... args) {
        if (m_registry.ctx().contains<T>()) return m_registry.ctx().get<T>();
        return m_registry.ctx().emplace<T>(std::forward<Args>(args)...);
    }

    template <typename T>
    T& GetResource() {
        return m_registry.ctx().get<T>();
    }

    template <typename T>
    const T& GetResource() const {
        return m_registry.ctx().get<T>();
    }

    template <typename T>
    bool HasResource() const {
        return m_registry.ctx().contains<T>();
    }

    // === Events ===

    template <typename T>
    EventChannel<T>& GetOrCreateChannel() {
        auto key = std::type_index(typeid(T));
        auto it = m_eventChannels.find(key);
        if (it == m_eventChannels.end()) {
            auto ch = std::make_unique<EventChannel<T>>();
            auto* raw = ch.get();
            m_eventChannels.emplace(key, std::move(ch));
            return *raw;
        }
        return *static_cast<EventChannel<T>*>(it->second.get());
    }

    template <typename T>
    EventReader<T> GetEventReader(uint64_t systemId) {
        return EventReader<T>(GetOrCreateChannel<T>(), systemId);
    }

    template <typename T>
    EventWriter<T> GetEventWriter() {
        return EventWriter<T>(GetOrCreateChannel<T>());
    }

    void FlushAllEventChannels() {
        for (auto& kv : m_eventChannels) kv.second->Flush();
    }

    // === Commands factory ===
    Commands MakeCommands() { return Commands(m_registry); }

private:
    entt::registry m_registry;
    std::unordered_map<std::type_index, std::unique_ptr<IEventChannel>> m_eventChannels;
};

} // namespace Arimu
