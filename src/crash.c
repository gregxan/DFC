#include "crash.h"

#if APP_USB_STATUS

#include "hardware/gpio.h"
#include "hardware/structs/m33.h"
#include "hardware/watchdog.h"

#include "tick.h"

#define WATCHDOG_MS 2000u
#ifndef PICO_DEFAULT_LED_PIN
#define PICO_DEFAULT_LED_PIN 25
#endif

static crash_t s_last;

void crash_capture(void) {
    uint32_t s0 = watchdog_hw->scratch[0];
    uint32_t reason = watchdog_hw->reason;
    if ((s0 & 0xFF000000u) == CRASH_MAGIC) {
        uint32_t kind = s0 & 0x00FF0000u;
        if (kind == CRASH_FAULT) {
            s_last.kind = CRASH_KIND_FAULT;
            s_last.pc = watchdog_hw->scratch[1];
            s_last.lr = watchdog_hw->scratch[2];
            s_last.cfsr = watchdog_hw->scratch[3];
        } else if (kind == CRASH_ARMED && (reason & WATCHDOG_REASON_TIMER_BITS)) {
            s_last.kind = CRASH_KIND_WATCHDOG;
            s_last.irq = watchdog_hw->scratch[1];
            s_last.isr_tick = watchdog_hw->scratch[2];
            s_last.loop_tick = watchdog_hw->scratch[3];
        }
        s_last.crumb = (uint8_t)(s0 >> 8);
        s_last.exception = (uint8_t)s0;
    }
    for (unsigned i = 0; i < 4u; i++) {
        watchdog_hw->scratch[i] = 0u;
    }

    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
}

void crash_arm(void) {
    crash_crumb(CRUMB_IDLE);
    watchdog_enable(WATCHDOG_MS, true); /* paused while a debugger halts the core */
}

void crash_feed(void) {
    watchdog_update();
    uint32_t t = tick_count();
    watchdog_hw->scratch[3] = t;
    gpio_put(PICO_DEFAULT_LED_PIN, ((t >> 8) & 1u) != 0u); /* ~2 Hz blink */
}

void crash_disarm(void) {
    watchdog_disable();
    watchdog_hw->scratch[0] = 0u;
}

const crash_t *crash_last(void) {
    return &s_last;
}

/* HardFault (MemManage, BusFault and UsageFault escalate to it, as they are
 * not enabled separately): record the faulting PC/LR and CFSR, then reset
 * through the watchdog so the next report can show them. */
void __attribute__((used, noreturn)) crash_hardfault_c(const uint32_t *frame);
void __attribute__((used, noreturn)) crash_hardfault_c(const uint32_t *frame) {
    uint32_t xpsr = frame[7];
    uint32_t s0 = watchdog_hw->scratch[0];
    watchdog_hw->scratch[0] = CRASH_MAGIC | CRASH_FAULT | (s0 & 0x0000FF00u) | (xpsr & 0xFFu);
    watchdog_hw->scratch[1] = frame[6]; /* PC */
    watchdog_hw->scratch[2] = frame[5]; /* LR */
    watchdog_hw->scratch[3] = m33_hw->cfsr;
    hw_set_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_TRIGGER_BITS);
    for (;;) {
    }
}

void __attribute__((naked)) isr_hardfault(void) {
    __asm volatile("tst lr, #4\n"
                   "ite eq\n"
                   "mrseq r0, msp\n"
                   "mrsne r0, psp\n"
                   "b crash_hardfault_c\n");
}

#else

typedef int crash_unused_t; /* ISO C: no empty translation unit */

#endif
