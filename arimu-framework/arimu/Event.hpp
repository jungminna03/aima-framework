#pragma once
#include <vector>
#include <cstdint>
#include <unordered_map>
#include <memory>
#include <algorithm>
#include <climits>
#include <type_traits>

namespace Arimu {

// Type-erased channel interface — uniform flush across types.
class IEventChannel {
public:
    virtual ~IEventChannel() = default;
    virtual void Flush() = 0;
    virtual void ClearAll() = 0;
};

// One channel per event type T. Multi-reader cursors keyed by systemId.
template <typename T>
class EventChannel : public IEventChannel {
public:
    void Send(T event) {
        m_events.push_back({ std::move(event), m_writeTick });
        ++m_writeTick;
    }

    // Returns copies of events >= cursor; advances cursor to latest.
    std::vector<T> ReadAndAdvance(uint64_t systemId) {
        auto& cursor = m_cursors[systemId];
        std::vector<T> out;
        for (const auto& e : m_events) {
            if (e.tick >= cursor) out.push_back(e.value);
        }
        cursor = m_writeTick;
        return out;
    }

    // Remove events consumed by all known readers.
    void Flush() override {
        if (m_cursors.empty()) return;
        uint64_t minCursor = ULLONG_MAX;
        for (const auto& kv : m_cursors) {
            if (kv.second < minCursor) minCursor = kv.second;
        }
        auto it = std::find_if(m_events.begin(), m_events.end(),
            [minCursor](const Entry& e) { return e.tick >= minCursor; });
        m_events.erase(m_events.begin(), it);
    }

    void ClearAll() override {
        m_events.clear();
        for (auto& kv : m_cursors) kv.second = m_writeTick;
    }

private:
    struct Entry { T value; uint64_t tick; };
    std::vector<Entry> m_events;
    std::unordered_map<uint64_t, uint64_t> m_cursors;  // systemId -> consumed-up-to tick
    uint64_t m_writeTick = 0;
};

// Reader handle — wraps channel + systemId.
template <typename T>
class EventReader {
public:
    EventReader(EventChannel<T>& ch, uint64_t systemId)
        : m_channel(ch), m_systemId(systemId) {}

    std::vector<T> Read() { return m_channel.ReadAndAdvance(m_systemId); }
private:
    EventChannel<T>& m_channel;
    uint64_t m_systemId;
};

// Writer handle.
template <typename T>
class EventWriter {
public:
    explicit EventWriter(EventChannel<T>& ch) : m_channel(ch) {}
    void Send(T event) { m_channel.Send(std::move(event)); }
private:
    EventChannel<T>& m_channel;
};

// Trait detection
template <typename T> struct IsEventReader : std::false_type {};
template <typename T> struct IsEventReader<EventReader<T>> : std::true_type { using inner = T; };

template <typename T> struct IsEventWriter : std::false_type {};
template <typename T> struct IsEventWriter<EventWriter<T>> : std::true_type { using inner = T; };

} // namespace Arimu
