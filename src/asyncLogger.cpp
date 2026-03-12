/**
 * @file asyncLogger.cpp
 * @brief Implementation of the asynchronous logging subsystem.
 */

#include "asyncLogger.h"

#include <cerrno>
#include <filesystem>
#include <iostream>
#include <system_error>

namespace counter_drone {

/// @details Stores a reference to the shared log queue. No file output.
AsyncLogger::AsyncLogger(ThreadSafeQueue<std::string>& log_queue)
    : m_log_queue(log_queue) {}

/**
 * @brief Construct the logger with an optional log file.
 *
 * Opens the file in append mode so that repeated runs accumulate rather
 * than overwrite.  If the file cannot be opened, a @c std::system_error
 * is thrown immediately (fail-fast, not silently lost).
 *
 * @param log_queue     Shared queue of pre-formatted log strings.
 * @param log_file_path Path to the output log file.
 * @throws std::system_error If the file cannot be opened for writing.
 */
AsyncLogger::AsyncLogger(ThreadSafeQueue<std::string>& log_queue,
                         const std::string& log_file_path)
    : m_log_queue(log_queue) {
    if (!log_file_path.empty()) {
        // Ensure parent directories exist (e.g. "log/" when running from build/).
        std::filesystem::path file_path(log_file_path);
        if (file_path.has_parent_path()) {
            std::filesystem::create_directories(file_path.parent_path());
        }

        m_log_file.open(log_file_path, std::ios::out | std::ios::app);
        if (!m_log_file.is_open()) {
            throw std::system_error(errno, std::system_category(),
                                    "Failed to open log file: " + log_file_path);
        }
    }
}

/**
 * @brief Write a single message to all active output sinks.
 *
 * Always writes to @c stdout. If a log file is open, writes there too
 * and flushes to ensure durability in case of unexpected termination.
 *
 * @param message The pre-formatted log string to emit.
 */
void AsyncLogger::write_message(const std::string& message) {
    // Always emit to stdout.
    std::cout << message;

    // Mirror to the log file when one is configured.
    if (m_log_file.is_open()) {
        m_log_file << message;
        m_log_file.flush();
    }
}

/**
 * @brief Main loop — pops pre-formatted strings and writes them to all sinks.
 *
 * Workflow:
 *   1. Block in @c pop() until a message arrives or the stop token fires.
 *   2. Write the message to stdout and (optionally) the log file.
 *   3. After the stop token fires, drain any remaining queued messages
 *      so that no log lines are lost during graceful shutdown.
 *
 * @param stop_token Cooperative cancellation token from @c std::jthread.
 */
void AsyncLogger::run(std::stop_token stop_token) {
    // Primary loop — blocks on the queue until shutdown is requested.
    while (!stop_token.stop_requested()) {
        auto maybe_message = m_log_queue.pop(stop_token);
        if (!maybe_message.has_value()) break; // Queue shut down — exit loop.

        write_message(maybe_message.value());
    }

    // Drain any messages that were pushed between the stop request and now.
    // A pre-stopped token makes pop() return immediately when the queue is
    // empty, but still dequeues any items that are already present.
    std::stop_source drain_source;
    drain_source.request_stop();
    auto drain_token = drain_source.get_token();

    while (true) {
        auto remaining_message = m_log_queue.pop(drain_token);
        if (!remaining_message.has_value()) break;

        write_message(remaining_message.value());
    }
}

} // namespace counter_drone
