#pragma once
#include <cstdint>
#include <cstddef>
#include <netinet/in.h>
#include <arpa/inet.h>

// ── Network constants ─────────────────────────────────────────────────────────
static constexpr uint16_t DAEMON_PORT      = 7788;   // daemon listens here
static constexpr uint16_t DEVICE_BASE_PORT = 7789;   // device_id N uses port 7789+N
static constexpr size_t   MAX_PACKET       = 65536 + 6; // 6 = sizeof(Header)
static constexpr size_t   MTU_PAYLOAD      = 1400;       // max payload per UDP datagram

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

// Payload for CmdType::DISCOVER_REPLY
// Daemon sends this unicast to the device that issued a DISCOVER broadcast.
// Device learns daemon IP from UDP sender address; port is here for completeness.
struct DiscoverReplyPayload
{
    uint16_t daemon_port;  // DAEMON_PORT, little-endian
} __attribute__((packed));

static_assert(sizeof(DiscoverReplyPayload) == 2, "DiscoverReplyPayload must be 2 bytes");

enum class CmdType : uint8_t {
    DISCOVER         = 0x01,  // device → daemon: broadcast discovery request (no payload)
    DISCOVER_REPLY   = 0x02,  // daemon → device: unicast reply
    DEVICE_EVENT     = 0x10,  // device connected/disconnected
    OPTIMIZED_DATA   = 0x20,
    RAW_DATA         = 0x30,  // single-datagram USB/IP pass-through
    RAW_FRAG         = 0x31,  // fragmented USB/IP pass-through (payload > MTU_PAYLOAD)
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

// Follows the 6-byte Header when cmd_type == RAW_FRAG.
// Allows reassembly of USB/IP transfers larger than MTU_PAYLOAD.
struct FragHeader
{
    uint16_t transfer_seq; // per-device rolling counter identifying the transfer
    uint8_t  frag_idx;     // 0-based index of this fragment
    uint8_t  frag_total;   // total number of fragments for this transfer
} __attribute__((packed));

static_assert(sizeof(FragHeader) == 4, "FragHeader must be 4 bytes");