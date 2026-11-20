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
 * Step 2 commands:
 *   HELLO  (0x01) -> HELLO reply: protocol version, link, running slot,
 *                    chip revision, flash and slot size, largest block,
 *                    board ID, build ID.
 *   REBOOT (0x7F) -> reply, then reboot into the current firmware.
 */
#include "updater.h"

#include <stddef.h>
#include <string.h>

#include "hardware/gpio.h"
#include "hardware/structs/powman.h"
#include "boot/picoboot_constants.h"
#include "pico/bootrom.h"
#include "pico/platform.h"
#include "pico/time.h"

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

/* ------------------------------------------------------------- protocol -- */

#define UPD_PROTO_VERSION 1u
#define UPD_SOF0 0xA5u
#define UPD_SOF1 0x5Au
#define UPD_MAX_PAYLOAD (4096u + 16u)
#define UPD_FRAME_GAP_US 500000u /* a frame stalled this long is dropped */

enum {
    UPD_CMD_HELLO = 0x01,
    UPD_CMD_REBOOT = 0x7F,
    UPD_RSP_ERROR = 0xFF,
};

enum {
    UPD_ERR_CRC = 1,
    UPD_ERR_UNKNOWN_CMD = 2,
    UPD_ERR_LENGTH = 3,
};

#define UPD_BOARD_ID "DFC-XIAO-RP2350"
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (2u * 1024u * 1024u)
#endif
#define UPD_SLOT_BYTES (1012u * 1024u) /* partition_table.json */

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
static uint8_t s_tx[64];

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

static void send_error(uint8_t code) {
    send_frame(UPD_RSP_ERROR, &code, 1u);
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
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
    static const char k_build[] = __DATE__ " " __TIME__;
    memcpy(&s_tx[32], k_build, sizeof k_build - 1u < 32u ? sizeof k_build - 1u : 32u);
    send_frame(UPD_CMD_HELLO | 0x80u, s_tx, 64u);
}

static void __attribute__((noreturn)) reboot_to_app(void) {
    rom_reboot(REBOOT2_FLAG_REBOOT_TYPE_NORMAL | REBOOT2_FLAG_NO_RETURN_ON_SUCCESS, 10, 0, 0);
    for (;;) {
        tight_loop_contents();
    }
}

/* Returns true if the frame was valid (resets the idle timeout). */
static bool handle(uint8_t cmd, uint16_t len) {
    (void)len;
    switch (cmd) {
    case UPD_CMD_HELLO:
        reply_hello();
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
