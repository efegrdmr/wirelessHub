#include "usbip/VhciDriver.h"
#include "core/DeviceRegistry.h"
#include "core/Dispatcher.h"

#include <cstdio>
#include <cstring>
#include <csignal>
#include <cerrno>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

// ── Globals ────────────────────────────────────────────────────────────────
static volatile bool g_running = true;
static void onSignal(int) { g_running = false; }

static constexpr int    MAX_EPOLL_EVENTS = 32;
static constexpr size_t BUF_SIZE         = 65536;
static constexpr char   UNIX_SOCK_PATH[] = "/tmp/wirelesshub.sock";

// ── Per-device session ─────────────────────────────────────────────────────
// Daemon bridges between the device (client_fd) and the kernel vhci (vhci_fd).
//
//   WasdMouseTest <──(sock)──(client_fd)── daemon ──(vhci_fd=spv[1])──(spv[0])──> kernel vhci-hcd
//
// The kernel holds spv[0]; the daemon proxies bytes between client_fd and vhci_fd.
// USB resets only affect the vhci side and don't cut the WasdMouseTest connection.
struct DeviceSession {
    int      client_fd = -1;   // socket from WasdMouseTest
    int      vhci_fd   = -1;   // spv[1], daemon's side of the kernel socketpair
    uint32_t device_id =  0;
    int      vhci_port = -1;
};

static DeviceSession sessions[16];
static int           session_count = 0;

// Find a session by either of its two fds
static DeviceSession* findSession(int fd) {
    for (int i = 0; i < session_count; ++i)
        if (sessions[i].client_fd == fd || sessions[i].vhci_fd == fd)
            return &sessions[i];
    return nullptr;
}

static void removeSession(DeviceSession& s, VhciDriver& vhci,
                          DeviceRegistry& registry, int epfd) {
    printf("[main] device_id=%u disconnected, detaching vhci_port=%d\n",
           s.device_id, s.vhci_port);
    epoll_ctl(epfd, EPOLL_CTL_DEL, s.client_fd, nullptr);
    epoll_ctl(epfd, EPOLL_CTL_DEL, s.vhci_fd,   nullptr);
    registry.remove(s.device_id);
    if (s.vhci_port >= 0) vhci.detach(s.vhci_port);
    close(s.client_fd);
    close(s.vhci_fd);
    // swap-remove
    int idx = static_cast<int>(&s - sessions);
    sessions[idx] = sessions[--session_count];
}

// ── main ───────────────────────────────────────────────────────────────────
int main() {
    struct sigaction sa{};
    sa.sa_handler = onSignal;
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    VhciDriver vhci;
    if (!vhci.init()) {
        fprintf(stderr, "[main] VhciDriver init failed\n");
        return 1;
    }

    DeviceRegistry registry;

    // Unix domain socket server
    unlink(UNIX_SOCK_PATH);

    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("[main] socket"); return 1; }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, UNIX_SOCK_PATH, sizeof(addr.sun_path) - 1);

    if (bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("[main] bind"); return 1;
    }
    if (listen(server_fd, 8) < 0) {
        perror("[main] listen"); return 1;
    }
    printf("[main] listening on %s\n", UNIX_SOCK_PATH);

    // epoll
    int epfd = epoll_create1(0);
    if (epfd < 0) { perror("[main] epoll_create1"); return 1; }

    auto epollAdd = [&](int fd, uint32_t events) {
        epoll_event ev{};
        ev.events  = events;
        ev.data.fd = fd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
    };
    epollAdd(server_fd, EPOLLIN);

    uint8_t        buf[BUF_SIZE];
    epoll_event    events[MAX_EPOLL_EVENTS];

    while (g_running) {
        int n = epoll_wait(epfd, events, MAX_EPOLL_EVENTS, 500);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("[main] epoll_wait");
            break;
        }

        for (int i = 0; i < n; ++i) {
            int      fd  = events[i].data.fd;
            uint32_t evs = events[i].events;

            // ── New connection from a device (WasdMouseTest / ESP32) ───────
            if (fd == server_fd) {
                int client_fd = accept(server_fd, nullptr, nullptr);
                if (client_fd < 0) { perror("[main] accept"); continue; }

                if (session_count >= 16) {
                    fprintf(stderr, "[main] session limit reached, rejecting\n");
                    close(client_fd);
                    continue;
                }

                // Create socketpair for vhci-hcd:
                //   spv[0] → given to kernel via sysfs attach
                //   spv[1] → daemon proxies bytes here ↔ client_fd
                int spv[2];
                if (socketpair(AF_UNIX, SOCK_STREAM, 0, spv) < 0) {
                    perror("[main] socketpair");
                    close(client_fd);
                    continue;
                }

                uint32_t device_id = static_cast<uint32_t>(session_count);
                constexpr uint32_t SPEED = 2; // USB_SPEED_FULL

                int port = vhci.attach(spv[0], device_id, SPEED);
                close(spv[0]); // kernel now owns it via its own file reference
                if (port < 0) {
                    fprintf(stderr, "[main] vhci.attach failed\n");
                    close(spv[1]);
                    close(client_fd);
                    continue;
                }

                registry.add(device_id, Dispatcher::resolve(0x00, 0x00));

                DeviceSession& sess = sessions[session_count++];
                sess.client_fd  = client_fd;
                sess.vhci_fd    = spv[1];
                sess.device_id  = device_id;
                sess.vhci_port  = port;

                // Watch both fds for incoming data and disconnect
                epollAdd(client_fd, EPOLLIN | EPOLLRDHUP | EPOLLERR);
                epollAdd(spv[1],    EPOLLIN | EPOLLRDHUP | EPOLLERR);

                printf("[main] device connected: device_id=%u vhci_port=%d "
                       "client_fd=%d vhci_fd=%d\n",
                       device_id, port, client_fd, spv[1]);
                continue;
            }

            // ── Find session for this fd ────────────────────────────────────
            DeviceSession* sess = findSession(fd);
            if (!sess) {
                epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
                close(fd);
                continue;
            }

            // ── Disconnect on either side → tear down whole session ────────
            if (evs & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                if (fd == sess->client_fd) {
                    // Device disconnected → full teardown
                    printf("[main] device disconnected, removing session\n");
                    removeSession(*sess, vhci, registry, epfd);
                } else {
                    // Kernel released socket (normal USB port reset after enumeration).
                    // Re-attach with a new socketpair while keeping client_fd alive.
                    printf("[main] kernel USB reset on vhci_port=%d — re-attaching\n",
                           sess->vhci_port);
                    epoll_ctl(epfd, EPOLL_CTL_DEL, sess->vhci_fd, nullptr);
                    close(sess->vhci_fd);
                    sess->vhci_fd = -1;
                    vhci.freePort(sess->vhci_port);
                    sess->vhci_port = -1;

                    usleep(50000); // 50 ms — let kernel finish reset sequence

                    int spv[2];
                    if (socketpair(AF_UNIX, SOCK_STREAM, 0, spv) < 0) {
                        perror("[main] re-attach socketpair");
                        removeSession(*sess, vhci, registry, epfd);
                        continue;
                    }
                    int port = vhci.attach(spv[0], sess->device_id, 2 /*FULL*/);
                    close(spv[0]);
                    if (port < 0) {
                        fprintf(stderr, "[main] re-attach failed\n");
                        close(spv[1]);
                        removeSession(*sess, vhci, registry, epfd);
                        continue;
                    }
                    sess->vhci_fd   = spv[1];
                    sess->vhci_port = port;
                    epollAdd(spv[1], EPOLLIN | EPOLLRDHUP | EPOLLERR);
                    printf("[main] re-attached: vhci_port=%d vhci_fd=%d\n", port, spv[1]);
                }
                continue;
            }

            // ── Data: forward bytes between client_fd <-> vhci_fd ──────────
            if (evs & EPOLLIN) {
                ssize_t len = read(fd, buf, sizeof(buf));
                if (len <= 0) {
                    removeSession(*sess, vhci, registry, epfd);
                    continue;
                }

                // Determine direction and destination
                bool to_kernel = (fd == sess->client_fd);
                int  dst       = to_kernel ? sess->vhci_fd : sess->client_fd;
                printf("[bridge] %s → %s  %zd bytes  (device_id=%u)\n",
                       to_kernel ? "device" : "kernel",
                       to_kernel ? "kernel" : "device",
                       len, sess->device_id);

                ssize_t written = 0;
                while (written < len) {
                    ssize_t r = write(dst, buf + written,
                                      static_cast<size_t>(len - written));
                    if (r <= 0) {
                        perror("[main] write bridge");
                        break;
                    }
                    written += r;
                }
            }
        }
    }

    // ── Cleanup ────────────────────────────────────────────────────────────
    printf("[main] shutting down\n");
    for (int i = 0; i < session_count; ++i) {
        if (sessions[i].vhci_port >= 0) vhci.detach(sessions[i].vhci_port);
        close(sessions[i].client_fd);
        close(sessions[i].vhci_fd);
    }
    vhci.detachAll();
    close(server_fd);
    close(epfd);
    unlink(UNIX_SOCK_PATH);
    return 0;
}
