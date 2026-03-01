#include "UsbipParser.h"
#include <cstring>
#include <cstdio>
#include <arpa/inet.h>  // ntohl / htonl

// ── Helpers ────────────────────────────────────────────────────────────────

uint32_t UsbipParser::peekCommand(const uint8_t* buf, size_t buf_len) {
    if (buf_len < 4) return 0;
    uint32_t cmd;
    memcpy(&cmd, buf, 4);
    return ntohl(cmd);
}

// ── URB messages ───────────────────────────────────────────────────────────

bool UsbipParser::parseSubmit(const uint8_t* buf, size_t buf_len, UsbRequest& out_req, uint32_t& out_seq) {
    if (buf_len < sizeof(UsbipCmdSubmit)) {
        printf("[UsbipParser] buffer too small: %zu bytes (expected >= %zu)\n",
               buf_len, sizeof(UsbipCmdSubmit));
        return false;
    }

    UsbipCmdSubmit cmd;
    memcpy(&cmd, buf, sizeof(UsbipCmdSubmit));

    // convert all uint32_t fields from big-endian (network) to host byte order
    uint32_t command   = ntohl(cmd.basic.command);
    uint32_t seqnum    = ntohl(cmd.basic.seqnum);
    uint32_t direction = ntohl(cmd.basic.direction);
    uint32_t ep        = ntohl(cmd.basic.ep);
    uint32_t buf_len32 = ntohl(cmd.transfer_buffer_length);

    if (command != USBIP_CMD_SUBMIT) {
        printf("[UsbipParser] parseSubmit: unexpected command: 0x%08X\n", command);
        return false;
    }

    out_seq          = seqnum;
    out_req.endpoint = static_cast<uint8_t>(ep);
    out_req.urb_type = static_cast<uint8_t>(direction);
    memcpy(out_req.setup, cmd.setup, 8);
    out_req.data.clear();

    // payload is only present for DIR_OUT requests
    if (direction == USBIP_DIR_OUT && buf_len32 > 0) {
        size_t data_offset = sizeof(UsbipCmdSubmit);
        if (data_offset + buf_len32 > buf_len) {
            printf("[UsbipParser] transfer_buffer_length %u exceeds buffer\n", buf_len32);
            return false;
        }
        out_req.data.assign(buf + data_offset, buf + data_offset + buf_len32);
    }

    return true;
}

std::vector<uint8_t> UsbipParser::serializeSubmitReply(const UsbResponse& res, uint32_t seq_num, uint32_t direction) {
    UsbipRetSubmit ret{};

    ret.basic.command     = htonl(USBIP_RET_SUBMIT);
    ret.basic.seqnum      = htonl(seq_num);
    ret.basic.devid       = 0;
    ret.basic.direction   = 0;
    ret.basic.ep          = 0;
    ret.status            = htonl(static_cast<uint32_t>(res.status));
    ret.actual_length     = htonl(static_cast<uint32_t>(res.data.size()));
    ret.start_frame       = 0;
    ret.number_of_packets = htonl(0xffffffff);
    ret.error_count       = 0;

    std::vector<uint8_t> out(sizeof(UsbipRetSubmit));
    memcpy(out.data(), &ret, sizeof(UsbipRetSubmit));

    // payload is only sent for DIR_IN responses
    if (direction == USBIP_DIR_IN && !res.data.empty())
        out.insert(out.end(), res.data.begin(), res.data.end());

    return out;
}

bool UsbipParser::parseUnlink(const uint8_t* buf, size_t buf_len,
                               uint32_t& out_seq, uint32_t& out_unlink_seq) {
    if (buf_len < sizeof(UsbipCmdUnlink)) {
        printf("[UsbipParser] parseUnlink: buffer too small: %zu bytes\n", buf_len);
        return false;
    }
    UsbipCmdUnlink cmd;
    memcpy(&cmd, buf, sizeof(UsbipCmdUnlink));
    out_seq        = ntohl(cmd.basic.seqnum);
    out_unlink_seq = ntohl(cmd.unlink_seqnum);
    return true;
}

std::vector<uint8_t> UsbipParser::serializeUnlinkReply(uint32_t seq_num, uint32_t status) {
    UsbipRetUnlink ret{};
    ret.basic.command  = htonl(USBIP_RET_UNLINK);
    ret.basic.seqnum   = htonl(seq_num);
    ret.status         = htonl(status);

    std::vector<uint8_t> out(sizeof(UsbipRetUnlink));
    memcpy(out.data(), &ret, sizeof(UsbipRetUnlink));
    return out;
}

// ── OP handshake messages ───────────────────────────────────────────────────

bool UsbipParser::parseImportRequest(const uint8_t* buf, size_t buf_len,
                                     std::string& out_busid) {
    if (buf_len < sizeof(OpReqImport)) {
        printf("[UsbipParser] parseImportRequest: buffer too small: %zu bytes\n", buf_len);
        return false;
    }
    OpReqImport req;
    memcpy(&req, buf, sizeof(OpReqImport));

    if (ntohs(req.op_code) != OP_REQ_IMPORT) {
        printf("[UsbipParser] parseImportRequest: unexpected op_code: 0x%04X\n", ntohs(req.op_code));
        return false;
    }
    // busid must be null-terminated within its 32-byte field
    req.busid[31] = '\0';
    out_busid = req.busid;
    return true;
}

std::vector<uint8_t> UsbipParser::serializeImportReply(const UsbDeviceInfo& dev) {
    // header (8 bytes) + path(256) + busid(32) + fields = 312 bytes total
    struct __attribute__((packed)) {
        uint16_t version;
        uint16_t op_code;
        uint32_t status;
        char     path[256];
        char     busid[32];
        uint32_t busnum;
        uint32_t devnum;
        uint32_t speed;
        uint16_t idVendor;
        uint16_t idProduct;
        uint16_t bcdDevice;
        uint8_t  bDeviceClass;
        uint8_t  bDeviceSubClass;
        uint8_t  bDeviceProtocol;
        uint8_t  bConfigurationValue;
        uint8_t  bNumConfigurations;
        uint8_t  bNumInterfaces;
    } rep{};

    rep.version  = htons(USBIP_VERSION);
    rep.op_code  = htons(OP_REP_IMPORT);
    rep.status   = 0;

    strncpy(rep.path,  dev.path.c_str(),  sizeof(rep.path)  - 1);
    strncpy(rep.busid, dev.busid.c_str(), sizeof(rep.busid) - 1);

    rep.busnum             = htonl(dev.busnum);
    rep.devnum             = htonl(dev.devnum);
    rep.speed              = htonl(dev.speed);
    rep.idVendor           = htons(dev.idVendor);
    rep.idProduct          = htons(dev.idProduct);
    rep.bcdDevice          = htons(dev.bcdDevice);
    rep.bDeviceClass       = dev.bDeviceClass;
    rep.bDeviceSubClass    = dev.bDeviceSubClass;
    rep.bDeviceProtocol    = dev.bDeviceProtocol;
    rep.bConfigurationValue = dev.bConfigurationValue;
    rep.bNumConfigurations = dev.bNumConfigurations;
    rep.bNumInterfaces     = dev.bNumInterfaces;

    std::vector<uint8_t> out(sizeof(rep));
    memcpy(out.data(), &rep, sizeof(rep));
    return out;
}

std::vector<uint8_t> UsbipParser::serializeImportError(uint32_t status) {
    struct __attribute__((packed)) { uint16_t version; uint16_t op_code; uint32_t status; } rep{};
    rep.version = htons(USBIP_VERSION);
    rep.op_code = htons(OP_REP_IMPORT);
    rep.status  = htonl(status);

    std::vector<uint8_t> out(sizeof(rep));
    memcpy(out.data(), &rep, sizeof(rep));
    return out;
}

bool UsbipParser::parseDevlistRequest(const uint8_t* buf, size_t buf_len) {
    if (buf_len < sizeof(OpReqDevlist)) {
        printf("[UsbipParser] parseDevlistRequest: buffer too small: %zu bytes\n", buf_len);
        return false;
    }
    OpReqDevlist req;
    memcpy(&req, buf, sizeof(req));
    if (ntohs(req.op_code) != OP_REQ_DEVLIST) {
        printf("[UsbipParser] parseDevlistRequest: unexpected op_code: 0x%04X\n", ntohs(req.op_code));
        return false;
    }
    return true;
}

std::vector<uint8_t> UsbipParser::serializeDevlistReply(const std::vector<UsbDeviceInfo>& devices) {
    // fixed header: version(2) + op_code(2) + status(4) + num_devices(4) = 12 bytes
    std::vector<uint8_t> out;
    out.reserve(12 + devices.size() * 0x138);

    auto push16 = [&](uint16_t v) {
        v = htons(v);
        out.insert(out.end(), reinterpret_cast<uint8_t*>(&v),
                              reinterpret_cast<uint8_t*>(&v) + 2);
    };
    auto push32 = [&](uint32_t v) {
        v = htonl(v);
        out.insert(out.end(), reinterpret_cast<uint8_t*>(&v),
                              reinterpret_cast<uint8_t*>(&v) + 4);
    };
    auto pushStr = [&](const std::string& s, size_t field_len) {
        size_t copy_len = std::min(s.size(), field_len - 1);
        out.insert(out.end(), s.begin(), s.begin() + copy_len);
        out.insert(out.end(), field_len - copy_len, 0x00);
    };
    auto push8 = [&](uint8_t v) { out.push_back(v); };

    push16(USBIP_VERSION);
    push16(OP_REP_DEVLIST);
    push32(0);                                         // status: OK
    push32(static_cast<uint32_t>(devices.size()));    // num_devices

    for (const auto& dev : devices) {
        pushStr(dev.path,  256);
        pushStr(dev.busid, 32);
        push32(dev.busnum);
        push32(dev.devnum);
        push32(dev.speed);
        push16(dev.idVendor);
        push16(dev.idProduct);
        push16(dev.bcdDevice);
        push8(dev.bDeviceClass);
        push8(dev.bDeviceSubClass);
        push8(dev.bDeviceProtocol);
        push8(dev.bConfigurationValue);
        push8(dev.bNumConfigurations);
        push8(dev.bNumInterfaces);
        // one interface entry per interface (4 bytes each) — zeroed as placeholder
        for (uint8_t i = 0; i < dev.bNumInterfaces; ++i) {
            push8(0); push8(0); push8(0); push8(0); // class, subclass, protocol, padding
        }
    }

    return out;
}
