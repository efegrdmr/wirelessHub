#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <netinet/in.h>

// ── TcpSocket ────────────────────────────────────────────────────────────────
// Wraps an accepted, connected TCP socket.
class TcpSocket {
public:
    explicit TcpSocket(int fd) : fd_(fd) {}
    ~TcpSocket();

    // Non-copyable, movable
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;
    TcpSocket(TcpSocket&&) noexcept;
    TcpSocket& operator=(TcpSocket&&) noexcept;

    int fd() const { return fd_; }

    // Send exact number of bytes. 
    // Returns true on success, false if connection closed or error.
    bool sendExactly(const void* buf, size_t len);

    // Receive exact number of bytes.
    // Returns the number of bytes read (which will be `len` on success),
    // 0 on clean disconnect, -1 on error (e.g. EAGAIN).
    ssize_t recvExactly(void* buf, size_t len);

private:
    int fd_ = -1;
};

// ── TcpServer ────────────────────────────────────────────────────────────────
// Wraps a listening socket on DAEMON_PORT.
class TcpServer {
public:
    // Binds to 0.0.0.0:bind_port and listens. Throws on error.
    explicit TcpServer(uint16_t bind_port);
    ~TcpServer();

    // Non-copyable, movable
    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;
    TcpServer(TcpServer&&) noexcept;
    TcpServer& operator=(TcpServer&&) noexcept;

    int fd() const { return fd_; }

    // Accept an incoming connection. Returns a valid fd, or -1 on EAGAIN/error.
    // Fills client_addr with the sender details.
    int acceptConnection(sockaddr_in& client_addr);

private:
    int fd_ = -1;
};
