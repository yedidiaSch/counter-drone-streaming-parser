/**
 * @file ringBuffer.cpp
 * @brief Implementation of the RingBuffer circular byte buffer.
 */

#include "ringBuffer.h"

#include <algorithm>
#include <cstring>

namespace counter_drone {

/// @details Allocates the backing array once; no further heap allocations occur.
RingBuffer::RingBuffer(std::size_t capacity)
    : m_buf(capacity), m_capacity(capacity) {}

/**
 * @brief Append bytes at the Head pointer, wrapping around as needed.
 *
 * Performs a two-part memcpy when the data spans the end of the
 * backing array (wrap-around).
 *
 * @param data Span of bytes to push.
 * @throws std::overflow_error If @p data.size() exceeds free_space().
 */
void RingBuffer::push(std::span<const std::byte> data) {
    // Guard: reject writes that would overflow the buffer.
    if (data.size() > free_space()) {
        throw std::overflow_error(
            "RingBuffer overflow: tried to push " +
            std::to_string(data.size()) + " bytes, only " +
            std::to_string(free_space()) + " free");
    }

    // First chunk: from Head to end of backing array.
    const std::size_t first_chunk = std::min(data.size(), m_capacity - m_head);
    std::memcpy(m_buf.data() + m_head, data.data(), first_chunk);

    if (first_chunk < data.size()) {
        // Wrap around — copy the remainder starting at index 0.
        std::memcpy(m_buf.data(), data.data() + first_chunk,
                    data.size() - first_chunk);
    }

    // Advance the Head pointer (modular arithmetic handles the wrap).
    m_head = (m_head + data.size()) % m_capacity;
    m_count += data.size();
}

/// @return Number of unread bytes between Tail and Head.
std::size_t RingBuffer::size() const noexcept {
    return m_count;
}

/**
 * @brief Copy bytes from the Tail into @p dest without advancing the Tail.
 *
 * Handles wrap-around transparently with a two-part memcpy.
 * This is the key enabler of the deferred-consume parser design.
 *
 * @param dest Destination span — its size() determines how many bytes to peek.
 * @throws std::out_of_range If @p dest.size() exceeds available bytes.
 */
void RingBuffer::peek(std::span<std::byte> dest) const {
    const std::size_t count = dest.size();
    // Guard: can't read more than what's available.
    if (count > m_count) {
        throw std::out_of_range(
            "RingBuffer peek: requested " + std::to_string(count) +
            " bytes, only " + std::to_string(m_count) + " available");
    }

    // First chunk: from Tail to end of backing array.
    const std::size_t first_chunk = std::min(count, m_capacity - m_tail);
    std::memcpy(dest.data(), m_buf.data() + m_tail, first_chunk);

    if (first_chunk < count) {
        // Wrap around — copy the remaining bytes from the start of the array.
        std::memcpy(dest.data() + first_chunk, m_buf.data(),
                    count - first_chunk);
    }
}

/**
 * @brief Advance the Tail pointer, discarding @p count bytes.
 *
 * O(1) operation — just pointer arithmetic, no data movement.
 *
 * @param count Bytes to discard.
 * @throws std::out_of_range If @p count exceeds available bytes.
 */
void RingBuffer::consume(std::size_t count) {
    // Guard: can't discard more than what's available.
    if (count > m_count) {
        throw std::out_of_range(
            "RingBuffer consume: tried to discard " + std::to_string(count) +
            " bytes, only " + std::to_string(m_count) + " available");
    }

    // Advance Tail pointer (modular arithmetic handles wrap-around).
    m_tail = (m_tail + count) % m_capacity;
    m_count -= count;
}

/// @details Resets Head, Tail, and count to zero (capacity unchanged).
void RingBuffer::clear() noexcept {
    m_head  = 0;
    m_tail  = 0;
    m_count = 0;
}

/// @return Total capacity in bytes (set at construction, never changes).
std::size_t RingBuffer::capacity() const noexcept {
    return m_capacity;
}

/// @return Bytes available for writing (capacity - size).
std::size_t RingBuffer::free_space() const noexcept {
    return m_capacity - m_count;
}

} // namespace counter_drone
