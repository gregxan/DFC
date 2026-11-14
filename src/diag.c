#include "diag.h"

#include "hardware/clocks.h"

volatile uint32_t g_diag_ctr[DIAG_COUNT];
volatile uint32_t g_diag_max_cycles[DIAG_T_COUNT];
volatile uint32_t g_diag_rx_hwm[2];
volatile uint32_t g_diag_tx_hwm[2];
volatile uint32_t g_diag_tx_irq[2];
uint32_t g_diag_actual_baud[2];

static uint32_t s_loop_last;
static bool s_loop_started;
static uint32_t s_loop_excluded; /* cycles of excluded work this iteration */
static uint32_t s_cycles_per_us;

void diag_init(void) {
#if DIAG_TIMING
    m33_hw->demcr |= M33_DEMCR_TRCENA_BITS;
    m33_hw->dwt_cyccnt = 0u;
    m33_hw->dwt_ctrl |= M33_DWT_CTRL_CYCCNTENA_BITS;
#endif
    s_cycles_per_us = clock_get_hz(clk_sys) / 1000000u;
    if (s_cycles_per_us == 0u) {
        s_cycles_per_us = 1u;
    }
}

uint32_t diag_take(diag_ctr_t c) {
    cs_t cs = cs_enter();
    uint32_t v = g_diag_ctr[c];
    g_diag_ctr[c] = 0u;
    cs_exit(cs);
    return v;
}

void diag_take_all(uint32_t out[DIAG_COUNT]) {
    /* 15 load/store pairs: well under 1 us at 150 MHz. */
    cs_t cs = cs_enter();
    for (unsigned i = 0; i < DIAG_COUNT; i++) {
        out[i] = g_diag_ctr[i];
        g_diag_ctr[i] = 0u;
    }
    cs_exit(cs);
}

void diag_loop_mark(void) {
    uint32_t now = diag_cycles();
    if (s_loop_started) {
        diag_max(DIAG_T_LOOP, now - s_loop_last - s_loop_excluded);
    }
    s_loop_started = true;
    s_loop_last = now;
    s_loop_excluded = 0u;
}

uint32_t diag_excluded_begin(void) {
    return diag_cycles();
}

void diag_excluded_end(uint32_t t0) {
    uint32_t d = diag_cycles() - t0;
    s_loop_excluded += d;
    diag_max(DIAG_T_STATUS, d);
}

uint32_t diag_cycles_to_us(uint32_t cycles) {
    return (cycles + s_cycles_per_us - 1u) / s_cycles_per_us;
}
