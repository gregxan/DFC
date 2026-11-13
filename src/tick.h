/*
 * 1 kHz hardware-timer tick (M-6, M-6a). Runs for the life of the
 * application; drives U-8, U-15a and the magnetometer schedule.
 */
#ifndef TICK_H
#define TICK_H

#include <stdint.h>

/* Start the tick. Call last during initialisation. */
void tick_init(void);

/* Scheduled (target) time of tick number idx, in microseconds since boot.
 * Ticks are on a fixed grid: tick_time_us(idx) = t0 + idx * 1000. */
uint64_t tick_time_us(uint32_t idx);

#endif /* TICK_H */
