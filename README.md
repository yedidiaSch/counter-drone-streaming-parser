# Counter-Drone Streaming Parser

A high-performance, multi-threaded C++20 TCP server that simulates the communication layer of a counter-drone system. The server receives binary telemetry packets from multiple drones over TCP, parses them using a streaming state machine, and raises real-time alerts when safety thresholds are violated.

---

## Architecture

```
                        ThreadSafeQueue              ThreadSafeQueue
                         <DataChunk>                  <Telemetry>
┌──────────────────┐          │          ┌──────────────────┐          │          ┌────────────────┐
│  NetworkListener │──────────┼─────────▶│  StreamingParser │──────────┼─────────▶│ BusinessLogic  │
│  (epoll TCP      │          │          │  (3-State        │          │          │ (Drone Tracker │
│   Server)        │          │          │   Machine)       │          │          │  & Alerting)   │
└──────────────────┘          │          └──────────────────┘          │          └────────────────┘
      Thread 1                │                Thread 2               │                Thread 3
          │                   │                    │                  │                    │
          │       ThreadSafeQueue<string>          │                  │                    │
          └───────────────────┼────────────────────┘                  │                    │
                              │                                       │                    │
                              ▼                                       │                    │
                     ┌────────────────┐◀──────────────────────────────┘────────────────────┘
                     │  AsyncLogger   │
                     │  (stdout +     │
                     │   file)        │
                     └────────────────┘
                          Thread 4
```

### Threading Model

The application runs **4 threads**, each as a `std::jthread` with cooperative `std::stop_token` cancellation:

| Thread | Component | Responsibility |
|--------|-----------|----------------|
| 1 | **NetworkListener** | Single-threaded `epoll` event loop. Accepts TCP connections, reads raw bytes with non-blocking `recv()`, packages them into `DataChunk` structs tagged by client socket FD, and pushes them to the raw data queue. Uses `eventfd` for graceful wakeup on shutdown. |
| 2 | **StreamingParser** | Consumes `DataChunk` objects, routes bytes to per-client `RingBuffer` instances, and runs a 3-state machine to extract and validate telemetry packets. Pushes validated `Telemetry` structs to the parsed queue. |
| 3 | **BusinessLogic** | Maintains an in-memory drone state table keyed by `drone_id`. Checks altitude (>120 m) and speed (>50 m/s) thresholds, prints alerts, and reports throughput (packets/sec) every 5 seconds. |
| 4 | **AsyncLogger** | Dedicated I/O thread that drains a shared `ThreadSafeQueue<string>` and writes to both stdout and a log file. Keeps I/O off the hot paths of threads 1–3. |

All inter-thread communication uses `ThreadSafeQueue<T>`, a bounded FIFO backed by `std::condition_variable_any` with native `std::stop_token` integration — no manual shutdown flags needed.

---

## Wire Protocol

Every telemetry packet on the TCP stream follows this binary layout (big-endian / network byte order):

```
┌──────────┬──────────┬──────────────────────┬──────────┐
│ HEADER   │ LENGTH   │      PAYLOAD         │   CRC    │
│ 2 bytes  │ 2 bytes  │   LENGTH bytes       │ 2 bytes  │
│ 0xAA55   │ uint16   │                      │ uint16   │
└──────────┴──────────┴──────────────────────┴──────────┘
```

**Payload structure:**

| Field | Type | Size | Description |
|-------|------|------|-------------|
| `id_len` | `uint16_t` | 2 | Length of the drone ID string |
| `drone_id` | `char[]` | `id_len` | ASCII drone identifier |
| `latitude` | `double` | 8 | Decimal degrees |
| `longitude` | `double` | 8 | Decimal degrees |
| `altitude` | `double` | 8 | Metres above ground |
| `speed` | `double` | 8 | Metres per second |
| `timestamp` | `uint64_t` | 8 | Unix epoch timestamp |

**CRC**: CRC-16/MODBUS computed over `HEADER + LENGTH + PAYLOAD`.

---

## Parsing Logic & Resynchronization

### 3-State Machine (Deferred-Consume Design)

The parser uses a **deferred-consume** approach — bytes are **peeked** from the ring buffer during validation but only **consumed** once an entire packet is confirmed valid. This makes CRC-failure recovery trivial.

```
    ┌─────────────┐
    │ WAIT_HEADER │◄──────────────────────────────────────┐
    │             │                                       │
    │ Peek 2B,   │    No 0xAA55?                         │
    │ check magic │───────────────▶ consume(1), retry     │
    └──────┬──────┘                                       │
           │ Found 0xAA55 (don't consume)                 │
           ▼                                              │
    ┌─────────────┐                                       │
    │ WAIT_LENGTH │                                       │
    │             │    Length = 0 or > 256?                │
    │ Peek 4B,   │───────────────▶ consume(1), WAIT_HEADER│
    │ extract len │                                       │
    └──────┬──────┘                                       │
           │ Valid length (don't consume)                  │
           ▼                                              │
    ┌──────────────────┐                                  │
    │ WAIT_PAYLOAD_CRC │                                  │
    │                  │    CRC mismatch?                  │
    │ Peek full packet │───────────────▶ consume(1)───────┘
    │ Validate CRC     │
    └──────┬───────────┘
           │ CRC OK
           ▼
     consume(total), deserialize,
     push Telemetry, → WAIT_HEADER
```

### Resynchronization on CRC Failure

When a CRC check fails, the parser does **not** discard the entire packet. Instead, it:

1. **Drops exactly 1 byte** from the ring buffer (the first byte of the false `0xAA55` header).
2. **Returns to `WAIT_HEADER`** and rescans from the next byte forward.

This **sliding-window** approach ensures that if a valid packet begins within what appeared to be a corrupted packet's payload, it will still be found. The deferred-consume design makes this O(1) — no data needs to be copied or moved back.

### Multi-Client Isolation

Each connected drone gets its own `RingBuffer` and state machine context (stored in a `std::unordered_map<int, ClientState>`). Data from different clients is **never interleaved**, preventing cross-contamination of streams.

---

## Build Instructions

### Prerequisites

- **Linux** (uses `epoll`, `eventfd`, `recv`)
- **GCC 12+** or **Clang 15+** (C++20 required)
- **CMake 3.20+**
- **Python 3.10+** with `rich` library (for load testing only)

### Build

```bash
# Configure
cmake -B build

# Build
cmake --build build -j$(nproc)
```

The server binary is produced at `build/counter_drone_server`.

### Run

```bash
# Start the server (default port 9000, logs to log/counter_drone.log)
./build/counter_drone_server

# Custom port
./build/counter_drone_server 9001

# Custom log file
./build/counter_drone_server 9000 --log-file /tmp/drone.log
```

Press **Ctrl+C** for graceful shutdown. You should see:

```
[Main] SIGINT received — initiating graceful shutdown...
[NetworkListener] Shutdown event received. Shutting down...
[StreamingParser] Shutting down (CRC failures: N)
[BusinessLogic] Shutting down (processed N packets, N unique drones, avg N.N pkt/s)
[Main] All threads joined. Goodbye.
```

### Load Test

```bash
# Install dependency
pip install rich

# Run with 50 drones for 60 seconds at 30 packets/sec each
python3 tests/drone_load_test.py --drones 50 --duration 60 --rate 30
```

> **Tip:** To reach ~1000 packets/sec (the project's target throughput), run with **90–100 drones** at the default rate, e.g. `--drones 100`.

The load test features a real-time Rich terminal dashboard showing instant/avg/peak PPS, per-profile packet counts, and connection statistics. Six drone profiles are assigned round-robin:

| Profile | Behaviour |
|---------|-----------|
| Normal | Steady stream of valid packets |
| Threshold | Every packet exceeds altitude or speed limits |
| Corrupted | ~20% of packets have intentionally bad CRC |
| Churner | Rapid connect → burst → disconnect cycles |
| Ramper | Delayed start, gradual rate increase |
| Trickler | Byte-by-byte fragmented delivery |

### Memory Check with Valgrind

```bash
valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes --track-fds=yes \
  ./build/counter_drone_server 9000
```

A clean run shows `All heap blocks were freed -- no leaks are possible` and only 3 open file descriptors (stdin/stdout/stderr).

---

## Project Structure

```
.
├── CMakeLists.txt              # Build configuration
├── include/
│   ├── asyncLogger.h           # Async logging thread (stdout + file)
│   ├── businessLogic.h         # Drone state tracking & alerting
│   ├── listener.h              # epoll-based TCP server
│   ├── ringBuffer.h            # Zero-allocation circular buffer
│   ├── streamingParser.h       # 3-state binary parser
│   ├── telemetry.h             # Packet constants & Telemetry struct
│   └── threadSafeQueue.h       # Thread-safe bounded FIFO queue
├── src/
│   ├── asyncLogger.cpp
│   ├── businessLogic.cpp
│   ├── listener.cpp
│   ├── main.cpp                # Entry point — wires 4 jthreads
│   ├── ringBuffer.cpp
│   └── streamingParser.cpp
├── tests/
│   └── drone_load_test.py      # Python asyncio load test with Rich dashboard
└── log/                        # Auto-created log directory
```

---

## C++20 Features Used

| Feature | Usage |
|---------|-------|
| `std::jthread` | All 4 threads — auto-joining, built-in stop tokens |
| `std::stop_token` / `std::stop_callback` | Cooperative cancellation without manual flags |
| `std::condition_variable_any` | Queue blocking with stop_token-aware wait |
| `std::span<const std::byte>` | Zero-copy buffer views in ring buffer API |
| `std::byte` | Type-safe binary data (not `char` or `uint8_t`) |
| `std::optional` | Safe return from `deserialize_payload()` |
| `std::endian` + `if constexpr` | Compile-time endian detection for byte swapping |
| Template lambdas | Generic `append_be` serialiser in tests |
| `std::filesystem` | Auto-create log directory |

---

## Design Decisions

| Decision | Rationale |
|----------|-----------|
| **Single epoll thread** (not thread-per-client) | Scales to thousands of connections without thread overhead |
| **Edge-triggered epoll** (`EPOLLET`) for clients | Reduced syscalls; requires draining reads in a loop |
| **Per-client RingBuffer** | Isolates binary streams — no interleaving across drones |
| **Deferred-consume state machine** | Makes CRC resync trivial: just `consume(1)` |
| **1-byte sliding window resync** | Maximises recovery from corruption; never skips valid data |
| **RAII `SocketHandle`** for all FDs | Prevents "Address already in use" and FD leaks |
| **Dedicated logger thread** | Keeps `std::cout` off hot paths; single-writer pattern eliminates output garbling |
| **`ThreadSafeQueue<string>` for logging** | Same proven queue template, just `T = string`; unifies all inter-thread data flow |
| **`std::system_error` for syscalls** | Structured error codes, thread-safe messages, programmatic catchability |

---

## Improvements for Production Deployment

| Area | Improvement |
|------|-------------|
| **TLS encryption** | Wrap TCP sockets with OpenSSL for encrypted drone-to-server communication |
| **Authentication** | Add a handshake protocol with token-based or certificate-based mutual authentication per drone |
| **Thread pool for parsing** | Shard the `unordered_map<int, ClientState>` across N parser threads for higher throughput |
| **Persistent storage** | Write telemetry to a time-series database (InfluxDB, TimescaleDB) for historical analysis |
| **Config file** | Replace CLI args with XML config for port, thresholds, log paths, queue sizes |
| **Rate limiting** | Per-client byte/packet rate limits to prevent a rogue drone from monopolising the parser |
| **Connection timeout** | Evict idle clients after N seconds of silence to free epoll slots and ring buffer memory |
| **Binary logging** | Log raw malformed packets to a binary file for offline forensic analysis |
| **IPv6 support** | Extend `NetworkListener` to dual-stack `AF_INET6` with `IPV6_V6ONLY=0` |

---

## License

This project was developed as a technical assignment demonstrating production-grade C++20 network programming on Linux.
