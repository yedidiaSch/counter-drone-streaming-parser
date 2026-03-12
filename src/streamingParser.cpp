/**
 * @file streamingParser.cpp
 * @brief Implementation of the streaming binary StreamingParser (state machine + CRC).
 */

#include "streamingParser.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <optional>
#include <sstream>

namespace counter_drone {

/**
 * @brief Convert a value from big-endian (network byte order) to host order.
 *
 * Uses @c if @c constexpr so the byte-reversal is compiled away
 * entirely on big-endian targets (zero overhead).
 */
template <typename T>
static T from_big_endian(T value) {
    if constexpr (std::endian::native == std::endian::little && sizeof(T) > 1) {
        auto* bytes = reinterpret_cast<std::byte*>(&value);
        std::reverse(bytes, bytes + sizeof(T));
    }
    return value;
}

/**
 * @brief Construct the StreamingParser.
 *
 * Binds the parser to its input (raw DataChunk) and output (validated
 * Telemetry) queues. No threads are started — call @c run() from a
 * @c std::jthread.
 *
 * @param raw_queue    Queue of tagged raw-data chunks from the NetworkListener.
 * @param parsed_queue Queue to push validated Telemetry packets into.
 */
StreamingParser::StreamingParser(ThreadSafeQueue<DataChunk>& raw_queue, ThreadSafeQueue<Telemetry>& parsed_queue,
                                 ThreadSafeQueue<std::string>& log_queue)
    : m_raw_queue(raw_queue), m_parsed_queue(parsed_queue), m_log_queue(log_queue) {}

/**
 * @brief Main loop — pops DataChunks, routes bytes, drives per-client parsing.
 *
 * Workflow per chunk:
 *   1. Empty chunk → client disconnected — erase its state.
 *   2. Non-empty → push bytes into the client's RingBuffer, run the state machine.
 *
 * Exits cleanly when the stop token is triggered.
 *
 * @param stop_token Cooperative cancellation token from std::jthread.
 */
void StreamingParser::run(std::stop_token stop_token) {
    m_log_queue.push("[StreamingParser] Running\n");

    while (!stop_token.stop_requested()) {
        // Block until a DataChunk is available or shutdown is requested.
        auto maybe_chunk = m_raw_queue.pop(stop_token);
        if (!maybe_chunk.has_value()) break;  // Queue shut down — exit loop.

        auto& chunk = maybe_chunk.value();

        if (chunk.data.empty()) {
            // Empty chunk = client disconnected — erase its ring buffer and state.
            m_clients.erase(chunk.client_id);
            {
                std::ostringstream log_message;
                log_message << "[StreamingParser] Cleaned up state for client fd="
                            << chunk.client_id << "\n";
                m_log_queue.push(std::move(log_message).str());
            }
            continue;
        }

        // Route the incoming bytes into this client's dedicated ring buffer.
        auto& client_state = m_clients[chunk.client_id];
        client_state.ring_buffer.push(chunk.data);

        // Drive the state machine — may extract 0, 1, or many packets.
        process_client(client_state);
    }

    {
        std::ostringstream log_message;
        log_message << "[StreamingParser] Shutting down (CRC failures: " << m_crc_failures << ")\n";
        m_log_queue.push(std::move(log_message).str());
    }
}

/**
 * @brief Drive the 3-state parser on a single client's ring buffer.
 *
 * State machine (deferred-consume design):
 *   - **WAIT_HEADER** — Peek 2 bytes; if != 0xAA55, consume(1) and retry.
 *   - **WAIT_LENGTH** — Peek 4 bytes (header + length); validate range.
 *   - **WAIT_PAYLOAD_AND_CRC** — Peek full packet, verify CRC.
 *     - CRC OK  → consume all, deserialize, push Telemetry.
 *     - CRC BAD → consume(1), resync from WAIT_HEADER.
 *
 * @param client_state Mutable reference to the client's parser state.
 */
void StreamingParser::process_client(ClientState& client_state) {
    // Loop until the ring buffer can't satisfy the current state's requirement.
    while (true) {
        switch (client_state.state) {

        case State::WAIT_HEADER: {
            // Need at least 2 bytes for the header magic.
            if (client_state.ring_buffer.size() < HEADER_SIZE) return;

            // Peek the first 2 bytes to check for the magic header.
            std::array<std::byte, HEADER_SIZE> header_buf{};
            client_state.ring_buffer.peek(header_buf);
            uint16_t header = 0;
            std::memcpy(&header, header_buf.data(), HEADER_SIZE);
            header = from_big_endian(header);

            if (header != PACKET_HEADER) {
                // Not the magic marker — sliding-window resync: drop 1 byte and retry.
                client_state.ring_buffer.consume(1);
                continue;
            }

            // Header found — do NOT consume yet; move to WAIT_LENGTH.
            client_state.state = State::WAIT_LENGTH;
            continue;
        }

        case State::WAIT_LENGTH: {
            // Need header (2) + length (2) = 4 bytes in the buffer.
            if (client_state.ring_buffer.size() < HEADER_SIZE + LENGTH_SIZE) return;

            // Peek header + length together (still no consume).
            std::array<std::byte, HEADER_SIZE + LENGTH_SIZE> header_length_buf{};
            client_state.ring_buffer.peek(header_length_buf);

            // Extract the 2-byte payload length, convert from big-endian.
            std::memcpy(&client_state.payload_length,
                        header_length_buf.data() + HEADER_SIZE, LENGTH_SIZE);
            client_state.payload_length = from_big_endian(client_state.payload_length);

            if (client_state.payload_length == 0 || client_state.payload_length > MAX_PAYLOAD_SIZE) {
                // Absurd length — the 0xAA55 was a coincidence. Drop 1 byte, re-hunt.
                client_state.ring_buffer.consume(1);
                client_state.state = State::WAIT_HEADER;
                continue;
            }

            // Length looks valid — move to final state (still deferred consume).
            client_state.state = State::WAIT_PAYLOAD_AND_CRC;
            continue;
        }

        case State::WAIT_PAYLOAD_AND_CRC: {
            // Total bytes needed: header + length + payload + CRC.
            const std::size_t total =
                HEADER_SIZE + LENGTH_SIZE + client_state.payload_length + CRC_SIZE;

            if (client_state.ring_buffer.size() < total) return;

            // Peek the entire packet (header through CRC) into scratch.
            client_state.scratch_buf.resize(total);
            client_state.ring_buffer.peek(std::span<std::byte>(client_state.scratch_buf));

            // CRC is computed over HEADER + LENGTH + PAYLOAD (excludes the CRC field itself).
            const std::size_t crc_data_len =
                HEADER_SIZE + LENGTH_SIZE + client_state.payload_length;
            uint16_t computed_crc =
                compute_crc16(client_state.scratch_buf.data(), crc_data_len);

            uint16_t received_crc = 0;
            std::memcpy(&received_crc,
                        client_state.scratch_buf.data() + crc_data_len, CRC_SIZE);
            received_crc = from_big_endian(received_crc);

            if (received_crc != computed_crc) {
                ++m_crc_failures;
                m_log_queue.push("[StreamingParser] CRC mismatch — resyncing\n");

                // Drop only the first byte of the false header and re-hunt.
                client_state.ring_buffer.consume(1);
                client_state.state = State::WAIT_HEADER;
                continue;
            }

            // CRC valid — consume the full packet and deserialize payload.
            client_state.ring_buffer.consume(total);

            const std::byte* payload_ptr =
                client_state.scratch_buf.data() + HEADER_SIZE + LENGTH_SIZE;
            auto maybe_telemetry = deserialize_payload(payload_ptr, client_state.payload_length);

            if (maybe_telemetry.has_value()) {
                m_parsed_queue.push(std::move(maybe_telemetry.value()));
            }

            else {
                ++m_crc_failures;
                m_log_queue.push("[StreamingParser] Malformed payload — skipping\n");
            }

            client_state.state = State::WAIT_HEADER;
            continue;
        }

        } // switch
    } // while
}

/**
 * @brief Compute CRC-16/MODBUS over a byte range.
 *
 * Uses the standard MODBUS polynomial 0xA001 (bit-reversed 0x8005),
 * initial value 0xFFFF, and LSB-first processing.
 *
 * @param data Pointer to the byte range.
 * @param len  Number of bytes to process.
 * @return The 16-bit CRC value.
 */
uint16_t StreamingParser::compute_crc16(const std::byte* data, std::size_t len) {
    uint16_t crc = 0xFFFF; // MODBUS initial value.
    for (std::size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint16_t>(static_cast<uint8_t>(data[i]));
        for (int j = 0; j < 8; ++j) { // Process each bit of the byte.
            if (crc & 1)
                crc = (crc >> 1) ^ 0xA001; // Polynomial: bit-reversed 0x8005.
            else
                crc >>= 1;
        }
    }
    return crc;
}

/**
 * @brief Deserialize a binary payload into a Telemetry struct safely.
 *
 * Extracts fields in wire-order (Big-Endian). Uses strict bounds checking
 * for every read operation to prevent buffer overflows or reading
 * incomplete packets (silent failures).
 *
 * @param data Pointer to the start of the payload.
 * @param len  Length of the payload in bytes.
 * @return std::optional<Telemetry> containing the valid data, or std::nullopt if the payload is malformed.
 */
std::optional<Telemetry> StreamingParser::deserialize_payload(const std::byte* data, std::size_t len) {
    Telemetry telemetry;
    std::size_t offset = 0;

    // Generic lambda that reads a field, converts from big-endian, and advances the offset.
    // Returns false if there are not enough bytes left in the buffer to read the requested type.
    auto read_field = [&]<typename T>(T& field) -> bool {
        if (offset + sizeof(T) > len) {
            return false; // Out of bounds - payload is truncated.
        }
        std::memcpy(&field, data + offset, sizeof(T));
        field = from_big_endian(field);
        offset += sizeof(T);
        return true;
    };

    // 1. Read the drone_id length prefix (uint16_t).
    uint16_t id_len = 0;
    if (!read_field(id_len)) {
        return std::nullopt; 
    }

    // 2. Bounds-check the declared string length against the remaining payload capacity.
    if (offset + id_len > len) {
        return std::nullopt; // Declared string length exceeds actual received data.
    }
    
    // Safely extract the string using assign and the validated length.
    telemetry.drone_id.assign(reinterpret_cast<const char*>(data + offset), id_len);
    offset += id_len;

    // 3. Read the remaining fixed-size telemetry fields in wire order.
    // Short-circuit evaluation ensures that if any read fails, we abort immediately.
    if (!read_field(telemetry.latitude)  ||
        !read_field(telemetry.longitude) ||
        !read_field(telemetry.altitude)  ||
        !read_field(telemetry.speed)     ||
        !read_field(telemetry.timestamp)) {
        return std::nullopt;
    }

    // 4. Payload successfully and safely parsed.
    return telemetry;
}

} // namespace counter_drone
