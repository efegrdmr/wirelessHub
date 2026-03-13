#include "TcpSocket.h"

#include <stdexcept>
#include <string>
#include <cstring>
#include <cerrno>

#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

// ── TcpSocket ────────────────────────────────────────────────────────────────

TcpSocket::~TcpSocket()
{
    if (fd_ >= 0)
        ::close(fd_);
}

TcpSocket::TcpSocket(TcpSocket&& o) noexcept : fd_(o.fd_)
{
    o.fd_ = -1;
}

TcpSocket& TcpSocket::operator=(TcpSocket&& o) noexcept
{
    if (this != &o) {
        if (fd_ >= 0) ::close(fd_);
        fd_   = o.fd_;
        o.fd_ = -1;
    }
    return *this;
}

bool TcpSocket::sendExactly(const void* buf, size_t len)
{
    const uint8_t* data = static_cast<const uint8_t*>(buf);
    size_t written = 0;
    while (written < len) {
        // MSG_NOSIGNAL prevents SIGPIPE if the other end closes
        ssize_t res = ::send(fd_, data + written, len - written, MSG_NOSIGNAL);
        if (res < 0) {
            if (errno == EINTR) continue;
            // EAGAIN is technically possible if the socket buffer is full,
            // but we treat it as an error for now to keep it simple, or we block.
            // Since this socket might be non-blocking in a full implementation,
            // we should technically handle EAGAIN, but PassthroughHandler
            // previously used blocking writes to kernel, so we'll assume blocking send
            // or we just fail.
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Not ideal, but we can sleep and retry if we really hit a full buffer
                usleep(1000);
                continue;
            }
            return false;
        }
        if (res == 0) {
            // Should not happen for send, but handle it
            return false;
        }
        written += static_cast<size_t>(res);
    }
    return true;
}

ssize_t TcpSocket::recvExactly(void* buf, size_t len)
{
    uint8_t* data = static_cast<uint8_t*>(buf);
    size_t bytes_read = 0;
    while (bytes_read < len) {
        ssize_t res = ::recv(fd_, data + bytes_read, len - bytes_read, 0);
        if (res < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (bytes_read == 0) {
                    return -1; // Clean EAGAIN, no data yet
                }
                // Partial read, we must wait for the rest to arrive because we
                // are in the middle of a frame.
                usleep(1000);
                continue;
            }
            return -1; // Actual error
        }
        if (res == 0) {
            // Connection closed
            return 0;
        }
        bytes_read += static_cast<size_t>(res);
    }
    return bytes_read;
}

// ── TcpServer ────────────────────────────────────────────────────────────────

TcpServer::TcpServer(uint16_t bind_port)
{
    fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd_ < 0)
        throw std::runtime_error(std::string("TcpServer: socket() failed: ") + strerror(errno));

    int opt = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(bind_port);

    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd_);
        throw std::runtime_error(std::string("TcpServer: bind() on port ")
                                 + std::to_string(bind_port) + " failed: " + strerror(errno));
    }

    if (::listen(fd_, 128) < 0) {
        ::close(fd_);
        throw std::runtime_error(std::string("TcpServer: listen() failed: ") + strerror(errno));
    }
}

TcpServer::~TcpServer()
{
    if (fd_ >= 0)
        ::close(fd_);
}

TcpServer::TcpServer(TcpServer&& o) noexcept : fd_(o.fd_)
{
    o.fd_ = -1;
}

TcpServer& TcpServer::operator=(TcpServer&& o) noexcept
{
    if (this != &o) {
        if (fd_ >= 0) ::close(fd_);
        fd_   = o.fd_;
        o.fd_ = -1;
    }
    return *this;
}

int TcpServer::acceptConnection(sockaddr_in& client_addr)
{
    socklen_t client_len = sizeof(client_addr);
    // Use accept4 to set NONBLOCK and CLOEXEC automatically
    int client_fd = ::accept4(fd_, reinterpret_cast<sockaddr*>(&client_addr),
                              &client_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
    return client_fd;
}
