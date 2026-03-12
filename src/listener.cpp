/**
 * @file listener.cpp
 * @brief Implementation of SocketHandle (RAII FD wrapper) and
 *        NetworkListener (epoll-based TCP server).
 */

#include "listener.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sstream>
#include <system_error>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

namespace counter_drone {

/* ================================================================== */
/*  SocketHandle RAII implementation                                  */
/* ================================================================== */

/// @details Closes the owned file descriptor if it is valid (>= 0).
SocketHandle::~SocketHandle() {
    if (m_fd >= 0) {
        ::close(m_fd);
    }
}

/// @details Steals the fd from @p other, leaving it invalidated (-1).
SocketHandle::SocketHandle(SocketHandle&& other) noexcept : m_fd(other.m_fd) {
    other.m_fd = -1;
}

/// @details Closes the current fd (if valid), then steals from @p other.
SocketHandle& SocketHandle::operator=(SocketHandle&& other) noexcept {
    if (this != &other) {
        reset(other.m_fd);
        other.m_fd = -1;
    }
    return *this;
}

/// @details Returns the raw fd and relinquishes ownership (sets internal to -1).
int SocketHandle::release() noexcept {
    int fd = m_fd;
    m_fd = -1;
    return fd;
}

/// @details Closes the currently owned fd (if valid) and takes ownership of @p fd.
void SocketHandle::reset(int fd) noexcept {
    if (m_fd >= 0) {
        ::close(m_fd);
    }
    m_fd = fd;
}

/* ================================================================== */
/*  NetworkListener                                                   */
/* ================================================================== */

/** @brief Maximum events returned per epoll_wait call. */
static constexpr int    MAX_EPOLL_EVENTS = 64;

/** @brief Size of the pre-allocated recv buffer (per-read chunk). */
static constexpr size_t RECV_BUF_SIZE    = 4096;


/// @details Stores the port, queue and log queue references; pre-allocates the recv buffer.
NetworkListener::NetworkListener(uint16_t port, ThreadSafeQueue<DataChunk>& queue,
                                 ThreadSafeQueue<std::string>& log_queue)
    : m_port(port), m_queue(queue), m_log_queue(log_queue), m_recv_buf(RECV_BUF_SIZE) {}


/**
 * @brief Set a file descriptor to non-blocking mode via fcntl.
 * @param fd The file descriptor to modify.
 * @throws std::system_error If fcntl fails.
 */
void NetworkListener::set_nonblocking(int fd) {
    // Read the current flags on the FD.
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        throw std::system_error(errno, std::system_category(), "fcntl F_GETFL failed");
    }
    // Merge in O_NONBLOCK so reads/writes return immediately when not ready.
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        throw std::system_error(errno, std::system_category(), "fcntl F_SETFL O_NONBLOCK failed");
    }
}

/**
 * @brief Create, bind, and listen on the TCP server socket.
 *
 * Steps: create socket → enable SO_REUSEADDR → set non-blocking →
 * bind to the configured port → start listening.
 *
 * @throws std::system_error If any syscall fails.
 */
void NetworkListener::setup_server_socket() {
    // Create a TCP (SOCK_STREAM) IPv4 socket.
    int server_socket = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        throw std::system_error(errno, std::system_category(), "Failed to create socket");
    }

    // Transfer ownership to the RAII handle immediately.
    m_server_fd.reset(server_socket);

    int opt = 1; // Enable SO_REUSEADDR to allow quick port reuse after restart (avoids "Address already in use")
    if (::setsockopt(m_server_fd.get(), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        throw std::system_error(errno, std::system_category(), "setsockopt SO_REUSEADDR failed");
    }

    // Non-blocking is required for epoll edge-triggered mode.
    set_nonblocking(m_server_fd.get());

    // Bind to all interfaces (INADDR_ANY) on the configured port.
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(m_port);

    if (::bind(m_server_fd.get(),
               reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        throw std::system_error(errno, std::system_category(),
                                "Failed to bind on port " + std::to_string(m_port));
    }

    // SOMAXCONN lets the kernel pick the maximum backlog for pending connections.
    if (::listen(m_server_fd.get(), SOMAXCONN) < 0) {
        throw std::system_error(errno, std::system_category(), "Failed to listen");
    }

    {
        std::ostringstream log_message;
        log_message << "[NetworkListener] Listening on port " << m_port << "\n";
        m_log_queue.push(std::move(log_message).str());
    }
}

/**
 * @brief Initialise the epoll instance and eventfd shutdown mechanism.
 *
 * Registers two FDs with epoll:
 *   - The server socket (to detect incoming connections).
 *   - The eventfd (to wake the loop on graceful shutdown).
 *
 * @throws std::system_error If epoll_create1, eventfd, or epoll_ctl fails.
 */
void NetworkListener::setup_epoll() {
    // Create the epoll instance that multiplexes all FDs.
    int epoll_instance = ::epoll_create1(0);
    if (epoll_instance < 0) {
        throw std::system_error(errno, std::system_category(), "epoll_create1 failed");
    }

    m_epoll_fd.reset(epoll_instance);

    // Create a non-blocking eventfd used to wake epoll_wait on shutdown.
    int shutdown_fd = ::eventfd(0, EFD_NONBLOCK);
    if (shutdown_fd < 0) {
        throw std::system_error(errno, std::system_category(), "eventfd failed");
    }

    m_event_fd.reset(shutdown_fd);

    // Watch the server socket for incoming connections (level-triggered).
    epoll_event registration{};
    registration.events  = EPOLLIN;
    registration.data.fd = m_server_fd.get();
    if (::epoll_ctl(m_epoll_fd.get(), EPOLL_CTL_ADD, m_server_fd.get(), &registration) < 0) {
        throw std::system_error(errno, std::system_category(), "epoll_ctl add server_fd failed");
    }

    // Watch the eventfd for shutdown signals (level-triggered).
    registration.events  = EPOLLIN;
    registration.data.fd = m_event_fd.get();
    if (::epoll_ctl(m_epoll_fd.get(), EPOLL_CTL_ADD, m_event_fd.get(), &registration) < 0) {
        throw std::system_error(errno, std::system_category(), "epoll_ctl add event_fd failed");
    }
}

/**
 * @brief Accept all pending connections on the server socket.
 *
 * Loops until EAGAIN/EWOULDBLOCK (no more pending clients).
 * Each accepted client is set to non-blocking, registered with
 * epoll in edge-triggered mode (EPOLLET), and logged.
 */
void NetworkListener::handle_accept() {
    // Loop to accept ALL pending connections (non-blocking socket may have several queued).
    while (true) {
        sockaddr_in client_addr{};
        socklen_t   client_len = sizeof(client_addr);
        int raw_fd = ::accept(m_server_fd.get(),
                              reinterpret_cast<sockaddr*>(&client_addr),
                              &client_len);
        if (raw_fd < 0) {
            // EAGAIN means no more pending connections — normal exit.
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            {
                std::ostringstream log_message;
                log_message << "[NetworkListener] accept() error: "
                            << std::strerror(errno) << "\n";
                m_log_queue.push(std::move(log_message).str());
            }
            break;
        }

        // Immediately wrap in RAII — if anything below throws, the socket is still closed.
        SocketHandle client_handle(raw_fd);

        // Every client must be non-blocking for edge-triggered epoll.
        try {
            set_nonblocking(client_handle.get());
        } catch (const std::system_error& e) {
            {
                std::ostringstream log_message;
                log_message << "[NetworkListener] set_nonblocking failed for client fd="
                            << client_handle.get() << ": " << e.what() << "\n";
                m_log_queue.push(std::move(log_message).str());
            }
            // SocketHandle destructor closes the fd automatically.
            continue;
        }

        // Register the new client with epoll (edge-triggered for high throughput).
        epoll_event registration{};
        registration.events  = EPOLLIN | EPOLLET;
        registration.data.fd = client_handle.get();
        if (::epoll_ctl(m_epoll_fd.get(), EPOLL_CTL_ADD, client_handle.get(), &registration) < 0) {
            {
                std::ostringstream log_message;
                log_message << "[NetworkListener] epoll_ctl add client failed: "
                            << std::strerror(errno) << "\n";
                m_log_queue.push(std::move(log_message).str());
            }
            // SocketHandle destructor closes the fd automatically.
            continue;
        }

        // Log the new connection with its IP address.
        char addr_str[INET_ADDRSTRLEN]{};
        ::inet_ntop(AF_INET, &client_addr.sin_addr, addr_str, sizeof(addr_str));
        {
            std::ostringstream log_message;
            log_message << "[NetworkListener] Client connected (fd=" << client_handle.get()
                        << ", addr=" << addr_str << ")\n";
            m_log_queue.push(std::move(log_message).str());
        }

        // Transfer ownership into the active-clients map (O(1) lookup by FD).
        int fd_key = client_handle.get();
        m_clients.emplace(fd_key, std::move(client_handle));
    }
}

/**
 * @brief Drain all available data from a client socket.
 *
 * Reads in a loop until EAGAIN (edge-triggered epoll requires
 * draining). Each chunk is wrapped in a DataChunk and pushed to
 * the raw-data queue for the parser.
 *
 * @param client_fd The client's socket file descriptor.
 * @return @c true if the socket is still alive; @c false on disconnect/error.
 */
bool NetworkListener::handle_client_data(int client_fd) {
    // Edge-triggered epoll requires us to drain ALL available data in one go.
    while (true) {
        ssize_t bytes_received = ::recv(client_fd, m_recv_buf.data(), m_recv_buf.size(), 0);

        if (bytes_received > 0) {
            // Copy the received bytes into a DataChunk tagged with this client's FD.
            std::vector<std::byte> chunk_data(m_recv_buf.begin(),
                                              m_recv_buf.begin() + bytes_received);
            m_queue.push(DataChunk{client_fd, std::move(chunk_data)});
            continue;
        }

        if (bytes_received == 0) {
            // Zero bytes = the client closed its end of the connection.
            return false;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // No more data available right now — socket still alive.
            return true;
        }

        // Unexpected recv error — treat as disconnect.
        {
            std::ostringstream log_message;
            log_message << "[NetworkListener] recv() error on fd " << client_fd
                        << ": " << std::strerror(errno) << "\n";
            m_log_queue.push(std::move(log_message).str());
        }
        return false;
    }
}

/**
 * @brief Clean up a disconnected client.
 *
 * Removes the FD from epoll and erases it from the active-clients map.
 * The SocketHandle destructor automatically closes the underlying socket.
 * Finally, pushes an empty DataChunk to notify the parser.
 *
 * @param client_fd The client's socket file descriptor.
 */
void NetworkListener::disconnect_client(int client_fd) {
    // Remove from epoll interest list.
    ::epoll_ctl(m_epoll_fd.get(), EPOLL_CTL_DEL, client_fd, nullptr);

    // Erase from the map — SocketHandle destructor calls ::close(fd).
    m_clients.erase(client_fd);

    // Empty DataChunk signals the Parser that this client is gone.
    m_queue.push(DataChunk{client_fd, {}});

    {
        std::ostringstream log_message;
        log_message << "[NetworkListener] Client disconnected (fd=" << client_fd << ")\n";
        m_log_queue.push(std::move(log_message).str());
    }
}

/**
 * @brief Main epoll event loop — blocking, runs until shutdown.
 *
 * Workflow:
 *   1. Set up the server socket and epoll instance.
 *   2. Register a stop_callback that writes to eventfd (wakes epoll_wait).
 *   3. Loop: dispatch events for accept / client-data / disconnect / shutdown.
 *   4. On shutdown signal → return (RAII cleans up all FDs).
 *
 * @param stop_token Cooperative cancellation token from std::jthread.
 */
void NetworkListener::run(std::stop_token stop_token) {
    setup_server_socket();
    setup_epoll();

    // Register a stop callback that writes to eventfd, waking epoll_wait.
    std::stop_callback shutdown_waker(stop_token, [this]() {
        uint64_t wake_signal = 1;
        ::write(m_event_fd.get(), &wake_signal, sizeof(wake_signal));
    });

    std::vector<epoll_event> events(MAX_EPOLL_EVENTS);

    // Block in epoll_wait until network activity or shutdown signal.
    while (!stop_token.stop_requested()) {
        int ready_count = ::epoll_wait(m_epoll_fd.get(), events.data(),
                                       static_cast<int>(events.size()), -1);

        if (ready_count < 0) {
            if (errno == EINTR) continue; // Interrupted by signal — safe to retry.
            {
                std::ostringstream log_message;
                log_message << "[NetworkListener] epoll_wait error: "
                            << std::strerror(errno) << "\n";
                m_log_queue.push(std::move(log_message).str());
            }
            break;
        }

        // Dispatch each ready event.
        for (int i = 0; i < ready_count; ++i) {
            int source_fd     = events[static_cast<std::size_t>(i)].data.fd;
            uint32_t ev_flags = events[static_cast<std::size_t>(i)].events;

            // Shutdown signal via eventfd — drain and exit.
            if (source_fd == m_event_fd.get()) {
                uint64_t drain{};
                ::read(m_event_fd.get(), &drain, sizeof(drain));
                m_log_queue.push("[NetworkListener] Shutdown event received. Shutting down...\n");
                // m_clients map destruction closes all active client sockets via RAII.
                return;
            }

            // New incoming connection on the server socket.
            if (source_fd == m_server_fd.get()) {
                handle_accept();
                continue;
            }

            // Error or hangup on a client socket.
            if (ev_flags & (EPOLLERR | EPOLLHUP)) {
                disconnect_client(source_fd);
                continue;
            }

            // Data available on a client socket — drain it.
            if (ev_flags & EPOLLIN) {
                if (!handle_client_data(source_fd)) {
                    disconnect_client(source_fd);
                }
            }
        }
    }
}

} // namespace counter_drone
