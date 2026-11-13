/*
 * Reference application: a single bare-metal main loop on core 0 (EX-1,
 * HW-8). It consumes magnetometer samples and, per APP_MODE:
 *
 *   APP_MODE_ECHO     every byte received on a UART is sent back out of the
 *                     same UART (V-1 soak, V-2/V-3 latency).
 *   APP_MODE_PATTERN  each UART transmits a continuous 8-bit counting
 *                     pattern, keeping its TX queue non-empty (V-1, V-4),
 *                     and checks that received data is the same pattern.
 *
 * Test GPIOs (APP_TEST_PINS):
 *   RX_SEEN  toggles when link_read() returns data      (V-2)
 *   TX_WRITE toggles immediately before link_write()    (V-3)
 */
#include <stdbool.h>
#include <stdint.h>

#include "hardware/gpio.h"
#include "hardware/structs/sio.h"
#include "pico/stdlib.h"

#include "config.h"
#include "diag.h"
#include "link.h"
#include "mag.h"
#include "status.h"
#include "tick.h"

/* Observable from a debugger (and reported by status.c). */
volatile int32_t g_app_mag_xyz[3];
volatile uint32_t g_app_mag_gaps;    /* sequence discontinuities between valid samples */
volatile uint32_t g_app_mag_valid;   /* valid samples consumed                         */
volatile uint32_t g_app_mag_invalid; /* invalid (M-9) samples consumed                 */
volatile uint32_t g_app_mag_stale;   /* iterations on which the latest sample was stale */
volatile bool g_app_baud_ok;
volatile uint32_t g_app_rx_pattern_errors[LINK_COUNT]; /* APP_MODE_PATTERN only */

#if APP_TEST_PINS
static const uint32_t k_rx_seen_mask[LINK_COUNT] = {1u << TEST_PIN_LINK0_RX_SEEN, 1u << TEST_PIN_LINK1_RX_SEEN};
static const uint32_t k_tx_write_mask[LINK_COUNT] = {1u << TEST_PIN_LINK0_TX_WRITE, 1u << TEST_PIN_LINK1_TX_WRITE};

static void test_pins_init(void) {
    const uint pins[] = {TEST_PIN_LINK0_RX_SEEN, TEST_PIN_LINK0_TX_WRITE, TEST_PIN_LINK1_RX_SEEN,
                         TEST_PIN_LINK1_TX_WRITE};
    for (unsigned i = 0; i < sizeof pins / sizeof pins[0]; i++) {
        gpio_init(pins[i]);
        gpio_set_dir(pins[i], GPIO_OUT);
        gpio_put(pins[i], false);
    }
}
#define TEST_TOGGLE(mask) (sio_hw->gpio_togl = (mask))
#else
static void test_pins_init(void) {}
#define TEST_TOGGLE(mask) ((void)(mask))
#endif

static uint8_t s_buf[256];

#if APP_MODE == APP_MODE_PATTERN
static uint8_t s_tx_next[LINK_COUNT];
static uint8_t s_rx_next[LINK_COUNT];
static bool s_rx_synced[LINK_COUNT];

static void pattern_rx(unsigned i, const uint8_t *p, size_t n) {
    for (size_t k = 0; k < n; k++) {
        if (s_rx_synced[i] && p[k] != s_rx_next[i]) {
            g_app_rx_pattern_errors[i]++;
        }
        s_rx_synced[i] = true;
        s_rx_next[i] = (uint8_t)(p[k] + 1u);
    }
}

static void pattern_tx(unsigned i) {
    static uint8_t chunk[256];
    if (link_tx_free((link_id_t)i) < sizeof chunk) {
        return;
    }
    for (unsigned k = 0; k < sizeof chunk; k++) {
        chunk[k] = (uint8_t)(s_tx_next[i] + k);
    }
    TEST_TOGGLE(k_tx_write_mask[i]);
    if (link_write((link_id_t)i, chunk, sizeof chunk)) {
        s_tx_next[i] = (uint8_t)(s_tx_next[i] + sizeof chunk);
    }
}
#endif

static void mag_consume(void) {
    static bool have_last;
    static uint32_t last_seq;
    static uint32_t last_published_seq;
    static bool have_published;

    mag_reading_t r;
    if (!mag_read_latest(&r)) {
        return;
    }
    if (r.stale) {
        g_app_mag_stale++;
    }
    if (have_published && r.s.seq == last_published_seq) {
        return; /* nothing new */
    }
    have_published = true;
    last_published_seq = r.s.seq;

    if (!r.s.valid) {
        g_app_mag_invalid++;
        have_last = false; /* gap expected across an abort; counted by counters */
        return;
    }
    if (have_last && r.s.seq != last_seq + 1u) {
        g_app_mag_gaps++;
    }
    have_last = true;
    last_seq = r.s.seq;
    g_app_mag_valid++;

    int32_t xyz[3];
    mag_convert(r.s.raw, xyz);
    g_app_mag_xyz[0] = xyz[0];
    g_app_mag_xyz[1] = xyz[1];
    g_app_mag_xyz[2] = xyz[2];
}

int main(void) {
#if APP_USB_STATUS
    status_init(); /* USB CDC; priority lowered below all UART interrupts (B-20) */
#endif
    diag_init();
    test_pins_init();

    g_app_baud_ok = link_init();
    (void)mag_init();
    tick_init(); /* M-6: runs even if the magnetometer is faulted */

    for (;;) {
        diag_loop_mark();

        for (unsigned i = 0; i < LINK_COUNT; i++) {
            link_id_t id = (link_id_t)i;
            size_t n = link_read(id, s_buf, sizeof s_buf);
#if APP_MODE == APP_MODE_PATTERN
            if (n != 0u) {
                TEST_TOGGLE(k_rx_seen_mask[i]);
                pattern_rx(i, s_buf, n);
            }
            pattern_tx(i);
#else
            if (n != 0u) {
                TEST_TOGGLE(k_rx_seen_mask[i]);
                TEST_TOGGLE(k_tx_write_mask[i]);
                (void)link_write(id, s_buf, n); /* reject is counted */
            }
#endif
        }

        mag_consume();

#if APP_USB_STATUS
        status_poll();
#endif
    }
}
