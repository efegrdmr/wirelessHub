#pragma once
#include "UdpSocket.h"
#include "../protocol/Protocol.h"
#include "../handlers/PassthroughHandler.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <netinet/in.h>
#include <unistd.h>

// ── DeviceSession ─────────────────────────────────────────────────────────────
struct DeviceSession {
    uint8_t     device_id  = 0;
    int         vhci_port  = -1;
    int         vhci_fd    = -1;   // spv[1] — daemon side
    int         kernel_fd  = -1;   // spv[0] — kernel side
    sockaddr_in dev_addr   = {};
    std::unique_ptr<PassthroughHandler> handler;

    ~DeviceSession() {
        if (vhci_fd   >= 0) ::close(vhci_fd);
        if (kernel_fd >= 0) ::close(kernel_fd);
    }
};

// ── HubServer ─────────────────────────────────────────────────────────────────
// Owns the daemon-side UDP socket and the live device session map.
// Dispatches inbound UDP to session handlers and fires callbacks into main when
// device lifecycle events arrive.
class HubServer {
public:
    // Callback signatures for main's epoll/vhci code.
    using ConnectCb    = std::function<void(uint8_t device_id, uint8_t speed,
                                            sockaddr_in dev_addr)>;
    using DisconnectCb = std::function<void(uint8_t device_id)>;

    // Binds to DAEMON_PORT.
    explicit HubServer();

    // fd to register in main's epoll (EPOLLIN).
    int udpFd() const;

    // Call from main's epoll loop whenever udpFd() is readable.
    void onUdpReadable();

    // Lifecycle callbacks — set before entering the epoll loop.
    void setConnectCallback(ConnectCb cb);
    void setDisconnectCallback(DisconnectCb cb);

    // Called by main after a successful vhci.attach().
    // Creates the PassthroughHandler and inserts into the session maps.
    void addSession(uint8_t device_id, int vhci_port,
                    int vhci_fd, int kernel_fd, const sockaddr_in& dev_addr);

    // Closes vhci_fd + kernel_fd and erases the session from the maps.
    void removeSession(uint8_t device_id);

    // Session lookups for main's vhci HUP handling.
    DeviceSession* findByDeviceId(uint8_t device_id);
    DeviceSession* findByVhciFd(int vhci_fd);

private:
    UdpSocket sock_;
    std::unordered_map<uint8_t, std::unique_ptr<DeviceSession>> sessions_;
    std::unordered_map<int, DeviceSession*>                      fd_map_;

    ConnectCb    on_connect_;
    DisconnectCb on_disconnect_;

    uint8_t pkt_buf_[MAX_PACKET];
};
