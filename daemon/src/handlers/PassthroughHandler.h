#pragma once
#include "../network/TcpSocket.h"
#include "../protocol/Protocol.h"

#include <cstdint>
#include <cstddef>
#include <netinet/in.h>

// PassthroughHandler bridges one vhci socketpair fd ↔ one TCP device endpoint.
//
// Kernel  <──(spv[0])──> vhci-hcd
//                        vhci-hcd <──(spv[1]=vhci_fd)──> PassthroughHandler <──(TCP)──> Device
//
// Kernel → Device: bytes are wrapped in RAW_DATA. onVhciReadable() drains the
//   kernel fd completely per call.
// Device → Kernel: RAW_DATA payload is written straight to vhci_fd.
class PassthroughHandler {
public:
    static constexpr size_t READ_BUF = 65536;

    PassthroughHandler(int vhci_fd, TcpSocket& sock, uint8_t device_id);
    ~PassthroughHandler() = default;

    PassthroughHandler(const PassthroughHandler&) = delete;
    PassthroughHandler& operator=(const PassthroughHandler&) = delete;

    // Drains all available data from vhci_fd and forwards to device via TCP.
    // Returns false if the fd has been closed (EOF) by the kernel.
    bool onVhciReadable();

    // Called for a complete RAW_DATA payload from the device.
    // Writes bytes directly to vhci_fd.
    bool onDeviceData(const uint8_t* payload, size_t len);

    int        vhciFd()    const { return vhci_fd_; }
    uint8_t    deviceId()  const { return device_id_; }

private:
    // Send a single RAW_DATA packet.
    void sendRawDataPacket(const uint8_t* data, size_t len);

    // Write to vhci_fd with full retry on EINTR.
    bool writeToKernel(const uint8_t* data, size_t len);

    int          vhci_fd_;
    TcpSocket&   sock_;
    uint8_t      device_id_;
    uint8_t      seq_           = 0;  // rolling seq for RAW_DATA headers

    uint8_t buf_[READ_BUF];
};
