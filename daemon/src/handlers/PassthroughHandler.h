#pragma once
#include "../network/UdpSocket.h"
#include "../protocol/Protocol.h"

#include <cstdint>
#include <cstddef>
#include <netinet/in.h>

// PassthroughHandler bridges one vhci socketpair fd ↔ one UDP device endpoint.
//
// Kernel  <──(spv[0])──> vhci-hcd
//                        vhci-hcd <──(spv[1]=vhci_fd)──> PassthroughHandler <──(UDP)──> Device
//
// It does zero USB/IP parsing: bytes from the kernel are wrapped in a
// WirelessHub RAW_DATA header and sent to the device, and bytes from the
// device (RAW_DATA payload) are written straight to the kernel fd.
class PassthroughHandler {
public:
    static constexpr size_t READ_BUF = 65536;

    // vhci_fd  : spv[1], the daemon's side of the kernel socketpair
    // sock     : shared UDP socket (daemon listens on this)
    // dev_addr : device endpoint to send USB/IP bytes to
    // device_id: the DeviceId for this slot (Header::device_id)
    PassthroughHandler(int vhci_fd, UdpSocket& sock,
                       const sockaddr_in& dev_addr, uint8_t device_id);

    ~PassthroughHandler() = default;

    // Non-copyable
    PassthroughHandler(const PassthroughHandler&) = delete;
    PassthroughHandler& operator=(const PassthroughHandler&) = delete;

    // Call when epoll signals vhci_fd is readable.
    // Reads all available data and forwards it to the device via UDP.
    // Returns false if the fd has been closed (EOF) by the kernel.
    bool onVhciReadable();

    // Call when a RAW_DATA packet arrives from the device.
    // Writes payload bytes directly to vhci_fd.
    // Returns false if the write fails (broken pipe, etc.).
    bool onDeviceData(const uint8_t* payload, size_t len);

    int       vhciFd()     const { return vhci_fd_; }
    uint8_t   deviceId()   const { return device_id_; }
    const sockaddr_in& deviceAddr() const { return dev_addr_; }

    // Update the device reply address (in case it changes across re-connects).
    void setDeviceAddr(const sockaddr_in& addr) { dev_addr_ = addr; }

private:
    int          vhci_fd_;
    UdpSocket&   sock_;
    sockaddr_in  dev_addr_;
    uint8_t      device_id_;
    uint8_t      seq_ = 0; // rolling sequence counter

    uint8_t buf_[READ_BUF];
};
