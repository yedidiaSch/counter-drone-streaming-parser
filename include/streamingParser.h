/**
 * @file streamingParser.h
 * @brief Streaming binary parser with CRC validation and state-machine design.
 */

#pragma once

#include "ringBuffer.h"
#include "telemetry.h"
#include "threadSafeQueue.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <unordered_map>
#include <vector>

namespace counter_drone {

/**
 * @class StreamingParser
 * @brief Streaming binary parser using a state-machine approach.
 *
 * Consumes tagged DataChunk objects from the raw-data queue, routes bytes
 * to per-client ring buffers, validates packet framing and CRC, and
 * outputs validated Telemetry packets to the parsed queue.
 */
class StreamingParser {
public:
    /**
     * @brief Construct the parser.
     * @param raw_queue    Queue of tagged DataChunk objects produced by the NetworkListener.
     * @param parsed_queue Queue to push validated Telemetry packets into.
     * @param log_queue    Shared queue for asynchronous log message output.
     */
    StreamingParser(ThreadSafeQueue<DataChunk>& raw_queue, ThreadSafeQueue<Telemetry>& parsed_queue,
                    ThreadSafeQueue<std::string>& log_queue);

    /**
     * @brief Main loop — call from a @c std::jthread (honours the stop token).
     * @param stop_token Token checked for cooperative cancellation.
     */
    void run(std::stop_token stop_token);

    /**
     * @brief Get the number of CRC failures observed so far.
     * @return CRC failure count.
     */
    uint64_t crc_failure_count() const { return m_crc_failures; }

private:
    /**
     * @brief Maximum allowed payload size (bytes).
     *
     * Any length field exceeding this is treated as a false header;
     * the parser drops 1 byte and re-scans.
     */
    static constexpr uint16_t MAX_PAYLOAD_SIZE = 256;

    /**
     * @enum State
     * @brief Parser state-machine states (3-state model).
     *
     * The header and length bytes are **not** consumed until the full
     * packet (header + length + payload + CRC) is validated.  This
     * "deferred-consume" design makes CRC-failure resync trivial:
     * just @c consume(1) and restart from @c WAIT_HEADER.
     */
    enum class State {
        WAIT_HEADER,          /**< Searching for the 0xAA55 header marker. */
        WAIT_LENGTH,          /**< Peeking the 2-byte payload length field. */
        WAIT_PAYLOAD_AND_CRC, /**< Waiting for full payload + CRC, then validating. */
    };

    /**
     * @struct ClientState
     * @brief Per-client parser context (ring buffer + state machine).
     */
    struct ClientState {
        State                  state = State::WAIT_HEADER; /**< Current state-machine state. */
        RingBuffer             ring_buffer;   /**< Circular buffer of accumulated raw bytes. */
        std::vector<std::byte> scratch_buf;   /**< Pre-allocated scratch for full-packet peeks. */
        uint16_t               payload_length = 0; /**< Payload length of current packet. */
    };

    ThreadSafeQueue<DataChunk>&    m_raw_queue;       /**< Source of tagged raw-data chunks. */
    ThreadSafeQueue<Telemetry>&    m_parsed_queue;    /**< Destination for valid packets. */
    ThreadSafeQueue<std::string>&  m_log_queue;       /**< Shared queue for async log output. */
    uint64_t                       m_crc_failures = 0; /**< Running CRC failure counter. */

    /** @brief Per-client state, keyed by socket FD. */
    std::unordered_map<int, ClientState> m_clients;

    /**
     * @brief Run the state machine on a specific client's ring buffer.
     * @param client_id Socket FD of the client.
     * @param cs        Mutable reference to the client's parser state.
     */
    void process_client(ClientState& client_state);

    /**
     * @brief Compute a CRC-16 checksum over a span of bytes.
     * @param data Pointer to the data.
     * @param len  Number of bytes.
     * @return The computed CRC-16 value.
     */
    static uint16_t compute_crc16(const std::byte* data, std::size_t len);

    /**
     * @brief Deserialize a Telemetry struct from a binary payload.
     * @param data Pointer to the payload data.
     * @param len  Payload length in bytes.
     * @return The deserialized Telemetry object, or @c std::nullopt if the payload is malformed.
     */
    static std::optional<Telemetry> deserialize_payload(const std::byte* data, std::size_t len);
};

} // namespace counter_drone
