#include "diag.h"

#include "hardware/clocks.h"
#include "hardware/irq.h"
#include "hardware/structs/scb.h"
#include "hardware/sync.h"

#include "crash.h"

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

volatile uint32_t g_diag_isr_cycles;

#if APP_USB_STATUS && DIAG_TIMING
/* Rev 2.22: every enabled interrupt goes through isr_wrap(), which times
 * the outermost handler (nested ones fall inside it) and then calls the
 * original handler. Diagnostic builds only: production has no USB stack
 * preempting the main loop, and keeps the plain vector table. */
static irq_handler_t s_isr_orig[NUM_IRQS];
static volatile uint32_t s_isr_depth;
static uint32_t s_isr_t0;

static void __not_in_flash_func(isr_wrap)(void) {
    uint32_t irq = __get_current_exception() - VTABLE_FIRST_IRQ;
    uint32_t save = save_and_disable_interrupts();
    /* Read the time with interrupts off (bench fix, Rev 2.25). Read before,
     * a higher-priority handler could run in between, be counted as
     * outermost, and then be counted again inside this one's span. The
     * double count made the API timings go negative (wrapping to ~2.9 s). */
    uint32_t t = diag_cycles();
    bool outer = (s_isr_depth++ == 0u);
    if (outer) {
        s_isr_t0 = t;
    }
    restore_interrupts(save);

    /* D-9: which interrupt is running, for the watchdog report. */
    uint32_t prev = watchdog_hw->scratch[1];
    crash_irq(irq + 1u);
    s_isr_orig[irq]();
    crash_irq(prev);

    save = save_and_disable_interrupts();
    if (outer) {
        g_diag_isr_cycles = g_diag_isr_cycles + (diag_cycles() - s_isr_t0);
    }
    s_isr_depth = s_isr_depth - 1u;
    restore_interrupts(save);
}

void diag_isr_accounting_init(void) {
    irq_handler_t *vt = (irq_handler_t *)scb_hw->vtor;
    uint32_t save = save_and_disable_interrupts();
    for (uint i = 0; i < NUM_IRQS; i++) {
        if (irq_is_enabled(i)) {
            s_isr_orig[i] = vt[VTABLE_FIRST_IRQ + i];
            vt[VTABLE_FIRST_IRQ + i] = isr_wrap;
        }
    }
    __dsb();
    __isb();
    restore_interrupts(save);
}
#else
void diag_isr_accounting_init(void) {
}
#endif

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
    /* DIAG_COUNT (about 20) load/store pairs: well under 1 us at 150 MHz. */
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
