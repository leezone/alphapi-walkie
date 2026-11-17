#pragma once

#include <atomic>
#include <stddef.h>
#include <stdint.h>

namespace walkie
{

// Lock-free SPSC byte ring (single producer / single consumer).
// Producer: ringPush()      Consumer: ringPop()
// Indices are atomics with acquire/release ordering, so the data write happens
// before the index publish and the data read happens before the index release.
template <typename W, typename R>
inline size_t ringAvailable(const W &writePos, const R &readPos, size_t capacity)
{
    if (capacity == 0)
        return 0;
    size_t w = writePos.load(std::memory_order_acquire);
    size_t r = readPos.load(std::memory_order_acquire);
    return (w + capacity - r) % capacity;
}

// Push one byte. Returns false if the buffer is full (byte dropped).
template <typename W, typename R>
inline bool ringPush(uint8_t *buf, size_t capacity, W &writePos, R &readPos, uint8_t value)
{
    if (!buf || capacity == 0)
        return false;
    size_t w = writePos.load(std::memory_order_relaxed);
    size_t r = readPos.load(std::memory_order_acquire);
    size_t next = (w + 1) % capacity;
    if (next == r)
        return false; // full
    buf[w] = value;
    writePos.store(next, std::memory_order_release);
    return true;
}

// Pop one byte. Returns false if the buffer is empty.
template <typename R, typename W>
inline bool ringPop(const uint8_t *buf, size_t capacity, R &readPos, W &writePos, uint8_t &out)
{
    if (!buf || capacity == 0)
        return false;
    size_t r = readPos.load(std::memory_order_relaxed);
    size_t w = writePos.load(std::memory_order_acquire);
    if (r == w)
        return false; // empty
    out = buf[r];
    readPos.store((r + 1) % capacity, std::memory_order_release);
    return true;
}

// Reset both indices (only safe when producer and consumer are quiescent).
template <typename W, typename R> inline void ringReset(W &writePos, R &readPos)
{
    writePos.store(0, std::memory_order_release);
    readPos.store(0, std::memory_order_release);
}

} // namespace walkie
