/*
 * Initialisation and the single bare-metal main loop on core 0 (EX-1,
 * HW-8). The application itself lives in app_user.c or app_test.c (app.h),
 * selected by the CMake option APP_MODE.
 *
 * User application (APP_MODE=user): 1 ms epochs (EX-10, Rev 2.14).
 *   The loop waits for the next 1 ms tick with interrupts enabled; DMA and
 *   the interrupt handlers keep receiving UART data and publishing
 *   magnetometer samples meanwhile (EX-2). Just after each tick it runs
 *   app_poll() once. Recorded per epoch (D-4):
 *     - work time of app_poll() (PROF_EPOCH: last / max / average),
 *     - start latency from the tick (g_epoch_start_max_us),
 *     - epochs that started late because the previous one ran past its
 *       tick, plus epochs skipped entirely (DIAG_EPOCH_OVERRUN).
 *   With APP_TEST_PINS, D8 (TEST_PIN_LINK0_RX_SEEN) is high while
 *   app_poll() runs, for logic-analyzer profiling.
 *
 * Test applications (echo / pattern): app_poll() runs on every pass, as
 * fast as possible; the pass time is recorded as DIAG_T_LOOP (D-4).
 */
#include <stdbool.h>
#include <stdint.h>

#include "hardware/gpio.h"
#include "hardware/structs/sio.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

#include "app.h"
#include "config.h"
#include "diag.h"
#include "link.h"
#include "mag.h"
#include "prof.h"
#include "status.h"
#include "tick.h"

volatile bool g_app_baud_ok; /* U-2 result of link_init(); reported by status.c */

#if APP_MODE == APP_MODE_USER

volatile uint32_t g_epoch_start_max_us; /* worst epoch start latency after its tick */

#if APP_TEST_PINS
#define EPOCH_PIN_MASK (1u << TEST_PIN_LINK0_RX_SEEN) /* D8 */
static void epoch_pin_init(void) {
    gpio_init(TEST_PIN_LINK0_RX_SEEN);
    gpio_set_dir(TEST_PIN_LINK0_RX_SEEN, GPIO_OUT);
    gpio_put(TEST_PIN_LINK0_RX_SEEN, false);
}
#define EPOCH_PIN_HIGH() (sio_hw->gpio_set = EPOCH_PIN_MASK)
#define EPOCH_PIN_LOW() (sio_hw->gpio_clr = EPOCH_PIN_MASK)
#else
static void epoch_pin_init(void) {}
#define EPOCH_PIN_HIGH() ((void)0)
#define EPOCH_PIN_LOW() ((void)0)
#endif

static void __attribute__((noreturn)) run_epochs(void) {
    epoch_pin_init();
    uint32_t epoch = tick_count();
    for (;;) {
        /* Wait for the next 1 ms tick. Interrupts stay enabled: UART DMA,
         * the magnetometer and the tick keep running (EX-2). */
        uint32_t now;
        while ((now = tick_count()) == epoch) {
#if APP_USB_STATUS
            status_poll(); /* diagnostic builds only; not part of the epoch */
#endif
            /* __wfi(); */ /* optional: sleep until the next interrupt */
        }
        /* EX-8, D-1: epochs that were never run. More than one tick passed
         * since the last epoch began: app_poll() overran by more than a
         * whole period, or the tick itself skipped ahead (M-6a). */
        uint32_t skipped = now - epoch - 1u;
        if (skipped != 0u) {
            g_diag_ctr[DIAG_EPOCH_OVERRUN] = g_diag_ctr[DIAG_EPOCH_OVERRUN] + skipped;
        }

        /* Start latency: time since the first tick after the previous
         * epoch began, i.e. when this epoch should have started. That tick
         * has index `epoch` (the count before it fired). */
        uint32_t late_us = (uint32_t)(time_us_64() - tick_time_us(epoch));
        if (late_us > g_epoch_start_max_us) {
            g_epoch_start_max_us = late_us;
        }
        epoch = now;

        EPOCH_PIN_HIGH();
        PROF_BEGIN(PROF_EPOCH);
        app_poll();
        PROF_END(PROF_EPOCH);
        EPOCH_PIN_LOW();

        /* EX-8: the work must end before the next tick. If it did not, the
         * next epoch starts late; count it. (Further whole epochs lost are
         * counted as skipped, above.) */
        if (tick_count() != epoch) {
            diag_inc(DIAG_EPOCH_OVERRUN);
        }
    }
}

#else /* test applications */

static void __attribute__((noreturn)) run_free(void) {
    for (;;) {
        diag_loop_mark(); /* D-4: one pass of the test application */
        app_poll();
#if APP_USB_STATUS
        uint32_t ts = diag_excluded_begin(); /* D-4: not part of EX-8 */
        status_poll();
        diag_excluded_end(ts);
#endif
    }
}

#endif

int main(void) {
#if APP_USB_STATUS
    status_init(); /* USB CDC; priority lowered below all UART interrupts (B-20) */
#endif
    diag_init();

    g_app_baud_ok = link_init();
    (void)mag_init();
    app_init();
    tick_init(); /* M-6: runs even if the magnetometer is faulted */

#if APP_MODE == APP_MODE_USER
    run_epochs();
#else
    run_free();
#endif
}
