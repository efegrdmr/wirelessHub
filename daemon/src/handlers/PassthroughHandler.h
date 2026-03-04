#pragma once
#include "../network/UdpSocket.h"
#include "../protocol/Protocol.h"

#include <cstdint>
#include <cstddef>
#include <netinet/in.h>
#include <unordered_map>
#include <vector>

// PassthroughHandler bridges one vhci socketpair fd ↔ one UDP device endpoint.
//
// Kernel  <──(spv[0])──> vhci-hcd
//                        vhci-hcd <──(spv[1]=vhci_fd)──> PassthroughHandler <──(UDP)──> Device
//
// Kernel → Device: bytes are wrapped in RAW_DATA (≤MTU_PAYLOAD) or split into
//   multiple RAW_FRAG datagrams (>MTU_PAYLOAD). onVhciReadable() drains the
//   kernel fd completely per call.
// Device → Kernel: RAW_DATA payload is written straight to vhci_fd.
//   RAW_FRAG chunks are reassembled in order before writing to vhci_fd.
class PassthroughHandler {
public:
    static constexpr size_t READ_BUF = 65536;

    PassthroughHandler(int vhci_fd, UdpSocket& sock,
                       const sockaddr_in& dev_addr, uint8_t device_id);
    ~PassthroughHandler() = default;

    PassthroughHandler(const PassthroughHandler&) = delete;
    PassthroughHandler& operator=(const PassthroughHandler&) = delete;

    // Drains all available data from vhci_fd and forwards to device via UDP.
    // Returns false if the fd has been closed (EOF) by the kernel.
    bool onVhciReadable();

    // Called for a complete RAW_DATA payload from the device.
    // Writes bytes directly to vhci_fd.
    bool onDeviceData(const uint8_t* payload, size_t len);

    // Called for each RAW_FRAG chunk from the device.
    // Assembles fragments; writes to vhci_fd only when all chunks have arrived.
    bool onDeviceFragData(uint16_t transfer_seq, uint8_t frag_idx, uint8_t frag_total,
                          const uint8_t* chunk, size_t chunk_len);

    int        vhciFd()    const { return vhci_fd_; }
    uint8_t    deviceId()  const { return device_id_; }
    const sockaddr_in& deviceAddr() const { return dev_addr_; }
    void setDeviceAddr(const sockaddr_in& addr) { dev_addr_ = addr; }

private:
    // Send a single RAW_DATA packet (payload ≤ MTU_PAYLOAD).
    void sendRawDataPacket(const uint8_t* data, size_t len);

    // Split into multiple RAW_FRAG packets and send all.
    void sendFragmented(const uint8_t* data, size_t len);

    // Write to vhci_fd with full retry on EINTR.
    bool writeToKernel(const uint8_t* data, size_t len);

    int          vhci_fd_;
    UdpSocket&   sock_;
    sockaddr_in  dev_addr_;
    uint8_t      device_id_;
    uint8_t      seq_           = 0;  // rolling seq for RAW_DATA / RAW_FRAG headers
    uint16_t     transfer_seq_  = 0;  // rolling id for outgoing fragmented transfers

    uint8_t buf_[READ_BUF];

    // Reassembly table for incoming RAW_FRAG from device.
    struct ReassemblyEntry {
        std::vector<std::vector<uint8_t>> frags;
        uint8_t received = 0;
        uint8_t total    = 0;
    };
    std::unordered_map<uint16_t, ReassemblyEntry> reassembly_;
};
