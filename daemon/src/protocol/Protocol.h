#pragma once
#include <cstdint>
#include <cstddef>
#include <netinet/in.h>
#include <arpa/inet.h>

// ── Network constants ─────────────────────────────────────────────────────────
static constexpr uint16_t DAEMON_PORT      = 7788;   // daemon listens here
static constexpr uint16_t DEVICE_BASE_PORT = 7789;   // device_id N uses port 7789+N
static constexpr size_t   MAX_PACKET       = 65536 + 6; // 6 = sizeof(Header)

// Build a sockaddr_in from a dotted-decimal IP string and port.
// Pass nullptr or "" for INADDR_ANY.
inline sockaddr_in makeAddr(const char* ip, uint16_t port)
{
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (ip == nullptr || ip[0] == '\0')
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    else
        ::inet_pton(AF_INET, ip, &addr.sin_addr);
    return addr;
}

// 6-byte packet header (all multi-byte fields are little-endian)
struct Header
{
    uint8_t  cmd_type;
    uint8_t  device_id;    // 0x00-0x03 = USB ports 0-3, 0x04 = Ethernet
    uint8_t  endpoint;
    uint8_t  seq;
    uint16_t payload_len;
} __attribute__((packed));

static_assert(sizeof(Header) == 6, "Header must be 6 bytes");

enum class CmdType : uint8_t {
    DEVICE_EVENT     = 0x10,  // device connected/disconnected
    OPTIMIZED_DATA   = 0x20,
    RAW_DATA         = 0x30,
    ACK              = 0x40,
    ERROR            = 0xFF
};

// device_id constants
enum class DeviceId : uint8_t {
    USB0     = 0x00,
    USB1     = 0x01,
    USB2     = 0x02,
    USB3     = 0x03,
    ETHERNET = 0x04
};

enum class DeviceEvent : uint8_t {
    CONNECT    = 0x01,
    DISCONNECT = 0x02
};

enum class UsbSpeed : uint8_t {
    LOW  = 0x01,
    FULL = 0x02,
    HIGH = 0x03
};

// Payload for CmdType::DEVICE_EVENT
struct DeviceEventPayload
{
    uint8_t  device_id;   // mirrors Header::device_id
    uint8_t  event;       // DeviceEvent
    uint8_t  speed;       // UsbSpeed
    uint8_t  usb_class;
    uint8_t  subclass;
    uint8_t  protocol;
    uint16_t reply_port;  // UDP port on device side for responses
} __attribute__((packed));

static_assert(sizeof(DeviceEventPayload) == 8, "DeviceEventPayload must be 8 bytes");