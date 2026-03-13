#include "usbip/VhciDriver.h"
#include "network/HubServer.h"
#include "network/TapDevice.h"
#include "protocol/Protocol.h"
#include "Log.h"

#include <cstdio>
#include <cstring>
#include <csignal>
#include <cerrno>

#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

// ── Constants ─────────────────────────────────────────────────────────────────
static constexpr int MAX_EVENTS = 16;

// ── Globals ───────────────────────────────────────────────────────────────────
static volatile bool g_running = true;
static void onSignal(int) { g_running = false; }

// ── epoll helpers ─────────────────────────────────────────────────────────────
static void epollAdd(int epfd, int fd, uint32_t evts)
{
    epoll_event ev{};
    ev.events  = evts;
    ev.data.fd = fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
}

static void epollDel(int epfd, int fd)
{
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
}

// ── main ──────────────────────────────────────────────────────────────────────
int main()
{
    struct sigaction sa{};
    sa.sa_handler = onSignal;
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    VhciDriver vhci;
    if (!vhci.init()) { LOG_ERR("[main] VhciDriver init failed"); return 1; }

    TapDevice tap;

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) { LOG_ERR("[main] epoll_create1: %s", strerror(errno)); return 1; }

    HubServer hub;
    epollAdd(epfd, hub.udpFd(), EPOLLIN);
    epollAdd(epfd, hub.tcpServerFd(), EPOLLIN);

    // ── Helper: socketpair + vhci.attach + hub.addSession + epollAdd (USB) ──
    auto doUsbAttach = [&](uint8_t dev_id, uint8_t speed, const sockaddr_in& dev_addr)
    {
        int spv[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, spv) < 0) {
            LOG_ERR("[main] socketpair: %s", strerror(errno)); return;
        }
        uint32_t devid = (1u << 16) | static_cast<uint32_t>(dev_id + 1u);
        int port = vhci.attach(spv[0], devid, speed);
        if (port < 0) {
            LOG_ERR("[main] vhci.attach() failed for device_id=0x%02X", dev_id);
            ::close(spv[0]); ::close(spv[1]); return;
        }
        
        // Extract the pending TCP socket via the "fake addr" hack implemented in HubServer
        int client_fd = ntohs(dev_addr.sin_port); 
        auto tcp_sock = hub.extractPendingSocket(client_fd);
        if (!tcp_sock) {
            LOG_ERR("[main] could not extract pending TCP socket for fd=%d", client_fd);
            ::close(spv[0]); ::close(spv[1]); return;
        }
        
        LOG_INFO("[main] USB device_id=0x%02X attached — vhci_port=%d  socketpair_fd=%d  tcp_fd=%d",
                 dev_id, port, spv[1], tcp_sock->fd());
                 
        // Register the new TCP socket in epoll
        epollAdd(epfd, tcp_sock->fd(), EPOLLIN | EPOLLRDHUP);
        
        hub.addSession(dev_id, port, spv[1], spv[0], dev_addr, std::move(tcp_sock));
        epollAdd(epfd, spv[1], EPOLLIN | EPOLLRDHUP);
    };

    // ── Helper: TAP open + hub.addSession + epollAdd (Ethernet) ─────────────
    auto doEthAttach = [&](uint8_t dev_id, const sockaddr_in& dev_addr)
    {
        if (tap.isOpen()) {
            LOG_WARN("[main] TAP already open, ignoring duplicate CONNECT for dev_id=0x%02X", dev_id);
            return;
        }
        if (!tap.open("wh_eth0")) {
            LOG_ERR("[main] TapDevice::open() failed");
            return;
        }
        int tap_fd = tap.release();   // DeviceSession RAII takes ownership
        
        // Extract the pending TCP socket via the "fake addr" hack implemented in HubServer
        int client_fd = ntohs(dev_addr.sin_port); 
        auto tcp_sock = hub.extractPendingSocket(client_fd);
        if (!tcp_sock) {
            LOG_ERR("[main] could not extract pending TCP socket for fd=%d", client_fd);
            ::close(tap_fd); return;
        }
        
        // Register the new TCP socket in epoll
        epollAdd(epfd, tcp_sock->fd(), EPOLLIN | EPOLLRDHUP);
        
        // vhci_port = -1 signals "TAP session, no VHCI" to the event loop
        hub.addSession(dev_id, /*vhci_port=*/-1, /*vhci_fd=*/tap_fd,
                       /*kernel_fd=*/-1, dev_addr, std::move(tcp_sock));
        epollAdd(epfd, tap_fd, EPOLLIN);
        LOG_INFO("[main] Ethernet TAP wh_eth0 open (fd=%d) — dev_id=0x%02X session active over TCP", tap_fd, dev_id);
    };

    // ── Callbacks ─────────────────────────────────────────────────────────────
    hub.setConnectCallback([&](uint8_t dev_id, uint8_t speed, sockaddr_in dev_addr)
    {
        if (dev_id == DEVICE_ID_ETHERNET)
            doEthAttach(dev_id, dev_addr);
        else
            doUsbAttach(dev_id, speed, dev_addr);
    });

    hub.setDisconnectCallback([&](uint8_t dev_id)
    {
        DeviceSession* s = hub.findByDeviceId(dev_id);
        if (!s) { LOG_WARN("[main] DISCONNECT for unknown dev_id=0x%02X", dev_id); return; }
        LOG_INFO("[main] DISCONNECT dev_id=0x%02X — tearing down session", dev_id);
        epollDel(epfd, s->vhci_fd);
        if (s->tcp_sock) {
            epollDel(epfd, s->tcp_sock->fd());
        }
        if (s->vhci_port >= 0)
            vhci.detach(s->vhci_port);   // USB only
        hub.removeSession(dev_id);  // RAII closes fds
    });

    LOG_INFO("[main] event loop started — waiting for incoming connections");

    epoll_event events[MAX_EVENTS];

    while (g_running) {
        int nfds = ::epoll_wait(epfd, events, MAX_EVENTS, 500);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            perror("[main] epoll_wait"); break;
        }

        for (int i = 0; i < nfds; ++i) {
            int      ev_fd = events[i].data.fd;
            uint32_t evts  = events[i].events;

            // ── UDP Discovery ────────────────────────────────────────────────
            if (ev_fd == hub.udpFd()) {
                hub.onUdpReadable();
                continue;
            }
            
            // ── TCP Server Accept ────────────────────────────────────────────
            if (ev_fd == hub.tcpServerFd()) {
                hub.onTcpAccept();
                continue;
            }
            
            // ── Check if this is a TCP client socket ─────────────────────────
            // It could be a pending socket or an active session socket
            // HubServer::onTcpReadable handles both
            auto pending_it = hub.extractPendingSocket(-2); // Dummy call to check if it's a TCP, wait we need to check if it's TCP
            // We just pass it to HubServer, and if HubServer knows it, it will read it.
            // Wait, we need a way to know if ev_fd is a TCP socket we own
            // We can just add a method to HubServer like HubServer::isTcpSocket(fd)
            // But we can also just check if findByTcpFd(ev_fd) is true, OR if it's in pending.
            // Let's modify HubServer to have a general `handleTcpEvent(fd, evts)` or we just 
            // call onTcpReadable if `isTcpSocket` is true.
            
            // For now, let's just let HubServer check if it knows the socket
            // To do this properly without breaking our nice architecture:
            // Since `HubServer::findByTcpFd(ev_fd)` exists for active sessions:
            DeviceSession* tcp_sess = hub.findByTcpFd(ev_fd);
            if (tcp_sess) {
                if (evts & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                    LOG_WARN("[main] TCP socket EOF/err dev_id=0x%02X fd=%d  triggering disconnect", tcp_sess->device_id, ev_fd);
                    uint8_t dev_id = tcp_sess->device_id;
                    hub.removeSession(dev_id); // This will clean it up
                    continue;
                }
                if (evts & EPOLLIN) {
                    hub.onTcpReadable(ev_fd);
                }
                continue;
            }

            // ── vhci_fd event (Kernel EOF or Kernel Data) ─────────────────────
            DeviceSession* s = hub.findByVhciFd(ev_fd);
            if (s) {
                if (evts & (EPOLLHUP | EPOLLRDHUP | EPOLLERR)) {
                    if (s->vhci_port >= 0) {
                        // USB reset — re-attach same device
                        LOG_WARN("[main] USB reset  dev_id=0x%02X  vhci_fd=%d  epoll_evts=0x%02X — re-attaching",
                                 s->device_id, ev_fd, evts);
                        uint8_t     dev_id   = s->device_id;
                        sockaddr_in dev_addr = s->dev_addr;
                        // Move the tcp socket out so we don't close it
                        auto tcp_sock = std::move(s->tcp_sock);
                        
                        epollDel(epfd, ev_fd);
                        vhci.freePort(s->vhci_port);
                        hub.removeSession(dev_id);
                        
                        // We need a modified doUsbAttach that takes an existing TCP socket
                        int spv[2];
                        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, spv) == 0) {
                            uint32_t devid = (1u << 16) | static_cast<uint32_t>(dev_id + 1u);
                            int port = vhci.attach(spv[0], devid, 2); // speed generic
                            if (port >= 0) {
                                LOG_INFO("[main] USB device_id=0x%02X re-attached — vhci_port=%d  socketpair_fd=%d  tcp_fd=%d",
                                         dev_id, port, spv[1], tcp_sock->fd());
                                hub.addSession(dev_id, port, spv[1], spv[0], dev_addr, std::move(tcp_sock));
                                epollAdd(epfd, spv[1], EPOLLIN | EPOLLRDHUP);
                            } else {
                                close(spv[0]); close(spv[1]);
                            }
                        }
                    } else {
                        // TAP EOF — just remove session (ESP32 will re-CONNECT)
                        LOG_WARN("[main] TAP EOF  dev_id=0x%02X  fd=%d — session removed",
                                 s->device_id, ev_fd);
                        uint8_t dev_id = s->device_id;
                        epollDel(epfd, ev_fd);
                        hub.removeSession(dev_id);
                    }
                    continue;
                }
    
                if (evts & EPOLLIN) {
                    bool ok = s->handler->onVhciReadable();
                    if (!ok) {
                        LOG_WARN("[main] handler returned error for dev_id=0x%02X  detaching",
                                 s->device_id);
                        uint8_t dev_id = s->device_id;
                        // Trigger full disconnect
                        hub.removeSession(dev_id);
                    }
                }
                continue;
            }
            
            // If it's a pending TCP socket (in HubServer's internal map)
            // It will trigger onTcpReadable which invokes the ConnectCb and passes
            // the fd inside the fake sockaddr_in
            // We know it's pending if it's none of the above
            if (evts & EPOLLIN) {
                hub.onTcpReadable(ev_fd); // Will internally check if it's a pending socket
            }
        }
    }

    LOG_INFO("[main] shutting down");
    vhci.detachAll();
    close(epfd);
    return 0;
}
