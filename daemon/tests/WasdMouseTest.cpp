// tests/WasdMouseTest.cpp
// Fake ESP32: speaks the WirelessHub UDP protocol to the daemon.
// Sends a DEVICE_EVENT CONNECT, then bridges raw USB/IP bytes wrapped in
// RAW_DATA headers. WASD keys → cursor movement.
//
// Transport:  UDP
//   device listens on: 0.0.0.0:7789  (reply_port in DEVICE_EVENT)
//   daemon  listens on: 127.0.0.1:7788
//
// Build: see CMakeLists.txt (target wasd_mouse_test)
// Run:   sudo ./build/wasd_mouse_test

#include "protocol/Protocol.h"

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <cerrno>
#include <vector>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/uio.h>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <netinet/in.h>

// ── UDP transport globals ──────────────────────────────────────────────────

static int         g_udp_fd   = -1;
static sockaddr_in g_daemon_addr{};
static uint8_t     g_udp_seq  = 0;
static uint8_t     MY_DEVICE_ID = 0x00;   // overridden by argv[1]

// Wrap payload in a WirelessHub RAW_DATA header and send via UDP to the daemon.
static void udpSend(const uint8_t* payload, size_t len)
{
    Header hdr{};
    hdr.cmd_type    = static_cast<uint8_t>(CmdType::RAW_DATA);
    hdr.device_id   = MY_DEVICE_ID;
    hdr.endpoint    = 0x00;
    hdr.seq         = g_udp_seq++;
    hdr.payload_len = static_cast<uint16_t>(len);

    // Combine into one UDP datagram
    std::vector<uint8_t> pkt(sizeof(hdr) + len);
    memcpy(pkt.data(),              &hdr, sizeof(hdr));
    if (len) memcpy(pkt.data() + sizeof(hdr), payload, len);

    sendto(g_udp_fd, pkt.data(), pkt.size(), 0,
           reinterpret_cast<const sockaddr*>(&g_daemon_addr), sizeof(g_daemon_addr));
}

// ── USB/IP wire structs ────────────────────────────────────────────────────

static constexpr uint32_t USBIP_CMD_SUBMIT = 0x00000001;
static constexpr uint32_t USBIP_CMD_UNLINK = 0x00000002;
static constexpr uint32_t USBIP_RET_SUBMIT = 0x00000003;
static constexpr uint32_t USBIP_RET_UNLINK = 0x00000004;
static constexpr uint32_t USBIP_DIR_OUT    = 0;
static constexpr uint32_t USBIP_DIR_IN     = 1;

struct UsbipHeaderBasic {
    uint32_t command;
    uint32_t seqnum;
    uint32_t devid;
    uint32_t direction;
    uint32_t ep;
};

struct UsbipCmdSubmit {
    UsbipHeaderBasic basic;               // 20 bytes
    uint32_t         transfer_flags;
    uint32_t         transfer_buffer_length;
    uint32_t         start_frame;
    uint32_t         number_of_packets;
    uint32_t         interval;
    uint8_t          setup[8];
};  // 48 bytes total

struct UsbipRetSubmit {
    UsbipHeaderBasic basic;               // 20 bytes
    uint32_t         status;
    uint32_t         actual_length;
    uint32_t         start_frame;
    uint32_t         number_of_packets;
    uint32_t         error_count;
    uint8_t          _padding[8];
};  // 48 bytes total

// ── HID descriptors ────────────────────────────────────────────────────────

static const uint8_t HID_REPORT_DESC[] = {
    0x05, 0x01,  0x09, 0x02,  0xA1, 0x01,  0x09, 0x01,  0xA1, 0x00,
    0x05, 0x09,  0x19, 0x01,  0x29, 0x03,  0x15, 0x00,  0x25, 0x01,
    0x95, 0x03,  0x75, 0x01,  0x81, 0x02,
    0x95, 0x01,  0x75, 0x05,  0x81, 0x03,
    0x05, 0x01,  0x09, 0x30,  0x09, 0x31,  0x09, 0x38,
    0x15, 0x81,  0x25, 0x7F,  0x75, 0x08,  0x95, 0x03,  0x81, 0x06,
    0xC0, 0xC0,
};
static constexpr uint8_t HID_REPORT_DESC_LEN = sizeof(HID_REPORT_DESC);

static const uint8_t DEVICE_DESC[] = {
    0x12, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40,
    0x34, 0x12,  // idVendor  0x1234
    0x78, 0x56,  // idProduct 0x5678
    0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
};

static const uint8_t CONFIG_DESC[] = {
    0x09, 0x02, 0x22, 0x00, 0x01, 0x01, 0x00, 0xA0, 0x32,
    0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x01, 0x02, 0x00,
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, HID_REPORT_DESC_LEN, 0x00,
    0x07, 0x05, 0x81, 0x03, 0x04, 0x00, 0x0A,
};

static const uint8_t STRING_LANG_DESC[] = { 0x04, 0x03, 0x09, 0x04 };

// ── Mouse state ────────────────────────────────────────────────────────────

static volatile bool g_running  = true;
static int8_t        g_dx       = 0;
static int8_t        g_dy       = 0;
static uint8_t       g_buttons  = 0;

static termios g_old_termios{};
static void restoreTerminal() { tcsetattr(STDIN_FILENO, TCSANOW, &g_old_termios); }
static void onSignal(int)     { g_running = false; restoreTerminal(); }

// ── USB/IP helpers ─────────────────────────────────────────────────────────

static const char* descTypeName(uint8_t t) {
    switch (t) {
    case 0x01: return "DEVICE";    case 0x02: return "CONFIG";
    case 0x03: return "STRING";    case 0x21: return "HID";
    case 0x22: return "HID_REPORT"; default: return "?";
    }
}

// Send a USB/IP reply wrapped in a WirelessHub RAW_DATA UDP packet.
static void sendRetSubmit(int /*unused*/, uint32_t seqnum, int status,
                          const uint8_t* payload, uint32_t payload_len)
{
    UsbipRetSubmit ret{};
    ret.basic.command   = htonl(USBIP_RET_SUBMIT);
    ret.basic.seqnum    = htonl(seqnum);
    ret.status          = htonl(static_cast<uint32_t>(status));
    ret.actual_length   = htonl(payload_len);

    // Pack RET_SUBMIT header + optional payload, then send via UDP
    std::vector<uint8_t> usbip_pkt(sizeof(ret) + payload_len);
    memcpy(usbip_pkt.data(), &ret, sizeof(ret));
    if (payload && payload_len) memcpy(usbip_pkt.data() + sizeof(ret), payload, payload_len);

    udpSend(usbip_pkt.data(), usbip_pkt.size());
}

static uint16_t getWLength(const uint8_t* setup) {
    return uint16_t(setup[6] | (setup[7] << 8));
}

static void handleControlIn(int fd, uint32_t seqnum, const uint8_t* setup)
{
    uint8_t  req_type = setup[0]; uint8_t bRequest = setup[1];
    uint8_t  desc_idx = setup[2]; uint8_t desc_type = setup[3];
    uint16_t wLength  = getWLength(setup);

    if (bRequest == 0x06) {
        const uint8_t* desc = nullptr; uint16_t desc_len = 0;

        if (req_type == 0x80) {
            switch (desc_type) {
            case 0x01: desc = DEVICE_DESC;      desc_len = sizeof(DEVICE_DESC); break;
            case 0x02: desc = CONFIG_DESC;      desc_len = sizeof(CONFIG_DESC); break;
            case 0x03:
                if (desc_idx == 0) { desc = STRING_LANG_DESC; desc_len = sizeof(STRING_LANG_DESC); break; }
                sendRetSubmit(fd, seqnum, -32, nullptr, 0); return;
            default:
                sendRetSubmit(fd, seqnum, -32, nullptr, 0); return;
            }
        } else if (req_type == 0x81 && desc_type == 0x22) {
            desc = HID_REPORT_DESC; desc_len = HID_REPORT_DESC_LEN;
        }

        if (desc && desc_len > 0) {
            uint16_t send_len = (wLength < desc_len) ? wLength : desc_len;
            if (desc_type == 0x01 || desc_type == 0x02 || desc_type == 0x22)
                printf("  GET_DESCRIPTOR %s\n", descTypeName(desc_type));
            sendRetSubmit(fd, seqnum, 0, desc, send_len);
            if (desc_type == 0x22) {
                printf("\n====================================================\n");
                printf("  HID REPORT DESCRIPTOR sent — enumeration complete!\n");
                printf("  Kernel loading HID driver... press WASD to move mouse\n");
                printf("  Q = quit\n");
                printf("====================================================\n\n");
                fflush(stdout);
            }
            return;
        }
        sendRetSubmit(fd, seqnum, -32, nullptr, 0);
        return;
    }
    if (req_type == 0x80 && bRequest == 0x08) {
        uint8_t val = 0x01; sendRetSubmit(fd, seqnum, 0, &val, 1); return;
    }
    if (req_type == 0x81 && bRequest == 0x0A) {
        uint8_t val = 0x00; sendRetSubmit(fd, seqnum, 0, &val, 1); return;
    }
    printf("  CTRL IN  UNKNOWN req_type=0x%02X bRequest=0x%02X → STALL\n",
           req_type, bRequest);
    sendRetSubmit(fd, seqnum, -32, nullptr, 0);
}

static void handleControlOut(int fd, uint32_t seqnum, const uint8_t* setup) {
    // Log only SET_CONFIGURATION (bReq=0x09) — the key enumeration step
    if (setup[1] == 0x09)
        printf("  SET_CONFIGURATION(%u)\n", setup[2]);
    sendRetSubmit(fd, seqnum, 0, nullptr, 0);
}

static void handleInterruptIn(int fd, uint32_t seqnum) {
    uint8_t report[4] = { g_buttons, uint8_t(g_dx), uint8_t(g_dy), 0x00 };
    if (g_dx || g_dy || g_buttons) {
        printf(">>> INT  IN  seq=%-4u ep=1 mouse: btn=%u dx=%d dy=%d\n",
               seqnum, g_buttons, int(int8_t(g_dx)), int(int8_t(g_dy)));
        fflush(stdout);
    }
    g_dx = 0; g_dy = 0;
    sendRetSubmit(fd, seqnum, 0, report, sizeof(report));
}

static void dispatchUsbip(int fd, const uint8_t* buf, size_t len) {
    if (len < sizeof(UsbipCmdSubmit)) { printf("[test] too-short usbip pkt\n"); return; }

    UsbipCmdSubmit cmd;
    memcpy(&cmd, buf, sizeof(cmd));
    uint32_t command   = ntohl(cmd.basic.command);
    uint32_t seqnum    = ntohl(cmd.basic.seqnum);
    uint32_t direction = ntohl(cmd.basic.direction);
    uint32_t ep        = ntohl(cmd.basic.ep);

    if (command == USBIP_CMD_UNLINK) {
        uint32_t unlink_seq; memcpy(&unlink_seq, buf + 20, 4); unlink_seq = ntohl(unlink_seq);
        (void)unlink_seq;
        uint8_t reply[48] = {};
        uint32_t v;
        v = htonl(USBIP_RET_UNLINK); memcpy(reply +  0, &v, 4);
        v = htonl(seqnum);           memcpy(reply +  4, &v, 4);
        v = htonl(0);                memcpy(reply + 20, &v, 4);
        udpSend(reply, sizeof(reply));
        return;
    }
    if (command != USBIP_CMD_SUBMIT) {
        printf("[test] unexpected command 0x%08X\n", command); return;
    }

    if (ep == 0) {
        if (direction == USBIP_DIR_IN) handleControlIn(fd, seqnum, cmd.setup);
        else                           handleControlOut(fd, seqnum, cmd.setup);
    } else if (ep == 1 && direction == USBIP_DIR_IN) {
        handleInterruptIn(fd, seqnum);
    } else {
        sendRetSubmit(fd, seqnum, -32, nullptr, 0);
    }
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[])
{
    // Optional args: device_id (0x00-0x04)  reply_port (default 7789)
    // Usage:  wasd_mouse_test [device_id] [reply_port]
    //   e.g.: wasd_mouse_test 0 7789
    //         wasd_mouse_test 1 7790   (second virtual device)
    uint16_t device_udp_port = DEVICE_BASE_PORT;
    if (argc >= 2) {
        MY_DEVICE_ID     = static_cast<uint8_t>(strtoul(argv[1], nullptr, 0));
        device_udp_port  = static_cast<uint16_t>(DEVICE_BASE_PORT + MY_DEVICE_ID);
    }
    if (argc >= 3) {
        device_udp_port = static_cast<uint16_t>(strtoul(argv[2], nullptr, 0));
    }
    printf("[test] device_id=0x%02X  reply_port=%u\n", MY_DEVICE_ID, device_udp_port);
    struct sigaction sa{};
    sa.sa_handler = onSignal;
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // ── Create device UDP socket ──────────────────────────────────────────
    g_udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_udp_fd < 0) { perror("[test] socket"); return 1; }

    sockaddr_in my_addr = makeAddr(nullptr, device_udp_port);
    if (bind(g_udp_fd, reinterpret_cast<sockaddr*>(&my_addr), sizeof(my_addr)) < 0) {
        perror("[test] bind"); return 1;
    }
    printf("[test] bound device UDP socket on port %u\n", device_udp_port);

    // Daemon address
    g_daemon_addr = makeAddr("127.0.0.1", DAEMON_PORT);

    // ── Send DEVICE_EVENT CONNECT ─────────────────────────────────────────
    DeviceEventPayload dep{};
    dep.device_id  = MY_DEVICE_ID;
    dep.event      = static_cast<uint8_t>(DeviceEvent::CONNECT);
    dep.speed      = static_cast<uint8_t>(UsbSpeed::FULL);
    dep.usb_class  = 0x03; // HID
    dep.subclass   = 0x01; // Boot
    dep.protocol   = 0x02; // Mouse
    dep.reply_port = device_udp_port;

    Header hdr{};
    hdr.cmd_type    = static_cast<uint8_t>(CmdType::DEVICE_EVENT);
    hdr.device_id   = MY_DEVICE_ID;
    hdr.seq         = g_udp_seq++;
    hdr.payload_len = sizeof(dep);

    uint8_t connect_pkt[sizeof(hdr) + sizeof(dep)];
    memcpy(connect_pkt,               &hdr, sizeof(hdr));
    memcpy(connect_pkt + sizeof(hdr), &dep, sizeof(dep));
    sendto(g_udp_fd, connect_pkt, sizeof(connect_pkt), 0,
           reinterpret_cast<const sockaddr*>(&g_daemon_addr), sizeof(g_daemon_addr));
    printf("[test] sent DEVICE_EVENT CONNECT for device_id=0x%02X  reply_port=%u → daemon 127.0.0.1:%u\n",
           MY_DEVICE_ID, device_udp_port, DAEMON_PORT);

    // ── Raw terminal ──────────────────────────────────────────────────────
    tcgetattr(STDIN_FILENO, &g_old_termios);
    termios raw = g_old_termios;
    raw.c_lflag &= ~tcflag_t(ICANON | ECHO);
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    int fl = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, fl | O_NONBLOCK);

    printf("[test] WASD=move  Q=quit\n");

    // ── epoll: watch UDP socket + stdin ───────────────────────────────────
    int epfd = epoll_create1(0);
    if (epfd < 0) { perror("[test] epoll_create1"); return 1; }

    auto addEpoll = [&](int fd, uint32_t evts) {
        epoll_event ev{}; ev.events = evts; ev.data.fd = fd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
    };
    addEpoll(g_udp_fd, EPOLLIN);
    addEpoll(STDIN_FILENO, EPOLLIN);

    uint8_t pkt_buf[65536 + sizeof(Header)];
    epoll_event events[4];

    while (g_running) {
        int n = epoll_wait(epfd, events, 4, 100);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("[test] epoll_wait"); break;
        }

        for (int i = 0; i < n; ++i) {
            int fd    = events[i].data.fd;
            uint32_t ev = events[i].events;

            // ── UDP packet from daemon ────────────────────────────────────
            if (fd == g_udp_fd && (ev & EPOLLIN)) {
                sockaddr_in sender{};
                socklen_t   slen = sizeof(sender);
                ssize_t len = recvfrom(g_udp_fd, pkt_buf, sizeof(pkt_buf), 0,
                                       reinterpret_cast<sockaddr*>(&sender), &slen);
                if (len < static_cast<ssize_t>(sizeof(Header))) continue;

                Header rhdr;
                memcpy(&rhdr, pkt_buf, sizeof(rhdr));
                const uint8_t* payload = pkt_buf + sizeof(rhdr);
                size_t pay_len = static_cast<size_t>(len) - sizeof(rhdr);

                if (static_cast<CmdType>(rhdr.cmd_type) != CmdType::RAW_DATA) {
                    printf("[test] unexpected cmd=0x%02X, ignoring\n", rhdr.cmd_type);
                    continue;
                }
                if (rhdr.device_id != MY_DEVICE_ID) {
                    printf("[test] wrong device_id=0x%02X, ignoring\n", rhdr.device_id);
                    continue;
                }

                // Process multi-packet USB/IP stream (multiple CMD_SUBMITs in payload)
                size_t offset = 0;
                while (offset + sizeof(UsbipCmdSubmit) <= pay_len) {
                    UsbipCmdSubmit usbip_hdr;
                    memcpy(&usbip_hdr, payload + offset, sizeof(usbip_hdr));
                    uint32_t dir   = ntohl(usbip_hdr.basic.direction);
                    uint32_t extra = (dir == USBIP_DIR_OUT)
                                     ? ntohl(usbip_hdr.transfer_buffer_length) : 0;
                    size_t pkt_sz = sizeof(UsbipCmdSubmit) + extra;
                    if (offset + pkt_sz > pay_len) {
                        printf("[test] incomplete usbip pkt at offset %zu\n", offset);
                        break;
                    }
                    dispatchUsbip(fd, payload + offset, pkt_sz);
                    offset += pkt_sz;
                }
            }

            // ── WASD keyboard ─────────────────────────────────────────────
            if (fd == STDIN_FILENO && (ev & EPOLLIN)) {
                char ch = 0;
                if (read(STDIN_FILENO, &ch, 1) == 1) {
                    constexpr int8_t STEP = 8;
                    switch (ch) {
                    case 'w': case 'W': g_dy -= STEP;
                        printf("[KEY] W→dy=%d\n", int(g_dy)); fflush(stdout); break;
                    case 's': case 'S': g_dy += STEP;
                        printf("[KEY] S→dy=%d\n", int(g_dy)); fflush(stdout); break;
                    case 'a': case 'A': g_dx -= STEP;
                        printf("[KEY] A→dx=%d\n", int(g_dx)); fflush(stdout); break;
                    case 'd': case 'D': g_dx += STEP;
                        printf("[KEY] D→dx=%d\n", int(g_dx)); fflush(stdout); break;
                    case 'q': case 'Q': case 3: g_running = false; break;
                    default: printf("[KEY] 0x%02X\n", (unsigned char)ch); break;
                    }
                }
            }
        }
    }

    restoreTerminal();

    // ── Send DEVICE_EVENT DISCONNECT before exiting ───────────────────────
    dep.event = static_cast<uint8_t>(DeviceEvent::DISCONNECT);
    hdr.seq   = g_udp_seq++;
    memcpy(connect_pkt,               &hdr, sizeof(hdr));
    memcpy(connect_pkt + sizeof(hdr), &dep, sizeof(dep));
    sendto(g_udp_fd, connect_pkt, sizeof(connect_pkt), 0,
           reinterpret_cast<const sockaddr*>(&g_daemon_addr), sizeof(g_daemon_addr));
    printf("[test] sent DEVICE_EVENT DISCONNECT\n");

    close(g_udp_fd);
    close(epfd);
    printf("[test] done\n");
    return 0;
}

