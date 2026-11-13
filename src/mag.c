#include "mag.h"

#include <string.h>

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/structs/m33.h"
#include "pico/time.h"

#include "config.h"
#include "diag.h"
#include "tick.h"

#if MAG_BUS_CLEAR
#include "i2c_bus_clear.pio.h"
#endif

#define MAG_I2C i2c1
#define MAG_I2C_IRQ I2C1_IRQ

#define CMD_READ I2C_IC_DATA_CMD_CMD_BITS
#define CMD_STOP I2C_IC_DATA_CMD_STOP_BITS
#define CMD_RESTART I2C_IC_DATA_CMD_RESTART_BITS

#define I2C_TX_FIFO_DEPTH 16u
#define SLOT_NONE 2u

#if MAG_SR_MODE == MAG_SR_AUTO
#define CTRL0_PERSIST MMC_CTRL0_AUTO_SR_EN /* M-5 */
#elif MAG_SR_MODE == MAG_SR_PULSE
#define CTRL0_PERSIST 0u
_Static_assert(MAG_SET_INTERVAL >= 1u && MAG_SET_INTERVAL <= 100u, "M-4: SET at least every 100 samples");
#else
#error "MAG_SR_MODE must be MAG_SR_AUTO or MAG_SR_PULSE"
#endif

typedef enum {
    MAG_FAULT,    /* M-2 failed: no transactions                       */
    MAG_IDLE,     /* ready to start on the next tick                    */
    MAG_BUSY,     /* transaction in flight                              */
    MAG_ABORTING, /* tick requested an abort (M-16), TX_ABRT not seen   */
    MAG_ABORTED,  /* abort handled; waiting for DMA/I2C/bus to go idle  */
} mag_state_t;

/* M-11: double buffer. s_latest is SLOT_NONE until the first publish. */
static mag_sample_t s_slots[2];
static volatile uint32_t s_latest = SLOT_NONE;

static volatile mag_state_t s_state = MAG_FAULT;
static bool s_fault = true;

static volatile uint32_t s_cur_slot;
static volatile uint32_t s_cur_tick;
static volatile bool s_cur_has_set;
static volatile bool s_trig_ok;      /* previous transaction's TM_M write completed */
static volatile uint32_t s_trig_tick; /* tick of that transaction                    */
static uint32_t s_since_set;

/* Command lists: [0] trigger only, [1] SET pulse then trigger (MAG_SR_PULSE). */
static uint32_t s_cmd[2][I2C_TX_FIFO_DEPTH] __attribute__((aligned(4)));
static uint32_t s_cmd_len[2];

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

static size_t build_cmds(uint32_t *c, bool with_set) {
    size_t i = 0;
    c[i++] = MMC_REG_XOUT0; /* START + W(0x00) */
    for (unsigned k = 0; k < MMC_SAMPLE_BYTES; k++) {
        c[i++] = CMD_READ | (k == 0u ? CMD_RESTART : 0u); /* RESTART + R x7 */
    }
    if (with_set) {
        c[i++] = CMD_RESTART | MMC_REG_CTRL0;
        c[i++] = CTRL0_PERSIST | MMC_CTRL0_SET;
    }
    c[i++] = CMD_RESTART | MMC_REG_CTRL0;                     /* RESTART + W(0x09)  */
    c[i++] = CMD_STOP | CTRL0_PERSIST | MMC_CTRL0_TM_M;        /* TM_M, STOP (M-5)   */
    return i;
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
#if MAG_BUS_CLEAR
    if (!bus_clear_done()) {
        return false;
    }
#endif
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

/* Start one DMA transaction (M-7). Tick ISR only, I2C IRQ masked. */
static void __not_in_flash_func(mag_start)(uint32_t tick) {
    i2c_hw_t *hw = i2c_regs();

    /* Discard anything left in the RX FIFO by an aborted transfer
     * (bounded by the FIFO depth) and clear stale interrupt status. */
    for (unsigned k = 0; k < I2C_TX_FIFO_DEPTH && hw->rxflr != 0u; k++) {
        (void)hw->data_cmd;
    }
    (void)hw->clr_tx_abrt;
    (void)hw->clr_stop_det;

    unsigned v = 0;
#if MAG_SR_MODE == MAG_SR_PULSE
    if (s_since_set + 1u >= MAG_SET_INTERVAL) {
        v = 1;
    }
#endif

    uint32_t slot = (s_latest == 0u) ? 1u : 0u; /* M-11 */
    s_cur_slot = slot;
    s_cur_tick = tick;
    s_cur_has_set = (v == 1u);
    s_state = MAG_BUSY;

    /* Data channel first so it is ready for the first received byte. */
    dma_hw->ch[s_dma_data].write_addr = (uint32_t)(uintptr_t)s_slots[slot].raw;
    dma_hw->ch[s_dma_data].al1_transfer_count_trig = MMC_SAMPLE_BYTES;
    dma_hw->ch[s_dma_cmd].read_addr = (uint32_t)(uintptr_t)s_cmd[v];
    dma_hw->ch[s_dma_cmd].al1_transfer_count_trig = s_cmd_len[v];
}

/* M-16: abort without waiting. */
static void __not_in_flash_func(issue_abort)(void) {
    dma_hw->abort = s_dma_mask;
    hw_set_bits(&i2c_regs()->enable, I2C_IC_ENABLE_ABORT_BITS);
}

/* M-12 (Rev 2.1): publish the completed slot. Called from the I2C
 * STOP-detect interrupt after both DMA channels are confirmed finished. */
static void __not_in_flash_func(publish)(void) {
    mag_sample_t *s = &s_slots[s_cur_slot];
    uint32_t tick = s_cur_tick;
    if (s_trig_ok) {
        s->valid = true;
        s->seq = s_trig_tick;
        s->timestamp_us = tick_time_us(s_trig_tick);
    } else {
        /* M-9: no prior measurement. */
        s->valid = false;
        s->seq = tick - 1u;
        s->timestamp_us = tick_time_us(tick) - TICK_PERIOD_US;
    }
    __dmb();
    s_latest = s_cur_slot; /* single atomic 32-bit write */

    s_trig_ok = true;
    s_trig_tick = tick;
    if (s_cur_has_set) {
        s_since_set = 0;
    } else {
        s_since_set++;
    }
    s_state = MAG_IDLE;
}

/* ------------------------------------------------------------- ISRs ---- */

/* I2C1: STOP_DET = transaction complete; TX_ABRT = abort (M-15). */
static void __not_in_flash_func(i2c_isr)(void) {
    uint32_t t0 = diag_cycles();
    i2c_hw_t *hw = i2c_regs();
    uint32_t st = hw->intr_stat;

    if (st & I2C_IC_INTR_STAT_R_TX_ABRT_BITS) {
        /* Stop both channels first (no wait). The register reads below give
         * any in-flight DMA write time to retire into the still-flushed TX
         * FIFO before the flush is released. */
        dma_hw->abort = s_dma_mask;
        uint32_t src = hw->tx_abrt_source;
        (void)hw->clr_tx_abrt;       /* releases the TX FIFO flush     */
        (void)hw->clr_stop_det;      /* the abort's STOP is not completion */
        bool user = (src & I2C_IC_TX_ABRT_SOURCE_ABRT_USER_ABRT_BITS) != 0u;
        if (!user) {
            diag_inc(DIAG_MAG_I2C_ERROR); /* NACK, arbitration loss, ... */
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
            if (data_done && cmd_done) {
                publish();
            } else {
                /* Unexpected STOP mid-transaction. */
                dma_hw->abort = s_dma_mask;
                diag_inc(DIAG_MAG_I2C_ERROR);
                s_trig_ok = false;
                s_state = MAG_ABORTED;
            }
        }
    }
    diag_max(DIAG_T_ISR_I2C, diag_cycles() - t0);
}

/* M-6, M-16. Called from the tick ISR (lower priority than the I2C ISR,
 * which is masked here so the state cannot change underneath). */
void __not_in_flash_func(mag_tick_isr)(uint32_t tick) {
    if (s_state == MAG_FAULT) {
        return;
    }
    i2c_irq_mask();
    switch (s_state) {
    case MAG_IDLE:
        mag_start(tick);
        break;
    case MAG_BUSY:
        diag_inc(DIAG_MAG_MISSED_TICK);
        diag_inc(DIAG_MAG_TIMEOUT);
        issue_abort();
        s_trig_ok = false;
        s_state = MAG_ABORTING;
        break;
    case MAG_ABORTING:
    case MAG_ABORTED:
        if (hw_idle()) {
            s_trig_ok = false;
            mag_start(tick); /* M-9: this sample will be invalid */
        } else {
            diag_inc(DIAG_MAG_MISSED_TICK);
            if (controller_hung()) {
                diag_inc(DIAG_MAG_TIMEOUT);
                issue_abort();
                s_state = MAG_ABORTING;
            }
        }
        break;
    case MAG_FAULT:
    default:
        break;
    }
    i2c_irq_unmask();
}

/* ---------------------------------------------------------- main loop -- */

bool __not_in_flash_func(mag_read_latest)(mag_reading_t *r) {
    uint32_t t0 = diag_cycles();
    cs_t cs = cs_enter();
    uint32_t l = s_latest;
    if (l < SLOT_NONE) {
        r->s = s_slots[l];
    }
    cs_exit(cs);

    r->present = l < SLOT_NONE;
    if (!r->present) {
        memset(&r->s, 0, sizeof r->s);
        r->stale = true;
    } else {
        uint64_t now = time_us_64();
        r->stale = (now - r->s.timestamp_us) > MAG_STALE_US;
    }
    diag_max(DIAG_T_API_MAG, diag_cycles() - t0);
    return r->present;
}

/* --------------------------------------------------------------- init -- */

static bool reg_write(uint8_t reg, uint8_t val) {
    uint8_t b[2] = {reg, val};
    return i2c_write_timeout_us(MAG_I2C, MAG_I2C_ADDR, b, 2, false, 5000) == 2;
}

static bool reg_read(uint8_t reg, uint8_t *val) {
    if (i2c_write_timeout_us(MAG_I2C, MAG_I2C_ADDR, &reg, 1, true, 5000) != 1) {
        return false;
    }
    return i2c_read_timeout_us(MAG_I2C, MAG_I2C_ADDR, val, 1, false, 5000) == 1;
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

bool mag_init(void) {
    sleep_ms(10); /* MMC5983MA power-on time */

#if MAG_BUS_CLEAR
    startup_bus_clear();
#endif

    i2c_init(MAG_I2C, MAG_I2C_HZ); /* M-1 */
    gpio_set_function(MAG_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(MAG_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(MAG_SDA_PIN);
    gpio_pull_up(MAG_SCL_PIN);

    /* M-2 */
    uint8_t id = 0;
    bool ok = reg_read(MMC_REG_PRODUCT_ID, &id) && id == MMC_PRODUCT_ID;
    /* M-3: 800 Hz bandwidth; continuous mode off; one SET to start clean. */
    ok = ok && reg_write(MMC_REG_CTRL1, MMC_CTRL1_BW_800HZ);
    ok = ok && reg_write(MMC_REG_CTRL2, 0u);
    ok = ok && reg_write(MMC_REG_CTRL0, (uint8_t)(CTRL0_PERSIST | MMC_CTRL0_SET));
    if (!ok) {
        g_diag_ctr[DIAG_MAG_PRODUCT_ID_FAULT] = 1u;
        s_fault = true;
        s_state = MAG_FAULT;
        return false;
    }
    sleep_us(10);

    s_cmd_len[0] = (uint32_t)build_cmds(s_cmd[0], false);
    s_cmd_len[1] = (uint32_t)build_cmds(s_cmd[1], true);

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
    dma_channel_configure(s_dma_cmd, &c, &hw->data_cmd, s_cmd[0], 0, false);

    /* Data channel: IC_DATA_CMD -> slot, 8-bit. */
    s_dma_data = (uint)dma_claim_unused_channel(true);
    c = dma_channel_get_default_config(s_dma_data);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, i2c_get_dreq(MAG_I2C, false));
    dma_channel_configure(s_dma_data, &c, s_slots[0].raw, &hw->data_cmd, 0, false);

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
    s_since_set = 0;
    s_fault = false;
    s_state = MAG_IDLE;
    return true;
}
