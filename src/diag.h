/*
 * Status counters, high-water marks and timing instrumentation (section 6).
 */
#ifndef DIAG_H
#define DIAG_H

#include <stdbool.h>
#include <stdint.h>

#include "hardware/structs/m33.h"
#include "hardware/sync.h"

#include "config.h"

/* D-1. Each counter has exactly one incrementing context, so a plain
 * read-modify-write is safe; the main loop clears with diag_take(). */
typedef enum {
    DIAG_L0_RX_OVERFLOW,
    DIAG_L0_TX_REJECT,
    DIAG_L0_FRAMING,
    DIAG_L0_BREAK,
    DIAG_L0_OVERRUN,
    DIAG_L1_RX_OVERFLOW,
    DIAG_L1_TX_REJECT,
    DIAG_L1_FRAMING,
    DIAG_L1_BREAK,
    DIAG_L1_OVERRUN,
    DIAG_MAG_I2C_ERROR,
    DIAG_MAG_MISSED_TICK,
    DIAG_MAG_TIMEOUT,
    DIAG_MAG_PRODUCT_ID_FAULT,
    DIAG_MAG_BUS_CLEAR, /* D-1 (Rev 2.2): bus clears, start-up and post-abort */
    DIAG_MAG_RECOVERED, /* D-1 (Rev 2.7): re-probes that cleared the fault flag */
    DIAG_MAG_NOT_READY, /* D-1 (Rev 2.8): samples read with Meas_M_Done clear */
    DIAG_COUNT
} diag_ctr_t;

/* Per-link counter = base + DIAG_LINK_STRIDE * link. */
#define DIAG_LINK_STRIDE (DIAG_L1_RX_OVERFLOW - DIAG_L0_RX_OVERFLOW)

/* Timing slots, maximum cycles (V-5). */
typedef enum {
    DIAG_T_ISR_UART0_ERR,
    DIAG_T_ISR_UART1_ERR,
    DIAG_T_ISR_TX_DMA,
    DIAG_T_ISR_I2C,
    DIAG_T_ISR_TICK,
    DIAG_T_CRITICAL, /* longest critical section in the main-loop API       */
    DIAG_T_API_READ, /* link_read overhead, payload copy excluded (EX-7)    */
    DIAG_T_API_WRITE,
    DIAG_T_API_MAG,
    DIAG_T_LOOP, /* main-loop iteration (D-4)                               */
    DIAG_T_COUNT
} diag_timing_t;

extern volatile uint32_t g_diag_ctr[DIAG_COUNT];
extern volatile uint32_t g_diag_max_cycles[DIAG_T_COUNT];
extern volatile uint32_t g_diag_rx_hwm[2]; /* D-3, bytes */
extern volatile uint32_t g_diag_tx_hwm[2];
extern uint32_t g_diag_actual_baud[2];

void diag_init(void);

static inline void diag_inc(diag_ctr_t c) {
    g_diag_ctr[c] = g_diag_ctr[c] + 1u;
}

/* D-2: atomic read-and-clear of one counter. Main loop only. */
uint32_t diag_take(diag_ctr_t c);

/* Read-and-clear every counter in one critical section. Main loop only. */
void diag_take_all(uint32_t out[DIAG_COUNT]);

static inline void diag_hwm(volatile uint32_t *hwm, uint32_t level) {
    if (level > *hwm) {
        *hwm = level;
    }
}

/* ---- cycle counter --------------------------------------------------- */

static inline uint32_t diag_cycles(void) {
#if DIAG_TIMING
    return m33_hw->dwt_cyccnt;
#else
    return 0u;
#endif
}

static inline void diag_max(diag_timing_t t, uint32_t cycles) {
#if DIAG_TIMING
    if (cycles > g_diag_max_cycles[t]) {
        g_diag_max_cycles[t] = cycles;
    }
#else
    (void)t;
    (void)cycles;
#endif
}

/* ---- critical sections (EX-3) ---------------------------------------- */

typedef struct {
    uint32_t irq;
    uint32_t t0;
} cs_t;

static inline cs_t cs_enter(void) {
    cs_t c;
    c.irq = save_and_disable_interrupts();
    c.t0 = diag_cycles();
    return c;
}

/* Only used from the main loop, so DIAG_T_CRITICAL has a single writer. */
static inline void cs_exit(cs_t c) {
    uint32_t d = diag_cycles() - c.t0;
    restore_interrupts(c.irq);
    diag_max(DIAG_T_CRITICAL, d);
}

/* D-4: call once at the top of every main-loop iteration. */
void diag_loop_mark(void);

/* Convert cycles to microseconds (rounded up) at the current clk_sys. */
uint32_t diag_cycles_to_us(uint32_t cycles);

#endif /* DIAG_H */
