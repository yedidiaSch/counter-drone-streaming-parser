/**
 * @file ringBuffer.h
 * @brief High-performance circular (ring) buffer for per-client streaming parsing.
 *
 * Designed for zero dynamic allocation on the hot path — memory is allocated
 * exactly once at construction. All push/peek/consume operations work by
 * moving Head and Tail index pointers around a fixed-size backing array.
 */

#pragma once

#include <cstddef>
#include <span>
#include <stdexcept>
#include <vector>

namespace counter_drone {

/**
 * @class RingBuffer
 * @brief Fixed-capacity circular byte buffer with peek/consume separation.
 *
 * Supports the sliding-window resynchronisation pattern required by the
 * streaming parser: data can be peeked (read without consuming) and then
 * selectively consumed — including a 1-byte advance on CRC failure.
 *
 * **Not thread-safe** — each instance is owned by a single parser thread
 * and is accessed only via that thread's per-client state map.
 */
class RingBuffer {
public:
    /** @brief Default capacity in bytes (64 KiB). */
    static constexpr std::size_t DEFAULT_CAPACITY = 65536;

    /**
     * @brief Construct the ring buffer.
     * @param capacity Total byte capacity (allocated once, never resized).
     */
    explicit RingBuffer(std::size_t capacity = DEFAULT_CAPACITY);

    /**
     * @brief Push bytes into the buffer at the Head pointer.
     *
     * Copies incoming data into the circular storage, wrapping around the
     * end of the backing array as needed.
     *
     * @param data Span of bytes to append.
     * @throws std::overflow_error If there is not enough free space.
     */
    void push(std::span<const std::byte> data);

    /**
     * @brief Number of unread bytes currently available.
     * @return Byte count between Tail and Head.
     */
    [[nodiscard]] std::size_t size() const noexcept;

    /**
     * @brief Read bytes from the Tail without consuming them, into a provided buffer.
     *
     * Avoids dynamic memory allocation by writing directly into the provided span.
     * The Tail pointer is not advanced — the data remains in the buffer.
     * Handles wrap-around transparently.
     *
     * @param dest Span representing the destination buffer. Its @c size() dictates
     *             how many bytes to peek.
     * @throws std::out_of_range If @p dest.size() exceeds @c size().
     */
    void peek(std::span<std::byte> dest) const;

    /**
     * @brief Advance the Tail pointer by @p count bytes, discarding them.
     *
     * - **Success case:** @c consume(packet_total_size) after a valid CRC.
     * - **Resync case:** @c consume(1) to drop one byte and re-hunt for
     *   the @c 0xAA55 header (sliding window).
     *
     * @param count Number of bytes to discard.
     * @throws std::out_of_range If @p count exceeds @c size().
     */
    void consume(std::size_t count);

    /**
     * @brief Reset the buffer — discard all data.
     *
     * Resets Head and Tail to 0. Used when a client disconnects to flush
     * any remaining corrupted state without deallocating memory.
     */
    void clear() noexcept;

    /**
     * @brief Total capacity of the buffer (fixed at construction).
     * @return Capacity in bytes.
     */
    [[nodiscard]] std::size_t capacity() const noexcept;

    /**
     * @brief Free space remaining.
     * @return @c capacity() - size().
     */
    [[nodiscard]] std::size_t free_space() const noexcept;

private:
    std::vector<std::byte> m_buf;       /**< Backing storage (allocated once). */
    std::size_t            m_capacity;  /**< Fixed capacity. */
    std::size_t            m_head = 0;  /**< Write index (next push position). */
    std::size_t            m_tail = 0;  /**< Read index (next peek/consume position). */
    std::size_t            m_count = 0; /**< Number of unread bytes. */
};

} // namespace counter_drone
