/*
 * MMC5983MA sampled at 1 kHz over I2C1 entirely by DMA (section 5).
 *
 * Each 1 ms tick starts one transaction:
 *   START  W(0x00)  RESTART R x9  [RESTART W(0x09, SET|RESET)]
 *          RESTART W(0x09, TM_M)  STOP
 * i.e. read the measurement triggered by the previous transaction, then
 * trigger the next one. The I2C STOP_DET interrupt marks completion and
 * publishes the sample; TX_ABRT is the abort path (M-15).
 */
#ifndef MAG_H
#define MAG_H

#include <stdbool.h>
#include <stdint.h>

#include "mmc5983ma.h"

#include "config.h"

/* M-4 (Rev 2.8): SET/RESET strategy, MAG_SR_INIT_ONLY (0) or MAG_SR_PULSE
 * (1), see config.h. Read at the start of every transaction, so a
 * change (e.g. written from the debugger) takes effect within 1 ms. Invalid
 * values are ignored and the previous mode stays in use. */
extern volatile uint32_t g_mag_sr_mode;

/* MAG_SR_PULSE only: samples from one SET pulse to the next. Values outside
 * 1..MAG_SR_INTERVAL_MAX are clamped when used. */
extern volatile uint32_t g_mag_sr_interval;

/* M-9, M-10 (Rev 2.11): why a sample is or is not valid. */
typedef enum {
    MAG_SAMPLE_OK = 0,          /* valid                                           */
    MAG_SAMPLE_NO_PRIOR = 1,    /* first after start-up/abort: nothing was triggered */
    MAG_SAMPLE_AFTER_RESET = 2, /* measured right after RESET: inverted polarity    */
    MAG_SAMPLE_NOT_READY = 3,   /* read with Meas_M_Done clear: incomplete          */
} mag_sample_reason_t;

/* M-10. raw, tout and status are filled by one 9-byte DMA read of
 * registers 0x00..0x08 and must stay adjacent (checked in mag.c). */
typedef struct {
    uint8_t raw[MMC_SAMPLE_BYTES];
    uint8_t tout;          /* register 0x07; not updated (TM_T unused)         */
    uint8_t status;        /* register 0x08 at read time (Meas_M_Done = bit 0) */
    bool valid;            /* true exactly when reason == MAG_SAMPLE_OK (M-9)  */
    uint8_t reason;        /* mag_sample_reason_t (Rev 2.11)                   */
    uint32_t seq;          /* index of the tick whose transaction triggered it   */
    uint64_t timestamp_us; /* us since boot when the trigger (TM_M) write completed
                            * (Rev 2.6); for M-9 invalid samples with no prior
                            * trigger, the nominal previous tick time            */
} mag_sample_t;

/* Result of mag_read_latest(). */
typedef struct {
    mag_sample_t s;
    bool present; /* at least one sample has been published */
    bool stale;   /* M-17: timestamp older than MAG_STALE_US, or none yet */
    /* M-4b (Rev 2.9): bridge offset from the last SET/RESET calibration, in
     * the same signed counts as mag_convert(). Subtract it from every axis:
     * field = mag_convert(raw) - offset. Not applied by the firmware. */
    int32_t offset[3];
    bool offset_valid; /* false until the first successful calibration */
} mag_reading_t;

/* Blocking start-up (before the main loop): bus clear, software reset,
 * product-ID and OTP check (M-2), configuration (M-3) and the first offset
 * calibration (M-4b). On failure the fault flag is set and a product-ID
 * fault is counted; the tick then re-probes at 5 Hz (M-2a) and keeps
 * running for the UARTs (M-6). Returns false on fault. */
bool mag_init(void);

bool mag_fault(void);

/* M-13: copy the latest sample in a short critical section. Main loop only.
 * Returns r->present. */
bool mag_read_latest(mag_reading_t *r);

/* Convert raw bytes to signed 18-bit counts (offset 131072 removed).
 * 16384 counts/G at 18 bits. Intended for the main loop (M-10). */
static inline void mag_convert(const uint8_t raw[MMC_SAMPLE_BYTES], int32_t xyz[3]) {
    uint32_t x = ((uint32_t)raw[0] << 10) | ((uint32_t)raw[1] << 2) | (((uint32_t)raw[6] >> 6) & 3u);
    uint32_t y = ((uint32_t)raw[2] << 10) | ((uint32_t)raw[3] << 2) | (((uint32_t)raw[6] >> 4) & 3u);
    uint32_t z = ((uint32_t)raw[4] << 10) | ((uint32_t)raw[5] << 2) | (((uint32_t)raw[6] >> 2) & 3u);
    xyz[0] = (int32_t)x - 131072;
    xyz[1] = (int32_t)y - 131072;
    xyz[2] = (int32_t)z - 131072;
}

/* Convenience: field in signed counts with the calibrated offset removed
 * (offset ignored if not valid). 16384 counts/G. */
static inline void mag_field(const mag_reading_t *r, int32_t xyz[3]) {
    mag_convert(r->s.raw, xyz);
    if (r->offset_valid) {
        for (int i = 0; i < 3; i++) {
            xyz[i] -= r->offset[i];
        }
    }
}

/* Field at 16-bit scale: 4096 counts/G, so +/-8 G spans the int16 range.
 * Calibrated offset removed, rounded to nearest (halves away from zero) and
 * clamped to [-32768, 32767]. At BW = 11 the sensor noise (1.2 mG RMS, ~5
 * counts here) is well above the 0.25 mG step, so nothing useful is lost;
 * it halves the size of X/Y/Z in a message (6 bytes instead of 12). */
static inline void mag_field16(const mag_reading_t *r, int16_t xyz[3]) {
    int32_t v[3];
    mag_field(r, v);
    for (int i = 0; i < 3; i++) {
        int32_t q = v[i] >= 0 ? (v[i] + 2) / 4 : -((-v[i] + 2) / 4);
        if (q > INT16_MAX) {
            q = INT16_MAX;
        } else if (q < INT16_MIN) {
            q = INT16_MIN;
        }
        xyz[i] = (int16_t)q;
    }
}

/* Called from the 1 ms tick ISR only. */
void mag_tick_isr(uint32_t tick_index);

#endif /* MAG_H */
