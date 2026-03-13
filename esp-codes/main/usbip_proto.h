#pragma once

/*
 * usbip_proto.h — Device-side USB/IP wire structures (ESP firmware).
 *
 * All multi-byte fields on the USB/IP wire are BIG-endian.
 * ESP32 is little-endian, so all fields must be byte-swapped on read/write.
 * Use the USBIP_U32/U16 macros to convert.
 *
 * References:
 *   - Linux kernel: tools/usb/usbip/usbip_protocol.txt
 *   - drivers/usb/usbip/usbip_common.h
 */

#include <stdint.h>
#include <stddef.h>

/* ── Endian helpers ──────────────────────────────────────────────────────── */
/*
 * USB/IP uses network (big-endian) byte order.
 * These macros convert between host (little-endian) and wire order.
 * They are symmetric: host↔wire in both directions.
 */
#define USBIP_U32(x)  (__builtin_bswap32((uint32_t)(x)))
#define USBIP_U16(x)  (__builtin_bswap16((uint16_t)(x)))

/* ── Command / return codes ──────────────────────────────────────────────── */
#define USBIP_CMD_SUBMIT   ((uint32_t)0x00000001)
#define USBIP_CMD_UNLINK   ((uint32_t)0x00000002)
#define USBIP_RET_SUBMIT   ((uint32_t)0x00000003)
#define USBIP_RET_UNLINK   ((uint32_t)0x00000004)

/* ── Direction ───────────────────────────────────────────────────────────── */
#define USBIP_DIR_OUT  0u
#define USBIP_DIR_IN   1u

/* ── CMD_SUBMIT (host → device, 48 bytes) ────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t command;               /* USBIP_CMD_SUBMIT in BE             */
    uint32_t seqnum;                /* unique sequence number in BE       */
    uint32_t devid;                 /* (busnum<<16)|devnum in BE          */
    uint32_t direction;             /* USBIP_DIR_OUT / IN in BE           */
    uint32_t ep;                    /* endpoint number (0-15) in BE       */

    uint32_t transfer_flags;        /* URB transfer_flags in BE           */
    uint32_t transfer_buffer_length;/* bytes expected / to send in BE     */
    uint32_t start_frame;           /* for ISO; 0 for other types in BE   */
    uint32_t number_of_packets;     /* for ISO; 0xFFFFFFFF otherwise in BE*/
    uint32_t interval;              /* polling interval in BE             */

    uint8_t  setup[8];              /* setup packet for EP0; zeros otherwise */
    /* followed by transfer_buffer_length bytes of OUT data if direction==OUT */
} usbip_cmd_submit_t;

/* ── RET_SUBMIT (device → host, 48 bytes) ────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t command;               /* USBIP_RET_SUBMIT in BE             */
    uint32_t seqnum;                /* echoes cmd_submit.seqnum in BE     */
    uint32_t devid;                 /* 0 in BE                            */
    uint32_t direction;             /* echoes cmd_submit.direction in BE  */
    uint32_t ep;                    /* echoes cmd_submit.ep in BE         */

    int32_t  status;                /* 0=success, negative errno in BE    */
    uint32_t actual_length;         /* bytes actually transferred in BE   */
    uint32_t start_frame;           /* 0 in BE                            */
    uint32_t number_of_packets;     /* 0xFFFFFFFF for non-ISO in BE       */
    int32_t  error_count;           /* 0 in BE                            */

    uint8_t  padding[8];            /* zeroed (mirrors setup[] position)  */
    /* followed by actual_length bytes of IN data if direction==IN */
} usbip_ret_submit_t;

/* ── CMD_UNLINK (host → device, 48 bytes) ────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t command;               /* USBIP_CMD_UNLINK in BE             */
    uint32_t seqnum;                /* this packet's seqnum in BE         */
    uint32_t devid;                 /* in BE                              */
    uint32_t direction;             /* in BE                              */
    uint32_t ep;                    /* in BE                              */

    uint32_t unlink_seqnum;         /* seqnum of the submit to cancel BE  */
    uint8_t  padding[24];
} usbip_cmd_unlink_t;

/* ── RET_UNLINK (device → host, 48 bytes) ────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t command;               /* USBIP_RET_UNLINK in BE             */
    uint32_t seqnum;                /* echoes cmd_unlink.seqnum in BE     */
    uint32_t devid;                 /* 0 in BE                            */
    uint32_t direction;             /* 0 in BE                            */
    uint32_t ep;                    /* 0 in BE                            */

    int32_t  status;                /* -ECONNRESET (-104) if cancelled BE */
    uint8_t  padding[24];
} usbip_ret_unlink_t;

/* Size assertions */
_Static_assert(sizeof(usbip_cmd_submit_t)  == 48, "usbip_cmd_submit must be 48 bytes");
_Static_assert(sizeof(usbip_ret_submit_t)  == 48, "usbip_ret_submit must be 48 bytes");
_Static_assert(sizeof(usbip_cmd_unlink_t)  == 48, "usbip_cmd_unlink must be 48 bytes");
_Static_assert(sizeof(usbip_ret_unlink_t)  == 48, "usbip_ret_unlink must be 48 bytes");
