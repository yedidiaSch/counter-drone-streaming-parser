/**
 * @file telemetry.h
 * @brief Binary packet constants and the Telemetry data structure.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace counter_drone {

/** @brief Magic header value marking the start of every telemetry packet. */
constexpr uint16_t PACKET_HEADER = 0xAA55;

/** @brief Size of the packet header field in bytes. */
constexpr std::size_t HEADER_SIZE = 2;

/** @brief Size of the packet length field in bytes. */
constexpr std::size_t LENGTH_SIZE = 2;

/** @brief Size of the CRC-16 checksum field in bytes. */
constexpr std::size_t CRC_SIZE    = 2;

/**
 * @struct Telemetry
 * @brief Telemetry data received from a drone.
 */
struct Telemetry {
    std::string drone_id;          /**< Unique identifier of the drone. */
    double      latitude  = 0.0;   /**< Latitude in decimal degrees. */
    double      longitude = 0.0;   /**< Longitude in decimal degrees. */
    double      altitude  = 0.0;   /**< Altitude in metres above ground. */
    double      speed     = 0.0;   /**< Speed in metres per second. */
    uint64_t    timestamp = 0;     /**< Unix timestamp (epoch). */
};

/**
 * @struct DataChunk
 * @brief Tagged raw-data chunk produced by the NetworkListener.
 *
 * Each chunk carries the socket FD that identifies the originating drone,
 * along with the raw bytes received in a single @c recv() call.
 * An empty @c data vector signals that the client has disconnected.
 */
struct DataChunk {
    int                    client_id; /**< Socket FD identifying the drone. */
    std::vector<std::byte> data;      /**< Raw bytes (empty = disconnect signal). */
};

} // namespace counter_drone
