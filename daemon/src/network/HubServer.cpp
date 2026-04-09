#include "HubServer.h"
#include "../Log.h"

#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <errno.h>

HubServer::HubServer() : udp_sock_(DAEMON_PORT), tcp_server_(DAEMON_PORT)
{
    LOG_INFO("[HubServer] listening on UDP and TCP port %u", DAEMON_PORT);
}

int HubServer::udpFd() const
{
    return udp_sock_.fd();
}

int HubServer::tcpServerFd() const
{
    return tcp_server_.fd();
}

void HubServer::onUdpReadable()
{
    sockaddr_in sender{};
    ssize_t n = udp_sock_.recvFrom(pkt_buf_, sizeof(pkt_buf_), sender);
    if (n < static_cast<ssize_t>(sizeof(Header))) {
        LOG_WARN("[HubServer] runt UDP packet (%zd bytes) from %s — dropped",
                 n, UdpSocket::addrToStr(sender).c_str());
        return;
    }

    Header hdr;
    memcpy(&hdr, pkt_buf_, sizeof(hdr));
    auto cmd = static_cast<CmdType>(hdr.cmd_type);

    // ── DISCOVER ───────────────────────────────────────────────────
    if (cmd == CmdType::DISCOVER) {
        DiscoverReplyPayload resp = { static_cast<uint16_t>(DAEMON_PORT) };
        Header respHeader{
            .cmd_type = static_cast<uint8_t>(CmdType::DISCOVER_REPLY),
            .device_id = 0,
            .endpoint = 0,
            .seq = 0,
            .payload_len = sizeof(resp)
        };

        uint8_t buf[sizeof(respHeader) + sizeof(resp)];
        memcpy(buf, &respHeader, sizeof(respHeader));
        memcpy(buf + sizeof(respHeader), &resp, sizeof(resp));

        udp_sock_.sendTo(sender, buf, sizeof(buf));
        LOG_INFO("[HubServer] DISCOVER from %s — replied via UDP",
                 UdpSocket::addrToStr(sender).c_str());
        return;
    }

    LOG_WARN("[HubServer] unhandled UDP cmd=0x%02X from %s (UDP is now only for DISCOVER)",
             hdr.cmd_type, UdpSocket::addrToStr(sender).c_str());
}

void HubServer::onTcpAccept()
{
    sockaddr_in client_addr{};
    int client_fd = tcp_server_.acceptConnection(client_addr);
    if (client_fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            LOG_ERR("[HubServer] acceptConnection failed: %s", strerror(errno));
        }
        return;
    }

    LOG_INFO("[HubServer] accepted new TCP connection from %s (fd=%d)",
             UdpSocket::addrToStr(client_addr).c_str(), client_fd);

    pending_tcp_socks_[client_fd] = std::make_unique<TcpSocket>(client_fd);
}

bool HubServer::readTcpPacket(int fd, Header& hdr, std::vector<uint8_t>& payload)
{
    TcpSocket* sock = nullptr;

    // Check where the socket lives
    auto it_pending = pending_tcp_socks_.find(fd);
    if (it_pending != pending_tcp_socks_.end()) {
        sock = it_pending->second.get();
    } else {
        auto it_session = tcp_fd_map_.find(fd);
        if (it_session != tcp_fd_map_.end()) {
            sock = it_session->second->tcp_sock.get();
        }
    }

    if (!sock) return false;

    // Read header exactly
    ssize_t res = sock->recvExactly(&hdr, sizeof(Header));
    if (res <= 0) {
        if (res == 0) {
            LOG_INFO("[HubServer] TCP connection closed by peer (fd=%d)", fd);
        } else {
            LOG_ERR("[HubServer] TCP recvExactly(Header) failed or EAGAIN (fd=%d)", fd);
        }
        return false; // Connection closed or error
    }

    // Allocate payload buffer
    uint16_t plen = hdr.payload_len;
    payload.resize(plen);

    if (plen > 0) {
        res = sock->recvExactly(payload.data(), plen);
        if (res <= 0) {
            if (res == 0) {
                LOG_INFO("[HubServer] TCP connection closed while reading payload (fd=%d)", fd);
            } else {
                LOG_ERR("[HubServer] TCP recvExactly(Payload) failed or EAGAIN (fd=%d)", fd);
            }
            return false;
        }
    }

    return true;
}

void HubServer::onTcpReadable(int client_fd)
{
    Header hdr;
    std::vector<uint8_t> payload;
    
    if (!readTcpPacket(client_fd, hdr, payload)) {
        // Disconnect handling
        auto it_pending = pending_tcp_socks_.find(client_fd);
        if (it_pending != pending_tcp_socks_.end()) {
            pending_tcp_socks_.erase(it_pending);
            return;
        }
        
        auto it_session = tcp_fd_map_.find(client_fd);
        if (it_session != tcp_fd_map_.end()) {
            uint8_t device_id = it_session->second->device_id;
            LOG_INFO("[HubServer] TCP socket read error/EOF, triggering disconnect for dev_id=0x%02X", device_id);
            if (on_disconnect_) on_disconnect_(device_id);
            return;
        }
        
        return;
    }

    auto cmd = static_cast<CmdType>(hdr.cmd_type);

    // ── DEVICE_EVENT (CONNECT / DISCONNECT) ──────────────────────────────
    if (cmd == CmdType::DEVICE_EVENT) {
        if (payload.size() < sizeof(DeviceEventPayload)) return;
        DeviceEventPayload dep;
        memcpy(&dep, payload.data(), sizeof(dep));

        if (static_cast<DeviceEvent>(dep.event) == DeviceEvent::CONNECT) {
            const char* spd = (dep.speed == USB_SPEED_LOW)  ? "low"  :
                              (dep.speed == USB_SPEED_FULL) ? "full" :
                              (dep.speed == USB_SPEED_HIGH) ? "high" : "?";

            LOG_INFO("[HubServer] CONNECT  dev_id=0x%02X  speed=%s over TCP  class=%02X/%02X/%02X",
                     dep.device_id, spd, dep.usb_class, dep.subclass, dep.protocol);

            if (findByDeviceId(dep.device_id)) {
                LOG_WARN("[HubServer] dev_id=0x%02X already has a session — closing old one first", dep.device_id);
                if (on_disconnect_) on_disconnect_(dep.device_id);
            }

            // Move socket from pending to the future session via the connect callback
            auto it = pending_tcp_socks_.find(client_fd);
            if (it != pending_tcp_socks_.end()) {
                // Pass ownership temporarily side-channel by setting dev_addr to client_fd 
                // Wait, ConnectCb takes sockaddr_in. Pass fd in reply_port field.
                sockaddr_in fake_addr{}; 
                fake_addr.sin_port = htons(client_fd); // Hack to pass fd to main.cpp
                if (on_connect_) on_connect_(dep.device_id, dep.speed, fake_addr);
            }
            return;
        }

        if (static_cast<DeviceEvent>(dep.event) == DeviceEvent::DISCONNECT) {
            LOG_INFO("[HubServer] DISCONNECT  dev_id=0x%02X via TCP", dep.device_id);
            if (on_disconnect_) on_disconnect_(dep.device_id);
            return;
        }

        return;
    }

    // ── RAW_DATA ─────────────────────────────────────────────────────────
    if (cmd == CmdType::RAW_DATA) {
        auto it = sessions_.find(hdr.device_id);
        if (it == sessions_.end()) {
            LOG_WARN("[HubServer] RAW_DATA for unknown dev_id=0x%02X (%zu B) — dropped",
                     hdr.device_id, payload.size());
            return;
        }
        LOG_DBG("[HubServer] RAW_DATA  dev_id=0x%02X  %zu B  device→kernel",
                hdr.device_id, payload.size());
        it->second->handler->onDeviceData(payload.data(), payload.size());
        return;
    }

    // ── CMD_LOG ──────────────────────────────────────────────────────────
    if (cmd == CmdType::LOG) {
        if (payload.size() < sizeof(LogPayload)) return;
        LogPayload lp;
        memcpy(&lp, payload.data(), sizeof(lp));
        const char* msg     = reinterpret_cast<const char*>(payload.data() + sizeof(lp));
        size_t      msg_len = payload.size() - sizeof(lp);

        while (msg_len > 0 && (msg[msg_len - 1] == '\n' || msg[msg_len - 1] == '\r'))
            --msg_len;

        switch (lp.log_level) {
            case LOG_LEVEL_ERROR:   LOG_ERR(  "[ESP TCP] %.*s", (int)msg_len, msg); break;
            case LOG_LEVEL_WARN:    LOG_WARN( "[ESP TCP] %.*s", (int)msg_len, msg); break;
            case LOG_LEVEL_DEBUG:
            case LOG_LEVEL_VERBOSE: LOG_DBG(  "[ESP TCP] %.*s", (int)msg_len, msg); break;
            default:                LOG_INFO( "[ESP TCP] %.*s", (int)msg_len, msg); break;
        }
        return;
    }

    LOG_WARN("[HubServer] unhandled TCP cmd=0x%02X", hdr.cmd_type);
}

void HubServer::setConnectCallback(ConnectCb cb)    { on_connect_    = std::move(cb); }
void HubServer::setDisconnectCallback(DisconnectCb cb) { on_disconnect_ = std::move(cb); }

std::unique_ptr<TcpSocket> HubServer::extractPendingSocket(int fd) {
    auto it = pending_tcp_socks_.find(fd);
    if (it != pending_tcp_socks_.end()) {
        auto ptr = std::move(it->second);
        pending_tcp_socks_.erase(it);
        return ptr;
    }
    return nullptr;
}

void HubServer::addSession(uint8_t device_id, int vhci_port,
                            int vhci_fd, int kernel_fd,
                            const sockaddr_in& dev_addr,
                            std::unique_ptr<TcpSocket> tcp_sock)
{
    auto s       = std::make_unique<DeviceSession>();
    s->device_id = device_id;
    s->vhci_port = vhci_port;
    s->vhci_fd   = vhci_fd;
    s->kernel_fd = kernel_fd;
    s->dev_addr  = dev_addr;
    int tcp_fd   = tcp_sock ? tcp_sock->fd() : -1;
    s->tcp_sock  = std::move(tcp_sock);
    
    if (s->tcp_sock) {
        s->handler   = std::make_unique<PassthroughHandler>(vhci_fd, *(s->tcp_sock), device_id);
        tcp_fd_map_[tcp_fd] = s.get();
    }

    vhci_fd_map_[vhci_fd] = s.get();
    sessions_[device_id]  = std::move(s);
}

void HubServer::removeSession(uint8_t device_id)
{
    auto it = sessions_.find(device_id);
    if (it == sessions_.end()) return;

    DeviceSession& s = *it->second;
    LOG_INFO("[HubServer] removing session  dev_id=0x%02X  vhci_port=%d  vhci_fd=%d  tcp_fd=%d",
             s.device_id, s.vhci_port, s.vhci_fd, s.tcp_sock ? s.tcp_sock->fd() : -1);

    vhci_fd_map_.erase(s.vhci_fd);
    if (s.tcp_sock) {
        tcp_fd_map_.erase(s.tcp_sock->fd());
    }
    
    sessions_.erase(it);
}

DeviceSession* HubServer::findByDeviceId(uint8_t device_id)
{
    auto it = sessions_.find(device_id);
    return (it != sessions_.end()) ? it->second.get() : nullptr;
}

DeviceSession* HubServer::findByVhciFd(int vhci_fd)
{
    auto it = vhci_fd_map_.find(vhci_fd);
    return (it != vhci_fd_map_.end()) ? it->second : nullptr;
}

DeviceSession* HubServer::findByTcpFd(int tcp_fd)
{
    auto it = tcp_fd_map_.find(tcp_fd);
    return (it != tcp_fd_map_.end()) ? it->second : nullptr;
}
