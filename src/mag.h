/*
 * MMC5983MA sampled over I2C1 entirely by DMA (section 5), at 1000, 200,
 * 100 or 50 Hz (M-3b, Rev 2.25).
 *
 * Every Nth 1 ms tick (N = 1, 5, 10, 20) starts one transaction:
 *   START W(0x08) RESTART R x1 RESTART W(0x00) RESTART R x8
 *         [RESTART W(0x0A, BW)]  (rate change only)
 *         [RESTART W(0x09, SET|RESET)]  RESTART W(0x09, TM_M)  STOP
 * i.e. read the measurement triggered by the previous transaction, then
 * trigger the next one. The I2C STOP_DET interrupt marks completion and
 * publishes the sample; TX_ABRT is the abort path (M-15).
 */
#ifndef MAG_H
#define MAG_H

#include <stdbool.h>
#include <stdint.h>

#include "mmc5983ma.h"

#include "config.h"

/* M-4 (Rev 2.8): SET/RESET strategy, MAG_SR_INIT_ONLY (0) or MAG_SR_PULSE
 * (1), see config.h. Read at the start of every transaction, so a
 * change (e.g. written from the debugger) takes effect within 1 ms. Invalid
 * values are ignored and the previous mode stays in use. */
extern volatile uint32_t g_mag_sr_mode;

/* MAG_SR_PULSE only: samples from one SET pulse to the next. Values outside
 * 1..MAG_SR_INTERVAL_MAX are clamped when used. */
extern volatile uint32_t g_mag_sr_interval;

/* M-3b (Rev 2.25): requested output rate in Hz (1000, 200, 100 or 50).
 * Read at the start of every sample transaction; a new value takes effect
 * at the next sample (within one sample period of the old rate). Invalid
 * values are ignored and the previous rate stays in use. */
extern volatile uint32_t g_mag_rate_hz;

/* Rate the sensor is configured for now (Hz): equals g_mag_rate_hz once a
 * change has taken effect. */
uint32_t mag_rate_hz(void);

/* True if hz is one of the supported rates. */
bool mag_rate_supported(uint32_t hz);

/* The supported rate after hz in the list 1000 -> 200 -> 100 -> 50 -> 1000
 * (for stepping through them from a console). */
uint32_t mag_rate_next(uint32_t hz);

/* Sensor bandwidth (Internal Control 1 BW, MMC_CTRL1_BW_*) used at rate hz;
 * the current rate's if hz is not supported. */
uint8_t mag_rate_bw(uint32_t hz);

/* M-9, M-10 (Rev 2.11): why a sample is or is not valid. */
typedef enum {
    MAG_SAMPLE_OK = 0,          /* valid                                           */
    MAG_SAMPLE_NO_PRIOR = 1,    /* first after start-up/abort: nothing was triggered */
    MAG_SAMPLE_AFTER_RESET = 2, /* measured right after RESET: inverted polarity    */
    MAG_SAMPLE_NOT_READY = 3,   /* read with Meas_M_Done clear: incomplete          */
} mag_sample_reason_t;

/* M-10 (Rev 2.21). status, raw and tout are filled by one DMA read in this
 * order (Status 0x08 first, then 0x00..0x07) and must stay adjacent
 * (checked in mag.c). Status comes first because reading the output
 * registers clears Meas_M_Done on the real part (found at bring-up). */
typedef struct {
    uint8_t status;        /* register 0x08, read before the data (bit 0 = Meas_M_Done) */
    uint8_t raw[MMC_SAMPLE_BYTES];
    uint8_t tout;          /* register 0x07; not updated (TM_T unused)         */
    bool valid;            /* true exactly when reason == MAG_SAMPLE_OK (M-9)  */
    uint8_t reason;        /* mag_sample_reason_t (Rev 2.11)                   */
    uint8_t period_ticks;  /* M-3b (Rev 2.25): sample period in 1 ms ticks
                            * (1, 5, 10, 20); the next sample's seq is
                            * seq + period_ticks                              */
    uint32_t seq;          /* index of the tick whose transaction triggered it   */
    uint64_t timestamp_us; /* us since boot when the trigger (TM_M) write completed
                            * (Rev 2.6); for M-9 invalid samples with no prior
                            * trigger, the nominal previous tick time            */
} mag_sample_t;

/* Result of mag_read_latest(). */
typedef struct {
    mag_sample_t s;
    bool present; /* at least one sample has been published */
    bool stale;   /* M-17: timestamp older than MAG_STALE_US per sample period
                   * (3 ms at 1 kHz, 60 ms at 50 Hz), or none yet */
    /* M-4b (Rev 2.9): bridge offset from the last SET/RESET calibration, in
     * the same signed counts as mag_convert(). Subtract it from every axis:
     * field = mag_convert(raw) - offset. Not applied by the firmware. */
    int32_t offset[3];
    bool offset_valid; /* false until the first successful calibration */
} mag_reading_t;

/* Blocking start-up (before the main loop): bus clear, software reset,
 * product-ID and OTP check (M-2), configuration (M-3) and the first offset
 * calibration (M-4b). On failure the fault flag is set and a product-ID
 * fault is counted (no_ack or wrong_id), and
 * mag_fault_cause() says why; the tick then re-probes at 5 Hz (M-2a) and keeps
 * running for the UARTs (M-6). Returns false on fault. */
bool mag_init(void);

/* D-1 (Rev 2.17): why the fault flag is set. MAG_CAUSE_NONE while it is
 * clear. Updated by every failed start-up check, probe or configuration,
 * so it always describes the latest failure. */
typedef enum {
    MAG_CAUSE_NONE = 0,
    MAG_CAUSE_NO_ACK = 1,         /* nothing acknowledged the address: no sensor,
                                   * no power, or SDA/SCL wiring               */
    MAG_CAUSE_WRONG_ID = 2,       /* a device answered with another product ID
                                   * (the byte is in mag_fault_id())           */
    MAG_CAUSE_BUS_ERROR = 3,      /* other I2C failure: data NACK, arbitration
                                   * lost, unexpected STOP, timeout            */
    MAG_CAUSE_OTP_NOT_LOADED = 4, /* start-up: OTP_Rd_Done clear after SW_RST */
    MAG_CAUSE_CONFIG_FAILED = 5,  /* start-up: a configuration write failed   */
    MAG_CAUSE_NOT_READY = 6,      /* run time: persistent not-ready samples (M-2b) */
} mag_fault_cause_t;

mag_fault_cause_t mag_fault_cause(void);

/* Product-ID byte of the latest MAG_CAUSE_WRONG_ID failure. */
uint8_t mag_fault_id(void);

#if APP_USB_STATUS
/* Rev 2.19 (diagnostic builds): start-up results and a measurement-timing
 * experiment, run once with blocking I2C after the normal start-up.
 * Times are us from the TM_M write to Meas_M_Done (Status read directly);
 * MAG_BOOT_NEVER = not done within 20 ms, 0 = I2C error / not run. */
#define MAG_BOOT_NEVER 0xFFFFu
typedef struct {
    bool ran;            /* product ID read OK, experiment run              */
    uint8_t id;          /* product-ID byte at start-up (0 if no answer)     */
    uint8_t status;      /* Status after SW_RST (OTP_Rd_Done = bit 4)        */
    uint8_t cause;       /* mag_fault_cause_t at the end of start-up         */
    bool cal_ok;         /* start-up offset calibration succeeded            */
    uint8_t cal_status;  /* Status byte of the last start-up burst read      */
    uint16_t t_fast_us;  /* BW = 800 Hz, each write ended by STOP            */
    uint16_t t_slow_us;  /* BW = 100 Hz (default), each write ended by STOP  */
    uint16_t t_rs_us;    /* BW = 800 Hz written, then TM_M after a repeated START */
    uint16_t t_200_us;   /* Rev 2.25: BW = 200 Hz, STOP-terminated writes (the
                          * 200 Hz rate allows ~4.6 ms; datasheet 4 ms)     */
    uint8_t burst_status;  /* Status at the end of a 9-byte burst, 2 ms after TM_M */
    uint8_t direct_status; /* Status read on its own, 2 ms after another TM_M */
} mag_boot_t;

const mag_boot_t *mag_boot(void);

/* Bench diagnostic (diagnostic builds): why the magnetometer cannot start
 * a new transaction after an abort (M-16). Updated on every tick that finds
 * the hardware not idle; ticks = consecutive such ticks (0 once idle). */
#define MAG_STUCK_BUS_CLEAR 0x001u  /* PIO bus clear has not finished       */
#define MAG_STUCK_DMA_ABORT 0x002u  /* DMA CHAN_ABORT still pending          */
#define MAG_STUCK_DMA_CMD 0x004u    /* command DMA channel busy              */
#define MAG_STUCK_DMA_DATA 0x008u   /* data DMA channel busy                 */
#define MAG_STUCK_IC_ABORT 0x010u   /* IC_ENABLE.ABORT still set             */
#define MAG_STUCK_MST_ACT 0x020u    /* controller reports master activity    */
#define MAG_STUCK_TX_NOT_EMPTY 0x040u /* controller TX FIFO not empty        */
#define MAG_STUCK_SCL_LOW 0x080u    /* SCL pin reads low                     */
#define MAG_STUCK_SDA_LOW 0x100u    /* SDA pin reads low                     */
#define MAG_STUCK_IC_DISABLED 0x200u /* IC_ENABLE.ENABLE clear               */
typedef struct {
    uint32_t flags;     /* MAG_STUCK_* at the latest stuck tick           */
    uint32_t ticks;     /* consecutive stuck ticks, 0 when not stuck      */
    uint32_t max_ticks; /* longest run since boot                         */
    uint32_t episodes;  /* stuck runs longer than 10 ticks                */
    uint32_t abrt_src;  /* IC_TX_ABRT_SOURCE of the latest abort          */
    uint32_t txflr;     /* IC_TXFLR at the latest stuck tick              */
    uint32_t status;    /* IC_STATUS at the latest stuck tick             */
    uint8_t state;      /* internal state (2 busy, 3 aborting, 4 aborted) */
} mag_stuck_t;

const mag_stuck_t *mag_stuck(void);
#endif

/* True while the sensor is not confirmed and configured: after a failed
 * start-up check (M-2), or at run time after persistent not-ready samples
 * (M-2b), until a re-probe succeeds (M-2a). */
bool mag_fault(void);

/* M-13: copy the latest sample in a short critical section. Main loop only.
 * Returns r->present. */
bool mag_read_latest(mag_reading_t *r);

/* Convert raw bytes to signed 18-bit counts (offset 131072 removed).
 * 16384 counts/G at 18 bits. Intended for the main loop (M-10). */
static inline void mag_convert(const uint8_t raw[MMC_SAMPLE_BYTES], int32_t xyz[3]) {
    uint32_t x = ((uint32_t)raw[0] << 10) | ((uint32_t)raw[1] << 2) | (((uint32_t)raw[6] >> 6) & 3u);
    uint32_t y = ((uint32_t)raw[2] << 10) | ((uint32_t)raw[3] << 2) | (((uint32_t)raw[6] >> 4) & 3u);
    uint32_t z = ((uint32_t)raw[4] << 10) | ((uint32_t)raw[5] << 2) | (((uint32_t)raw[6] >> 2) & 3u);
    xyz[0] = (int32_t)x - 131072;
    xyz[1] = (int32_t)y - 131072;
    xyz[2] = (int32_t)z - 131072;
}

/* Convenience: field in signed counts with the calibrated offset removed
 * (offset ignored if not valid). 16384 counts/G. */
static inline void mag_field(const mag_reading_t *r, int32_t xyz[3]) {
    mag_convert(r->s.raw, xyz);
    if (r->offset_valid) {
        for (int i = 0; i < 3; i++) {
            xyz[i] -= r->offset[i];
        }
    }
}

/* Field at 16-bit scale: 4096 counts/G, so +/-8 G spans the int16 range.
 * Calibrated offset removed, rounded to nearest (halves away from zero) and
 * clamped to [-32768, 32767]. It halves the size of X/Y/Z in a message (6
 * bytes instead of 12). At 1 kHz (BW = 11) the sensor noise (1.2 mG RMS) is
 * about 5 steps of 0.25 mG, so nothing useful is lost. At 100 and 50 Hz
 * (0.4 mG) it is under 2 steps: use mag_field() if that matters. */
static inline void mag_field16(const mag_reading_t *r, int16_t xyz[3]) {
    int32_t v[3];
    mag_field(r, v);
    for (int i = 0; i < 3; i++) {
        int32_t q = v[i] >= 0 ? (v[i] + 2) / 4 : -((-v[i] + 2) / 4);
        if (q > INT16_MAX) {
            q = INT16_MAX;
        } else if (q < INT16_MIN) {
            q = INT16_MIN;
        }
        xyz[i] = (int16_t)q;
    }
}

/* Called from the 1 ms tick ISR only (every tick; a sample transaction
 * starts on every Nth one, M-3b). */
void mag_tick_isr(uint32_t tick_index);

#endif /* MAG_H */
