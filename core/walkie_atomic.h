#pragma once

#include <atomic>
#include <cstddef>

namespace walkie
{

// Atomic counter with implicit conversion to T, so it can be used in
// comparisons, indexing and printf-style varargs exactly like a plain variable
// while keeping load/store and increment atomic across FreeRTOS tasks.
template <typename T> class Atomic
{
  public:
    Atomic() : v_(T{}) {}
    Atomic(T v) : v_(v) {}
    // Copy loads the current value (std::atomic itself is not copyable).
    Atomic(const Atomic &o) : v_(o.v_.load(std::memory_order_relaxed)) {}

    operator T() const
    {
        return v_.load(std::memory_order_relaxed);
    }

    T load(std::memory_order order = std::memory_order_relaxed) const
    {
        return v_.load(order);
    }
    void store(T v, std::memory_order order = std::memory_order_relaxed)
    {
        v_.store(v, order);
    }

    Atomic &operator=(T v)
    {
        v_.store(v, std::memory_order_relaxed);
        return *this;
    }

    Atomic &operator++()
    {
        v_.fetch_add(1, std::memory_order_relaxed);
        return *this;
    }
    T operator++(int)
    {
        return v_.fetch_add(1, std::memory_order_relaxed);
    }
    Atomic &operator--()
    {
        v_.fetch_sub(1, std::memory_order_relaxed);
        return *this;
    }
    T operator--(int)
    {
        return v_.fetch_sub(1, std::memory_order_relaxed);
    }
    Atomic &operator+=(T d)
    {
        v_.fetch_add(d, std::memory_order_relaxed);
        return *this;
    }
    Atomic &operator-=(T d)
    {
        v_.fetch_sub(d, std::memory_order_relaxed);
        return *this;
    }

  private:
    std::atomic<T> v_;
};

} // namespace walkie
