#include "HubServer.h"

#include <cstdio>
#include <cstring>
#include <unistd.h>

HubServer::HubServer() : sock_(DAEMON_PORT)
{
    printf("[HubServer] listening on UDP port %u\n", DAEMON_PORT);
}

int HubServer::udpFd() const
{
    return sock_.fd();
}

void HubServer::onUdpReadable()
{
    sockaddr_in sender{};
    ssize_t n = sock_.recvFrom(pkt_buf_, sizeof(pkt_buf_), sender);
    if (n < static_cast<ssize_t>(sizeof(Header))) {
        printf("[HubServer] runt UDP packet (%zd bytes)\n", n);
        return;
    }

    Header hdr;
    memcpy(&hdr, pkt_buf_, sizeof(hdr));
    const uint8_t* payload = pkt_buf_ + sizeof(hdr);
    size_t pay_len = static_cast<size_t>(n) - sizeof(hdr);

    auto cmd = static_cast<CmdType>(hdr.cmd_type);

    // ── DEVICE_EVENT ─────────────────────────────────────────────────────
    if (cmd == CmdType::DEVICE_EVENT) {
        if (pay_len < sizeof(DeviceEventPayload)) return;
        DeviceEventPayload dep;
        memcpy(&dep, payload, sizeof(dep));

        if (static_cast<DeviceEvent>(dep.event) == DeviceEvent::CONNECT) {
            // If already connected, disconnect old session first.
            if (findByDeviceId(dep.device_id) && on_disconnect_)
                on_disconnect_(dep.device_id);

            sockaddr_in dev_addr = makeAddr(nullptr, dep.reply_port);
            dev_addr.sin_addr = sender.sin_addr; // same host, different port

            if (on_connect_)
                on_connect_(dep.device_id, dep.speed, dev_addr);
            return;
        }

        if (static_cast<DeviceEvent>(dep.event) == DeviceEvent::DISCONNECT) {
            if (on_disconnect_)
                on_disconnect_(dep.device_id);
            return;
        }

        return;
    }

    // ── RAW_DATA ─────────────────────────────────────────────────────────
    if (cmd == CmdType::RAW_DATA) {
        auto it = sessions_.find(hdr.device_id);
        if (it == sessions_.end()) {
            printf("[HubServer] RAW_DATA for unknown device_id=0x%02X, dropping\n",
                   hdr.device_id);
            return;
        }
        it->second->handler->onDeviceData(payload, pay_len);
        return;
    }

    printf("[HubServer] unhandled cmd=0x%02X\n", hdr.cmd_type);
}

void HubServer::setConnectCallback(ConnectCb cb)    { on_connect_    = std::move(cb); }
void HubServer::setDisconnectCallback(DisconnectCb cb) { on_disconnect_ = std::move(cb); }

void HubServer::addSession(uint8_t device_id, int vhci_port,
                            int vhci_fd, int kernel_fd,
                            const sockaddr_in& dev_addr)
{
    auto s       = std::make_unique<DeviceSession>();
    s->device_id = device_id;
    s->vhci_port = vhci_port;
    s->vhci_fd   = vhci_fd;
    s->kernel_fd = kernel_fd;
    s->dev_addr  = dev_addr;
    s->handler   = std::make_unique<PassthroughHandler>(vhci_fd, sock_, dev_addr, device_id);

    fd_map_[vhci_fd]      = s.get();
    sessions_[device_id]  = std::move(s);
}

void HubServer::removeSession(uint8_t device_id)
{
    auto it = sessions_.find(device_id);
    if (it == sessions_.end()) return;

    DeviceSession& s = *it->second;
    printf("[HubServer] removing session device_id=0x%02X port=%d\n",
           s.device_id, s.vhci_port);

    fd_map_.erase(s.vhci_fd);
    // Zero-out fds before unique_ptr destruction to avoid double-close;
    // ::close is already called by the RAII destructor of DeviceSession.
    // Nothing to do — destructor handles close.
    // (fds stay valid until the unique_ptr is destroyed below)
    sessions_.erase(it); // destructor closes vhci_fd + kernel_fd
}

DeviceSession* HubServer::findByDeviceId(uint8_t device_id)
{
    auto it = sessions_.find(device_id);
    return (it != sessions_.end()) ? it->second.get() : nullptr;
}

DeviceSession* HubServer::findByVhciFd(int vhci_fd)
{
    auto it = fd_map_.find(vhci_fd);
    return (it != fd_map_.end()) ? it->second : nullptr;
}
