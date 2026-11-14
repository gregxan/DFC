#include "status.h"

#include "config.h"

#if APP_USB_STATUS

#include <inttypes.h>
#include <stdio.h>

#include "hardware/irq.h"
#include "pico/stdio.h"
#include "pico/stdlib.h"
#include "tusb.h"

#include "diag.h"
#include "mag.h"

#include "prof.h"

extern volatile bool g_app_baud_ok;

#if APP_MODE == APP_MODE_USER
extern volatile uint32_t g_epoch_start_max_us;
#define N_LINES 9u /* adds EPOCH and PROF */
#else
/* Test application (app_test.c) results. */
extern volatile uint32_t g_app_mag_gaps;
extern volatile uint32_t g_app_mag_valid;
extern volatile uint32_t g_app_mag_invalid;
extern volatile uint32_t g_app_mag_stale;
extern volatile uint32_t g_app_rx_pattern_errors[2];
#define N_LINES 7u
#endif

#define STATUS_PERIOD_US 1000000u

static uint32_t s_total[DIAG_COUNT]; /* cumulative since boot */
static uint64_t s_next_us;
static unsigned s_line = N_LINES;    /* N_LINES = nothing pending */
static char s_buf[240]; /* < the 256-byte CDC TX buffer, so every line fits */
static int s_len;
static uint32_t s_seq;

void status_init(void) {
    stdio_init_all();
    /* B-20: USB below every UART-related interrupt, and below ours. */
    irq_set_priority(USBCTRL_IRQ, IRQ_PRIO_USB);
    s_next_us = time_us_64() + STATUS_PERIOD_US;
}

static uint32_t ns(diag_timing_t t) {
    /* cycles -> ns at clk_sys; diag_cycles_to_us is too coarse here. */
    return (uint32_t)(((uint64_t)g_diag_max_cycles[t] * 1000u) / (SYS_CLK_HZ / 1000000u));
}

/* Latest sample as the status line shows it (read once per report). */
static mag_reading_t s_mag;
static int32_t s_xyz[3];

#if APP_MODE == APP_MODE_USER
static uint32_t us(uint32_t cycles) {
    return diag_cycles_to_us(cycles);
}
#endif

static int format_line(unsigned line) {
    const unsigned n = sizeof s_buf;
    switch (line) {
    case 0:
        return snprintf(s_buf, n, "STAT seq=%" PRIu32 " mode=%u t_ms=%" PRIu32 " baud0=%" PRIu32 " baud1=%" PRIu32
                        " baud_ok=%u loop_max_us=%" PRIu32 " status_max_us=%" PRIu32 " tick_overrun=%" PRIu32 "\r\n",
                        s_seq, (unsigned)APP_MODE, (uint32_t)(time_us_64() / 1000u), g_diag_actual_baud[0], g_diag_actual_baud[1],
                        (unsigned)g_app_baud_ok, diag_cycles_to_us(g_diag_max_cycles[DIAG_T_LOOP]),
                        diag_cycles_to_us(g_diag_max_cycles[DIAG_T_STATUS]), s_total[DIAG_TICK_OVERRUN]);
    case 1:
    case 2: {
        unsigned k = line - 1u;
        unsigned b = DIAG_LINK_STRIDE * k;
        return snprintf(s_buf, n, "L%u rx_overflow=%" PRIu32 " tx_reject=%" PRIu32 " framing=%" PRIu32
                        " break=%" PRIu32 " overrun=%" PRIu32 " rx_hwm=%" PRIu32 " tx_hwm=%" PRIu32
#if APP_MODE != APP_MODE_USER
                        " pattern_err=%" PRIu32
#endif
                        " tx_irq=%" PRIu32 "\r\n",
                        k, s_total[b + DIAG_L0_RX_OVERFLOW], s_total[b + DIAG_L0_TX_REJECT],
                        s_total[b + DIAG_L0_FRAMING], s_total[b + DIAG_L0_BREAK], s_total[b + DIAG_L0_OVERRUN],
                        g_diag_rx_hwm[k], g_diag_tx_hwm[k],
#if APP_MODE != APP_MODE_USER
                        g_app_rx_pattern_errors[k],
#endif
                        g_diag_tx_irq[k]);
    }
    case 3:
        return snprintf(s_buf, n, "MAG fault=%u i2c_error=%" PRIu32 " missed_tick=%" PRIu32 " timeout=%" PRIu32
                        " pid_fault=%" PRIu32 " bus_clear=%" PRIu32
#if APP_MODE != APP_MODE_USER
                        " valid=%" PRIu32 " invalid=%" PRIu32 " gaps=%" PRIu32 " stale=%" PRIu32
#endif
                        "\r\n",
                        (unsigned)mag_fault(), s_total[DIAG_MAG_I2C_ERROR], s_total[DIAG_MAG_MISSED_TICK],
                        s_total[DIAG_MAG_TIMEOUT], s_total[DIAG_MAG_PRODUCT_ID_FAULT], s_total[DIAG_MAG_BUS_CLEAR]
#if APP_MODE != APP_MODE_USER
                        , g_app_mag_valid, g_app_mag_invalid, g_app_mag_gaps, g_app_mag_stale
#endif
        );
    case 4:
        /* Read the latest sample directly (works with any application). */
        s_xyz[0] = s_xyz[1] = s_xyz[2] = 0;
        if (mag_read_latest(&s_mag) && s_mag.s.valid) {
            mag_field(&s_mag, s_xyz);
        }
        return snprintf(s_buf, n, "MAGS sr_mode=%" PRIu32 " sr_interval=%" PRIu32 " recovered=%" PRIu32
                        " not_ready=%" PRIu32 " x=%" PRId32 " y=%" PRId32 " z=%" PRId32 "\r\n",
                        g_mag_sr_mode, g_mag_sr_interval, s_total[DIAG_MAG_RECOVERED],
                        s_total[DIAG_MAG_NOT_READY], s_xyz[0], s_xyz[1], s_xyz[2]);
    case 5:
        return snprintf(s_buf, n, "CAL offset_valid=%u offset_cal=%" PRIu32 " saturated=%" PRIu32 " ox=%" PRId32
                        " oy=%" PRId32 " oz=%" PRId32 "\r\n",
                        (unsigned)s_mag.offset_valid, s_total[DIAG_MAG_OFFSET_CAL], s_total[DIAG_MAG_SATURATED],
                        s_mag.offset[0], s_mag.offset[1], s_mag.offset[2]);
#if APP_MODE == APP_MODE_USER
    case 7: /* EX-8, D-4: 1 ms epoch work time */
        return snprintf(s_buf, n, "EPOCH last_us=%" PRIu32 " max_us=%" PRIu32 " avg_us=%" PRIu32
                        " overrun=%" PRIu32 " start_max_us=%" PRIu32 " count=%" PRIu32 "\r\n",
                        us(g_prof[PROF_EPOCH].last), us(g_prof[PROF_EPOCH].max), us(prof_avg(PROF_EPOCH)),
                        s_total[DIAG_EPOCH_OVERRUN], g_epoch_start_max_us, (uint32_t)g_prof[PROF_EPOCH].count);
    case 8: { /* D-6: sections, as last/max/avg in us */
        int len = snprintf(s_buf, n, "PROF");
        for (unsigned i = 1; i < PROF_COUNT && len > 0 && (unsigned)len < n; i++) {
            len += snprintf(s_buf + len, n - (unsigned)len, " %s=%" PRIu32 "/%" PRIu32 "/%" PRIu32, g_prof_name[i],
                            us(g_prof[i].last), us(g_prof[i].max), us(prof_avg((prof_id_t)i)));
        }
        if (len > 0 && (unsigned)len < n) {
            len += snprintf(s_buf + len, n - (unsigned)len, "\r\n");
        }
        return len;
    }
#endif
    default:
        return snprintf(s_buf, n, "TIM_NS isr_uart0=%" PRIu32 " isr_uart1=%" PRIu32 " isr_txdma=%" PRIu32
                        " isr_i2c=%" PRIu32 " isr_tick=%" PRIu32 " crit=%" PRIu32 " api_read=%" PRIu32
                        " api_write=%" PRIu32 " api_mag=%" PRIu32 "\r\n",
                        ns(DIAG_T_ISR_UART0_ERR), ns(DIAG_T_ISR_UART1_ERR), ns(DIAG_T_ISR_TX_DMA),
                        ns(DIAG_T_ISR_I2C), ns(DIAG_T_ISR_TICK), ns(DIAG_T_CRITICAL), ns(DIAG_T_API_READ),
                        ns(DIAG_T_API_WRITE), ns(DIAG_T_API_MAG));
    }
}

void status_poll(void) {
    if (s_line >= N_LINES) {
        if ((int64_t)(time_us_64() - s_next_us) < 0) {
            return;
        }
        s_next_us += STATUS_PERIOD_US;
        uint32_t snap[DIAG_COUNT];
        diag_take_all(snap); /* D-2 */
        for (unsigned i = 0; i < DIAG_COUNT; i++) {
            s_total[i] += snap[i];
        }
        s_seq++;
        s_line = 0;
        s_len = 0;
    }
    if (s_len == 0) {
        s_len = format_line(s_line);
        if (s_len <= 0 || s_len >= (int)sizeof s_buf) {
            s_len = 0;
            s_line++;
            return;
        }
        return; /* format and send on separate iterations to bound loop time */
    }
    if (!tud_cdc_connected()) {
        s_len = 0;
        s_line = N_LINES; /* drop this report */
        return;
    }
    if ((int)tud_cdc_write_available() < s_len) {
        return; /* try again next iteration */
    }
    stdio_put_string(s_buf, s_len, false, false);
    s_len = 0;
    s_line++;
}

#else

void status_init(void) {}
void status_poll(void) {}

#endif
