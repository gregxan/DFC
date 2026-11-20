#include "wirecheck.h"

#include "config.h"

#if APP_USB_STATUS

#include "hardware/gpio.h"
#include "pico/time.h"

static wirecheck_t s_wc;

/* ~25 kHz: slow enough for the internal ~50 kohm pull-ups alone. */
#define HALF_US 20u
/* Longest clock stretch accepted during the scan. */
#define STRETCH_US 500u

static void pin_input(uint pin, bool up, bool down) {
    gpio_set_dir(pin, GPIO_IN);
    gpio_set_pulls(pin, up, down);
}

static void od_low(uint pin) {
    gpio_put(pin, false);
    gpio_set_dir(pin, GPIO_OUT);
}

static void od_release(uint pin) {
    gpio_set_dir(pin, GPIO_IN);
}

/* Release SCL and wait for it to rise (clock stretching). */
static bool scl_high(uint scl) {
    od_release(scl);
    for (unsigned t = 0; t < STRETCH_US; t++) {
        if (gpio_get(scl)) {
            return true;
        }
        busy_wait_us(1);
    }
    return false;
}

/* One address-only write transaction: START, addr+W, read ACK, STOP.
 * Returns 1 for ACK, 0 for NACK, -1 if SCL is stuck low. */
static int probe_addr(uint sda, uint scl, uint8_t addr) {
    /* Bus idle: both high. START = SDA falls while SCL high. */
    od_release(sda);
    if (!scl_high(scl)) {
        return -1;
    }
    busy_wait_us(HALF_US);
    od_low(sda);
    busy_wait_us(HALF_US);
    od_low(scl);
    uint8_t byte = (uint8_t)(addr << 1); /* R/W = 0 */
    for (int i = 7; i >= 0; i--) {
        if (byte & (1u << i)) {
            od_release(sda);
        } else {
            od_low(sda);
        }
        busy_wait_us(HALF_US);
        if (!scl_high(scl)) {
            return -1;
        }
        busy_wait_us(HALF_US);
        od_low(scl);
    }
    /* ACK clock: release SDA, the device pulls it low to acknowledge. */
    od_release(sda);
    busy_wait_us(HALF_US);
    if (!scl_high(scl)) {
        return -1;
    }
    busy_wait_us(HALF_US / 2u);
    bool ack = !gpio_get(sda);
    busy_wait_us(HALF_US / 2u);
    od_low(scl);
    /* STOP = SDA rises while SCL high. */
    od_low(sda);
    busy_wait_us(HALF_US);
    (void)scl_high(scl);
    busy_wait_us(HALF_US);
    od_release(sda);
    busy_wait_us(HALF_US);
    return ack ? 1 : 0;
}

static void scan(unsigned s, uint sda, uint scl) {
    for (uint8_t a = 0x08u; a <= 0x77u; a++) {
        int r = probe_addr(sda, scl, a);
        if (r < 0) {
            return; /* SCL stuck: nothing more to learn */
        }
        if (r > 0) {
            if (s_wc.n_found[s] < WIRE_MAX_FOUND) {
                s_wc.found[s][s_wc.n_found[s]] = a;
            }
            if (s_wc.n_found[s] < 255u) {
                s_wc.n_found[s]++;
            }
        }
    }
}

void wirecheck_run(void) {
    const uint pin[2] = {MAG_SDA_PIN, MAG_SCL_PIN};
    for (unsigned i = 0; i < 2u; i++) {
        gpio_init(pin[i]);
        pin_input(pin[i], true, false);
    }
    busy_wait_us(200);

    /* 1. Held low against the internal pull-up. A sensor caught mid-read by
     * an MCU reset can hold SDA low until it is clocked out, so first give
     * it nine SCL pulses and a STOP (as the start-up bus clear does). */
    if (!gpio_get(pin[0]) && gpio_get(pin[1])) {
        for (unsigned k = 0; k < 9u && !gpio_get(pin[0]); k++) {
            od_low(pin[1]);
            busy_wait_us(HALF_US);
            od_release(pin[1]);
            busy_wait_us(HALF_US);
        }
        od_low(pin[0]);
        busy_wait_us(HALF_US);
        od_release(pin[0]);
        busy_wait_us(200);
    }
    for (unsigned i = 0; i < 2u; i++) {
        s_wc.held_low[i] = !gpio_get(pin[i]);
    }

    /* 2. External pull-up. Drive low first, then release with only the
     * pull-down: starting low also avoids RP2350-E9 (a pad left high can
     * stay high with only the internal pull-down). */
    for (unsigned i = 0; i < 2u; i++) {
        od_low(pin[i]);
        busy_wait_us(5);
        pin_input(pin[i], false, true);
        busy_wait_us(200);
        s_wc.ext_pullup[i] = gpio_get(pin[i]);
        pin_input(pin[i], true, false);
    }
    busy_wait_us(200);

    /* 3. Short between the lines (only meaningful if neither is held low). */
    if (!s_wc.held_low[0] && !s_wc.held_low[1]) {
        for (unsigned i = 0; i < 2u; i++) {
            od_low(pin[i]);
            busy_wait_us(20);
            bool other_low = !gpio_get(pin[1u - i]);
            od_release(pin[i]);
            busy_wait_us(100);
            s_wc.shorted = s_wc.shorted || other_low;
        }
    }

    /* 4. Address scans, as wired and swapped. */
    if (!s_wc.held_low[0] && !s_wc.held_low[1] && !s_wc.shorted) {
        scan(0u, MAG_SDA_PIN, MAG_SCL_PIN);
        scan(1u, MAG_SCL_PIN, MAG_SDA_PIN);
    }

    for (unsigned i = 0; i < 2u; i++) {
        pin_input(pin[i], true, false);
    }
    s_wc.ran = true;
}

const wirecheck_t *wirecheck_result(void) {
    return &s_wc;
}

bool wirecheck_found(unsigned s, uint8_t addr) {
    unsigned n = s_wc.n_found[s] < WIRE_MAX_FOUND ? s_wc.n_found[s] : WIRE_MAX_FOUND;
    for (unsigned k = 0; k < n; k++) {
        if (s_wc.found[s][k] == addr) {
            return true;
        }
    }
    return false;
}

const char *wirecheck_verdict(void) {
    const wirecheck_t *w = &s_wc;
    if (!w->ran) {
        return "not run";
    }
    if (w->held_low[0] && w->held_low[1]) {
        return "SDA and SCL both held low: shorted to GND, or the sensor's pull-ups go to an unpowered VDDIO";
    }
    if (w->held_low[0]) {
        return "SDA (D4) held low: shorted to GND, or pulled to an unpowered VDDIO";
    }
    if (w->held_low[1]) {
        return "SCL (D5) held low: shorted to GND, or pulled to an unpowered VDDIO";
    }
    if (w->shorted) {
        return "SDA and SCL are shorted together";
    }
    if (wirecheck_found(0u, (uint8_t)MAG_I2C_ADDR)) {
        if (!w->ext_pullup[0] || !w->ext_pullup[1]) {
            return "sensor answers at 0x30, but SDA/SCL lack external pull-ups (too weak for 400 kHz): add 4.7k to 3V3";
        }
        return "sensor answers at 0x30 as wired: wiring OK";
    }
    if (wirecheck_found(1u, (uint8_t)MAG_I2C_ADDR)) {
        return "sensor answers only with SDA/SCL swapped: swap the D4/D5 wires";
    }
    if (w->n_found[0] != 0u || w->n_found[1] != 0u) {
        return "a device answers, but not at 0x30: a different chip, or not an MMC5983MA";
    }
    if (!w->ext_pullup[0] && !w->ext_pullup[1]) {
        return "nothing answers and no pull-ups seen: sensor VDDIO not powered, or SDA/SCL not reaching the sensor";
    }
    if (!w->ext_pullup[0]) {
        return "nothing answers; pull-up on SCL but not SDA: check the SDA (D4) wire";
    }
    if (!w->ext_pullup[1]) {
        return "nothing answers; pull-up on SDA but not SCL: check the SCL (D5) wire";
    }
    return "pull-ups present but nothing answers: check CS is tied to VDDIO, VDD is powered, and GND";
}

#else

typedef int wirecheck_unused_t; /* ISO C: no empty translation unit */

#endif /* APP_USB_STATUS */
