/*
 * Hang and crash capture (diagnostic builds only, D-9, Rev 2.23).
 *
 * Diagnostic builds arm the RP2350 watchdog (2 s), fed by the main loop.
 * Breadcrumbs live in watchdog scratch registers 0-3, which survive the
 * watchdog reset (the SDK and bootrom use 4-7):
 *
 *   scratch[0]  0xDF | kind | main-loop breadcrumb | exception number
 *   scratch[1]  interrupt running now (IRQ + 1, 0 = none)    fault: PC
 *   scratch[2]  tick count, written by the tick ISR          fault: LR
 *   scratch[3]  tick count last seen by the main loop        fault: CFSR
 *   (panic: message format pointer and the first two arguments)
 *
 * If the main loop stops for 2 s (stuck in a loop, in a library call, or
 * starved by an interrupt that never returns), the watchdog resets the
 * board and the next report says where it was. A HardFault records the
 * faulting PC, LR and CFSR, then resets the same way.
 *
 * Production builds have none of this: they never reset themselves (D-5).
 */
#ifndef CRASH_H
#define CRASH_H

#include <stdbool.h>
#include <stdint.h>

#include "config.h"

/* Main-loop breadcrumbs. */
enum {
    CRUMB_NONE = 0,
    CRUMB_APP = 1,           /* inside app_poll()                        */
    CRUMB_STATUS_INPUT = 2,  /* status: reading the host's input (USB)   */
    CRUMB_STATUS_FORMAT = 3, /* status: formatting a report line         */
    CRUMB_STATUS_SEND = 4,   /* status: handing a line to USB            */
    CRUMB_IDLE = 5,          /* main loop, between the above             */
    CRUMB_COUNT
};

enum { CRASH_KIND_NONE = 0, CRASH_KIND_WATCHDOG = 1, CRASH_KIND_FAULT = 2, CRASH_KIND_PANIC = 3 };

typedef struct {
    uint8_t kind;      /* CRASH_KIND_*                                     */
    uint8_t crumb;     /* main-loop breadcrumb at the time                 */
    uint8_t exception; /* fault: active exception (0 = thread mode)        */
    uint32_t irq;      /* watchdog: interrupt running (IRQ + 1, 0 = none)  */
    uint32_t isr_tick; /* watchdog: last tick count from the tick ISR      */
    uint32_t loop_tick;/* watchdog: last tick count seen by the main loop  */
    uint32_t pc, lr, cfsr; /* fault                                       */
    const char *panic_fmt; /* panic: message format (in the image, still valid) */
    uint32_t panic_arg[2]; /* panic: first two arguments                   */
} crash_t;

#if APP_USB_STATUS

#include "hardware/structs/watchdog.h"

#define CRASH_MAGIC 0xDF000000u
#define CRASH_ARMED 0x00010000u
#define CRASH_FAULT 0x00020000u
#define CRASH_PANIC 0x00030000u

static inline void crash_crumb(unsigned c) {
    watchdog_hw->scratch[0] = CRASH_MAGIC | CRASH_ARMED | ((uint32_t)c << 8);
}

static inline void crash_tick(uint32_t tick) {
    watchdog_hw->scratch[2] = tick;
}

static inline void crash_irq(uint32_t irq_plus_1) {
    watchdog_hw->scratch[1] = irq_plus_1;
}

/* First thing in main(): read and clear what the last reset left. */
void crash_capture(void);
/* After start-up: arm the watchdog. */
void crash_arm(void);
/* Main loop: feed the watchdog, note the tick, blink the heartbeat LED. */
void crash_feed(void);
/* Before an intentional reboot (e.g. '~' to the USB bootloader). */
void crash_disarm(void);

const crash_t *crash_last(void);

#else

#define crash_crumb(c) ((void)0)
#define crash_tick(t) ((void)0)
#define crash_irq(i) ((void)0)

#endif

#endif /* CRASH_H */
