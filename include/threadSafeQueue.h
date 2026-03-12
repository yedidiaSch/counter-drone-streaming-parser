/**
 * @file threadSafeQueue.h
 * @brief Thread-safe, optionally bounded FIFO queue template with C++20 stop_token integration.
 */

#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <queue>
#include <stop_token>

namespace counter_drone {

/**
 * @class ThreadSafeQueue
 * @brief A thread-safe, optionally bounded FIFO queue with C++20 cooperative cancellation.
 *
 * Uses @c std::condition_variable_any together with @c std::stop_token so that
 * blocking @c pop() / @c push() calls are automatically unblocked when an
 * associated @c std::jthread requests a stop — no manual @c shutdown() needed.
 *
 * @tparam T The element type stored in the queue.
 */
template <typename T>
class ThreadSafeQueue {
public:
    /**
     * @brief Construct the queue.
     * @param max_size Maximum capacity (0 = unbounded).
     */
    explicit ThreadSafeQueue(std::size_t max_size = 0)
        : m_max_size(max_size) {}

    /**
     * @brief Push an item into the queue.
     *
     * Blocks if the queue is full (bounded mode).
     * Automatically unblocks and returns @c false when @p st is stop-requested.
     *
     * @param item The item to enqueue (moved in).
     * @param st   Stop token for cooperative cancellation.
     * @return @c true on success; @c false if a stop was requested.
     */
    bool push(T item, std::stop_token st = {});

    /**
     * @brief Pop an item from the queue.
     *
     * Blocks until an item is available or @p st is stop-requested.
     *
     * @param st Stop token for cooperative cancellation.
     * @return The dequeued item, or @c std::nullopt if a stop was requested and the queue is empty.
     */
    std::optional<T> pop(std::stop_token st = {});

    /**
     * @brief Get the current number of items in the queue.
     * @return Item count.
     */
    std::size_t size() const;

private:
    mutable std::mutex              m_mutex;      /**< Guards all mutable state. */
    std::condition_variable_any     m_not_empty;  /**< Signalled when an item is pushed. */
    std::condition_variable_any     m_not_full;   /**< Signalled when an item is popped (bounded mode). */
    std::queue<T>                   m_queue;      /**< Internal FIFO storage. */
    std::size_t                     m_max_size;   /**< Max capacity (0 = unbounded). */
};

/* ------------------------------------------------------------------ */
/*  Template implementation                                           */
/* ------------------------------------------------------------------ */

template <typename T>
bool ThreadSafeQueue<T>::push(T item, std::stop_token st) {
    std::unique_lock lock(m_mutex);
    if (m_max_size > 0) {
        if (!m_not_full.wait(lock, st, [this] { return m_queue.size() < m_max_size; })) {
            return false;  // stop requested
        }
    }
    if (st.stop_requested()) return false;
    m_queue.push(std::move(item));
    m_not_empty.notify_one();
    return true;
}

template <typename T>
std::optional<T> ThreadSafeQueue<T>::pop(std::stop_token st) {
    std::unique_lock lock(m_mutex);
    if (!m_not_empty.wait(lock, st, [this] { return !m_queue.empty(); })) {
        return std::nullopt;  // stop requested
    }
    T item = std::move(m_queue.front());
    m_queue.pop();
    m_not_full.notify_one();
    return item;
}

template <typename T>
std::size_t ThreadSafeQueue<T>::size() const {
    std::scoped_lock lock(m_mutex);
    return m_queue.size();
}

} // namespace counter_drone
