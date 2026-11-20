/*
 * Firmware updater (Rev 2.25, step 2). See updater.h.
 *
 * Protocol (both directions), little-endian:
 *
 *   0xA5 0x5A | cmd (1) | len (2) | payload (len) | crc32 (4)
 *
 * crc32 is the IEEE/zlib CRC-32 of cmd, len and payload. A reply has the
 * command's code with bit 7 set; an error reply is UPD_RSP_ERROR. Bytes
 * outside a frame are ignored, so the updater can print plain-text status
 * lines for a person at a terminal, and the host just skips them.
 *
 * Commands:
 *   HELLO  (0x01) -> HELLO reply: protocol version, link, running slot,
 *                    chip revision, flash and slot size, largest block,
 *                    board ID, build ID.
 *   BEGIN  (0x10) u32 size, u32 crc32 of the image -> u8 target slot,
 *                    u32 target flash offset, u32 slot size. The target is
 *                    the slot that is not running. Its first sector is
 *                    erased at once.
 *   DATA   (0x11) u32 offset, data (4096 bytes; the last block may be
 *                    shorter) -> u32 offset. offset is a multiple of 4096.
 *                    Each sector is erased, programmed and read back. The
 *                    first sector is only kept in RAM until FINISH.
 *   FINISH (0x12) -> checks that every block arrived and that the CRC of
 *                    the whole image matches. Only then does it write the
 *                    first sector (the image's start block, so a partial
 *                    image never becomes bootable), reply, and do a flash
 *                    update boot of the new slot. The bootrom then erases
 *                    the old slot's first sector (step 3: no trial boot yet).
 *   REBOOT (0x7F) -> reply, then reboot into the current firmware.
 * Blocks may be sent again (each write starts with an erase); send them in
 * any order.
 */
#include "updater.h"

#include <stddef.h>
#include <string.h>

#include "hardware/gpio.h"
#include "hardware/structs/powman.h"
#include "boot/picobin.h"
#include "boot/picoboot_constants.h"
#include "pico/bootrom.h"
#include "pico/platform.h"
#include "pico/time.h"

#include "build_id.h"
#include "config.h"

#if APP_USB_STATUS
#include "hardware/irq.h"
#include "pico/stdio.h"
#include "tusb.h"

#include "crash.h"
#endif

/* ------------------------------------------------------------ boot flag -- */

/* Boot flag key: written only by updater_request(), checked in all 64 bits
 * by updater_check_boot(). Different from UPDATER_REQUEST_KEY. */
#define BOOT_KEY_LO 0x17FE78A2u
#define BOOT_KEY_HI 0xE1852CD4u

#define FLAG_KEY_LO 0u /* POWMAN scratch register indices */
#define FLAG_KEY_HI 1u
#define FLAG_LINK 2u
#define FLAG_CHECK 3u

static uint32_t s_rejects;

static bool link_ok(uint32_t link) {
#if APP_USB_STATUS
    return link <= (uint32_t)UPDATER_LINK_USB;
#else
    return link <= (uint32_t)UPDATER_LINK_UART1;
#endif
}

static uint32_t flag_check(uint32_t lo, uint32_t hi, uint32_t link) {
    return ~(lo ^ hi ^ link);
}

uint32_t updater_rejects(void) {
    return s_rejects;
}

bool updater_request(updater_link_t link, uint64_t key) {
    if (key != UPDATER_REQUEST_KEY || !link_ok((uint32_t)link)) {
        s_rejects++;
        return false;
    }
    powman_hw->scratch[FLAG_KEY_LO] = BOOT_KEY_LO;
    powman_hw->scratch[FLAG_KEY_HI] = BOOT_KEY_HI;
    powman_hw->scratch[FLAG_LINK] = (uint32_t)link;
    powman_hw->scratch[FLAG_CHECK] = flag_check(BOOT_KEY_LO, BOOT_KEY_HI, (uint32_t)link);
#if APP_USB_STATUS
    crash_disarm(); /* an intended reboot, not a hang (D-9) */
#endif
    rom_reboot(REBOOT2_FLAG_REBOOT_TYPE_NORMAL | REBOOT2_FLAG_NO_RETURN_ON_SUCCESS, 10, 0, 0);
    for (;;) {
        tight_loop_contents();
    }
}

static void __attribute__((noreturn)) updater_run(updater_link_t link);

void updater_check_boot(void) {
    uint32_t lo = powman_hw->scratch[FLAG_KEY_LO];
    uint32_t hi = powman_hw->scratch[FLAG_KEY_HI];
    uint32_t link = powman_hw->scratch[FLAG_LINK];
    uint32_t chk = powman_hw->scratch[FLAG_CHECK];
    /* Cleared before anything else, valid or not, so no later reset can
     * come back here. */
    for (unsigned i = 0; i < 4u; i++) {
        powman_hw->scratch[i] = 0u;
    }
    if (lo == BOOT_KEY_LO && hi == BOOT_KEY_HI && chk == flag_check(lo, hi, link) && link_ok(link)) {
        updater_run((updater_link_t)link);
    }
}

/* ------------------------------------------------------ image info block -- */

/* Rev 2.25: present in every image, so the host tool and the updater can
 * tell that an image carries an updater, which protocol it speaks and which
 * firmware version it is, before installing it. Referenced by HELLO, so the
 * linker keeps it. 32 bytes, 4-byte aligned in the image (its 32-byte RAM
 * alignment does not carry over to the flash image: copy_to_ram).
 * Little-endian, shared with tools/dfc_update.py:
 *   0  u32 magic_lo 0x55434644 ("DFCU"), 4 u32 magic_hi 0x519C4450
 *   8  u16 info block format (1)
 *  10  u16 updater protocol (UPDATER_PROTOCOL)
 *  12  u16 firmware major, 14 u16 minor, 16 u16 patch
 *  18  u16 protocol floor (UPDATER_PROTOCOL_FLOOR)
 *  20  u32 reserved, 24 u32 reserved (0)
 *  28  u32 check: ~XOR of the seven words before it. A 64-bit magic plus
 *      this check word: an accidental match is not credible. */
#define INFO_MAGIC_LO 0x55434644u
#define INFO_MAGIC_HI 0x519C4450u
#define INFO_FORMAT 1u

typedef struct {
    uint32_t magic_lo;
    uint32_t magic_hi;
    uint16_t format;
    uint16_t protocol;
    uint16_t fw_major;
    uint16_t fw_minor;
    uint16_t fw_patch;
    uint16_t floor;
    uint32_t reserved[2];
    uint32_t check;
} upd_info_t;
_Static_assert(sizeof(upd_info_t) == 32u, "updater info block must be 32 bytes");

#define INFO_W2 ((uint32_t)INFO_FORMAT | ((uint32_t)UPDATER_PROTOCOL << 16))
#define INFO_W3 ((uint32_t)DFC_FW_MAJOR | ((uint32_t)DFC_FW_MINOR << 16))
#define INFO_W4 ((uint32_t)DFC_FW_PATCH | ((uint32_t)UPDATER_PROTOCOL_FLOOR << 16))

static const upd_info_t g_updater_info __attribute__((aligned(32), used)) = {
    INFO_MAGIC_LO,
    INFO_MAGIC_HI,
    INFO_FORMAT,
    UPDATER_PROTOCOL,
    DFC_FW_MAJOR,
    DFC_FW_MINOR,
    DFC_FW_PATCH,
    UPDATER_PROTOCOL_FLOOR,
    {0u, 0u},
    ~(INFO_MAGIC_LO ^ INFO_MAGIC_HI ^ INFO_W2 ^ INFO_W3 ^ INFO_W4),
};

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Updater protocol of a valid info block at p (32 bytes), or -1. */
static int info_at(const uint8_t *p) {
    uint32_t w[8];
    for (unsigned i = 0; i < 8u; i++) {
        w[i] = rd32(&p[4u * i]);
    }
    uint32_t chk = ~(w[0] ^ w[1] ^ w[2] ^ w[3] ^ w[4] ^ w[5] ^ w[6]);
    if (w[0] != INFO_MAGIC_LO || w[1] != INFO_MAGIC_HI || w[7] != chk || (w[2] & 0xFFFFu) < 1u) {
        return -1;
    }
    return (int)(w[2] >> 16);
}

/* Scans an image delivered in chunks, including blocks that straddle two
 * chunks. Chunks are multiples of 4 bytes except possibly the last. */
static struct {
    uint8_t tail[28]; /* last 28 bytes of the previous chunk */
    uint32_t tail_n;
    int proto;
} s_scan;

static void scan_reset(void) {
    s_scan.tail_n = 0u;
    s_scan.proto = -1;
}

static void scan_chunk(const uint8_t *p, uint32_t n) {
    if (s_scan.proto >= 0) {
        return;
    }
    /* Across the boundary: tail of the previous chunk + start of this one. */
    if (s_scan.tail_n != 0u && n >= 28u) {
        uint8_t win[56];
        memcpy(win, s_scan.tail, s_scan.tail_n);
        memcpy(&win[s_scan.tail_n], p, 28u);
        for (uint32_t i = 0; i + 32u <= s_scan.tail_n + 28u && s_scan.proto < 0; i += 4u) {
            s_scan.proto = info_at(&win[i]);
        }
    }
    for (uint32_t i = 0; i + 32u <= n && s_scan.proto < 0; i += 4u) {
        s_scan.proto = info_at(&p[i]);
    }
    uint32_t k = n < 28u ? n : 28u;
    memcpy(s_scan.tail, &p[n - k], k);
    s_scan.tail_n = k;
}

/* ------------------------------------------------------------- protocol -- */

#define UPD_PROTO_VERSION UPDATER_PROTOCOL
#define UPD_SOF0 0xA5u
#define UPD_SOF1 0x5Au
#define UPD_MAX_PAYLOAD (4096u + 16u)
#define UPD_FRAME_GAP_US 500000u /* a frame stalled this long is dropped */

enum {
    UPD_CMD_HELLO = 0x01,
    UPD_CMD_BEGIN = 0x10,
    UPD_CMD_DATA = 0x11,
    UPD_CMD_FINISH = 0x12,
    UPD_CMD_REBOOT = 0x7F,
    UPD_RSP_ERROR = 0xFF,
};

/* Error reply payload: u8 code, then (optional) u32 detail. */
enum {
    UPD_ERR_CRC = 1,         /* frame CRC wrong: send it again              */
    UPD_ERR_UNKNOWN_CMD = 2,
    UPD_ERR_LENGTH = 3,      /* payload length wrong for the command        */
    UPD_ERR_NO_PT = 4,       /* no A/B partition table, or not booted from A/B */
    UPD_ERR_STATE = 5,       /* DATA or FINISH without a BEGIN              */
    UPD_ERR_OFFSET = 6,      /* offset not a multiple of 4096, or past the end */
    UPD_ERR_FLASH = 7,       /* bootrom flash operation failed (detail: code) */
    UPD_ERR_VERIFY = 8,      /* read-back differs from the data written     */
    UPD_ERR_IMAGE_CRC = 9,   /* whole-image CRC wrong (detail: CRC computed) */
    UPD_ERR_MISSING = 10,    /* FINISH before every block arrived (detail: first missing offset) */
    UPD_ERR_TOO_BIG = 11,    /* image larger than the slot (detail: slot size) */
    UPD_ERR_NO_INFO = 12,    /* image has no updater info block             */
    UPD_ERR_OLD_UPDATER = 13, /* image's updater protocol below the floor (detail: its protocol) */
};

#define UPD_BOARD_ID "DFC-XIAO-RP2350"
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (2u * 1024u * 1024u)
#endif
#define UPD_SLOT_BYTES (1012u * 1024u) /* partition_table.json */

/* ---------------------------------------------------------------- flash -- */

#define SECTOR 4096u
#define MAX_SECTORS 512u /* 2 MB: bitmap of received blocks */

/* Bootrom flash operation on physical flash offsets ("storage" addresses),
 * checked against the partition table's permissions, so it cannot touch
 * the partition table itself. Called directly, without the SDK's
 * flash_safe_execute() lockout (the bootrom lock is still taken):
 * the whole image runs from RAM (copy_to_ram), so nothing fetches from
 * flash while the bootrom has it out of XIP mode, interrupts included. */
static int flash_op(uint32_t op, uint32_t offset, uint32_t n, uint8_t *buf) {
    rom_flash_op_fn f = (rom_flash_op_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_OP);
    cflash_flags_t fl = {(op << CFLASH_OP_LSB) | (CFLASH_ASPACE_VALUE_STORAGE << CFLASH_ASPACE_LSB) |
                         (CFLASH_SECLEVEL_VALUE_SECURE << CFLASH_SECLEVEL_LSB)};
    /* The SDK enables the bootrom's hardware locks at start-up, so the
     * flash operation must hold the flash lock (else it returns
     * BOOTROM_ERROR_LOCK_REQUIRED, -19; bench, Rev 2.25). */
    if (!bootrom_try_acquire_lock(BOOTROM_LOCK_FLASH_OP)) {
        return BOOTROM_ERROR_LOCK_REQUIRED;
    }
    int r = f(fl, XIP_BASE + offset, n, buf);
    bootrom_release_lock(BOOTROM_LOCK_FLASH_OP);
    return r;
}

static uint8_t s_work[4096] __attribute__((aligned(4))); /* bootrom work area */

/* Flash offset and size of partition n, from the bootrom's partition table. */
static bool partition_location(unsigned n, uint32_t *offset, uint32_t *size) {
    (void)rom_load_partition_table(s_work, sizeof s_work, false);
    uint32_t info[3];
    int r = rom_get_partition_table_info(info, 3u, PT_INFO_PARTITION_LOCATION_AND_FLAGS | PT_INFO_SINGLE_PARTITION |
                                                       (n << 24));
    if (r != 3 || !(info[0] & PT_INFO_PARTITION_LOCATION_AND_FLAGS)) {
        return false;
    }
    uint32_t first = (info[1] & PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_BITS) >> PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_LSB;
    uint32_t last = (info[1] & PICOBIN_PARTITION_LOCATION_LAST_SECTOR_BITS) >> PICOBIN_PARTITION_LOCATION_LAST_SECTOR_LSB;
    if (last < first) {
        return false;
    }
    *offset = first * SECTOR;
    *size = (last + 1u - first) * SECTOR;
    return true;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n) {
    crc = ~crc;
    while (n-- != 0u) {
        crc ^= *p++;
        for (unsigned k = 0; k < 8u; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

/* ---------------------------------------------------------------- ports -- */

/* One byte-stream port. Step 2: USB only (diagnostic builds). */
typedef struct {
    void (*init)(void);
    int (*read)(void);                         /* byte, or -1 if none */
    void (*write)(const uint8_t *p, size_t n); /* blocking */
    bool (*connected)(void);
    void (*poll)(void);
} upd_port_t;

#if APP_USB_STATUS
static void usb_init(void) {
    stdio_init_all();
    /* Same priority for the USB controller IRQ and the SDK's tud_task() IRQ
     * (Rev 2.24): the SDK assumes neither preempts the other. */
    irq_set_priority(USBCTRL_IRQ, IRQ_PRIO_USB);
    irq_set_priority(PICO_STDIO_USB_LOW_PRIORITY_IRQ, IRQ_PRIO_USB);
}

static int usb_read(void) {
    uint8_t b;
    return (tud_cdc_available() && tud_cdc_read(&b, 1) == 1u) ? (int)b : -1;
}

static void usb_write(const uint8_t *p, size_t n) {
    /* Blocking is fine here; give up if the host goes away. */
    absolute_time_t give_up = make_timeout_time_ms(1000);
    while (n != 0u && tud_cdc_connected()) {
        uint32_t room = tud_cdc_write_available();
        if (room == 0u) {
            tud_cdc_write_flush();
            if (time_reached(give_up)) {
                return;
            }
            continue;
        }
        uint32_t k = n < room ? (uint32_t)n : room;
        k = tud_cdc_write(p, k);
        p += k;
        n -= k;
    }
    tud_cdc_write_flush();
}

static bool usb_connected(void) {
    return tud_cdc_connected();
}

static void usb_poll(void) {}

static const upd_port_t k_usb_port = {usb_init, usb_read, usb_write, usb_connected, usb_poll};
#endif

/* -------------------------------------------------------------- updater -- */

#ifndef PICO_DEFAULT_LED_PIN
#define PICO_DEFAULT_LED_PIN 25
#endif

static const upd_port_t *s_port;
static updater_link_t s_link;
static uint8_t s_rx[UPD_MAX_PAYLOAD];
static uint8_t s_tx[96];

static void send_frame(uint8_t cmd, const uint8_t *payload, uint16_t len) {
    uint8_t h[5] = {UPD_SOF0, UPD_SOF1, cmd, (uint8_t)len, (uint8_t)(len >> 8)};
    uint32_t crc = crc32_update(0u, &h[2], 3u);
    crc = crc32_update(crc, payload, len);
    uint8_t c[4] = {(uint8_t)crc, (uint8_t)(crc >> 8), (uint8_t)(crc >> 16), (uint8_t)(crc >> 24)};
    s_port->write(h, sizeof h);
    if (len != 0u) {
        s_port->write(payload, len);
    }
    s_port->write(c, sizeof c);
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void send_error_detail(uint8_t code, uint32_t detail) {
    uint8_t b[5] = {code};
    put32(&b[1], detail);
    send_frame(UPD_RSP_ERROR, b, sizeof b);
}

static void send_error(uint8_t code) {
    send_error_detail(code, 0u);
}

static void reply_hello(void) {
    boot_info_t bi;
    int8_t slot = rom_get_boot_info(&bi) ? bi.partition : (int8_t)-9;
    memset(s_tx, 0, sizeof s_tx);
    s_tx[0] = UPD_PROTO_VERSION;
    s_tx[1] = (uint8_t)s_link;
    s_tx[2] = (uint8_t)slot; /* 0 = A, 1 = B, 0xFF = no partition table */
    s_tx[3] = rp2350_chip_version();
    put32(&s_tx[4], PICO_FLASH_SIZE_BYTES);
    put32(&s_tx[8], UPD_SLOT_BYTES);
    put32(&s_tx[12], 4096u); /* largest data block (later steps) */
    memcpy(&s_tx[16], UPD_BOARD_ID, sizeof UPD_BOARD_ID - 1u);
    size_t bl = strlen(g_build_id);
    memcpy(&s_tx[32], g_build_id, bl < 32u ? bl : 32u);
    /* 64: the running image's info block, as stored in it. */
    memcpy(&s_tx[64], &g_updater_info, sizeof g_updater_info); /* the whole block */
    send_frame(UPD_CMD_HELLO | 0x80u, s_tx, 96u);
}

static void __attribute__((noreturn)) reboot_to_app(void) {
    rom_reboot(REBOOT2_FLAG_REBOOT_TYPE_NORMAL | REBOOT2_FLAG_NO_RETURN_ON_SUCCESS, 10, 0, 0);
    for (;;) {
        tight_loop_contents();
    }
}

/* ----------------------------------------------------------- the update -- */

static struct {
    bool active;
    unsigned target;      /* partition being written */
    uint32_t offset;      /* its flash offset        */
    uint32_t slot_size;
    uint32_t size;        /* image size              */
    uint32_t crc;         /* expected image CRC-32   */
    uint32_t sectors;
    uint8_t got[MAX_SECTORS / 8u];
} s_up;

static uint8_t s_sector0[SECTOR] __attribute__((aligned(4))); /* written last */
static uint8_t s_buf[SECTOR] __attribute__((aligned(4)));

static bool running_partition(unsigned *p) {
    boot_info_t bi;
    if (!rom_get_boot_info(&bi) || (bi.partition != 0 && bi.partition != 1)) {
        return false;
    }
    *p = (unsigned)bi.partition;
    return true;
}

static void do_begin(uint16_t len) {
    if (len != 8u) {
        send_error(UPD_ERR_LENGTH);
        return;
    }
    s_up.active = false;
    unsigned run;
    uint32_t off;
    uint32_t sz;
    if (!running_partition(&run) || !partition_location(run ^ 1u, &off, &sz)) {
        send_error(UPD_ERR_NO_PT);
        return;
    }
    uint32_t size = get32(&s_rx[0]);
    if (size == 0u || size > sz || (size + SECTOR - 1u) / SECTOR > MAX_SECTORS) {
        send_error_detail(UPD_ERR_TOO_BIG, sz);
        return;
    }
    /* Erase the target's first sector now, so whatever it held stops being
     * a bootable image for the whole of the update. */
    int r = flash_op(CFLASH_OP_VALUE_ERASE, off, SECTOR, NULL);
    if (r != 0) {
        send_error_detail(UPD_ERR_FLASH, (uint32_t)r);
        return;
    }
    s_up.active = true;
    s_up.target = run ^ 1u;
    s_up.offset = off;
    s_up.slot_size = sz;
    s_up.size = size;
    s_up.crc = get32(&s_rx[4]);
    s_up.sectors = (size + SECTOR - 1u) / SECTOR;
    memset(s_up.got, 0, sizeof s_up.got);
    memset(s_sector0, 0xFF, sizeof s_sector0);
    uint8_t b[12];
    b[0] = (uint8_t)s_up.target;
    b[1] = b[2] = b[3] = 0u;
    put32(&b[4], off);
    put32(&b[8], sz);
    send_frame(UPD_CMD_BEGIN | 0x80u, b, sizeof b);
}

/* Erase, program and verify one sector of the target slot. */
static int write_sector(uint32_t index, const uint8_t *data) {
    uint32_t at = s_up.offset + index * SECTOR;
    int r = flash_op(CFLASH_OP_VALUE_ERASE, at, SECTOR, NULL);
    if (r == 0) {
        memcpy(s_buf, data, SECTOR);
        r = flash_op(CFLASH_OP_VALUE_PROGRAM, at, SECTOR, s_buf);
    }
    if (r == 0) {
        r = flash_op(CFLASH_OP_VALUE_READ, at, SECTOR, s_buf);
    }
    if (r != 0) {
        send_error_detail(UPD_ERR_FLASH, (uint32_t)r);
        return -1;
    }
    if (memcmp(s_buf, data, SECTOR) != 0) {
        send_error_detail(UPD_ERR_VERIFY, index * SECTOR);
        return -1;
    }
    return 0;
}

static void do_data(uint16_t len) {
    if (!s_up.active) {
        send_error(UPD_ERR_STATE);
        return;
    }
    if (len < 5u) {
        send_error(UPD_ERR_LENGTH);
        return;
    }
    uint32_t offset = get32(&s_rx[0]);
    uint32_t n = (uint32_t)len - 4u;
    if (offset % SECTOR != 0u || offset >= s_up.size) {
        send_error_detail(UPD_ERR_OFFSET, offset);
        return;
    }
    uint32_t want = s_up.size - offset < SECTOR ? s_up.size - offset : SECTOR;
    if (n != want) {
        send_error_detail(UPD_ERR_LENGTH, want);
        return;
    }
    uint32_t index = offset / SECTOR;
    uint8_t *data = (index == 0u) ? s_sector0 : s_buf;
    memset(data, 0xFF, SECTOR); /* pad a short last block with erased bytes */
    memcpy(data, &s_rx[4], n);
    if (index != 0u) {
        static uint8_t copy[SECTOR] __attribute__((aligned(4)));
        memcpy(copy, data, SECTOR);
        if (write_sector(index, copy) != 0) {
            return;
        }
    }
    s_up.got[index / 8u] = (uint8_t)(s_up.got[index / 8u] | (1u << (index % 8u)));
    uint8_t b[4];
    put32(b, offset);
    send_frame(UPD_CMD_DATA | 0x80u, b, sizeof b);
}

static void __attribute__((noreturn)) flash_update_boot(uint32_t offset) {
    rom_reboot(REBOOT2_FLAG_REBOOT_TYPE_FLASH_UPDATE | REBOOT2_FLAG_NO_RETURN_ON_SUCCESS, 10, XIP_BASE + offset, 0);
    for (;;) {
        tight_loop_contents();
    }
}

static void do_finish(void) {
    if (!s_up.active) {
        send_error(UPD_ERR_STATE);
        return;
    }
    for (uint32_t i = 0; i < s_up.sectors; i++) {
        if (!(s_up.got[i / 8u] & (1u << (i % 8u)))) {
            send_error_detail(UPD_ERR_MISSING, i * SECTOR);
            return;
        }
    }
    /* CRC of the image as it will be: sector 0 from RAM, the rest read
     * back from flash. */
    uint32_t first = s_up.size < SECTOR ? s_up.size : SECTOR;
    uint32_t crc = crc32_update(0u, s_sector0, first);
    scan_reset();
    scan_chunk(s_sector0, first);
    for (uint32_t i = 1; i < s_up.sectors; i++) {
        int r = flash_op(CFLASH_OP_VALUE_READ, s_up.offset + i * SECTOR, SECTOR, s_buf);
        if (r != 0) {
            send_error_detail(UPD_ERR_FLASH, (uint32_t)r);
            return;
        }
        uint32_t n = s_up.size - i * SECTOR < SECTOR ? s_up.size - i * SECTOR : SECTOR;
        crc = crc32_update(crc, s_buf, n);
        scan_chunk(s_buf, n);
    }
    if (crc != s_up.crc) {
        send_error_detail(UPD_ERR_IMAGE_CRC, crc);
        return;
    }
    /* Rev 2.25: only an image whose updater this protocol line can still
     * drive may be installed; otherwise the unit could never be updated
     * again. */
    if (s_scan.proto < 0) {
        send_error(UPD_ERR_NO_INFO);
        return;
    }
    if ((uint32_t)s_scan.proto < UPDATER_PROTOCOL_FLOOR) {
        send_error_detail(UPD_ERR_OLD_UPDATER, (uint32_t)s_scan.proto);
        return;
    }
    /* Everything else is in place and checked: now the start block. */
    static uint8_t copy[SECTOR] __attribute__((aligned(4)));
    memcpy(copy, s_sector0, SECTOR);
    if (write_sector(0u, copy) != 0) {
        return;
    }
    uint8_t b[4];
    put32(b, crc);
    send_frame(UPD_CMD_FINISH | 0x80u, b, sizeof b);
    sleep_ms(300); /* let the reply reach the host before the port goes */
    s_up.active = false;
    flash_update_boot(s_up.offset);
}

/* Returns true if the frame was valid (resets the idle timeout). */
static bool handle(uint8_t cmd, uint16_t len) {
    switch (cmd) {
    case UPD_CMD_HELLO:
        reply_hello();
        return true;
    case UPD_CMD_BEGIN:
        do_begin(len);
        return true;
    case UPD_CMD_DATA:
        do_data(len);
        return true;
    case UPD_CMD_FINISH:
        do_finish();
        return true;
    case UPD_CMD_REBOOT:
        send_frame(UPD_CMD_REBOOT | 0x80u, NULL, 0u);
        sleep_ms(300); /* let the host read the reply before the port goes */
        reboot_to_app();
    default:
        send_error(UPD_ERR_UNKNOWN_CMD);
        return true;
    }
}

static void say(const char *s) {
    s_port->write((const uint8_t *)s, strlen(s));
}

static void __attribute__((noreturn)) updater_run(updater_link_t link) {
    s_link = link;
    s_port = NULL;
#if APP_USB_STATUS
    if (link == UPDATER_LINK_USB) {
        s_port = &k_usb_port;
    }
#endif
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

    if (s_port != NULL) {
        s_port->init();
    }

    enum { WAIT_SOF0, WAIT_SOF1, HEADER, PAYLOAD, CRC } st = WAIT_SOF0;
    uint8_t hdr[3] = {0};
    uint8_t crcb[4] = {0};
    unsigned got = 0;
    uint16_t len = 0;
    bool greeted = false;
    uint64_t idle_until = time_us_64() + (uint64_t)UPDATER_IDLE_TIMEOUT_S * 1000000u;
    uint64_t frame_until = 0;

    for (;;) {
        uint64_t now = time_us_64();
        gpio_put(PICO_DEFAULT_LED_PIN, ((now >> 16) & 1u) != 0u); /* ~8 Hz blink */
        if (now >= idle_until) {
            if (s_port != NULL && s_port->connected()) {
                say("\r\nDFC updater: idle timeout, rebooting into the firmware.\r\n");
                sleep_ms(50);
            }
            reboot_to_app();
        }
        if (s_port == NULL) {
            continue; /* this link is not implemented yet: wait for the timeout */
        }
        s_port->poll();
        if (!greeted && s_port->connected()) {
            greeted = true;
            say("\r\nDFC updater: waiting for the host (tools/dfc_update.py). Reboots into the firmware "
                "after 90 s without a command.\r\n");
        }
        if (st != WAIT_SOF0 && now >= frame_until) {
            st = WAIT_SOF0; /* frame stalled: resynchronise */
        }
        int c = s_port->read();
        if (c < 0) {
            continue;
        }
        uint8_t b = (uint8_t)c;
        switch (st) {
        case WAIT_SOF0:
            if (b == UPD_SOF0) {
                st = WAIT_SOF1;
                frame_until = now + UPD_FRAME_GAP_US;
            }
            break;
        case WAIT_SOF1:
            st = (b == UPD_SOF1) ? HEADER : (b == UPD_SOF0 ? WAIT_SOF1 : WAIT_SOF0);
            got = 0;
            break;
        case HEADER:
            hdr[got++] = b;
            if (got == 3u) {
                len = (uint16_t)(hdr[1] | (hdr[2] << 8));
                got = 0;
                if (len > UPD_MAX_PAYLOAD) {
                    send_error(UPD_ERR_LENGTH);
                    st = WAIT_SOF0;
                } else {
                    st = (len != 0u) ? PAYLOAD : CRC;
                }
            }
            break;
        case PAYLOAD:
            s_rx[got++] = b;
            if (got == len) {
                got = 0;
                st = CRC;
            }
            break;
        case CRC:
            crcb[got++] = b;
            if (got == 4u) {
                uint32_t want = (uint32_t)crcb[0] | ((uint32_t)crcb[1] << 8) | ((uint32_t)crcb[2] << 16) |
                                ((uint32_t)crcb[3] << 24);
                uint32_t crc = crc32_update(crc32_update(0u, hdr, 3u), s_rx, len);
                st = WAIT_SOF0;
                if (crc != want) {
                    send_error(UPD_ERR_CRC);
                } else if (handle(hdr[0], len)) {
                    greeted = true; /* no more text once the host talks */
                    idle_until = time_us_64() + (uint64_t)UPDATER_IDLE_TIMEOUT_S * 1000000u;
                }
            }
            break;
        default:
            st = WAIT_SOF0;
            break;
        }
    }
}
