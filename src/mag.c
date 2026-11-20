#include "mag.h"

#include <stddef.h>
#include <string.h>

#include "hardware/dma.h"
#include "hardware/structs/dma_debug.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/structs/m33.h"
#include "pico/time.h"

#include "config.h"
#include "diag.h"
#include "tick.h"
#include "wirecheck.h"

#if MAG_BUS_CLEAR
#include "i2c_bus_clear.pio.h"
#endif

#define MAG_I2C i2c1
#define MAG_I2C_IRQ I2C1_IRQ

#define CMD_READ I2C_IC_DATA_CMD_CMD_BITS
#define CMD_STOP I2C_IC_DATA_CMD_STOP_BITS
#define CMD_RESTART I2C_IC_DATA_CMD_RESTART_BITS

#define I2C_TX_FIFO_DEPTH 16u

/* IC_TX_ABRT_SOURCE abort-cause flags (bits 16:0); bits 31:23 are the
 * TX_FLUSH_CNT field, not a cause. */
#define ABRT_REASON_BITS 0x0001ffffu
#define SLOT_NONE 2u

/* Pulse inserted before the trigger write. */
enum { PRE_NONE = 0, PRE_SET = 1, PRE_RESET = 2, PRE_COUNT = 3 };

volatile uint32_t g_mag_sr_mode = MAG_SR_MODE_DEFAULT;
volatile uint32_t g_mag_sr_interval = MAG_SR_INTERVAL_DEFAULT;

typedef enum {
    MAG_FAULT,    /* product ID not confirmed: re-probe at 5 Hz (M-2)   */
    MAG_IDLE,     /* ready to start on the next tick                    */
    MAG_BUSY,     /* transaction in flight                              */
    MAG_ABORTING, /* tick requested an abort (M-16), TX_ABRT not seen   */
    MAG_ABORTED,  /* abort handled; waiting for DMA/I2C/bus to go idle  */
} mag_state_t;

/* M-11: double buffer. s_latest is SLOT_NONE until the first publish. */
static mag_sample_t s_slots[2];
static volatile uint32_t s_latest = SLOT_NONE;

static volatile mag_state_t s_state = MAG_FAULT;
static volatile bool s_fault = true;

/* Kind of the transaction in flight (or last aborted). */
typedef enum { TXN_SAMPLE, TXN_PROBE, TXN_CONFIG } mag_txn_t;
static volatile mag_txn_t s_txn = TXN_SAMPLE;

/* M-2 (Rev 2.7) run-time re-probe. */
static uint32_t s_probe_wait;         /* ticks spent in MAG_FAULT since last probe */
static uint32_t s_not_ready_score;    /* M-2b: leaky score of not-ready samples     */
static volatile bool s_config_pending;/* probe matched: configure on next tick     */
static uint8_t s_probe_id;            /* product-ID byte received by DMA           */
static volatile uint8_t s_cause = MAG_CAUSE_NONE; /* mag_fault_cause_t (Rev 2.17)   */
static volatile uint8_t s_cause_id;   /* ID byte of the latest WRONG_ID failure    */
static uint32_t s_probe_cmd[2];
#define CFG_CMD_LEN 10u
static uint32_t s_cfg_cmd[CFG_CMD_LEN];

static volatile uint32_t s_cur_slot;
static volatile uint32_t s_cur_tick;
static volatile uint32_t s_cur_pre;  /* PRE_* used by the transaction in flight */
static volatile bool s_trig_ok;       /* previous transaction's TM_M write completed */
static volatile uint32_t s_trig_tick; /* tick of that transaction                    */
static uint64_t s_trig_time_us;       /* when its trigger write completed (M-10)    */
static volatile bool s_trig_reset;    /* ...and it RESET first: polarity inverted    */
static volatile bool s_trig_cal_set;  /* ...and it was the SET of a RESET->SET cycle  */

/* M-4b (Rev 2.9) offset calibration. Written in the I2C ISR (and once at
 * start-up); read by mag_read_latest() inside its critical section. */
static int32_t s_cal_reset_xyz[3];  /* measurement after RESET: -H + offset */
static bool s_cal_have_reset;
static int32_t s_offset[3];
static volatile bool s_offset_valid;

/* M-4 state. Tick ISR (mode selection) and I2C ISR (publish) only; the I2C
 * IRQ is masked while the tick runs, so they never interleave. */
static uint32_t s_mode_applied;   /* mode the transactions currently follow    */
static uint32_t s_cycle_step;     /* 0 none, 1 RESET pending, 2 SET pending      */
static uint32_t s_since_pulse;    /* completed transactions since the last SET  */
static bool s_pulse_now;          /* entering MAG_SR_PULSE: pulse immediately   */

/* Sample command lists, one per PRE_*. */
static uint32_t s_cmd[PRE_COUNT][I2C_TX_FIFO_DEPTH] __attribute__((aligned(4)));
static uint32_t s_cmd_len[PRE_COUNT];

/* The 9-byte DMA read lands in status, raw[] and tout in one go (Rev 2.21). */
_Static_assert(offsetof(mag_sample_t, raw) == offsetof(mag_sample_t, status) + 1u &&
                   offsetof(mag_sample_t, tout) == offsetof(mag_sample_t, raw) + MMC_SAMPLE_BYTES,
               "mag_sample_t: status, raw, tout must be contiguous");
_Static_assert(sizeof(mag_sample_t) == 24u, "reason must fit in existing padding");

static uint s_dma_cmd;
static uint s_dma_data;
static uint32_t s_dma_mask;

#if MAG_BUS_CLEAR
static PIO s_pio;
static uint s_sm;
static volatile bool s_bc_running;
#endif

bool mag_fault(void) {
    return s_fault;
}

mag_fault_cause_t mag_fault_cause(void) {
    return (mag_fault_cause_t)s_cause;
}

uint8_t mag_fault_id(void) {
    return s_cause_id;
}

/* ------------------------------------------------------------ helpers --- */

static inline i2c_hw_t *i2c_regs(void) {
    return i2c_get_hw(MAG_I2C);
}

static inline void i2c_irq_mask(void) {
    m33_hw->nvic_icer[MAG_I2C_IRQ / 32u] = 1u << (MAG_I2C_IRQ % 32u);
    __dsb();
    __isb();
}

static inline void i2c_irq_unmask(void) {
    m33_hw->nvic_iser[MAG_I2C_IRQ / 32u] = 1u << (MAG_I2C_IRQ % 32u);
}

/* M-7 (Rev 2.8). No persistent Internal Control 0 bits are used (M-5:
 * Auto_SR_en is never set), so each write carries only its own bit. */
static size_t build_cmds(uint32_t *c, unsigned pre) {
    size_t i = 0;
    /* Rev 2.21: Status first. Reading the output registers clears
     * Meas_M_Done, so a burst ending at Status always read "not ready". */
    c[i++] = MMC_REG_STATUS;                 /* START + W(0x08)      */
    c[i++] = CMD_RESTART | CMD_READ;         /* RESTART + R x1       */
    c[i++] = CMD_RESTART | MMC_REG_XOUT0;    /* RESTART + W(0x00)    */
    for (unsigned k = 0; k < MMC_READ_BYTES - 1u; k++) {
        c[i++] = CMD_READ | (k == 0u ? CMD_RESTART : 0u); /* RESTART + R x8 */
    }
    if (pre != PRE_NONE) {
        c[i++] = CMD_RESTART | MMC_REG_CTRL0;
        c[i++] = (pre == PRE_SET ? MMC_CTRL0_SET : MMC_CTRL0_RESET);
    }
    c[i++] = CMD_RESTART | MMC_REG_CTRL0;    /* RESTART + W(0x09)  */
    c[i++] = CMD_STOP | MMC_CTRL0_TM_M;       /* TM_M, STOP         */
    return i;
}

static inline uint32_t sr_interval(void) {
    uint32_t n = g_mag_sr_interval;
    if (n < 1u) {
        n = 1u;
    } else if (n > MAG_SR_INTERVAL_MAX) {
        n = MAG_SR_INTERVAL_MAX;
    }
    return n;
}

/* M-4: pick the pulse for the next transaction, applying any mode change. */
static unsigned __not_in_flash_func(sr_select)(void) {
    uint32_t mode = g_mag_sr_mode;
    if (mode > MAG_SR_PULSE) {
        mode = s_mode_applied; /* invalid value: ignore */
    }
    if (mode != s_mode_applied) {
        if (s_cycle_step != 0u) {
            /* Leaving an unfinished cycle: a RESET may already have been
             * applied, so always finish with SET to restore polarity. */
            s_cycle_step = 2u;
        }
        if (mode == MAG_SR_INIT_ONLY) {
            s_cycle_step = 1u; /* one RESET -> SET cycle */
        }
        s_pulse_now = (mode == MAG_SR_PULSE);
        s_mode_applied = mode;
    }

    if (s_cycle_step == 1u) {
        return PRE_RESET;
    }
    if (s_cycle_step == 2u) {
        return PRE_SET;
    }
    if (mode == MAG_SR_PULSE && (s_pulse_now || s_since_pulse + 1u >= sr_interval())) {
        return PRE_SET;
    }
    return PRE_NONE;
}

#if MAG_BUS_CLEAR
/* D-5, M-15, M-16 (Rev 2.2). Called from the I2C ISR after an abort. Non-blocking: the PIO clocks
 * SCL in the background (~100 us); the tick checks for completion. */
static void __not_in_flash_func(bus_clear_start)(void) {
    if (s_bc_running) {
        return;
    }
    s_bc_running = true;
    gpio_set_function(MAG_SCL_PIN, (gpio_function_t)pio_get_funcsel(s_pio));
    pio_sm_put(s_pio, s_sm, 0u);
    diag_inc(DIAG_MAG_BUS_CLEAR);
}

static bool __not_in_flash_func(bus_clear_done)(void) {
    if (!s_bc_running) {
        return true;
    }
    if (pio_sm_is_rx_fifo_empty(s_pio, s_sm)) {
        return false;
    }
    (void)pio_sm_get(s_pio, s_sm);
    gpio_set_function(MAG_SCL_PIN, GPIO_FUNC_I2C);
    s_bc_running = false;
    return true;
}
#endif

/* Both DMA channels and the I2C controller idle (M-16). Non-blocking. */
static bool __not_in_flash_func(hw_idle)(void) {
#if MAG_BUS_CLEAR
    /* First, and regardless of the controller's state: finishing a bus
     * clear (handing SCL back to I2C) must never wait on the controller,
     * or a controller that is active during a bus clear would block both
     * this and controller_hung() for good. */
    if (!bus_clear_done()) {
        return false;
    }
#endif
    if (dma_hw->abort & s_dma_mask) {
        return false;
    }
    if ((dma_hw->ch[s_dma_cmd].ctrl_trig | dma_hw->ch[s_dma_data].ctrl_trig) & DMA_CH0_CTRL_TRIG_BUSY_BITS) {
        return false;
    }
    i2c_hw_t *hw = i2c_regs();
    if (hw->enable & I2C_IC_ENABLE_ABORT_BITS) {
        return false;
    }
    uint32_t status = hw->status;
    if ((status & I2C_IC_STATUS_MST_ACTIVITY_BITS) || !(status & I2C_IC_STATUS_TFE_BITS)) {
        return false;
    }
    return true;
}

/* DMA finished and no abort pending, yet the controller still holds the
 * bus: e.g. a command word that reached the FIFO after an abort left the
 * master waiting for more. A fresh abort is required (M-16). */
static bool __not_in_flash_func(controller_hung)(void) {
#if MAG_BUS_CLEAR
    if (s_bc_running) {
        return false;
    }
#endif
    if (dma_hw->abort & s_dma_mask) {
        return false;
    }
    if ((dma_hw->ch[s_dma_cmd].ctrl_trig | dma_hw->ch[s_dma_data].ctrl_trig) & DMA_CH0_CTRL_TRIG_BUSY_BITS) {
        return false;
    }
    i2c_hw_t *hw = i2c_regs();
    return !(hw->enable & I2C_IC_ENABLE_ABORT_BITS) && (hw->status & I2C_IC_STATUS_MST_ACTIVITY_BITS);
}

/* Abort both magnetometer DMA channels without waiting (M-15, M-16).
 * RP2350-E5: a channel's EN bit must be cleared before CHAN_ABORT, or the
 * channel may re-trigger; start_dma() sets EN again. Clearing EN while BUSY
 * pauses the channel, and the abort then completes it. */
static inline void __not_in_flash_func(mag_dma_abort)(void) {
    hw_clear_bits(&dma_hw->ch[s_dma_cmd].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    hw_clear_bits(&dma_hw->ch[s_dma_data].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    dma_hw->abort = s_dma_mask;
}

/* Arm the data channel (if any data is expected), then the command
 * channel. Tick ISR only, I2C IRQ masked, hardware idle. */
static void __not_in_flash_func(start_dma)(mag_txn_t txn, const uint32_t *cmd, uint32_t n_cmd, uint8_t *dst,
                                           uint32_t n_data) {
    i2c_hw_t *hw = i2c_regs();

    /* Discard anything left in the RX FIFO by an aborted transfer
     * (bounded by the FIFO depth) and clear stale interrupt status. */
    for (unsigned k = 0; k < I2C_TX_FIFO_DEPTH && hw->rxflr != 0u; k++) {
        (void)hw->data_cmd;
    }
    (void)hw->clr_tx_abrt;
    (void)hw->clr_stop_det;

    s_txn = txn;
    s_state = MAG_BUSY;
    /* M-7a (Rev 2.12): resynchronise both channels' DREQ credit counters.
     * An abort flushes the I2C FIFOs, and the drain above reads the RX FIFO
     * with the CPU; either leaves a counter that no longer matches its FIFO
     * (datasheet 12.6.4.2), which would let the data channel read an empty
     * RX FIFO or starve the command channel. Writing DBG_CTDREQ clears the
     * counter and restarts the handshake. Both channels are idle here. */
    dma_debug_hw->ch[s_dma_data].dbg_ctdreq = 0u;
    dma_debug_hw->ch[s_dma_cmd].dbg_ctdreq = 0u;
    /* RP2350-E5: EN was cleared by the last abort, if any. */
    hw_set_bits(&dma_hw->ch[s_dma_data].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    hw_set_bits(&dma_hw->ch[s_dma_cmd].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    if (n_data != 0u) {
        dma_hw->ch[s_dma_data].write_addr = (uint32_t)(uintptr_t)dst;
        dma_hw->ch[s_dma_data].al1_transfer_count_trig = n_data;
    }
    dma_hw->ch[s_dma_cmd].read_addr = (uint32_t)(uintptr_t)cmd;
    dma_hw->ch[s_dma_cmd].al1_transfer_count_trig = n_cmd;
}

/* Start one sample transaction (M-7). Tick ISR only, I2C IRQ masked. */
static void __not_in_flash_func(mag_start)(uint32_t tick) {
    unsigned pre = sr_select();

    uint32_t slot = (s_latest == 0u) ? 1u : 0u; /* M-11 */
    s_cur_slot = slot;
    s_cur_tick = tick;
    s_cur_pre = pre;
    start_dma(TXN_SAMPLE, s_cmd[pre], s_cmd_len[pre], &s_slots[slot].status, MMC_READ_BYTES);
}

/* M-2 (Rev 2.7): read the product ID (non-blocking). */
static void __not_in_flash_func(probe_start)(void) {
    s_probe_id = 0u;
    start_dma(TXN_PROBE, s_probe_cmd, 2u, &s_probe_id, 1u);
}

/* M-2, M-3, M-4 (Rev 2.7): configure after a successful re-probe. Ends
 * with RESET; the first sample transaction then carries the SET. */
static void __not_in_flash_func(config_start)(uint32_t tick) {
    uint32_t mode = g_mag_sr_mode;
    if (mode > MAG_SR_PULSE) {
        mode = s_mode_applied;
    }
    s_mode_applied = mode;
    s_cur_tick = tick;
    start_dma(TXN_CONFIG, s_cfg_cmd, CFG_CMD_LEN, NULL, 0u);
}

/* M-16: abort without waiting. Returns true if a controller abort was
 * requested (a TX_ABRT interrupt will follow), false if only the DMA
 * channels needed stopping.
 *
 * IC_ENABLE.ABORT is set only when the master is active or has commands
 * queued. The datasheet says the bit clears itself "after the abort
 * operation" and cannot be cleared by software; it does not say what
 * happens on an idle master. If it stayed set there with no TX_ABRT, the
 * state machine could never leave MAG_ABORTING. */
static bool __not_in_flash_func(issue_abort)(void) {
    mag_dma_abort();
    i2c_hw_t *hw = i2c_regs();
    uint32_t status = hw->status;
    if ((status & I2C_IC_STATUS_MST_ACTIVITY_BITS) || !(status & I2C_IC_STATUS_TFE_BITS)) {
        hw_set_bits(&hw->enable, I2C_IC_ENABLE_ABORT_BITS);
        return true;
    }
    return false;
}

/* M-4b (Rev 2.9): datasheet p. 18, "Using SET and RESET to remove bridge
 * offset". After RESET a measurement is -H + offset; after SET it is
 * +H + offset; their mean is the offset. Uses the pair produced by every
 * RESET -> SET cycle (start-up, re-selecting mode 0, re-probe). Assumes the
 * field is steady over the ~2 ms between the two measurements. */
static void __not_in_flash_func(calibrate)(const mag_sample_t *s, bool done) {
    if (s_trig_reset) {
        s_cal_have_reset = done;
        if (done) {
            mag_convert(s->raw, s_cal_reset_xyz);
        }
        return;
    }
    if (s_trig_cal_set && done && s_cal_have_reset) {
        int32_t set_xyz[3];
        mag_convert(s->raw, set_xyz);
        for (unsigned i = 0; i < 3u; i++) {
            s_offset[i] = (set_xyz[i] + s_cal_reset_xyz[i]) / 2;
        }
        s_offset_valid = true;
        diag_inc(DIAG_MAG_OFFSET_CAL);
    }
    s_cal_have_reset = false;
}

/* D-1 (Rev 2.9): informational only; no action is taken (D-5). */
static void __not_in_flash_func(count_saturation)(const mag_sample_t *s) {
    int32_t xyz[3];
    mag_convert(s->raw, xyz);
    for (unsigned i = 0; i < 3u; i++) {
        if (xyz[i] >= 131071 - MAG_SAT_MARGIN || xyz[i] <= -131072 + MAG_SAT_MARGIN) {
            diag_inc(DIAG_MAG_SATURATED);
            return;
        }
    }
}

/* M-12 (Rev 2.1): publish the completed slot. Called from the I2C
 * STOP-detect interrupt after both DMA channels are confirmed finished. */
static void __not_in_flash_func(publish)(uint64_t now_us) {
    mag_sample_t *s = &s_slots[s_cur_slot];
    uint32_t tick = s_cur_tick;
    if (s_trig_ok) {
        /* M-9: a measurement triggered right after a RESET pulse has
         * inverted polarity (Rev 2.4), and one whose Meas_M_Done bit is
         * still clear was read before it finished (Rev 2.8). */
        bool done = (s->status & MMC_STATUS_MEAS_M_DONE) != 0u;
        if (!done) {
            diag_inc(DIAG_MAG_NOT_READY);
            s_not_ready_score += MAG_NOT_READY_STEP; /* M-2b */
        } else if (s_not_ready_score != 0u) {
            s_not_ready_score--;
        }
        s->reason = !done           ? (uint8_t)MAG_SAMPLE_NOT_READY
                    : s_trig_reset  ? (uint8_t)MAG_SAMPLE_AFTER_RESET
                                    : (uint8_t)MAG_SAMPLE_OK;
        s->valid = (s->reason == MAG_SAMPLE_OK);
        s->seq = s_trig_tick;
        s->timestamp_us = s_trig_time_us; /* M-10 (Rev 2.6) */
        calibrate(s, done);
        if (s->valid) {
            count_saturation(s);
        }
    } else {
        /* M-9: no prior measurement. */
        s_cal_have_reset = false;
        s->reason = (uint8_t)MAG_SAMPLE_NO_PRIOR;
        s->valid = false;
        s->seq = tick - 1u;
        s->timestamp_us = tick_time_us(tick) - TICK_PERIOD_US; /* no trigger: nominal */
    }
    __dmb();
    s_latest = s_cur_slot; /* single atomic 32-bit write */

    s_trig_ok = true;
    s_trig_tick = tick;
    s_trig_time_us = now_us; /* this transaction's TM_M write just completed */
    uint32_t pre = s_cur_pre;
    s_trig_reset = (pre == PRE_RESET);
    s_trig_cal_set = (pre == PRE_SET && s_cycle_step == 2u);
    if (pre == PRE_RESET) {
        s_cycle_step = 2u; /* SET next */
    } else if (pre == PRE_SET) {
        if (s_cycle_step == 2u) {
            s_cycle_step = 0u;
        }
        s_since_pulse = 0u;
        s_pulse_now = false;
    } else {
        s_since_pulse++;
    }
    s_state = MAG_IDLE;
}

/* ------------------------------------------------------------- ISRs ---- */

/* I2C1: STOP_DET = transaction complete; TX_ABRT = abort (M-15). */
static void __not_in_flash_func(i2c_isr)(void) {
    uint32_t t0 = diag_cycles();
    /* M-10 (Rev 2.6): STOP_DET follows the trigger byte's ACK by one bit
     * time (~2.5 us), so reading the timer first keeps the timestamp
     * within ~3 us + interrupt latency of the real trigger. */
    uint64_t now_us = time_us_64();
    i2c_hw_t *hw = i2c_regs();
    uint32_t st = hw->intr_stat;

    if (st & I2C_IC_INTR_STAT_R_TX_ABRT_BITS) {
        /* Stop both channels first (no wait). The register reads below give
         * any in-flight DMA write time to retire into the still-flushed TX
         * FIFO before the flush is released. */
        mag_dma_abort();
        uint32_t src = hw->tx_abrt_source;
        (void)hw->clr_tx_abrt;       /* releases the TX FIFO flush     */
        (void)hw->clr_stop_det;      /* the abort's STOP is not completion */
        bool user = (src & I2C_IC_TX_ABRT_SOURCE_ABRT_USER_ABRT_BITS) != 0u;
        /* M-15 (Rev 2.3): count an I2C error for any bus-reported cause
         * (NACK, arbitration loss, ...). An abort whose only cause is our
         * own timeout abort (M-16) was already counted as a timeout. */
        if (src & ABRT_REASON_BITS & ~I2C_IC_TX_ABRT_SOURCE_ABRT_USER_ABRT_BITS) {
            /* Rev 2.17: a probe or configuration not acknowledged at the
             * address means no sensor (no_ack); any other bus-reported
             * cause is an I2C error. Sample transactions keep counting
             * every cause as an I2C error (M-15). */
            bool no_ack = (src & I2C_IC_TX_ABRT_SOURCE_ABRT_7B_ADDR_NOACK_BITS) != 0u;
            if (s_txn != TXN_SAMPLE) {
                s_cause = no_ack ? MAG_CAUSE_NO_ACK : MAG_CAUSE_BUS_ERROR;
                diag_inc(no_ack ? DIAG_MAG_NO_ACK : DIAG_MAG_I2C_ERROR);
            } else {
                diag_inc(DIAG_MAG_I2C_ERROR);
            }
        } else if (s_txn != TXN_SAMPLE) {
            s_cause = MAG_CAUSE_BUS_ERROR; /* our own timeout abort (M-16) */
        }
        s_trig_ok = false;
        if (s_state == MAG_BUSY || s_state == MAG_ABORTING) {
            s_state = MAG_ABORTED;
        }
#if MAG_BUS_CLEAR
        if (user || (src & I2C_IC_TX_ABRT_SOURCE_ARB_LOST_BITS)) {
            bus_clear_start();
        }
#endif
    } else if (st & I2C_IC_INTR_STAT_R_STOP_DET_BITS) {
        (void)hw->clr_stop_det;
        if (s_state == MAG_BUSY) {
            bool data_done = (dma_hw->ch[s_dma_data].ctrl_trig & DMA_CH0_CTRL_TRIG_BUSY_BITS) == 0u;
            bool cmd_done = (dma_hw->ch[s_dma_cmd].ctrl_trig & DMA_CH0_CTRL_TRIG_BUSY_BITS) == 0u;
            if (!(data_done && cmd_done)) {
                /* Unexpected STOP mid-transaction. */
                mag_dma_abort();
                diag_inc(DIAG_MAG_I2C_ERROR);
                if (s_txn != TXN_SAMPLE) {
                    s_cause = MAG_CAUSE_BUS_ERROR;
                }
                s_trig_ok = false;
                s_state = MAG_ABORTED;
            } else if (s_txn == TXN_SAMPLE) {
                publish(now_us);
            } else if (s_txn == TXN_PROBE) {
                if (s_probe_id == MMC_PRODUCT_ID) {
                    s_config_pending = true; /* configure on the next tick */
                    s_state = MAG_IDLE;
                } else {
                    diag_inc(DIAG_MAG_WRONG_ID);
                    s_cause_id = s_probe_id;
                    s_cause = MAG_CAUSE_WRONG_ID;
                    s_probe_wait = 0u;
                    s_state = MAG_FAULT;
                }
            } else { /* TXN_CONFIG */
                /* The configuration ended with RESET + TM_M (Rev 2.9): the
                 * first sample is that post-RESET measurement. It is
                 * published invalid (M-9) and is the RESET half of an
                 * offset calibration; its transaction carries the SET. */
                s_trig_ok = true;
                s_trig_reset = true;
                s_trig_cal_set = false;
                s_trig_tick = s_cur_tick;
                s_trig_time_us = now_us;
                s_cycle_step = 2u;   /* SET next */
                s_since_pulse = 0u;
                s_pulse_now = false;
                s_fault = false;
                s_cause = MAG_CAUSE_NONE;
                s_not_ready_score = 0u;
                diag_inc(DIAG_MAG_RECOVERED);
                s_state = MAG_IDLE;
            }
        }
    }
    diag_max(DIAG_T_ISR_I2C, diag_cycles() - t0);
}

/* M-6, M-16. Called from the tick ISR (lower priority than the I2C ISR,
 * which is masked here so the state cannot change underneath). */
void __not_in_flash_func(mag_tick_isr)(uint32_t tick) {
    i2c_irq_mask();
    bool sampling = (s_txn == TXN_SAMPLE);
    switch (s_state) {
    case MAG_FAULT:
        /* M-2 (Rev 2.7): re-probe at 5 Hz while faulted. */
        if (++s_probe_wait >= MAG_REPROBE_PERIOD_TICKS && hw_idle()) {
            s_probe_wait = 0u;
            probe_start();
        }
        break;
    case MAG_IDLE:
        if (s_config_pending) {
            s_config_pending = false;
            config_start(tick);
        } else if (MAG_NOT_READY_FAULT_SCORE != 0u && s_not_ready_score >= MAG_NOT_READY_FAULT_SCORE) {
            /* M-2b (Rev 2.16): the sensor answers but its measurements keep
             * being read unfinished, the signature of a sensor that
             * power-cycled back to its defaults. Set the fault flag and let
             * the re-probe (M-2a) reconfigure it, starting on the next tick
             * rather than waiting a whole probe period. */
            diag_inc(DIAG_MAG_RUNTIME_FAULT);
            s_not_ready_score = 0u;
            s_trig_ok = false;
            s_fault = true;
            s_cause = MAG_CAUSE_NOT_READY;
            s_probe_wait = MAG_REPROBE_PERIOD_TICKS - 1u;
            s_state = MAG_FAULT;
        } else {
            mag_start(tick);
        }
        break;
    case MAG_BUSY:
        if (sampling) {
            diag_inc(DIAG_MAG_MISSED_TICK);
        }
        diag_inc(DIAG_MAG_TIMEOUT);
        if (!sampling) {
            s_cause = MAG_CAUSE_BUS_ERROR;
        }
        s_trig_ok = false;
        /* Controller idle (e.g. the command channel never delivered): the
         * DMA abort is all that is needed; no TX_ABRT will come. */
        s_state = issue_abort() ? MAG_ABORTING : MAG_ABORTED;
        break;
    case MAG_ABORTING:
    case MAG_ABORTED:
        if (hw_idle()) {
            s_trig_ok = false;
            if (sampling) {
                mag_start(tick); /* M-9: this sample will be invalid */
            } else {
                /* A probe or configuration failed: back to re-probing. */
                s_config_pending = false;
                s_probe_wait = 0u;
                s_state = MAG_FAULT;
            }
        } else {
            if (sampling) {
                diag_inc(DIAG_MAG_MISSED_TICK);
            }
            if (controller_hung()) {
                diag_inc(DIAG_MAG_TIMEOUT);
                s_state = issue_abort() ? MAG_ABORTING : MAG_ABORTED;
            }
        }
        break;
    default:
        break;
    }
    i2c_irq_unmask();
}

/* ---------------------------------------------------------- main loop -- */

bool __not_in_flash_func(mag_read_latest)(mag_reading_t *r) {
    diag_span_t t0 = diag_span_begin(); /* Rev 2.22: ISR time excluded */
    cs_t cs = cs_enter();
    uint32_t l = s_latest;
    if (l < SLOT_NONE) {
        r->s = s_slots[l];
    }
    r->offset[0] = s_offset[0];
    r->offset[1] = s_offset[1];
    r->offset[2] = s_offset[2];
    r->offset_valid = s_offset_valid;
    cs_exit(cs);

    r->present = l < SLOT_NONE;
    if (!r->present) {
        memset(&r->s, 0, sizeof r->s);
        r->stale = true;
    } else {
        uint64_t now = time_us_64();
        r->stale = (now - r->s.timestamp_us) > MAG_STALE_US;
    }
    diag_max(DIAG_T_API_MAG, diag_span_cycles(t0));
    return r->present;
}

/* --------------------------------------------------------------- init -- */

/* Start-up I2C result (Rev 2.17): the SDK's blocking calls return
 * PICO_ERROR_GENERIC when the transfer is not acknowledged and
 * PICO_ERROR_TIMEOUT on a timeout. Address and data NACKs are not told
 * apart; the 5 Hz re-probe (interrupt path) refines the cause. */
static int s_init_rc;

static bool reg_write(uint8_t reg, uint8_t val) {
    uint8_t b[2] = {reg, val};
    int rc = i2c_write_timeout_us(MAG_I2C, MAG_I2C_ADDR, b, 2, false, 5000);
    if (rc != 2) {
        s_init_rc = rc;
        return false;
    }
    return true;
}

static bool reg_read(uint8_t reg, uint8_t *val) {
    int rc = i2c_write_timeout_us(MAG_I2C, MAG_I2C_ADDR, &reg, 1, true, 5000);
    if (rc == 1) {
        rc = i2c_read_timeout_us(MAG_I2C, MAG_I2C_ADDR, val, 1, false, 5000);
        if (rc == 1) {
            return true;
        }
    }
    s_init_rc = rc;
    return false;
}

/* Record a failed start-up step (Rev 2.17). */
static void init_bus_fail(void) {
    if (s_init_rc == PICO_ERROR_TIMEOUT) {
        s_cause = MAG_CAUSE_BUS_ERROR;
        diag_inc(DIAG_MAG_I2C_ERROR);
    } else {
        s_cause = MAG_CAUSE_NO_ACK;
        diag_inc(DIAG_MAG_NO_ACK);
    }
}

/* Start-up only (blocking allowed): trigger one measurement, wait for it,
 * read the field and Status. Returns false on a bus error or if
 * Meas_M_Done is not set. */
#if APP_USB_STATUS
static mag_boot_t s_boot;

const mag_boot_t *mag_boot(void) {
    return &s_boot;
}
#endif

/* 9-byte burst read of 0x00..0x08 (blocking). */
static bool burst_read(uint8_t b[MMC_READ_BYTES]) {
    uint8_t reg = MMC_REG_XOUT0;
    return i2c_write_timeout_us(MAG_I2C, MAG_I2C_ADDR, &reg, 1, true, 5000) == 1 &&
           i2c_read_timeout_us(MAG_I2C, MAG_I2C_ADDR, b, MMC_READ_BYTES, false, 5000) == (int)MMC_READ_BYTES;
}

static bool blocking_measure(int32_t xyz[3]) {
    uint8_t b[MMC_READ_BYTES];
    if (!reg_write(MMC_REG_CTRL0, (uint8_t)MMC_CTRL0_TM_M)) {
        return false;
    }
    sleep_ms(2); /* 0.5 ms measurement at BW = 11, with margin */
    /* Rev 2.21: Status before the data (reading the data clears Meas_M_Done). */
    uint8_t st = 0;
    if (!reg_read(MMC_REG_STATUS, &st) || !burst_read(b)) {
        return false;
    }
#if APP_USB_STATUS
    s_boot.cal_status = st;
#endif
    if (!(st & MMC_STATUS_MEAS_M_DONE)) {
        return false;
    }
    mag_convert(b, xyz);
    return true;
}

#if MAG_BUS_CLEAR
/* Open-drain helpers for the startup bus clear: drive low or release. */
static void od_low(uint pin) {
    gpio_put(pin, false);
    gpio_set_dir(pin, GPIO_OUT);
}

static void od_release(uint pin) {
    gpio_set_dir(pin, GPIO_IN);
}

/* D-5, M-2 (Rev 2.2): one bus clear before the first I2C access, in case a
 * reset of this MCU interrupted a read and left the sensor driving SDA.
 * Blocking is allowed here (start-up, before the main loop). Nine SCL
 * clocks at ~100 kHz, then START + STOP so the sensor returns to idle.
 * Takes ~110 us. */
static void startup_bus_clear(void) {
    gpio_init(MAG_SDA_PIN);
    gpio_init(MAG_SCL_PIN);
    gpio_pull_up(MAG_SDA_PIN);
    gpio_pull_up(MAG_SCL_PIN);
    od_release(MAG_SDA_PIN);
    od_release(MAG_SCL_PIN);
    busy_wait_us(10);

    /* SCL held low by something else (clock stretching or a fault): give it
     * up to 1 ms, then clock anyway; the product-ID check reports a fault. */
    for (unsigned i = 0; i < 100u && !gpio_get(MAG_SCL_PIN); i++) {
        busy_wait_us(10);
    }
    for (unsigned i = 0; i < 9u; i++) {
        od_low(MAG_SCL_PIN);
        busy_wait_us(5);
        od_release(MAG_SCL_PIN);
        busy_wait_us(5);
    }
    /* START (SDA falls while SCL high), then STOP (SDA rises while SCL high). */
    od_low(MAG_SDA_PIN);
    busy_wait_us(5);
    od_release(MAG_SDA_PIN);
    busy_wait_us(5);
    diag_inc(DIAG_MAG_BUS_CLEAR);
}
#endif

#if APP_USB_STATUS
/* Rev 2.19: time one measurement by polling Status on its own. */
static uint16_t time_measurement(void) {
    uint64_t t0 = time_us_64();
    for (;;) {
        uint8_t st = 0;
        if (!reg_read(MMC_REG_STATUS, &st)) {
            return 0u;
        }
        uint64_t dt = time_us_64() - t0;
        if (st & MMC_STATUS_MEAS_M_DONE) {
            return dt < 1u ? 1u : (uint16_t)dt;
        }
        if (dt > 20000u) {
            return MAG_BOOT_NEVER;
        }
        busy_wait_us(50);
    }
}

/* Rev 2.19: measurement-timing experiment (diagnostic builds only). Shows
 * whether the bandwidth setting takes effect, whether a register write
 * followed by a repeated START (as in the DMA configuration) takes effect,
 * and whether Status reads correctly at the end of the 9-byte burst. */
static void boot_experiment(void) {
    uint8_t b[MMC_READ_BYTES];
    /* 1. Default bandwidth, every write ended by STOP: expect ~8 ms. */
    if (reg_write(MMC_REG_CTRL1, 0u) && reg_write(MMC_REG_CTRL0, (uint8_t)MMC_CTRL0_TM_M)) {
        s_boot.t_slow_us = time_measurement();
    }
    sleep_ms(10);
    /* 2. 800 Hz, every write ended by STOP: expect ~0.5 ms. */
    if (reg_write(MMC_REG_CTRL1, MMC_CTRL1_BW_800HZ) && reg_write(MMC_REG_CTRL0, (uint8_t)MMC_CTRL0_TM_M)) {
        s_boot.t_fast_us = time_measurement();
    }
    /* 2b. Two fresh measurements, 2 ms each: Status read on its own first,
     * vs Status at the end of a 9-byte burst (data read first). */
    if (reg_write(MMC_REG_CTRL0, (uint8_t)MMC_CTRL0_TM_M)) {
        sleep_ms(2);
        (void)reg_read(MMC_REG_STATUS, &s_boot.direct_status);
    }
    if (reg_write(MMC_REG_CTRL0, (uint8_t)MMC_CTRL0_TM_M)) {
        sleep_ms(2);
        if (burst_read(b)) {
            s_boot.burst_status = b[MMC_READ_BYTES - 1u];
        }
    }
    sleep_ms(10);
    /* 3. Back to the default, then 800 Hz written WITHOUT a STOP; the TM_M
     * write that follows starts with a repeated START, as the DMA
     * configuration does. ~0.5 ms = such writes take effect; ~8 ms = not. */
    if (reg_write(MMC_REG_CTRL1, 0u)) {
        uint8_t w[2] = {MMC_REG_CTRL1, MMC_CTRL1_BW_800HZ};
        if (i2c_write_timeout_us(MAG_I2C, MAG_I2C_ADDR, w, 2, true, 5000) == 2 &&
            reg_write(MMC_REG_CTRL0, (uint8_t)MMC_CTRL0_TM_M)) {
            s_boot.t_rs_us = time_measurement();
        }
    }
    sleep_ms(10);
    (void)reg_write(MMC_REG_CTRL1, MMC_CTRL1_BW_800HZ); /* restore */
    s_boot.ran = true;
}
#endif

bool mag_init(void) {
    sleep_ms(10); /* MMC5983MA power-on time */

#if APP_USB_STATUS
    /* Rev 2.18: diagnostic builds check the SDA/SCL wiring first. */
    wirecheck_run();
#endif

#if MAG_BUS_CLEAR
    startup_bus_clear();
#endif

    i2c_init(MAG_I2C, MAG_I2C_HZ); /* M-1 */
    gpio_set_function(MAG_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(MAG_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(MAG_SDA_PIN);
    gpio_pull_up(MAG_SCL_PIN);

    /* M-2 (Rev 2.8): an MCU reset does not reset the sensor. A software
     * reset returns every register to its default and reloads OTP. The
     * result is ignored: an absent sensor is reported by the probe below. */
    (void)reg_write(MMC_REG_CTRL1, MMC_CTRL1_SW_RST);
    sleep_ms(10); /* datasheet: power-on time after SW_RST is 10 ms */

    /* M-2: product ID, and (Rev 2.9) factory OTP loaded after the reset. */
    uint8_t id = 0;
    uint8_t st = 0;
    /* Rev 2.17: each failure records its cause and is counted once. */
    bool ok = true;
    bool id_read = reg_read(MMC_REG_PRODUCT_ID, &id);
#if APP_USB_STATUS
    s_boot.id = id_read ? id : 0u;
#endif
    if (!id_read) {
        init_bus_fail();
        ok = false;
    } else if (id != MMC_PRODUCT_ID) {
        s_cause_id = id;
        s_cause = MAG_CAUSE_WRONG_ID;
        diag_inc(DIAG_MAG_WRONG_ID);
        ok = false;
    } else if (!reg_read(MMC_REG_STATUS, &st)) {
        init_bus_fail();
        ok = false;
    } else if ((st & MMC_STATUS_OTP_RD_DONE) == 0u) {
        s_cause = MAG_CAUSE_OTP_NOT_LOADED;
        ok = false;
    }
#if APP_USB_STATUS
    s_boot.status = st;
#endif
    /* M-3: 800 Hz bandwidth; continuous mode off. */
    bool cfg = ok && reg_write(MMC_REG_CTRL1, MMC_CTRL1_BW_800HZ);
    cfg = cfg && reg_write(MMC_REG_CTRL2, 0u);
    cfg = cfg && reg_write(MMC_REG_CTRL3, 0u);
    if (ok && !cfg) {
        s_cause = MAG_CAUSE_CONFIG_FAILED;
        ok = false;
    }

    /* M-4 (Rev 2.4): one RESET -> SET cycle at start-up, in every mode,
     * ending in SET so measurements have normal polarity. */
    uint32_t mode = g_mag_sr_mode;
    if (mode > MAG_SR_PULSE) {
        mode = MAG_SR_MODE_DEFAULT;
    }
    /* M-4b (Rev 2.9): measure after each pulse for the offset calibration. */
    int32_t m_reset[3];
    int32_t m_set[3];
    bool cal = ok;
    ok = ok && reg_write(MMC_REG_CTRL0, (uint8_t)MMC_CTRL0_RESET);
    sleep_ms(1);
    cal = cal && ok && blocking_measure(m_reset);
    ok = ok && reg_write(MMC_REG_CTRL0, (uint8_t)MMC_CTRL0_SET);
    sleep_ms(1);
    cal = cal && ok && blocking_measure(m_set);
    if (!ok && s_cause == MAG_CAUSE_NONE) {
        s_cause = MAG_CAUSE_CONFIG_FAILED; /* the RESET or SET write failed */
    }
#if APP_USB_STATUS
    s_boot.cal_ok = cal;
    s_boot.cause = s_cause;
    if (id_read && id == MMC_PRODUCT_ID) {
        boot_experiment();
    }
#endif
    if (cal) {
        for (unsigned i = 0; i < 3u; i++) {
            s_offset[i] = (m_set[i] + m_reset[i]) / 2;
        }
        s_offset_valid = true;
        diag_inc(DIAG_MAG_OFFSET_CAL);
    }
    /* M-2 (Rev 2.7): on a fault (cause and count recorded above), set up
     * everything anyway so the tick can re-probe at 5 Hz. */
    s_probe_cmd[0] = MMC_REG_PRODUCT_ID;                 /* START + W(0x2F)  */
    s_probe_cmd[1] = CMD_RESTART | CMD_READ | CMD_STOP;  /* RESTART R x1 STOP */
    s_cfg_cmd[0] = MMC_REG_CTRL1;
    s_cfg_cmd[1] = MMC_CTRL1_BW_800HZ;
    s_cfg_cmd[2] = CMD_RESTART | MMC_REG_CTRL2;
    s_cfg_cmd[3] = 0u;
    s_cfg_cmd[4] = CMD_RESTART | MMC_REG_CTRL3; /* M-3: Internal Control 3 = 0 */
    s_cfg_cmd[5] = 0u;
    s_cfg_cmd[6] = CMD_RESTART | MMC_REG_CTRL0;
    s_cfg_cmd[7] = MMC_CTRL0_RESET;
    s_cfg_cmd[8] = CMD_RESTART | MMC_REG_CTRL0; /* Rev 2.9: measure after the */
    s_cfg_cmd[9] = CMD_STOP | MMC_CTRL0_TM_M;   /* RESET for the calibration  */
    for (unsigned pre = 0; pre < PRE_COUNT; pre++) {
        s_cmd_len[pre] = (uint32_t)build_cmds(s_cmd[pre], pre);
    }
    s_mode_applied = mode;
    s_cycle_step = 0u;
    s_since_pulse = 0u;
    s_pulse_now = false;
    s_trig_reset = false;

    i2c_hw_t *hw = i2c_regs();
    /* The blocking calls above left IC_TAR = 0x30, which the DMA
     * transactions reuse. */
    hw->dma_tdlr = 8u; /* TX DREQ while TX FIFO <= 8 entries */
    hw->dma_rdlr = 0u; /* RX DREQ as soon as 1 entry          */
    hw->dma_cr = I2C_IC_DMA_CR_TDMAE_BITS | I2C_IC_DMA_CR_RDMAE_BITS;

    /* Command channel: s_cmd -> IC_DATA_CMD, 32-bit. */
    s_dma_cmd = (uint)dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(s_dma_cmd);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, i2c_get_dreq(MAG_I2C, true));
    dma_channel_configure(s_dma_cmd, &c, &hw->data_cmd, s_cmd[PRE_NONE], 0, false);

    /* Data channel: IC_DATA_CMD -> slot, 8-bit. */
    s_dma_data = (uint)dma_claim_unused_channel(true);
    c = dma_channel_get_default_config(s_dma_data);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, i2c_get_dreq(MAG_I2C, false));
    dma_channel_configure(s_dma_data, &c, &s_slots[0].status, &hw->data_cmd, 0, false);

    s_dma_mask = (1u << s_dma_cmd) | (1u << s_dma_data);

#if MAG_BUS_CLEAR
    s_pio = pio0;
    s_sm = (uint)pio_claim_unused_sm(s_pio, true);
    uint offset = (uint)pio_add_program(s_pio, &i2c_bus_clear_program);
    pio_sm_config pc = i2c_bus_clear_program_get_default_config(offset);
    sm_config_set_set_pins(&pc, MAG_SCL_PIN, 1);
    sm_config_set_in_pins(&pc, MAG_SCL_PIN);
    sm_config_set_clkdiv_int_frac8(&pc, 24u, 0u);
    pio_sm_init(s_pio, s_sm, offset, &pc);
    pio_sm_set_pins_with_mask(s_pio, s_sm, 0u, 1u << MAG_SCL_PIN);
    pio_sm_set_pindirs_with_mask(s_pio, s_sm, 0u, 1u << MAG_SCL_PIN);
    pio_sm_set_enabled(s_pio, s_sm, true);
    s_bc_running = false;
#endif

    (void)hw->clr_intr;
    hw->intr_mask = I2C_IC_INTR_MASK_M_STOP_DET_BITS | I2C_IC_INTR_MASK_M_TX_ABRT_BITS;
    irq_set_exclusive_handler(MAG_I2C_IRQ, i2c_isr);
    irq_set_priority(MAG_I2C_IRQ, IRQ_PRIO_MAG);
    irq_set_enabled(MAG_I2C_IRQ, true);

    s_trig_ok = false; /* M-9: the first transaction's data is invalid */
    s_txn = TXN_SAMPLE;
    s_config_pending = false;
    s_probe_wait = 0u;
    s_fault = !ok;
    s_state = ok ? MAG_IDLE : MAG_FAULT;
    return ok;
}
