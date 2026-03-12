/**
 * @file asyncLogger.h
 * @brief Asynchronous logging subsystem for the counter-drone system.
 *
 * Decouples I/O from hot threads by consuming pre-formatted log messages
 * from a shared @c ThreadSafeQueue<std::string> and writing them to
 * @c stdout and optionally to a log file on a dedicated thread.
 */

#pragma once

#include "threadSafeQueue.h"
#include <fstream>
#include <stop_token>
#include <string>

namespace counter_drone {

/**
 * @class AsyncLogger
 * @brief Dedicated logging thread that drains a shared message queue to stdout
 *        and optionally to a file.
 *
 * Producers (NetworkListener, StreamingParser, BusinessLogic) format log
 * strings on their own threads and push them via @c std::move into the
 * shared queue.  The AsyncLogger pops each message and writes it to
 * @c stdout (always) and to a file (when a path is provided via CLI),
 * keeping all console I/O off the latency-sensitive paths.
 *
 * On shutdown the logger drains any remaining messages before returning,
 * ensuring no log lines are lost.
 */
class AsyncLogger {
public:
    /**
     * @brief Construct the async logger (stdout only).
     * @param log_queue Shared queue of pre-formatted log strings.
     */
    explicit AsyncLogger(ThreadSafeQueue<std::string>& log_queue);

    /**
     * @brief Construct the async logger with file output.
     * @param log_queue     Shared queue of pre-formatted log strings.
     * @param log_file_path Path to the output log file. If non-empty, messages
     *                      are written to both @c stdout and this file.
     * @throws std::system_error If the file cannot be opened for writing.
     */
    AsyncLogger(ThreadSafeQueue<std::string>& log_queue, const std::string& log_file_path);

    /**
     * @brief Main loop — call from a @c std::jthread.
     *
     * Blocks in @c pop() until a message arrives or the stop token fires.
     * After the stop token fires, drains any remaining messages so that
     * nothing is lost during graceful shutdown.
     *
     * @param stop_token Cooperative cancellation token from @c std::jthread.
     */
    void run(std::stop_token stop_token);

private:
    ThreadSafeQueue<std::string>& m_log_queue; /**< Source of pre-formatted log messages. */
    std::ofstream                 m_log_file;  /**< Optional file output stream (open = active). */

    /**
     * @brief Write a single message to all active output sinks.
     *
     * Always writes to @c stdout. If a log file is open, writes there too.
     *
     * @param message The pre-formatted log string to emit.
     */
    void write_message(const std::string& message);
};

} // namespace counter_drone
