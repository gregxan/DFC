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
    DIAG_MAG_NO_ACK,    /* D-1 (Rev 2.17): probes / start-up checks not
                         * acknowledged at the sensor address (no sensor)  */
    DIAG_MAG_WRONG_ID,  /* D-1 (Rev 2.17): probes / start-up checks that read
                         * a product ID other than 0x30                    */
    DIAG_MAG_BUS_CLEAR, /* D-1 (Rev 2.2): bus clears, start-up and post-abort */
    DIAG_MAG_RECOVERED, /* D-1 (Rev 2.7): re-probes that cleared the fault flag */
    DIAG_MAG_NOT_READY, /* D-1 (Rev 2.8): samples read with Meas_M_Done clear */
    DIAG_MAG_OFFSET_CAL, /* D-1 (Rev 2.9): successful offset calibrations      */
    DIAG_MAG_SATURATED, /* D-1 (Rev 2.9): valid samples at/near full scale    */
    DIAG_MAG_RUNTIME_FAULT, /* D-1 (Rev 2.16): fault flag set at run time by
                             * persistent not-ready samples (M-2b)            */
    DIAG_TICK_OVERRUN,  /* D-1 (Rev 2.11): 1 ms ticks skipped because the tick
                         * ISR ran a whole period late (M-6a)              */
    DIAG_EPOCH_OVERRUN, /* D-1 (Rev 2.14): user epochs whose app_poll() ran
                         * past the next tick (EX-8)                       */
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
    DIAG_T_LOOP,   /* main-loop iteration excluding the status reporter (D-4) */
    DIAG_T_STATUS, /* status reporter, diag builds only (D-4, Rev 2.11)       */
    DIAG_T_COUNT
} diag_timing_t;

extern volatile uint32_t g_diag_ctr[DIAG_COUNT];
extern volatile uint32_t g_diag_max_cycles[DIAG_T_COUNT];
extern volatile uint32_t g_diag_rx_hwm[2]; /* D-3, bytes */
extern volatile uint32_t g_diag_tx_hwm[2];
extern volatile uint32_t g_diag_tx_irq[2]; /* TX DMA completion interrupts (Rev 2.10), cumulative */
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

/* ---- API timing without interrupt time (Rev 2.22) -------------------- */

/* Cycles spent in interrupt handlers since boot (outermost handler entry to
 * exit, nested ones included once). Counted only in diagnostic builds,
 * where diag_isr_accounting_init() wraps every enabled interrupt; it stays
 * 0 otherwise. Lets the API timings (EX-7) leave out time the main loop was
 * preempted, e.g. by the USB stack, which is not part of the API's cost. */
extern volatile uint32_t g_diag_isr_cycles;

typedef struct {
    uint32_t t0;
    uint32_t i0;
} diag_span_t;

static inline diag_span_t diag_span_begin(void) {
    diag_span_t s;
    s.i0 = g_diag_isr_cycles;
    s.t0 = diag_cycles();
    return s;
}

/* Cycles since diag_span_begin(), minus interrupt time in between. */
static inline uint32_t diag_span_cycles(diag_span_t s) {
    uint32_t elapsed = diag_cycles() - s.t0;
    uint32_t isr = g_diag_isr_cycles - s.i0;
    return isr < elapsed ? elapsed - isr : 0u;
}

/* Diagnostic builds: call once, after every interrupt handler is installed. */
void diag_isr_accounting_init(void);

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

/* D-4 (Rev 2.11): bracket diagnostic-only work (the USB status reporter).
 * Its duration is recorded in DIAG_T_STATUS and excluded from the
 * iteration time measured by diag_loop_mark(). */
uint32_t diag_excluded_begin(void);
void diag_excluded_end(uint32_t t0);

/* Convert cycles to microseconds (rounded up) at the current clk_sys. */
uint32_t diag_cycles_to_us(uint32_t cycles);

#endif /* DIAG_H */
