#include "usbip/VhciDriver.h"
#include "network/HubServer.h"

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
    if (!vhci.init()) { fprintf(stderr, "[main] VhciDriver init failed\n"); return 1; }

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) { perror("[main] epoll_create1"); return 1; }

    HubServer hub;
    epollAdd(epfd, hub.udpFd(), EPOLLIN);

    // ── Helper: socketpair + vhci.attach + hub.addSession + epollAdd ─────────
    auto doAttach = [&](uint8_t dev_id, uint8_t speed, const sockaddr_in& dev_addr)
    {
        int spv[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, spv) < 0) {
            perror("[main] socketpair"); return;
        }
        uint32_t devid = (1u << 16) | static_cast<uint32_t>(dev_id + 1u);
        int port = vhci.attach(spv[0], devid, speed);
        if (port < 0) {
            fprintf(stderr, "[main] vhci.attach() failed for device_id=0x%02X\n", dev_id);
            ::close(spv[0]); ::close(spv[1]); return;
        }
        printf("[main] device_id=0x%02X attached on vhci port=%d  vhci_fd=%d\n",
               dev_id, port, spv[1]);
        hub.addSession(dev_id, port, spv[1], spv[0], dev_addr);
        epollAdd(epfd, spv[1], EPOLLIN | EPOLLRDHUP);
    };

    // ── Callbacks ─────────────────────────────────────────────────────────────
    hub.setConnectCallback([&](uint8_t dev_id, uint8_t speed, sockaddr_in dev_addr)
    {
        doAttach(dev_id, speed, dev_addr);
    });

    hub.setDisconnectCallback([&](uint8_t dev_id)
    {
        DeviceSession* s = hub.findByDeviceId(dev_id);
        if (!s) return;
        epollDel(epfd, s->vhci_fd);
        vhci.detach(s->vhci_port);
        hub.removeSession(dev_id);  // RAII closes fds
    });

    printf("[main] event loop started — waiting for DEVICE_EVENT\n");

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

            // ── UDP ──────────────────────────────────────────────────────────
            if (ev_fd == hub.udpFd()) {
                hub.onUdpReadable();
                continue;
            }

            // ── vhci_fd event ────────────────────────────────────────────────
            DeviceSession* s = hub.findByVhciFd(ev_fd);
            if (!s) continue;

            if (evts & (EPOLLHUP | EPOLLRDHUP | EPOLLERR)) {
                // USB reset — re-attach same device
                printf("[main] USB reset for device_id=0x%02X (vhci_fd=%d), re-attaching\n",
                       s->device_id, ev_fd);
                uint8_t     dev_id   = s->device_id;
                sockaddr_in dev_addr = s->dev_addr;

                epollDel(epfd, ev_fd);
                vhci.freePort(s->vhci_port);
                hub.removeSession(dev_id);     // RAII closes fds
                doAttach(dev_id, /*speed=*/2, dev_addr);
                continue;
            }

            if (evts & EPOLLIN) {
                bool ok = s->handler->onVhciReadable();
                if (!ok) {
                    uint8_t dev_id = s->device_id;
                    epollDel(epfd, ev_fd);
                    vhci.detach(s->vhci_port);
                    hub.removeSession(dev_id);
                }
            }
        }
    }

    printf("[main] shutting down\n");
    vhci.detachAll();
    close(epfd);
    return 0;
}

