#pragma once
#include <type_traits>

namespace Arimu {

class World;

// Read-only resource handle. System parameter expressing "I only read this resource".
template <typename T>
class Res {
public:
    explicit Res(const T& ref) : m_ref(ref) {}
    const T& operator*() const { return m_ref; }
    const T* operator->() const { return &m_ref; }
    const T& Get() const { return m_ref; }
private:
    const T& m_ref;
};

// Mutable resource handle.
template <typename T>
class ResMut {
public:
    explicit ResMut(T& ref) : m_ref(ref) {}
    T& operator*() const { return m_ref; }
    T* operator->() const { return &m_ref; }
    T& Get() const { return m_ref; }
private:
    T& m_ref;
};

// Trait detection
template <typename T> struct IsRes : std::false_type {};
template <typename T> struct IsRes<Res<T>> : std::true_type { using inner = T; };

template <typename T> struct IsResMut : std::false_type {};
template <typename T> struct IsResMut<ResMut<T>> : std::true_type { using inner = T; };

} // namespace Arimu
