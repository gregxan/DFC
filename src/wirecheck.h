/*
 * Magnetometer wiring self-test (diagnostic builds only, Rev 2.18).
 *
 * Runs once at start-up, before the I2C controller takes the pins, and
 * needs no test equipment: it uses the RP2350's own pull-up / pull-down
 * resistors and a slow bit-banged I2C address scan on the SDA/SCL pins.
 *
 *   1. Each line with the internal pull-up: a line that still reads low is
 *      shorted to GND, or held low (e.g. by pull-ups to an unpowered VDDIO).
 *   2. Each line driven low, then released with only the internal pull-down:
 *      a line that rises has an external pull-up (on the sensor board these
 *      go to VDDIO, so it also shows VDDIO is powered).
 *   3. SDA driven low while SCL is watched (and the reverse): SDA/SCL shorted.
 *   4. I2C address scan 0x08..0x77 with SDA/SCL as wired, then swapped in
 *      software, at ~25 kHz with the internal pull-ups on.
 *
 * Production builds do not include it (APP_USB_STATUS = 0).
 */
#ifndef WIRECHECK_H
#define WIRECHECK_H

#include <stdbool.h>
#include <stdint.h>

#define WIRE_SDA 0u
#define WIRE_SCL 1u
#define WIRE_MAX_FOUND 4u

typedef struct {
    bool ran;
    bool held_low[2];   /* [WIRE_SDA], [WIRE_SCL]: low with the internal pull-up */
    bool ext_pullup[2]; /* rises against the internal pull-down                 */
    bool shorted;       /* SDA and SCL joined                                   */
    uint8_t n_found[2]; /* addresses that ACKed: [0] as wired, [1] SDA/SCL swapped */
    uint8_t found[2][WIRE_MAX_FOUND];
} wirecheck_t;

/* Blocking, ~110 ms. Call before the pins are given to the I2C controller. */
void wirecheck_run(void);

const wirecheck_t *wirecheck_result(void);

/* True if addr ACKed in scan s (0 as wired, 1 swapped). */
bool wirecheck_found(unsigned s, uint8_t addr);

/* One-line conclusion for a person, e.g. "swap the SDA/SCL wires". */
const char *wirecheck_verdict(void);

#endif /* WIRECHECK_H */
