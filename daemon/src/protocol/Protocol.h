#pragma once
#include <cstdint>

struct Header
{
    uint8_t cmd_type;
    uint8_t endpoint;
    uint8_t seq;
    uint16_t payload_len;
} __attribute__((packed));

enum class CmdType : uint8_t {
    DEVICE_EVENT     = 0x10,  // device is connected/disconnected
    OPTIMIZED_DATA   = 0x20,
    RAW_DATA         = 0x30,         
    ACK              = 0x40,  
    ERROR            = 0xFF  
};