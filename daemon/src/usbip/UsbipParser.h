#pragma once

#include "core/IUsbRequestHandler.h"
#include <cstdint>
#include <vector>
#include <string>

// ── Protocol version ────────────────────────────────────────────────────────
static constexpr uint16_t USBIP_VERSION     = 0x0111;

// ── URB command codes (big-endian / network byte order) ─────────────────────
static constexpr uint32_t USBIP_CMD_SUBMIT  = 0x00000001;
static constexpr uint32_t USBIP_CMD_UNLINK  = 0x00000002;
static constexpr uint32_t USBIP_RET_SUBMIT  = 0x00000003;
static constexpr uint32_t USBIP_RET_UNLINK  = 0x00000004;

// ── OP codes (TCP handshake before URB traffic) ──────────────────────────────
static constexpr uint16_t OP_REQ_DEVLIST    = 0x8005;
static constexpr uint16_t OP_REP_DEVLIST    = 0x0005;
static constexpr uint16_t OP_REQ_IMPORT     = 0x8003;
static constexpr uint16_t OP_REP_IMPORT     = 0x0003;

// ── Transfer direction ───────────────────────────────────────────────────────
static constexpr uint32_t USBIP_DIR_OUT     = 0x00000000;
static constexpr uint32_t USBIP_DIR_IN      = 0x00000001;

// ── usbip_header_basic — shared by all URB messages (20 bytes) ──────────────
struct UsbipHeaderBasic {
    uint32_t command;    // command code
    uint32_t seqnum;     // sequence number
    uint32_t devid;      // (busnum << 16) | devnum; 0 in server responses
    uint32_t direction;  // USBIP_DIR_OUT / USBIP_DIR_IN; 0 in server responses
    uint32_t ep;         // endpoint number; 0 in server responses and UNLINK
} __attribute__((packed));

// ── USBIP_CMD_SUBMIT — full header 48 bytes, transfer_buffer follows ─────────
struct UsbipCmdSubmit {
    UsbipHeaderBasic basic;
    uint32_t transfer_flags;
    uint32_t transfer_buffer_length;
    uint32_t start_frame;        // 0 if not ISO
    uint32_t number_of_packets;  // 0xffffffff if not ISO
    uint32_t interval;
    uint8_t  setup[8];           // control transfer setup packet; zeros otherwise
    // followed by: transfer_buffer (transfer_buffer_length bytes if DIR_OUT)
} __attribute__((packed));

// ── USBIP_RET_SUBMIT — full header 48 bytes, transfer_buffer follows ─────────
struct UsbipRetSubmit {
    UsbipHeaderBasic basic;
    uint32_t status;              // 0 = success
    uint32_t actual_length;       // actual bytes transferred
    uint32_t start_frame;         // 0 if not ISO
    uint32_t number_of_packets;   // 0xffffffff if not ISO
    uint32_t error_count;
    uint8_t  padding[8];          // shall be 0
    // followed by: transfer_buffer (actual_length bytes if DIR_IN)
} __attribute__((packed));

// ── USBIP_CMD_UNLINK — fixed 48 bytes ────────────────────────────────────────
struct UsbipCmdUnlink {
    UsbipHeaderBasic basic;
    uint32_t unlink_seqnum;  // seqnum of the SUBMIT to cancel
    uint8_t  padding[24];    // shall be 0
} __attribute__((packed));

// ── USBIP_RET_UNLINK — fixed 48 bytes ────────────────────────────────────────
struct UsbipRetUnlink {
    UsbipHeaderBasic basic;
    uint32_t status;         // -ECONNRESET if unlink successful; 0 if already done
    uint8_t  padding[24];    // shall be 0
} __attribute__((packed));

// ── OP_REQ_DEVLIST — client requests device list (8 bytes) ───────────────────
struct OpReqDevlist {
    uint16_t version;   // USBIP_VERSION
    uint16_t op_code;   // OP_REQ_DEVLIST
    uint32_t status;    // unused, shall be 0
} __attribute__((packed));

// ── OP_REQ_IMPORT — client requests to import a device (40 bytes) ────────────
struct OpReqImport {
    uint16_t version;   // USBIP_VERSION
    uint16_t op_code;   // OP_REQ_IMPORT
    uint32_t status;    // unused, shall be 0
    char     busid[32]; // e.g. "1-1", null-terminated, unused bytes zeroed
} __attribute__((packed));

// ── Device descriptor info used in OP_REP_DEVLIST and OP_REP_IMPORT ──────────
struct UsbDeviceInfo {
    std::string busid;
    std::string path;             // sysfs path, e.g. "/sys/devices/..."
    uint32_t    busnum;
    uint32_t    devnum;
    uint32_t    speed;
    uint16_t    idVendor;
    uint16_t    idProduct;
    uint16_t    bcdDevice;
    uint8_t     bDeviceClass;
    uint8_t     bDeviceSubClass;
    uint8_t     bDeviceProtocol;
    uint8_t     bConfigurationValue;
    uint8_t     bNumConfigurations;
    uint8_t     bNumInterfaces;
};

class UsbipParser {
public:
    // ── Helpers ──────────────────────────────────────────────────────────────

    // Read the command/op_code from the first 4 bytes without a full parse
    static uint32_t peekCommand(const uint8_t* buf, size_t buf_len);

    // ── URB messages ─────────────────────────────────────────────────────────

    // Parse USBIP_CMD_SUBMIT into a UsbRequest; returns false on failure
    static bool parseSubmit(const uint8_t* buf, size_t buf_len,
                            UsbRequest& out_req, uint32_t& out_seq);

    // Parse USBIP_CMD_UNLINK; fills out_seq (of UNLINK) and out_unlink_seq (target)
    static bool parseUnlink(const uint8_t* buf, size_t buf_len,
                            uint32_t& out_seq, uint32_t& out_unlink_seq);

    // Wrap a UsbResponse into a USBIP_RET_SUBMIT packet
    static std::vector<uint8_t> serializeSubmitReply(const UsbResponse& res,
                                                     uint32_t seq_num,
                                                     uint32_t direction);

    // Build a USBIP_RET_UNLINK packet
    static std::vector<uint8_t> serializeUnlinkReply(uint32_t seq_num, uint32_t status);

    // ── OP handshake messages ─────────────────────────────────────────────────

    // Parse OP_REQ_IMPORT; fills out_busid, returns false on failure
    static bool parseImportRequest(const uint8_t* buf, size_t buf_len,
                                   std::string& out_busid);

    // Build an OP_REP_IMPORT success response
    static std::vector<uint8_t> serializeImportReply(const UsbDeviceInfo& dev);

    // Build an OP_REP_IMPORT error response
    static std::vector<uint8_t> serializeImportError(uint32_t status);

    // Parse OP_REQ_DEVLIST; validates the header, returns false on failure
    static bool parseDevlistRequest(const uint8_t* buf, size_t buf_len);

    // Build an OP_REP_DEVLIST response for a list of devices
    static std::vector<uint8_t> serializeDevlistReply(const std::vector<UsbDeviceInfo>& devices);
};
