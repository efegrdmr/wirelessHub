#include "PassthroughHandler.h"

#include <cstdio>
#include <cstring>
#include <cerrno>
#include <unistd.h>

PassthroughHandler::PassthroughHandler(int vhci_fd, UdpSocket& sock,
                                       const sockaddr_in& dev_addr,
                                       uint8_t device_id)
    : vhci_fd_(vhci_fd), sock_(sock), dev_addr_(dev_addr), device_id_(device_id)
{}

// ── Kernel → Device ──────────────────────────────────────────────────────────
bool PassthroughHandler::onVhciReadable()
{
    ssize_t n = ::read(vhci_fd_, buf_, sizeof(buf_));

    if (n == 0) {
        printf("[passthrough dev=0x%02X] kernel closed vhci_fd (EOF)\n", device_id_);
        return false;
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return true; // spurious wakeup, not an error
        printf("[passthrough dev=0x%02X] read error: %s\n", device_id_, strerror(errno));
        return false;
    }

    // Build WirelessHub header
    Header hdr{};
    hdr.cmd_type    = static_cast<uint8_t>(CmdType::RAW_DATA);
    hdr.device_id   = device_id_;
    hdr.endpoint    = 0x00; // control endpoint (USB/IP mixes all eps in this stream)
    hdr.seq         = seq_++;
    hdr.payload_len = static_cast<uint16_t>(n);

    // Pack header + payload into one buffer (UDP datagrams are atomic)
    // (UDP datagrams are already atomic; two sendto calls would be separate packets)
    uint8_t pkt[sizeof(Header) + READ_BUF];
    memcpy(pkt,               &hdr, sizeof(hdr));
    memcpy(pkt + sizeof(hdr), buf_, static_cast<size_t>(n));

    ssize_t sent = sock_.sendTo(dev_addr_, pkt, sizeof(hdr) + static_cast<size_t>(n));
    if (sent < 0) {
        printf("[passthrough dev=0x%02X] sendTo error: %s\n", device_id_, strerror(errno));
        // Not fatal — device might be temporarily unreachable; keep session alive
    }
    return true;
}

// ── Device → Kernel ──────────────────────────────────────────────────────────
bool PassthroughHandler::onDeviceData(const uint8_t* payload, size_t len)
{
    size_t written = 0;
    while (written < len) {
        ssize_t w = ::write(vhci_fd_, payload + written, len - written);
        if (w < 0) {
            if (errno == EINTR) continue;
            printf("[passthrough dev=0x%02X] write to kernel failed: %s\n",
                   device_id_, strerror(errno));
            return false;
        }
        written += static_cast<size_t>(w);
    }
    return true;
}
