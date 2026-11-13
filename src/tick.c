#include "tick.h"

#include "hardware/irq.h"
#include "hardware/timer.h"

#include "config.h"
#include "diag.h"
#include "link.h"
#include "mag.h"

static uint s_alarm;
static uint32_t s_alarm_bit;
static uint64_t s_t0;          /* time of tick 0                     */
static uint64_t s_next_target; /* time of the tick now being awaited */
static uint32_t s_next_index;

uint64_t __not_in_flash_func(tick_time_us)(uint32_t idx) {
    return s_t0 + (uint64_t)idx * TICK_PERIOD_US;
}

static void __not_in_flash_func(tick_isr)(void) {
    uint32_t t0 = diag_cycles();
    timer_hw->intr = s_alarm_bit;

    uint32_t idx = s_next_index;

    /* M-6a: schedule from the previous target, never from "now". */
    s_next_target += TICK_PERIOD_US;
    s_next_index = idx + 1u;
    timer_hw->alarm[s_alarm] = (uint32_t)s_next_target;

    /* Alarms match on equality of the low 32 bits. If the target is
     * already in the past (only possible if this ISR was held off for a
     * whole period, e.g. by a debugger), skip forward on the same grid. */
    while ((int32_t)(timer_hw->timerawl - (uint32_t)s_next_target) >= 0) {
        s_next_target += TICK_PERIOD_US;
        s_next_index++;
        diag_inc(DIAG_MAG_MISSED_TICK);
        timer_hw->alarm[s_alarm] = (uint32_t)s_next_target;
    }

    link_tick_isr();  /* U-8, U-15a */
    mag_tick_isr(idx); /* M-6, M-16 */

    diag_max(DIAG_T_ISR_TICK, diag_cycles() - t0);
}

void tick_init(void) {
    s_alarm = (uint)timer_hardware_alarm_claim_unused(timer_hw, true);
    s_alarm_bit = 1u << s_alarm;
    uint irq = timer_hardware_alarm_get_irq_num(timer_hw, s_alarm);
    irq_set_exclusive_handler(irq, tick_isr);
    irq_set_priority(irq, IRQ_PRIO_TICK);

    s_t0 = time_us_64() + TICK_PERIOD_US;
    s_next_target = s_t0;
    s_next_index = 0u;

    hw_set_bits(&timer_hw->inte, s_alarm_bit);
    timer_hw->alarm[s_alarm] = (uint32_t)s_next_target;
    irq_set_enabled(irq, true);
}
