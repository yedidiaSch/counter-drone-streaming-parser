/**
 * @file businessLogic.h
 * @brief Drone tracking, state management, and threshold alerting.
 */

#pragma once

#include "telemetry.h"
#include "threadSafeQueue.h"
#include <chrono>
#include <cstdint>
#include <stop_token>
#include <string>
#include <unordered_map>

namespace counter_drone {

/**
 * @class BusinessLogic
 * @brief Consumes parsed Telemetry packets, maintains drone state,
 *        and raises alerts for threshold violations.
 */
class BusinessLogic {
public:
    /**
     * @brief Construct the business-logic processor.
     * @param parsed_queue Queue of validated Telemetry packets produced by the Parser.
     * @param log_queue    Shared queue for asynchronous log message output.
     */
    explicit BusinessLogic(ThreadSafeQueue<Telemetry>& parsed_queue,
                           ThreadSafeQueue<std::string>& log_queue);

    /**
     * @brief Main loop — call from a @c std::jthread (honours the stop token).
     * @param stop_token Token checked for cooperative cancellation.
     */
    void run(std::stop_token stop_token);

    /**
     * @brief Get a snapshot of the current active drone count.
     * @return Number of unique drones seen so far.
     */
    std::size_t active_drone_count() const;

private:
    ThreadSafeQueue<Telemetry>&                       m_parsed_queue;       /**< Source of parsed packets. */
    ThreadSafeQueue<std::string>&                     m_log_queue;          /**< Shared queue for async log output. */
    std::unordered_map<std::string, Telemetry>        m_drones;             /**< Active drone state table. */
    uint64_t                                          m_packets_processed = 0; /**< Total packets handled. */

    /** @brief Clock used for throughput measurement. */
    using Clock = std::chrono::steady_clock;
    Clock::time_point m_start_time;                     /**< Timestamp when run() entered its loop. */
    Clock::time_point m_last_report_time;               /**< Timestamp of the last PPS report. */
    uint64_t          m_packets_at_last_report = 0;     /**< Packet count at last report snapshot. */

    /** @brief Interval between periodic throughput reports. */
    static constexpr auto REPORT_INTERVAL = std::chrono::seconds(5);

    static constexpr double ALTITUDE_THRESHOLD = 120.0; /**< Altitude alert threshold (metres). */
    static constexpr double SPEED_THRESHOLD    = 50.0;  /**< Speed alert threshold (m/s). */

    /**
     * @brief Check altitude/speed thresholds and print alerts to stdout.
     * @param telemetry The telemetry sample to evaluate.
     */
    void check_alerts(const Telemetry& telemetry);

    /**
     * @brief Log a periodic throughput report if the reporting interval has elapsed.
     */
    void report_throughput();
};

} // namespace counter_drone
