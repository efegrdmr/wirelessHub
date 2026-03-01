// tests/WasdMouseTest.cpp
// Fake ESP32: connects to the daemon's Unix socket and identifies as a HID mouse.
// The daemon calls vhci.attach() with the accepted fd, so the kernel speaks
// USB/IP directly to this process.  WASD keys → cursor movement.
//
// Build: see CMakeLists.txt (target wasd_mouse_test)
// Run:   sudo ./build/wasd_mouse_test     (needs vhci + daemon already running)

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <csignal>
#include <cerrno>
#include <vector>
#include <arpa/inet.h>   // ntohl / htonl
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/epoll.h>
#include <sys/uio.h>     // writev
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>

// ── USB/IP wire structs (matches kernel docs) ──────────────────────────────

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

// HID Report Descriptor: 3-button relative mouse (52 bytes)
static const uint8_t HID_REPORT_DESC[] = {
    0x05, 0x01,  // Usage Page (Generic Desktop)
    0x09, 0x02,  // Usage (Mouse)
    0xA1, 0x01,  // Collection (Application)
    0x09, 0x01,  //   Usage (Pointer)
    0xA1, 0x00,  //   Collection (Physical)
    // Buttons
    0x05, 0x09,  //     Usage Page (Button)
    0x19, 0x01,  //     Usage Minimum (1)
    0x29, 0x03,  //     Usage Maximum (3)
    0x15, 0x00,  //     Logical Minimum (0)
    0x25, 0x01,  //     Logical Maximum (1)
    0x95, 0x03,  //     Report Count (3)
    0x75, 0x01,  //     Report Size (1)
    0x81, 0x02,  //     Input (Data, Variable, Absolute)
    // Padding (5 bits)
    0x95, 0x01,  //     Report Count (1)
    0x75, 0x05,  //     Report Size (5)
    0x81, 0x03,  //     Input (Constant)
    // X, Y, Wheel (relative, 8-bit signed)
    0x05, 0x01,  //     Usage Page (Generic Desktop)
    0x09, 0x30,  //     Usage (X)
    0x09, 0x31,  //     Usage (Y)
    0x09, 0x38,  //     Usage (Wheel)
    0x15, 0x81,  //     Logical Minimum (-127)
    0x25, 0x7F,  //     Logical Maximum (127)
    0x75, 0x08,  //     Report Size (8)
    0x95, 0x03,  //     Report Count (3)
    0x81, 0x06,  //     Input (Data, Variable, Relative)
    0xC0,        //   End Collection
    0xC0,        // End Collection
};
static constexpr uint8_t HID_REPORT_DESC_LEN = sizeof(HID_REPORT_DESC); // 52

// Device Descriptor (18 bytes) – class/subclass at interface level
static const uint8_t DEVICE_DESC[] = {
    0x12,        // bLength
    0x01,        // bDescriptorType (Device)
    0x00, 0x02,  // bcdUSB (2.00)
    0x00,        // bDeviceClass (0 = per-interface)
    0x00,        // bDeviceSubClass
    0x00,        // bDeviceProtocol
    0x40,        // bMaxPacketSize0 (64)
    0x34, 0x12,  // idVendor  (0x1234 – generic test)
    0x78, 0x56,  // idProduct (0x5678 – generic test)
    0x00, 0x01,  // bcdDevice (1.00)
    0x00,        // iManufacturer
    0x00,        // iProduct
    0x00,        // iSerialNumber
    0x01,        // bNumConfigurations
};

// Full Configuration Descriptor (9 + 9 + 9 + 7 = 34 bytes)
static const uint8_t CONFIG_DESC[] = {
    // Configuration Descriptor
    0x09,        // bLength
    0x02,        // bDescriptorType (Config)
    0x22, 0x00,  // wTotalLength (34)
    0x01,        // bNumInterfaces
    0x01,        // bConfigurationValue
    0x00,        // iConfiguration
    0xA0,        // bmAttributes (bus-powered, remote wakeup)
    0x32,        // bMaxPower (100 mA)
    // Interface Descriptor
    0x09,        // bLength
    0x04,        // bDescriptorType (Interface)
    0x00,        // bInterfaceNumber
    0x00,        // bAlternateSetting
    0x01,        // bNumEndpoints
    0x03,        // bInterfaceClass (HID)
    0x01,        // bInterfaceSubClass (Boot)
    0x02,        // bInterfaceProtocol (Mouse)
    0x00,        // iInterface
    // HID Descriptor
    0x09,        // bLength
    0x21,        // bDescriptorType (HID)
    0x11, 0x01,  // bcdHID (1.11)
    0x00,        // bCountryCode
    0x01,        // bNumDescriptors
    0x22,        // bDescriptorType for subordinate (Report)
    HID_REPORT_DESC_LEN, 0x00,  // wDescriptorLength (little-endian)
    // Endpoint Descriptor – Interrupt IN, endpoint 1
    0x07,        // bLength
    0x05,        // bDescriptorType (Endpoint)
    0x81,        // bEndpointAddress (IN | 1)
    0x03,        // bmAttributes (Interrupt)
    0x04, 0x00,  // wMaxPacketSize (4 bytes)
    0x0A,        // bInterval (10 ms)
};

// Language ID descriptor (String 0)
static const uint8_t STRING_LANG_DESC[] = {
    0x04,        // bLength
    0x03,        // bDescriptorType (String)
    0x09, 0x04,  // wLANGID[0] = 0x0409 (English US)
};

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
    case 0x01: return "DEVICE";
    case 0x02: return "CONFIG";
    case 0x03: return "STRING";
    case 0x04: return "INTERFACE";
    case 0x05: return "ENDPOINT";
    case 0x06: return "DEVICE_QUALIFIER";
    case 0x07: return "OTHER_SPEED_CONFIG";
    case 0x0F: return "BOS";
    case 0x21: return "HID";
    case 0x22: return "HID_REPORT";
    default:   return "?";
    }
}

static void logSetup(const uint8_t* s) {
    printf("  setup: %02X %02X %02X %02X %02X %02X %02X %02X"
           "  (bmReqType=%02X bReq=%02X wVal=%04X wIdx=%04X wLen=%u)\n",
           s[0],s[1],s[2],s[3],s[4],s[5],s[6],s[7],
           s[0], s[1],
           static_cast<uint16_t>(s[2] | (s[3]<<8)),
           static_cast<uint16_t>(s[4] | (s[5]<<8)),
           static_cast<uint16_t>(s[6] | (s[7]<<8)));
}

static void sendRetSubmit(int fd, uint32_t seqnum, int status,
                          const uint8_t* payload, uint32_t payload_len) {
    UsbipRetSubmit ret{};
    ret.basic.command   = htonl(USBIP_RET_SUBMIT);
    ret.basic.seqnum    = htonl(seqnum);
    ret.status          = htonl(static_cast<uint32_t>(status));
    ret.actual_length   = htonl(payload_len);

    // Log outgoing reply
    if (payload_len > 0 && payload) {
        printf("  <<< REPLY seq=%-4u status=%-4d len=%u  data:", seqnum, status, payload_len);
        for (uint32_t i = 0; i < payload_len && i < 16; ++i)
            printf(" %02X", payload[i]);
        if (payload_len > 16) printf(" ...");
        printf("\n");
    } else {
        printf("  <<< REPLY seq=%-4u status=%d (no payload)\n", seqnum, status);
    }
    fflush(stdout);

    // Send header + payload atomically with writev so the kernel sees one
    // complete USB/IP reply in a single TCP segment, avoiding partial-read issues.
    if (payload && payload_len > 0) {
        struct iovec iov[2];
        iov[0].iov_base = &ret;
        iov[0].iov_len  = sizeof(ret);
        iov[1].iov_base = const_cast<uint8_t*>(payload);
        iov[1].iov_len  = payload_len;
        if (writev(fd, iov, 2) < 0) perror("[test] writev");
    } else {
        if (write(fd, &ret, sizeof(ret)) < 0) perror("[test] write header");
    }
}

// Clip descriptor to the wLength requested (little-endian in setup[6,7])
static uint16_t getWLength(const uint8_t* setup) {
    return static_cast<uint16_t>(setup[6] | (setup[7] << 8));
}

static void handleControlIn(int fd, uint32_t seqnum, const uint8_t* setup) {
    uint8_t  req_type = setup[0];
    uint8_t  bRequest = setup[1];
    uint8_t  desc_idx = setup[2];
    uint8_t  desc_type= setup[3];
    uint16_t wLength  = getWLength(setup);

    // ── Standard GET_DESCRIPTOR ──────────────────────────────────────────
    if (bRequest == 0x06) {
        const uint8_t* desc     = nullptr;
        uint16_t       desc_len = 0;

        if (req_type == 0x80) {           // Standard, Device
            switch (desc_type) {
            case 0x01:                    // Device Descriptor
                desc = DEVICE_DESC;  desc_len = sizeof(DEVICE_DESC);  break;
            case 0x02:                    // Configuration Descriptor
                desc = CONFIG_DESC;  desc_len = sizeof(CONFIG_DESC);  break;
            case 0x03:                    // String Descriptor
                if (desc_idx == 0) { desc = STRING_LANG_DESC; desc_len = sizeof(STRING_LANG_DESC); break; }
                // String idx > 0: STALL (no strings defined)
                printf(">>> CTRL IN  seq=%-4u GET_DESCRIPTOR STRING idx=%u → STALL\n", seqnum, desc_idx);
                sendRetSubmit(fd, seqnum, -32, nullptr, 0);
                return;
            case 0x06:                    // Device Qualifier (USB 2.0 high-speed)
            case 0x07:                    // Other Speed Config
            case 0x0F:                    // BOS
                // Not supported on a full-speed device → STALL
                printf(">>> CTRL IN  seq=%-4u GET_DESCRIPTOR %s → STALL (not supported)\n",
                       seqnum, descTypeName(desc_type));
                sendRetSubmit(fd, seqnum, -32, nullptr, 0);
                return;
            default: break;
            }
        } else if (req_type == 0x81) {    // Standard, Interface
            if (desc_type == 0x22) {      // HID Report Descriptor
                desc = HID_REPORT_DESC;  desc_len = HID_REPORT_DESC_LEN;
            }
        }

        if (desc && desc_len > 0) {
            uint16_t send_len = (wLength < desc_len) ? wLength : desc_len;
            printf(">>> CTRL IN  seq=%-4u GET_DESCRIPTOR %s idx=%u wLen=%u → %u bytes\n",
                   seqnum, descTypeName(desc_type), desc_idx, wLength, send_len);
            sendRetSubmit(fd, seqnum, 0, desc, send_len);
            // Print a banner when HID Report Descriptor is sent — this is the
            // last enumeration step; after this the kernel loads the HID driver
            // and starts sending Interrupt IN polls on ep=1.
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
        // GET_DESCRIPTOR for unhandled type
        printf(">>> CTRL IN  seq=%-4u GET_DESCRIPTOR type=0x%02X(%s) idx=%u → STALL\n",
               seqnum, desc_type, descTypeName(desc_type), desc_idx);
        sendRetSubmit(fd, seqnum, -32, nullptr, 0);
        return;
    }

    // ── GET_CONFIGURATION ────────────────────────────────────────────────
    if (req_type == 0x80 && bRequest == 0x08) {
        printf(">>> CTRL IN  seq=%-4u GET_CONFIGURATION\n", seqnum);
        uint8_t val = 0x01;
        sendRetSubmit(fd, seqnum, 0, &val, 1);
        return;
    }

    // ── GET_INTERFACE ────────────────────────────────────────────────────
    if (req_type == 0x81 && bRequest == 0x0A) {
        printf(">>> CTRL IN  seq=%-4u GET_INTERFACE\n", seqnum);
        uint8_t val = 0x00;
        sendRetSubmit(fd, seqnum, 0, &val, 1);
        return;
    }

    // Unknown IN request – STALL
    printf(">>> CTRL IN  seq=%-4u UNKNOWN req_type=0x%02X bRequest=0x%02X → STALL\n",
           seqnum, req_type, bRequest);
    logSetup(setup);
    sendRetSubmit(fd, seqnum, -32, nullptr, 0);
}

static const char* outRequestName(uint8_t req_type, uint8_t bReq) {
    if (req_type == 0x00) {
        switch (bReq) {
        case 0x05: return "SET_ADDRESS";
        case 0x09: return "SET_CONFIGURATION";
        default:   break;
        }
    }
    if (req_type == 0x01) { if (bReq == 0x0B) return "SET_INTERFACE"; }
    if (req_type == 0x21) {
        switch (bReq) {
        case 0x09: return "HID_SET_REPORT";
        case 0x0A: return "HID_SET_IDLE";
        case 0x0B: return "HID_SET_PROTOCOL";
        default:   break;
        }
    }
    return "?";
}

static void handleControlOut(int fd, uint32_t seqnum, const uint8_t* setup) {
    uint8_t req_type = setup[0];
    uint8_t bRequest = setup[1];
    printf(">>> CTRL OUT seq=%-4u %s (req_type=0x%02X bReq=0x%02X) → ACK\n",
           seqnum, outRequestName(req_type, bRequest), req_type, bRequest);
    sendRetSubmit(fd, seqnum, 0, nullptr, 0);
}

static void handleInterruptIn(int fd, uint32_t seqnum) {
    // HID mouse report: [buttons, X, Y, wheel]
    uint8_t report[4] = {
        g_buttons,
        static_cast<uint8_t>(g_dx),
        static_cast<uint8_t>(g_dy),
        0x00
    };
    // Only log when there's actual movement to avoid flooding the terminal
    if (g_dx != 0 || g_dy != 0 || g_buttons != 0) {
        printf(">>> INT  IN  seq=%-4u ep=1 SENDING mouse report: btn=%u dx=%d dy=%d\n",
               seqnum, g_buttons,
               static_cast<int>(static_cast<int8_t>(g_dx)),
               static_cast<int>(static_cast<int8_t>(g_dy)));
        fflush(stdout);
    }
    g_dx = 0;
    g_dy = 0;
    sendRetSubmit(fd, seqnum, 0, report, sizeof(report));
}

// Process one raw USB/IP packet received on sock_fd
static void dispatchUsbip(int sock_fd, const uint8_t* buf, size_t len) {
    if (len < sizeof(UsbipCmdSubmit)) {
        printf("[test] packet too small (%zu), ignoring\n", len);
        return;
    }

    UsbipCmdSubmit cmd;
    memcpy(&cmd, buf, sizeof(cmd));

    uint32_t command   = ntohl(cmd.basic.command);
    uint32_t seqnum    = ntohl(cmd.basic.seqnum);
    uint32_t direction = ntohl(cmd.basic.direction);
    uint32_t ep        = ntohl(cmd.basic.ep);

    if (command == USBIP_CMD_UNLINK) {
        // The kernel wants to cancel a pending URB. We must reply with
        // RET_UNLINK or the kernel's USB subsystem will deadlock.
        uint32_t unlink_seq;
        memcpy(&unlink_seq, buf + 20, 4);  // bytes 20-23 = unlink_seqnum
        unlink_seq = ntohl(unlink_seq);
        printf("[UNLINK] seq=%-4u cancelling seq=%u → ACK\n", seqnum, unlink_seq);
        fflush(stdout);

        // RET_UNLINK: 48-byte reply, same layout as RET_SUBMIT but command=0x04
        uint8_t reply[48] = {};
        uint32_t cmd_be   = htonl(USBIP_RET_UNLINK);
        uint32_t seq_be   = htonl(seqnum);
        uint32_t status_be = htonl(0);
        memcpy(reply +  0, &cmd_be,    4);
        memcpy(reply +  4, &seq_be,    4);
        memcpy(reply + 20, &status_be, 4);
        if (write(sock_fd, reply, sizeof(reply)) < 0) perror("[test] write RET_UNLINK");
        return;
    }

    if (command != USBIP_CMD_SUBMIT) {
        printf("[test] unexpected command 0x%08X\n", command);
        return;
    }

    // Log every incoming submit
    if (ep == 0) {
        printf("\n>>> CTRL %s seq=%-4u ep=0\n",
               direction == USBIP_DIR_IN ? "IN " : "OUT", seqnum);
        logSetup(cmd.setup);
    } else {
        printf("\n>>> ep=%u %s seq=%-4u len=%u\n",
               ep, direction == USBIP_DIR_IN ? "IN " : "OUT",
               seqnum, ntohl(cmd.transfer_buffer_length));
    }

    if (ep == 0) {
        if (direction == USBIP_DIR_IN)
            handleControlIn(sock_fd, seqnum, cmd.setup);
        else
            handleControlOut(sock_fd, seqnum, cmd.setup);
    } else if (ep == 1 && direction == USBIP_DIR_IN) {
        handleInterruptIn(sock_fd, seqnum);
    } else {
        printf(">>> ep=%u dir=%u seq=%u → STALL (unhandled)\n", ep, direction, seqnum);
        sendRetSubmit(sock_fd, seqnum, -32, nullptr, 0);
    }
}

// ── main ───────────────────────────────────────────────────────────────────

int main() {
    struct sigaction sa{};
    sa.sa_handler = onSignal;
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // ── Connect to daemon ─────────────────────────────────────────────────
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) { perror("[test] socket"); return 1; }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, "/tmp/wirelesshub.sock", sizeof(addr.sun_path) - 1);

    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("[test] connect (is the daemon running?)");
        return 1;
    }
    printf("[test] connected to daemon - vhci attach will follow shortly\n");

    // ── Raw terminal (non-canonical, no echo) ─────────────────────────────
    tcgetattr(STDIN_FILENO, &g_old_termios);
    termios raw = g_old_termios;
    raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);

    // Make stdin non-blocking
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    printf("[test] WASD=move  Q=quit\n");
    printf("[test] W=up  S=down  A=left  D=right\n");

    // ── epoll: watch socket (kernel→us) + stdin (WASD) ───────────────────
    int epfd = epoll_create1(0);
    if (epfd < 0) { perror("[test] epoll_create1"); return 1; }

    auto addEpoll = [&](int fd, uint32_t events) {
        epoll_event ev{};
        ev.events  = events;
        ev.data.fd = fd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
    };

    addEpoll(sock,          EPOLLIN | EPOLLRDHUP);
    addEpoll(STDIN_FILENO,  EPOLLIN);

    // Large buffer: kernel may queue 16+ Interrupt IN URBs at once (each 48 B)
    uint8_t buf[65536];
    epoll_event events[4];

    while (g_running) {
        int n = epoll_wait(epfd, events, 4, 100);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("[test] epoll_wait");
            break;
        }

        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;

            // ── Daemon/kernel disconnected ───────────────────────────────
            if (fd == sock && (events[i].events & (EPOLLRDHUP | EPOLLERR | EPOLLHUP))) {
                printf("[test] daemon disconnected\n");
                g_running = false;
                break;
            }

            // ── USB/IP from kernel ────────────────────────────────────────
            if (fd == sock && (events[i].events & EPOLLIN)) {
                ssize_t len = read(sock, buf, sizeof(buf));
                if (len <= 0) { g_running = false; break; }

                // Loop: one read() may contain multiple back-to-back CMD_SUBMITs.
                // Each packet = 48-byte header + optional OUT payload.
                size_t offset = 0;
                size_t total  = static_cast<size_t>(len);
                while (offset + sizeof(UsbipCmdSubmit) <= total) {
                    UsbipCmdSubmit hdr;
                    memcpy(&hdr, buf + offset, sizeof(hdr));
                    uint32_t dir   = ntohl(hdr.basic.direction);
                    uint32_t extra = (dir == USBIP_DIR_OUT)
                                     ? ntohl(hdr.transfer_buffer_length) : 0;
                    size_t pkt = sizeof(UsbipCmdSubmit) + extra;
                    if (offset + pkt > total) {
                        printf("[test] incomplete packet at offset %zu, stopping\n", offset);
                        break;
                    }
                    dispatchUsbip(sock, buf + offset, pkt);
                    offset += pkt;
                }
            }

            // ── Keyboard (WASD) ───────────────────────────────────────────
            if (fd == STDIN_FILENO && (events[i].events & EPOLLIN)) {
                char ch = 0;
                if (read(STDIN_FILENO, &ch, 1) == 1) {
                    constexpr int8_t STEP = 8;
                    switch (ch) {
                    case 'w': case 'W':
                        g_dy -= STEP;
                        printf("[KEY] W  →  dy=%d (queued, sends on next INT IN poll)\n", (int)g_dy);
                        fflush(stdout);
                        break;
                    case 's': case 'S':
                        g_dy += STEP;
                        printf("[KEY] S  →  dy=%d\n", (int)g_dy);
                        fflush(stdout);
                        break;
                    case 'a': case 'A':
                        g_dx -= STEP;
                        printf("[KEY] A  →  dx=%d\n", (int)g_dx);
                        fflush(stdout);
                        break;
                    case 'd': case 'D':
                        g_dx += STEP;
                        printf("[KEY] D  →  dx=%d\n", (int)g_dx);
                        fflush(stdout);
                        break;
                    case 'q': case 'Q': case 3 /* Ctrl+C */:
                        g_running = false;
                        break;
                    default:
                        printf("[KEY] unknown key 0x%02X\n", (unsigned char)ch);
                        fflush(stdout);
                        break;
                    }
                }
            }
        }
    }

    restoreTerminal();
    close(sock);
    close(epfd);
    printf("[test] done\n");
    return 0;
}
