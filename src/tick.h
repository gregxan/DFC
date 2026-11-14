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

/* EX-10 (Rev 2.14): number of grid ticks that have passed since
 * tick_init(). The handler of tick n (n = 0, 1, ...) normally sets it to
 * n + 1; the main loop sees the change once the handler (~3 us) has
 * returned. If the handler ran a whole period late it skips ahead (M-6a)
 * and the count rises by more than 1 in one step, so a caller must not
 * assume it advances one at a time. Tick index i is scheduled at
 * tick_time_us(i); the first tick after the count read c is index c. */
uint32_t tick_count(void);

#endif /* TICK_H */
