#pragma once

/*
 * Protocol.h — shared wire protocol between the WirelessHub daemon (C++) and
 * the ESP32 firmware (C).  The file is valid in both C11 and C++11 modes.
 *
 * All multi-byte fields on the wire are little-endian.
 */

#ifdef __cplusplus
#  include <cstdint>
#  include <cstddef>
#else
#  include <stdint.h>
#  include <stddef.h>
#endif

/* ── Network constants ───────────────────────────────────────────────────── */
#define DAEMON_PORT       ((uint16_t)7788)   /* daemon listens here           */
#define DEVICE_BASE_PORT  ((uint16_t)7789)   /* device_id N → port 7789+N     */
#define MAX_PACKET        ((size_t)65542)    /* 65536 data + 6 header bytes   */
#define MTU_PAYLOAD       ((size_t)1400)     /* max payload per UDP datagram  */

/* ── CmdType values (use CMD_* in C; CmdType:: enum class available in C++) */
#define CMD_DISCOVER        ((uint8_t)0x01)  /* device → daemon: broadcast, no payload */
#define CMD_DISCOVER_REPLY  ((uint8_t)0x02)  /* daemon → device: unicast reply         */
#define CMD_DEVICE_EVENT    ((uint8_t)0x10)  /* device connected / disconnected        */
#define CMD_OPTIMIZED_DATA  ((uint8_t)0x20)
#define CMD_RAW_DATA        ((uint8_t)0x30)  /* single-datagram USB/IP pass-through    */
#define CMD_RAW_FRAG        ((uint8_t)0x31)  /* fragmented USB/IP (payload > MTU)      */
#define CMD_ACK             ((uint8_t)0x40)
#define CMD_LOG             ((uint8_t)0x50)  /* device → daemon: ESP log line forwarding  */
#define CMD_ERROR           ((uint8_t)0xFF)

/* ── LogLevel values (payload byte 0 of CMD_LOG) ────────────────────────── */
#define LOG_LEVEL_ERROR    ((uint8_t)0x01)  /* 'E' — ESP_LOGE */
#define LOG_LEVEL_WARN     ((uint8_t)0x02)  /* 'W' — ESP_LOGW */
#define LOG_LEVEL_INFO     ((uint8_t)0x03)  /* 'I' — ESP_LOGI */
#define LOG_LEVEL_DEBUG    ((uint8_t)0x04)  /* 'D' — ESP_LOGD */
#define LOG_LEVEL_VERBOSE  ((uint8_t)0x05)  /* 'V' — ESP_LOGV */

/* ── DeviceId values ─────────────────────────────────────────────────────── */
#define DEVICE_ID_USB0      ((uint8_t)0x00)
#define DEVICE_ID_USB1      ((uint8_t)0x01)
#define DEVICE_ID_USB2      ((uint8_t)0x02)
#define DEVICE_ID_USB3      ((uint8_t)0x03)
#define DEVICE_ID_ETHERNET  ((uint8_t)0x04)

/* ── DeviceEvent values ──────────────────────────────────────────────────── */
#define DEVICE_EVENT_CONNECT    ((uint8_t)0x01)
#define DEVICE_EVENT_DISCONNECT ((uint8_t)0x02)

/* ── UsbSpeed values ─────────────────────────────────────────────────────── */
#define USB_SPEED_LOW   ((uint8_t)0x01)
#define USB_SPEED_FULL  ((uint8_t)0x02)
#define USB_SPEED_HIGH  ((uint8_t)0x03)

/* ── Packed wire structs (C and C++) ─────────────────────────────────────── */

/* 6-byte packet header */
typedef struct {
    uint8_t  cmd_type;
    uint8_t  device_id;    /* DEVICE_ID_* */
    uint8_t  endpoint;
    uint8_t  seq;
    uint16_t payload_len;
} __attribute__((packed)) Header;

/* Payload for CMD_DISCOVER_REPLY — daemon sends this unicast back.
 * ESP learns daemon IP from UDP sender address; daemon_port is redundant
 * but kept for completeness (equals DAEMON_PORT). */
typedef struct {
    uint16_t daemon_port;  /* little-endian */
} __attribute__((packed)) DiscoverReplyPayload;

/* Payload for CMD_DEVICE_EVENT */
typedef struct {
    uint8_t  device_id;   /* mirrors Header::device_id  */
    uint8_t  event;       /* DEVICE_EVENT_*             */
    uint8_t  speed;       /* USB_SPEED_*                */
    uint8_t  usb_class;
    uint8_t  subclass;
    uint8_t  protocol;
    uint16_t reply_port;  /* UDP port on device side for daemon responses */
} __attribute__((packed)) DeviceEventPayload;

/* Follows the 6-byte Header when cmd_type == CMD_RAW_FRAG */
typedef struct {
    uint16_t transfer_seq; /* rolling counter per device identifying the transfer */
    uint8_t  frag_idx;     /* 0-based index of this fragment                      */
    uint8_t  frag_total;   /* total number of fragments for this transfer         */
} __attribute__((packed)) FragHeader;

/* Payload for CMD_LOG — device → daemon log forwarding.
 * The fixed byte `log_level` is followed immediately by a NUL-terminated
 * (or length-delimited via Header::payload_len) UTF-8 log string.
 * Total payload is always ≤ MTU_PAYLOAD so it fits in one datagram. */
typedef struct {
    uint8_t log_level;   /* LOG_LEVEL_* */
    /* char msg[]; — variable-length, read via payload_len - 1 */
} __attribute__((packed)) LogPayload;

/* ── Static assertions ───────────────────────────────────────────────────── */
#ifdef __cplusplus
  static_assert(sizeof(Header)               == 6, "Header must be 6 bytes");
  static_assert(sizeof(DiscoverReplyPayload)  == 2, "DiscoverReplyPayload must be 2 bytes");
  static_assert(sizeof(DeviceEventPayload)    == 8, "DeviceEventPayload must be 8 bytes");
  static_assert(sizeof(FragHeader)            == 4, "FragHeader must be 4 bytes");
  static_assert(sizeof(LogPayload)            == 1, "LogPayload must be 1 byte");
#else
  _Static_assert(sizeof(Header)               == 6, "Header must be 6 bytes");
  _Static_assert(sizeof(DiscoverReplyPayload)  == 2, "DiscoverReplyPayload must be 2 bytes");
  _Static_assert(sizeof(DeviceEventPayload)    == 8, "DeviceEventPayload must be 8 bytes");
  _Static_assert(sizeof(FragHeader)            == 4, "FragHeader must be 4 bytes");
  _Static_assert(sizeof(LogPayload)            == 1, "LogPayload must be 1 byte");
#endif

/* ── C++ extras: enum class wrappers and makeAddr() helper ──────────────── */
#ifdef __cplusplus
#  include <netinet/in.h>
#  include <arpa/inet.h>

enum class CmdType : uint8_t {
    DISCOVER        = CMD_DISCOVER,
    DISCOVER_REPLY  = CMD_DISCOVER_REPLY,
    DEVICE_EVENT    = CMD_DEVICE_EVENT,
    OPTIMIZED_DATA  = CMD_OPTIMIZED_DATA,
    RAW_DATA        = CMD_RAW_DATA,
    RAW_FRAG        = CMD_RAW_FRAG,
    ACK             = CMD_ACK,
    LOG             = CMD_LOG,
    ERROR           = CMD_ERROR
};

enum class DeviceId : uint8_t {
    USB0     = DEVICE_ID_USB0,
    USB1     = DEVICE_ID_USB1,
    USB2     = DEVICE_ID_USB2,
    USB3     = DEVICE_ID_USB3,
    ETHERNET = DEVICE_ID_ETHERNET
};

enum class DeviceEvent : uint8_t {
    CONNECT    = DEVICE_EVENT_CONNECT,
    DISCONNECT = DEVICE_EVENT_DISCONNECT
};

enum class UsbSpeed : uint8_t {
    LOW  = USB_SPEED_LOW,
    FULL = USB_SPEED_FULL,
    HIGH = USB_SPEED_HIGH
};

/* Build a sockaddr_in from a dotted-decimal IP string and port.
 * Pass nullptr or "" for INADDR_ANY. */
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

#endif /* __cplusplus */