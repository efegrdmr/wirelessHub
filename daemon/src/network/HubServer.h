#pragma once
#include "UdpSocket.h"
#include "TcpSocket.h"
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
    
    // The TCP socket connected to the ESP32 for this session
    std::unique_ptr<TcpSocket> tcp_sock;
    std::unique_ptr<PassthroughHandler> handler;

    ~DeviceSession() {
        if (vhci_fd   >= 0) ::close(vhci_fd);
        if (kernel_fd >= 0) ::close(kernel_fd);
    }
};

// ── HubServer ─────────────────────────────────────────────────────────────────
// Owns the daemon-side UDP socket (for discovery), TCP server socket, 
// and the live device session map.
// Dispatches inbound data to session handlers and fires callbacks into main when
// device lifecycle events arrive.
class HubServer {
public:
    // Callback signatures for main's epoll/vhci code.
    using ConnectCb    = std::function<void(uint8_t device_id, uint8_t speed,
                                            sockaddr_in dev_addr)>;
    using DisconnectCb = std::function<void(uint8_t device_id)>;

    // Binds UDP and TCP to DAEMON_PORT.
    explicit HubServer();

    // fd to register in main's epoll (EPOLLIN) for discovery broadcasts.
    int udpFd() const;
    
    // fd to register in main's epoll (EPOLLIN) for incoming TCP connections.
    int tcpServerFd() const;

    // Call from main's epoll loop whenever udpFd() is readable.
    void onUdpReadable();
    
    // Call from main's epoll loop whenever tcpServerFd() is readable.
    // Accepts the connection and reads the first packet (which should be DEVICE_EVENT CONNECT)
    void onTcpAccept();
    
    // Call from main's epoll loop whenever a client's TCP socket is readable.
    void onTcpReadable(int client_fd);

    // Lifecycle callbacks — set before entering the epoll loop.
    void setConnectCallback(ConnectCb cb);
    void setDisconnectCallback(DisconnectCb cb);

    // Called by main after a successful vhci.attach().
    // Creates the PassthroughHandler and inserts into the session maps.
    void addSession(uint8_t device_id, int vhci_port,
                    int vhci_fd, int kernel_fd, const sockaddr_in& dev_addr,
                    std::unique_ptr<TcpSocket> tcp_sock);

    // Closes vhci_fd + kernel_fd and erases the session from the maps.
    void removeSession(uint8_t device_id);

    // Session lookups for main's vhci HUP handling.
    DeviceSession* findByDeviceId(uint8_t device_id);
    DeviceSession* findByVhciFd(int vhci_fd);
    DeviceSession* findByTcpFd(int tcp_fd);
    
     // Extracts and removes a pending TCP socket by fd
     std::unique_ptr<TcpSocket> extractPendingSocket(int fd);

private:
    UdpSocket udp_sock_;
    TcpServer tcp_server_;
    
    std::unordered_map<uint8_t, std::unique_ptr<DeviceSession>> sessions_;
    // Maps for fast fd lookups in epoll loop
    std::unordered_map<int, DeviceSession*> vhci_fd_map_;
    std::unordered_map<int, DeviceSession*> tcp_fd_map_;
    
    // Temporary storage for accepted sockets that haven't sent CONNECT yet
    std::unordered_map<int, std::unique_ptr<TcpSocket>> pending_tcp_socks_;

    ConnectCb    on_connect_;
    DisconnectCb on_disconnect_;

    uint8_t pkt_buf_[MAX_PACKET];
    
    // Helper to read a complete packet from a TCP socket
    bool readTcpPacket(int fd, Header& hdr, std::vector<uint8_t>& payload);
};
