/*
 * Test application (APP_MODE=echo or pattern). Rev 2.14: moved out of
 * main.c unchanged. app_poll() runs on every main-loop pass, as fast as
 * possible, so the latency tests can measure how quickly a byte is seen.
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
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hardware/gpio.h"
#include "hardware/structs/sio.h"

#include "app.h"
#include "config.h"
#include "link.h"
#include "mag.h"

#if APP_MODE == APP_MODE_USER
#error "app_test.c is built only for APP_MODE echo or pattern"
#endif

/* Observable from a debugger (and reported by status.c). */
volatile int32_t g_app_mag_xyz[3];       /* field, offset removed when calibrated */
volatile int32_t g_app_mag_offset[3];    /* latest calibrated offset (M-4b)       */
volatile bool g_app_mag_offset_valid;
volatile uint32_t g_app_mag_gaps;    /* sequence discontinuities between valid samples */
volatile uint32_t g_app_mag_valid;   /* valid samples consumed                         */
volatile uint32_t g_app_mag_invalid; /* invalid (M-9) samples consumed                 */
volatile uint32_t g_app_mag_invalid_by_reason[4]; /* indexed by mag_sample_reason_t */
volatile uint32_t g_app_mag_stale;   /* iterations on which the latest sample was stale */
volatile uint32_t g_app_rx_pattern_errors[LINK_COUNT]; /* APP_MODE_PATTERN only */

/* D-10 (Rev 2.25): per-axis noise of the valid samples since the last
 * app_mag_noise_take() (once per status report). Sums of the field and of
 * its square, in counts; exact in 64 bits for well over 10^5 samples. */
static int64_t s_noise_sum[3];
static int64_t s_noise_sq[3];
static uint32_t s_noise_n;

void app_mag_noise_take(uint32_t *count, uint32_t rms_ug[3]) {
    uint32_t k = s_noise_n;
    *count = k;
    for (unsigned i = 0; i < 3u; i++) {
        rms_ug[i] = 0u;
        if (k >= 2u) {
            /* variance = (n * sum(x^2) - sum(x)^2) / n^2, in counts^2 */
            int64_t num = (int64_t)k * s_noise_sq[i] - s_noise_sum[i] * s_noise_sum[i];
            float var = (float)num / ((float)k * (float)k);
            float sd = var > 0.0f ? sqrtf(var) : 0.0f;
            rms_ug[i] = (uint32_t)(sd * (1.0e6f / 16384.0f) + 0.5f); /* 16384 counts/G */
        }
        s_noise_sum[i] = 0;
        s_noise_sq[i] = 0;
    }
    s_noise_n = 0u;
}

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
#define TEST_TOGGLE(mask) ((void)0) /* argument not evaluated */
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
    static uint32_t last_period; /* M-3b: next seq = last_seq + last_period */
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
        if (r.s.reason < 4u) {
            g_app_mag_invalid_by_reason[r.s.reason]++;
        }
        have_last = false; /* gap expected across an abort; counted by counters */
        return;
    }
    if (have_last && r.s.seq != last_seq + last_period) {
        g_app_mag_gaps++;
    }
    have_last = true;
    last_seq = r.s.seq;
    last_period = r.s.period_ticks;
    g_app_mag_valid++;

    int32_t xyz[3];
    mag_field(&r, xyz); /* offset removed when calibrated (M-4b) */
    for (unsigned i = 0; i < 3u; i++) { /* D-10 */
        s_noise_sum[i] += xyz[i];
        s_noise_sq[i] += (int64_t)xyz[i] * xyz[i];
    }
    s_noise_n++;
    g_app_mag_offset[0] = r.offset[0];
    g_app_mag_offset[1] = r.offset[1];
    g_app_mag_offset[2] = r.offset[2];
    g_app_mag_offset_valid = r.offset_valid;
    g_app_mag_xyz[0] = xyz[0];
    g_app_mag_xyz[1] = xyz[1];
    g_app_mag_xyz[2] = xyz[2];
}

void app_init(void) {
    test_pins_init();
}

void app_poll(void) {
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
}
