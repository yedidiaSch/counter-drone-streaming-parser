/**
 * @file main.cpp
 * @brief Counter-Drone TCP Communication Layer — Main Entry Point.
 *
 * Wires together the four primary threads:
 *   1. NetworkListener  — accepts TCP connections, pushes raw bytes.
 *   2. StreamingParser  — state-machine streaming parser.
 *   3. BusinessLogic    — drone tracking & alerting.
 *   4. AsyncLogger      — dedicated logging thread (stdout).
 *
 * Handles SIGINT for graceful shutdown via @c std::jthread stop tokens.
 */

#include "asyncLogger.h"
#include "businessLogic.h"
#include "listener.h"
#include "streamingParser.h"
#include "telemetry.h"
#include "threadSafeQueue.h"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace {
    /** @brief Global atomic flag set by the SIGINT handler. */
    std::atomic<bool> g_shutdown_requested{false};

    /**
     * @brief POSIX signal handler for SIGINT (Ctrl+C).
     * @param signum Signal number (unused).
     */
    void signal_handler(int /*signum*/) {
        g_shutdown_requested.store(true);
    }
} // namespace

/**
 * @brief Application entry point.
 *
 * Wires together the four-thread pipeline:
 *   1. NetworkListener  — epoll-based TCP server.
 *   2. StreamingParser  — per-client state-machine binary parser.
 *   3. BusinessLogic    — drone tracking and threshold alerting.
 *   4. AsyncLogger      — dedicated stdout writer (off hot paths).
 *
 * All threads are launched as @c std::jthread instances (auto-joining).
 * SIGINT triggers graceful shutdown via stop tokens.
 *
 * @param argc Argument count.
 * @param argv Argument vector. Options:
 *             - @c argv[1] = TCP port (default 9000).
 *             - @c --log-file \<path\> = also write log output to a file.
 * @return 0 on success.
 */
int main(int argc, char* argv[]) {
    // ── Parse CLI arguments ────────────────────────────────────────
    uint16_t    port = 9000;
    std::string log_file_path = "log/counter_drone.log";

    for (int i = 1; i < argc; ++i) {
        std::string argument(argv[i]);

        if (argument == "--log-file" && i + 1 < argc) {
            // Next argument overrides the default log file path.
            log_file_path = argv[++i];
        } else {
            // Positional argument — treat as port number.
            port = static_cast<uint16_t>(std::atoi(argv[i]));
        }
    }

    // ── Register SIGINT handler ────────────────────────────────────
    std::signal(SIGINT, signal_handler);

    std::cout << "=== Counter-Drone Communication Layer ===\n";
    std::cout << "Starting on port " << port
              << "  (logging to " << log_file_path << ")"
              << "  (Ctrl+C to stop)\n\n";

    // ── Shared data structures ─────────────────────────────────────
    counter_drone::ThreadSafeQueue<counter_drone::DataChunk>  raw_queue;
    counter_drone::ThreadSafeQueue<counter_drone::Telemetry>  parsed_queue;
    counter_drone::ThreadSafeQueue<std::string>               log_queue;

    // ── Create components ──────────────────────────────────────────
    counter_drone::NetworkListener network_listener(port, raw_queue, log_queue);
    counter_drone::StreamingParser  parser(raw_queue, parsed_queue, log_queue);
    counter_drone::BusinessLogic   business_logic(parsed_queue, log_queue);
    counter_drone::AsyncLogger     async_logger(log_queue, log_file_path);

    // ── Launch threads (std::jthread auto-joins on destruction) ────
    // Each lambda captures the component by reference and forwards the stop_token.
    // The logger thread starts first so it can display messages from the other threads.
    std::jthread logger_thread  ([&](std::stop_token st) { async_logger.run(st); });
    std::jthread listener_thread([&](std::stop_token st) { network_listener.run(st); });
    std::jthread parser_thread  ([&](std::stop_token st) { parser.run(st); });
    std::jthread logic_thread   ([&](std::stop_token st) { business_logic.run(st); });

    // ── Wait for shutdown signal ───────────────────────────────────
    // Poll the atomic flag set by SIGINT (200ms granularity is fine for Ctrl+C).
    while (!g_shutdown_requested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cout << "\n[Main] SIGINT received — initiating graceful shutdown...\n";

    // ── Shut down pipeline threads BEFORE the logger ───────────────
    // Each pipeline thread pushes a final log message during shutdown.
    // We must wait for them to finish before stopping the logger,
    // otherwise the logger drains and exits before those messages arrive.

    // 1. Stop and join the listener (pushes "[NetworkListener] Shutting down...").
    listener_thread.request_stop();
    listener_thread.join();

    // 2. Stop and join the parser (pushes "[StreamingParser] Shutting down...").
    parser_thread.request_stop();
    parser_thread.join();

    // 3. Stop and join business logic (pushes "[BusinessLogic] Shutting down...").
    logic_thread.request_stop();
    logic_thread.join();

    // 4. NOW stop the logger — it will drain all remaining queued messages.
    logger_thread.request_stop();
    // logger_thread joins automatically in its destructor.

    std::cout << "[Main] All threads joined. Goodbye.\n";
    return 0;
}
