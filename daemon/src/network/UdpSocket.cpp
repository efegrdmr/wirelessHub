#include "UdpSocket.h"

#include <stdexcept>
#include <string>
#include <cstring>
#include <cerrno>

#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

UdpSocket::UdpSocket(uint16_t bind_port)
{
    fd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd_ < 0)
        throw std::runtime_error(std::string("UdpSocket: socket() failed: ") + strerror(errno));

    // Allow address reuse so daemon restarts don't block for TIME_WAIT
    int opt = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(bind_port);

    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd_);
        throw std::runtime_error(std::string("UdpSocket: bind() on port ")
                                 + std::to_string(bind_port) + " failed: " + strerror(errno));
    }
}

UdpSocket::~UdpSocket()
{
    if (fd_ >= 0)
        ::close(fd_);
}

UdpSocket::UdpSocket(UdpSocket&& o) noexcept : fd_(o.fd_)
{
    o.fd_ = -1;
}

UdpSocket& UdpSocket::operator=(UdpSocket&& o) noexcept
{
    if (this != &o) {
        if (fd_ >= 0) ::close(fd_);
        fd_   = o.fd_;
        o.fd_ = -1;
    }
    return *this;
}

ssize_t UdpSocket::sendTo(const sockaddr_in& dest, const void* buf, size_t len)
{
    return ::sendto(fd_, buf, len, 0,
                    reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
}

ssize_t UdpSocket::recvFrom(void* buf, size_t len, sockaddr_in& sender_out)
{
    socklen_t slen = sizeof(sender_out);
    return ::recvfrom(fd_, buf, len, 0,
                      reinterpret_cast<sockaddr*>(&sender_out), &slen);
}

std::string UdpSocket::addrToStr(const sockaddr_in& addr)
{
    char ip[INET_ADDRSTRLEN] = {};
    ::inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));
}
