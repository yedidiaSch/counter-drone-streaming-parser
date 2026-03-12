/**
 * @file listener.h
 * @brief RAII socket handle and epoll-based TCP listener for the counter-drone system.
 */

#pragma once

#include "telemetry.h"
#include "threadSafeQueue.h"
#include <string>
#include <system_error>
#include <stop_token>
#include <unordered_map>
#include <vector>

namespace counter_drone {

/**
 * @class SocketHandle
 * @brief RAII wrapper for POSIX file descriptors.
 *
 * Ensures automatic cleanup of sockets, epoll instances, and eventfds.
 * Non-copyable, move-only.
 */
class SocketHandle {
public:
    /** @brief Default constructor — creates an invalid handle. */
    SocketHandle() = default;

    /**
     * @brief Construct from an existing file descriptor.
     * @param fd The POSIX file descriptor to take ownership of.
     */
    explicit SocketHandle(int fd) noexcept : m_fd(fd) {}

    /** @brief Destructor — closes the owned fd if valid. */
    ~SocketHandle();

    SocketHandle(const SocketHandle&)            = delete;
    SocketHandle& operator=(const SocketHandle&) = delete;

    /** @brief Move constructor — transfers ownership from @p other. */
    SocketHandle(SocketHandle&& other) noexcept;

    /** @brief Move assignment — transfers ownership from @p other. */
    SocketHandle& operator=(SocketHandle&& other) noexcept;

    /**
     * @brief Release ownership and return the raw fd without closing it.
     * @return The previously owned file descriptor.
     */
    int release() noexcept;

    /**
     * @brief Get the underlying file descriptor.
     * @return The owned fd, or -1 if invalid.
     */
    [[nodiscard]] int get() const noexcept { return m_fd; }

    /**
     * @brief Check if the handle owns a valid fd.
     * @return @c true if fd >= 0.
     */
    [[nodiscard]] bool valid() const noexcept { return m_fd >= 0; }

    /** @brief Boolean conversion — returns valid(). */
    explicit operator bool() const noexcept { return valid(); }

    /**
     * @brief Close the currently owned fd (if any) and take ownership of @p fd.
     * @param fd New file descriptor to own (default -1 = none).
     */
    void reset(int fd = -1) noexcept;

private:
    int m_fd = -1; /**< The owned POSIX file descriptor. */
};

/**
 * @class NetworkListener
 * @brief High-performance, single-threaded TCP server using Linux epoll.
 *
 * Accepts multiple simultaneous drone connections with non-blocking I/O.
 * Uses an @c eventfd for instant, deadlock-free graceful shutdown
 * integrated with @c std::stop_token.
 */
class NetworkListener {
public:
    /**
     * @brief Construct the listener.
     * @param port      TCP port to bind to.
     * @param queue     Shared queue where tagged DataChunk objects are pushed.
     * @param log_queue Shared queue for asynchronous log message output.
     */
    explicit NetworkListener(uint16_t port, ThreadSafeQueue<DataChunk>& queue,
                             ThreadSafeQueue<std::string>& log_queue);

    /**
     * @brief Main event loop — call from a @c std::jthread.
     *
     * Blocks in @c epoll_wait until network activity or a shutdown signal
     * arrives via the @c eventfd. Honours the stop token for graceful exit.
     *
     * @param stop_token Token checked/used for cooperative cancellation.
     */
    void run(std::stop_token stop_token);

private:
    uint16_t                    m_port;      /**< TCP port to listen on. */
    ThreadSafeQueue<DataChunk>& m_queue;     /**< Shared queue for tagged DataChunks. */
    ThreadSafeQueue<std::string>& m_log_queue; /**< Shared queue for async log output. */

    SocketHandle      m_server_fd;    /**< Listening TCP socket. */
    SocketHandle      m_epoll_fd;     /**< epoll instance. */
    SocketHandle      m_event_fd;     /**< eventfd for shutdown signalling. */

    /**
     * @brief Active client connections, keyed by raw FD.
     *
     * Each accepted drone socket is immediately wrapped in a SocketHandle.
     * Erasing an entry closes the socket via RAII. On shutdown, the map's
     * destruction implicitly closes all remaining client connections.
     */
    std::unordered_map<int, SocketHandle> m_clients;

    std::vector<std::byte> m_recv_buf; /**< Pre-allocated receive buffer (avoids heap alloc in hot loop). */

    /**
     * @brief Create, bind, and listen on the server socket (non-blocking).
     * @throws std::system_error on socket/bind/listen failure.
     */
    void setup_server_socket();

    /**
     * @brief Create the epoll instance and eventfd; register server + event fds.
     * @throws std::system_error on epoll_create1/eventfd/epoll_ctl failure.
     */
    void setup_epoll();

    /**
     * @brief Set a file descriptor to non-blocking mode via @c fcntl.
     * @param fd The file descriptor to modify.
     * @throws std::system_error on fcntl failure.
     */
    static void set_nonblocking(int fd);

    /**
     * @brief Accept all pending connections (non-blocking accept loop).
     *
     * Drains the backlog until @c accept returns @c EAGAIN, registering
     * each new client with the epoll instance in edge-triggered mode.
     */
    void handle_accept();

    /**
     * @brief Read all available data from a client socket (drain until EAGAIN).
     * @param client_fd File descriptor of the connected client.
     * @return @c true if the client is still alive; @c false on disconnect or error.
     */
    bool handle_client_data(int client_fd);

    /**
     * @brief Remove a client from epoll, close its socket via RAII, and notify the parser.
     *
     * Erasing from @c m_clients triggers the SocketHandle destructor,
     * which calls @c ::close(fd). No manual close needed.
     *
     * @param client_fd File descriptor of the client to disconnect.
     */
    void disconnect_client(int client_fd);
};

} // namespace counter_drone
