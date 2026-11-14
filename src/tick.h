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

/* EX-10 (Rev 2.14): number of ticks that have occurred since tick_init().
 * The handler of tick n (n = 0, 1, ...) sets it to n + 1, so after it
 * changes, the tick that just fired is tick_count() - 1, scheduled at
 * tick_time_us(tick_count() - 1). The main loop sees the change once the
 * tick handler (~3 us) has returned. */
uint32_t tick_count(void);

#endif /* TICK_H */
