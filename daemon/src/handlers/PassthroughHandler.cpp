#include "PassthroughHandler.h"
#include "../Log.h"

#include <cstdio>
#include <cstring>
#include <cerrno>
#include <unistd.h>

PassthroughHandler::PassthroughHandler(int vhci_fd, TcpSocket& sock, uint8_t device_id)
    : vhci_fd_(vhci_fd), sock_(sock), device_id_(device_id)
{}

// ── Private helpers ───────────────────────────────────────────────────────────

void PassthroughHandler::sendRawDataPacket(const uint8_t* data, size_t len)
{
    // TCP does not need MTU payload limits, but since USB over IP packets
    // can be large (up to 65536 bytes depending on buffer limits), we just
    // send the Header directly, followed by the data.
    Header hdr{};
    hdr.cmd_type    = static_cast<uint8_t>(CmdType::RAW_DATA);
    hdr.device_id   = device_id_;
    hdr.endpoint    = 0x00;
    hdr.seq         = seq_++;
    // Maximum payload_len is uint16_t which is 65535.
    // We assume len <= 65535 based on READ_BUF of 65536. 
    // If it's larger we would need to handle it or drop, but 
    // READ_BUF is 65536 and USB bulk limits are usually well below this.
    // We cap at 65535 to fit in uint16_t payload_len if it happens to be 65536 exactly.
    size_t actual_len = (len > 65535) ? 65535 : len;
    hdr.payload_len = static_cast<uint16_t>(actual_len);

    if (!sock_.sendExactly(&hdr, sizeof(hdr))) {
        LOG_ERR("[passthrough dev=0x%02X] sendExactly(Header) failed: %s",
                device_id_, strerror(errno));
        return;
    }

    if (!sock_.sendExactly(data, actual_len)) {
        LOG_ERR("[passthrough dev=0x%02X] sendExactly(Data %zu B) failed: %s",
                device_id_, actual_len, strerror(errno));
        return;
    }

    LOG_DBG("[passthrough dev=0x%02X] kernel→ESP  RAW_DATA  %zu B  seq=%u",
            device_id_, actual_len, hdr.seq);
}

bool PassthroughHandler::writeToKernel(const uint8_t* data, size_t len)
{
    size_t written = 0;
    while (written < len) {
        ssize_t w = ::write(vhci_fd_, data + written, len - written);
        if (w < 0) {
            if (errno == EINTR) continue;
            LOG_ERR("[passthrough dev=0x%02X] write to kernel failed: %s",
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
    // packed CMD_SUBMIT frames. Each read produces one TCP send.
    while (true) {
        ssize_t n = ::read(vhci_fd_, buf_, sizeof(buf_));

        if (n == 0) {
            LOG_WARN("[passthrough dev=0x%02X] kernel closed vhci_fd (EOF)", device_id_);
            return false;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return true; // fd drained, done for this epoll wakeup
            LOG_ERR("[passthrough dev=0x%02X] read from kernel failed: %s",
                    device_id_, strerror(errno));
            return false;
        }

        size_t bytes = static_cast<size_t>(n);
        LOG_DBG("[passthrough dev=0x%02X] kernel→ESP  read %zu B  RAW_DATA",
                device_id_, bytes);
        
        sendRawDataPacket(buf_, bytes);
    }
}

// ── Device → Kernel (complete packet) ────────────────────────────────────────

bool PassthroughHandler::onDeviceData(const uint8_t* payload, size_t len)
{
    LOG_DBG("[passthrough dev=0x%02X] ESP→kernel  %zu B", device_id_, len);
    return writeToKernel(payload, len);
}
