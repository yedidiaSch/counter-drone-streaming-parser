/**
 * @file businessLogic.cpp
 * @brief Implementation of the BusinessLogic drone tracker and alert system.
 */

#include "businessLogic.h"

#include <iomanip>
#include <sstream>

namespace counter_drone {

/// @details Stores references to the parsed telemetry queue and log queue.
BusinessLogic::BusinessLogic(ThreadSafeQueue<Telemetry>& parsed_queue,
                             ThreadSafeQueue<std::string>& log_queue)
    : m_parsed_queue(parsed_queue), m_log_queue(log_queue) {}

/**
 * @brief Main loop — consumes Telemetry packets and maintains drone state.
 *
 * For each packet:
 *   1. Update the drone state table (keyed by drone_id).
 *   2. Increment the packet counter.
 *   3. Check altitude/speed thresholds and print alerts.
 *
 * Exits cleanly when the stop token is triggered or the queue is shut down.
 *
 * @param stop_token Cooperative cancellation token from std::jthread.
 */
void BusinessLogic::run(std::stop_token stop_token) {
    m_log_queue.push("[BusinessLogic] Running\n");

    m_start_time       = Clock::now();
    m_last_report_time = m_start_time;

    while (!stop_token.stop_requested()) {
        // Block until a parsed Telemetry packet arrives or shutdown is requested.
        auto maybe_telem = m_parsed_queue.pop(stop_token);
        if (!maybe_telem.has_value()) break; // Queue shut down — exit loop.

        const auto& telemetry = maybe_telem.value();

        // Upsert the drone's latest state in the tracking table.
        m_drones[telemetry.drone_id] = telemetry;
        ++m_packets_processed;

        // Evaluate altitude/speed thresholds and print alerts if needed.
        check_alerts(telemetry);

        // Periodically report throughput.
        report_throughput();
    }

    // Final shutdown summary with overall average PPS.
    {
        auto total_elapsed = std::chrono::duration<double>(Clock::now() - m_start_time).count();
        double avg_pps = (total_elapsed > 0.0)
                         ? static_cast<double>(m_packets_processed) / total_elapsed
                         : 0.0;

        std::ostringstream log_message;
        log_message << "[BusinessLogic] Shutting down (processed " << m_packets_processed
                    << " packets, " << m_drones.size() << " unique drones, avg "
                    << std::fixed << std::setprecision(1) << avg_pps << " pkt/s)\n";
        m_log_queue.push(std::move(log_message).str());
    }
}

/// @return Number of unique drone IDs in the state table.
std::size_t BusinessLogic::active_drone_count() const {
    return m_drones.size();
}

/**
 * @brief Check altitude and speed thresholds; print alerts to stdout.
 * @param telemetry The telemetry sample to evaluate.
 */
void BusinessLogic::check_alerts(const Telemetry& telemetry) {
    // Alert if the drone exceeds the safe altitude ceiling.
    if (telemetry.altitude > ALTITUDE_THRESHOLD) {
        std::ostringstream log_message;
        log_message << "[ALERT] Drone " << telemetry.drone_id
                    << " altitude " << telemetry.altitude
                    << " exceeds threshold " << ALTITUDE_THRESHOLD << "\n";
        m_log_queue.push(std::move(log_message).str());
    }
    // Alert if the drone exceeds the safe speed limit.
    if (telemetry.speed > SPEED_THRESHOLD) {
        std::ostringstream log_message;
        log_message << "[ALERT] Drone " << telemetry.drone_id
                    << " speed " << telemetry.speed
                    << " exceeds threshold " << SPEED_THRESHOLD << "\n";
        m_log_queue.push(std::move(log_message).str());
    }
}

/**
 * @brief Log a throughput report every REPORT_INTERVAL seconds.
 *
 * Reports both the interval PPS (packets in the last window) and the
 * cumulative average PPS since the start of run().
 */
void BusinessLogic::report_throughput() {
    auto now = Clock::now();
    auto interval_elapsed = std::chrono::duration<double>(now - m_last_report_time).count();

    if (interval_elapsed < std::chrono::duration<double>(REPORT_INTERVAL).count()) {
        return;
    }

    uint64_t interval_packets = m_packets_processed - m_packets_at_last_report;
    double interval_pps = (interval_elapsed > 0.0)
                          ? static_cast<double>(interval_packets) / interval_elapsed
                          : 0.0;

    double total_elapsed = std::chrono::duration<double>(now - m_start_time).count();
    double avg_pps = (total_elapsed > 0.0)
                     ? static_cast<double>(m_packets_processed) / total_elapsed
                     : 0.0;

    {
        std::ostringstream log_message;
        log_message << "[Throughput] " << std::fixed << std::setprecision(1)
                    << interval_pps << " pkt/s (interval), "
                    << avg_pps << " pkt/s (avg), "
                    << m_packets_processed << " total, "
                    << m_drones.size() << " drones\n";
        m_log_queue.push(std::move(log_message).str());
    }

    m_last_report_time       = now;
    m_packets_at_last_report = m_packets_processed;
}

} // namespace counter_drone
