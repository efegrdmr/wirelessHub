#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <netinet/in.h>

class UdpSocket {
public:
    // Binds to 0.0.0.0:port. Throws std::runtime_error on failure.
    explicit UdpSocket(uint16_t bind_port);
    ~UdpSocket();

    // Non-copyable, movable
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    UdpSocket(UdpSocket&&) noexcept;
    UdpSocket& operator=(UdpSocket&&) noexcept;

    // Returns the raw fd for use in epoll/select.
    int fd() const { return fd_; }

    // Send buf to destination. Returns bytes sent, or -1 on error.
    ssize_t sendTo(const sockaddr_in& dest, const void* buf, size_t len);

    // Receive into buf (up to len bytes). Fills sender_out with the sender address.
    // Returns bytes received, 0 on timeout/EAGAIN, -1 on error.
    ssize_t recvFrom(void* buf, size_t len, sockaddr_in& sender_out);

    // Convenience: string rep of an addr (for logging).
    static std::string addrToStr(const sockaddr_in& addr);

private:
    int fd_ = -1;
};
