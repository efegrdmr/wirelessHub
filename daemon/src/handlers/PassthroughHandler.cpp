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

// ── Private helpers ───────────────────────────────────────────────────────────

void PassthroughHandler::sendRawDataPacket(const uint8_t* data, size_t len)
{
    // Stack-allocate: len is guaranteed ≤ MTU_PAYLOAD ≤ 1400
    uint8_t pkt[sizeof(Header) + MTU_PAYLOAD];

    Header hdr{};
    hdr.cmd_type    = static_cast<uint8_t>(CmdType::RAW_DATA);
    hdr.device_id   = device_id_;
    hdr.endpoint    = 0x00;
    hdr.seq         = seq_++;
    hdr.payload_len = static_cast<uint16_t>(len);

    memcpy(pkt,               &hdr, sizeof(hdr));
    memcpy(pkt + sizeof(hdr), data, len);

    ssize_t sent = sock_.sendTo(dev_addr_, pkt, sizeof(hdr) + len);
    if (sent < 0)
        printf("[passthrough dev=0x%02X] sendTo(RAW_DATA) error: %s\n",
               device_id_, strerror(errno));
}

void PassthroughHandler::sendFragmented(const uint8_t* data, size_t len)
{
    // How many fragments do we need?
    const size_t frags_needed = (len + MTU_PAYLOAD - 1) / MTU_PAYLOAD;
    if (frags_needed > 255) {
        // Extremely unlikely (would need >355KB in one read), but guard it.
        printf("[passthrough dev=0x%02X] transfer too large to fragment (%zu B), dropping\n",
               device_id_, len);
        return;
    }
    const uint8_t total    = static_cast<uint8_t>(frags_needed);
    const uint16_t tseq    = transfer_seq_++;

    // Each RAW_FRAG datagram: Header(6) + FragHeader(4) + chunk(≤MTU_PAYLOAD)
    uint8_t pkt[sizeof(Header) + sizeof(FragHeader) + MTU_PAYLOAD];

    for (uint8_t idx = 0; idx < total; ++idx) {
        size_t offset     = static_cast<size_t>(idx) * MTU_PAYLOAD;
        size_t chunk_len  = (offset + MTU_PAYLOAD <= len) ? MTU_PAYLOAD : (len - offset);

        Header hdr{};
        hdr.cmd_type    = static_cast<uint8_t>(CmdType::RAW_FRAG);
        hdr.device_id   = device_id_;
        hdr.endpoint    = 0x00;
        hdr.seq         = seq_++;
        hdr.payload_len = static_cast<uint16_t>(sizeof(FragHeader) + chunk_len);

        FragHeader fhdr{};
        fhdr.transfer_seq = tseq;
        fhdr.frag_idx     = idx;
        fhdr.frag_total   = total;

        size_t off = 0;
        memcpy(pkt + off, &hdr,  sizeof(hdr));  off += sizeof(hdr);
        memcpy(pkt + off, &fhdr, sizeof(fhdr)); off += sizeof(fhdr);
        memcpy(pkt + off, data + offset, chunk_len);

        ssize_t sent = sock_.sendTo(dev_addr_, pkt, off + chunk_len);
        if (sent < 0)
            printf("[passthrough dev=0x%02X] sendTo(RAW_FRAG %u/%u) error: %s\n",
                   device_id_, idx + 1, total, strerror(errno));
    }
}

bool PassthroughHandler::writeToKernel(const uint8_t* data, size_t len)
{
    size_t written = 0;
    while (written < len) {
        ssize_t w = ::write(vhci_fd_, data + written, len - written);
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

// ── Kernel → Device ──────────────────────────────────────────────────────────

bool PassthroughHandler::onVhciReadable()
{
    // Drain the kernel fd completely — one epoll event may cover multiple
    // packed CMD_SUBMIT frames. Each read produces one UDP send.
    while (true) {
        ssize_t n = ::read(vhci_fd_, buf_, sizeof(buf_));

        if (n == 0) {
            printf("[passthrough dev=0x%02X] kernel closed vhci_fd (EOF)\n", device_id_);
            return false;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return true; // fd drained, done for this epoll wakeup
            printf("[passthrough dev=0x%02X] read error: %s\n", device_id_, strerror(errno));
            return false;
        }

        size_t bytes = static_cast<size_t>(n);
        if (bytes <= MTU_PAYLOAD)
            sendRawDataPacket(buf_, bytes);
        else
            sendFragmented(buf_, bytes);
    }
}

// ── Device → Kernel (complete packet) ────────────────────────────────────────

bool PassthroughHandler::onDeviceData(const uint8_t* payload, size_t len)
{
    return writeToKernel(payload, len);
}

// ── Device → Kernel (fragmented) ─────────────────────────────────────────────

bool PassthroughHandler::onDeviceFragData(uint16_t transfer_seq, uint8_t frag_idx,
                                          uint8_t frag_total,
                                          const uint8_t* chunk, size_t chunk_len)
{
    if (frag_total == 0) return true; // malformed, ignore

    auto& entry = reassembly_[transfer_seq];

    // First fragment for this tseq — initialise.
    if (entry.total == 0) {
        entry.total = frag_total;
        entry.frags.resize(frag_total);
    } else if (entry.total != frag_total) {
        // Mismatch — stale entry from a previous interrupted transfer; reset.
        printf("[passthrough dev=0x%02X] tseq=%u frag_total mismatch, resetting\n",
               device_id_, transfer_seq);
        entry = {};
        entry.total = frag_total;
        entry.frags.resize(frag_total);
    }

    if (frag_idx >= frag_total) {
        printf("[passthrough dev=0x%02X] tseq=%u frag_idx=%u out of range, dropping\n",
               device_id_, transfer_seq, frag_idx);
        return true;
    }

    // Store chunk (idempotent — duplicate datagrams are harmless).
    if (entry.frags[frag_idx].empty()) {
        entry.frags[frag_idx].assign(chunk, chunk + chunk_len);
        ++entry.received;
    }

    if (entry.received < entry.total)
        return true; // still waiting for more fragments

    // All fragments arrived — assemble and write to kernel.
    size_t total_len = 0;
    for (auto& f : entry.frags) total_len += f.size();

    std::vector<uint8_t> assembled;
    assembled.reserve(total_len);
    for (auto& f : entry.frags)
        assembled.insert(assembled.end(), f.begin(), f.end());

    reassembly_.erase(transfer_seq);
    return writeToKernel(assembled.data(), assembled.size());
}
