/*
 * MMC5983MA sampled at 1 kHz over I2C1 entirely by DMA (section 5).
 *
 * Each 1 ms tick starts one transaction:
 *   START  W(0x00)  RESTART R x7  RESTART W(0x09, ctrl0 | TM_M)  STOP
 * i.e. read the measurement triggered by the previous transaction, then
 * trigger the next one. The I2C STOP_DET interrupt marks completion and
 * publishes the sample; TX_ABRT is the abort path (M-15).
 */
#ifndef MAG_H
#define MAG_H

#include <stdbool.h>
#include <stdint.h>

#include "mmc5983ma.h"

/* M-10 */
typedef struct {
    uint8_t raw[MMC_SAMPLE_BYTES];
    bool valid;            /* false for the first sample after start/abort (M-9) */
    uint32_t seq;          /* index of the tick that triggered the measurement   */
    uint64_t timestamp_us; /* time of that tick                                  */
} mag_sample_t;

/* Result of mag_read_latest(). */
typedef struct {
    mag_sample_t s;
    bool present; /* at least one sample has been published */
    bool stale;   /* M-17: timestamp older than MAG_STALE_US, or none yet */
} mag_reading_t;

/* Blocking start-up (before the main loop): probe the product ID (M-2) and
 * configure the sensor (M-3). On failure the fault flag is set, the
 * product-ID-fault counter is set to 1, and no transactions are attempted;
 * the tick keeps running for the UARTs (M-6). Returns false on fault. */
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

/* Called from the 1 ms tick ISR only. */
void mag_tick_isr(uint32_t tick_index);

#endif /* MAG_H */
