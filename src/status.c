#include "status.h"

#include "config.h"

#if APP_USB_STATUS

#include <inttypes.h>
#include <stdbool.h>
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

/* Two output formats (Rev 2.16):
 *   readable (default) - a labelled block for a person at a terminal;
 *   machine            - compact "GROUP key=value" lines (N_LINES of them),
 *                        parsed by tools/fwstatus.py and tools/soak_test.py.
 * Send 'm' over the same USB port for machine lines, 'h' for readable. The
 * switch takes effect at the next report. */
static bool s_human = true;
static bool s_want_human = true;

static uint32_t s_total[DIAG_COUNT]; /* cumulative since boot */
static uint32_t s_delta[DIAG_COUNT]; /* change since the previous report */
static uint64_t s_next_us;
static unsigned s_line = 0xFFFFu;    /* >= line count: nothing pending */
static char s_buf[240];
static int s_len; /* length of the line in s_buf; 0 = none pending */
static int s_off; /* bytes of it already handed to USB */
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
                        " not_ready=%" PRIu32 " runtime_fault=%" PRIu32 " x=%" PRId32 " y=%" PRId32
                        " z=%" PRId32 "\r\n",
                        g_mag_sr_mode, g_mag_sr_interval, s_total[DIAG_MAG_RECOVERED],
                        s_total[DIAG_MAG_NOT_READY], s_total[DIAG_MAG_RUNTIME_FAULT], s_xyz[0], s_xyz[1],
                        s_xyz[2]);
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


/* ------------------------------------------------------ readable format */

#if APP_MODE != APP_MODE_USER
static uint32_t s_prev_valid, s_prev_gaps, s_d_valid, s_d_gaps;
static uint32_t s_prev_pattern[2], s_d_pattern[2];
#endif

enum {
    H_HEADER,
    H_BAUD,
    H_UART_HEAD,
    H_RX_OVERFLOW,
    H_TX_REJECT,
    H_FRAMING,
    H_BREAK,
    H_OVERRUN,
#if APP_MODE != APP_MODE_USER
    H_PATTERN,
#endif
    H_QUEUE,
    H_TX_IRQ,
    H_BLANK1,
    H_MAG_STATE,
#if APP_MODE != APP_MODE_USER
    H_MAG_SAMPLES,
#endif
    H_MAG_BUS,
    H_MAG_HEALTH,
    H_MAG_FIELD,
    H_MAG_CAL,
    H_BLANK2,
    H_TIM_HEAD,
    H_TIM_ISR,
    H_TIM_LOOP,
#if APP_MODE == APP_MODE_USER
    H_TIM_PROF,
#endif
    H_ATTENTION,
    H_COUNT
};

/* "123" or "123 (+4)" */
static const char *cell(char *b, size_t n, uint32_t total, uint32_t delta) {
    if (delta != 0u) {
        snprintf(b, n, "%" PRIu32 " (+%" PRIu32 ")", total, delta);
    } else {
        snprintf(b, n, "%" PRIu32, total);
    }
    return b;
}

static int uart_row(const char *label, diag_ctr_t base) {
    char a[24], b[24];
    unsigned i0 = (unsigned)base, i1 = (unsigned)base + DIAG_LINK_STRIDE;
    return snprintf(s_buf, sizeof s_buf, "  %-18s%14s%16s\r\n", label, cell(a, sizeof a, s_total[i0], s_delta[i0]),
                    cell(b, sizeof b, s_total[i1], s_delta[i1]));
}

static uint32_t max_isr_ns(void) {
    uint32_t m = 0;
    const diag_timing_t t[] = {DIAG_T_ISR_UART0_ERR, DIAG_T_ISR_UART1_ERR, DIAG_T_ISR_TX_DMA, DIAG_T_ISR_I2C,
                               DIAG_T_ISR_TICK};
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
        uint32_t v = ns(t[i]);
        m = v > m ? v : m;
    }
    return m;
}

static uint32_t max_api_ns(void) {
    uint32_t a = ns(DIAG_T_API_READ), b = ns(DIAG_T_API_WRITE), c = ns(DIAG_T_API_MAG);
    uint32_t m = a > b ? a : b;
    return m > c ? m : c;
}

static uint32_t loop_or_epoch_max_us(void) {
#if APP_MODE == APP_MODE_USER
    return us(g_prof[PROF_EPOCH].max);
#else
    return diag_cycles_to_us(g_diag_max_cycles[DIAG_T_LOOP]);
#endif
}

/* Append "; text" (or "text") to the attention list, keeping room for
 * the line ending. */
static int attn_add(int len, bool *first, const char *text) {
    const int room = (int)sizeof s_buf - 3;
    if (len < 0 || len >= room) {
        return len;
    }
    int w = snprintf(s_buf + len, (size_t)(room - len), "%s%s", *first ? "" : "; ", text);
    *first = false;
    if (w < 0) {
        return len;
    }
    return (len + w > room) ? room : len + w;
}

static int format_human(unsigned line) {
    const size_t n = sizeof s_buf;
    char a[24], b[24], c[24], d[24];
    switch (line) {
    case H_HEADER: {
        uint32_t t = (uint32_t)(time_us_64() / 1000000u);
        static const char *const mode[] = {"echo test", "pattern test", "user application"};
        return snprintf(s_buf, n,
                        "\r\n===== DFC status #%" PRIu32 " | up %02" PRIu32 ":%02" PRIu32 ":%02" PRIu32
                        " | %s | send 'm' for machine format =====\r\n",
                        s_seq, t / 3600u, (t / 60u) % 60u, t % 60u, mode[APP_MODE]);
    }
    case H_BAUD:
        return snprintf(s_buf, n, "Baud           UART0 %" PRIu32 "   UART1 %" PRIu32 "   %s\r\n", g_diag_actual_baud[0],
                        g_diag_actual_baud[1], g_app_baud_ok ? "OK" : "OUT OF TOLERANCE");
    case H_UART_HEAD:
        return snprintf(s_buf, n, "\r\nUART%30s%16s\r\n", "UART0", "UART1");
    case H_RX_OVERFLOW:
        return uart_row("RX overflows", DIAG_L0_RX_OVERFLOW);
    case H_TX_REJECT:
        return uart_row("TX rejects", DIAG_L0_TX_REJECT);
    case H_FRAMING:
        return uart_row("Framing errors", DIAG_L0_FRAMING);
    case H_BREAK:
        return uart_row("Breaks", DIAG_L0_BREAK);
    case H_OVERRUN:
        return uart_row("HW overruns", DIAG_L0_OVERRUN);
#if APP_MODE != APP_MODE_USER
    case H_PATTERN:
        return snprintf(s_buf, n, "  %-18s%14s%16s\r\n", "Pattern errors",
                        cell(a, sizeof a, g_app_rx_pattern_errors[0], s_d_pattern[0]),
                        cell(b, sizeof b, g_app_rx_pattern_errors[1], s_d_pattern[1]));
#endif
    case H_QUEUE:
        snprintf(a, sizeof a, "%" PRIu32 " / %" PRIu32, g_diag_rx_hwm[0], g_diag_tx_hwm[0]);
        snprintf(b, sizeof b, "%" PRIu32 " / %" PRIu32, g_diag_rx_hwm[1], g_diag_tx_hwm[1]);
        return snprintf(s_buf, n, "  %-18s%14s%16s\r\n", "Peak RX / TX bytes", a, b);
    case H_TX_IRQ:
        return snprintf(s_buf, n, "  %-18s%14" PRIu32 "%16" PRIu32 "\r\n", "TX interrupts", g_diag_tx_irq[0],
                        g_diag_tx_irq[1]);
    case H_BLANK1:
    case H_BLANK2:
        return snprintf(s_buf, n, "\r\n");
    case H_MAG_STATE:
        /* Read the latest sample once per report (also used below). */
        s_xyz[0] = s_xyz[1] = s_xyz[2] = 0;
        if (mag_read_latest(&s_mag) && s_mag.s.valid) {
            mag_field(&s_mag, s_xyz);
        }
        if (!mag_fault()) {
            return snprintf(s_buf, n, "MAGNETOMETER   OK\r\n");
        }
        if (s_delta[DIAG_MAG_RUNTIME_FAULT] != 0u) {
            return snprintf(s_buf, n, "MAGNETOMETER   FAULT - samples kept arriving unfinished (not ready); "
                                      "reconfiguring\r\n");
        }
        return snprintf(s_buf, n, "MAGNETOMETER   FAULT - not detected (no answer, or wrong product ID); "
                                  "re-probing 5 times a second\r\n");
#if APP_MODE != APP_MODE_USER
    case H_MAG_SAMPLES:
        return snprintf(s_buf, n, "  Samples      valid %s   invalid %" PRIu32 "   gaps %s   stale %" PRIu32 "\r\n",
                        cell(a, sizeof a, g_app_mag_valid, s_d_valid), g_app_mag_invalid,
                        cell(b, sizeof b, g_app_mag_gaps, s_d_gaps), g_app_mag_stale);
#endif
    case H_MAG_BUS:
        return snprintf(s_buf, n, "  Bus          I2C errors %s   timeouts %s   missed ticks %s   bus clears %" PRIu32
                                  "\r\n",
                        cell(a, sizeof a, s_total[DIAG_MAG_I2C_ERROR], s_delta[DIAG_MAG_I2C_ERROR]),
                        cell(b, sizeof b, s_total[DIAG_MAG_TIMEOUT], s_delta[DIAG_MAG_TIMEOUT]),
                        cell(c, sizeof c, s_total[DIAG_MAG_MISSED_TICK], s_delta[DIAG_MAG_MISSED_TICK]),
                        s_total[DIAG_MAG_BUS_CLEAR]);
    case H_MAG_HEALTH:
        return snprintf(s_buf, n, "  Health       product-ID faults %s   not ready %s   run-time faults %s   "
                                  "recoveries %" PRIu32 "\r\n",
                        cell(a, sizeof a, s_total[DIAG_MAG_PRODUCT_ID_FAULT], s_delta[DIAG_MAG_PRODUCT_ID_FAULT]),
                        cell(b, sizeof b, s_total[DIAG_MAG_NOT_READY], s_delta[DIAG_MAG_NOT_READY]),
                        cell(d, sizeof d, s_total[DIAG_MAG_RUNTIME_FAULT], s_delta[DIAG_MAG_RUNTIME_FAULT]),
                        s_total[DIAG_MAG_RECOVERED]);
    case H_MAG_FIELD:
        if (g_mag_sr_mode == MAG_SR_PULSE) {
            snprintf(a, sizeof a, "SET every %" PRIu32, g_mag_sr_interval);
        } else {
            snprintf(a, sizeof a, "single cycle");
        }
        return snprintf(s_buf, n, "  Field        x %7" PRId32 "   y %7" PRId32 "   z %7" PRId32
                                  "   counts, 16384/G   (SET/RESET: %s)\r\n",
                        s_xyz[0], s_xyz[1], s_xyz[2], a);
    case H_MAG_CAL:
        return snprintf(s_buf, n, "  Offset       x %7" PRId32 "   y %7" PRId32 "   z %7" PRId32
                                  "   %s, %" PRIu32 " calibrations   saturated %" PRIu32 "\r\n",
                        s_mag.offset[0], s_mag.offset[1], s_mag.offset[2],
                        s_mag.offset_valid ? "calibrated" : "not calibrated", s_total[DIAG_MAG_OFFSET_CAL],
                        s_total[DIAG_MAG_SATURATED]);
    case H_TIM_HEAD:
        return snprintf(s_buf, n, "TIMING (worst case since boot)\r\n");
    case H_TIM_ISR:
        return snprintf(s_buf, n, "  Handlers %" PRIu32 " ns (limit 5000)   critical section %" PRIu32
                                  " ns (limit 2000)   API %" PRIu32 " ns (limit 2000)\r\n",
                        max_isr_ns(), ns(DIAG_T_CRITICAL), max_api_ns());
    case H_TIM_LOOP:
#if APP_MODE == APP_MODE_USER
        return snprintf(s_buf, n, "  Epoch work last %" PRIu32 " us   avg %" PRIu32 " us   max %" PRIu32
                                  " us (limit 1000)   start latency max %" PRIu32 " us   overruns %s   tick overruns %s\r\n",
                        us(g_prof[PROF_EPOCH].last), us(prof_avg(PROF_EPOCH)), us(g_prof[PROF_EPOCH].max),
                        g_epoch_start_max_us,
                        cell(a, sizeof a, s_total[DIAG_EPOCH_OVERRUN], s_delta[DIAG_EPOCH_OVERRUN]),
                        cell(b, sizeof b, s_total[DIAG_TICK_OVERRUN], s_delta[DIAG_TICK_OVERRUN]));
#else
        return snprintf(s_buf, n, "  Main loop max %" PRIu32 " us (limit 1000)   tick overruns %s\r\n",
                        loop_or_epoch_max_us(),
                        cell(a, sizeof a, s_total[DIAG_TICK_OVERRUN], s_delta[DIAG_TICK_OVERRUN]));
#endif
#if APP_MODE == APP_MODE_USER
    case H_TIM_PROF: {
        int len = snprintf(s_buf, n, "  Sections (last/max/avg us):");
        for (unsigned i = 1; i < PROF_COUNT && len > 0 && (size_t)len < n; i++) {
            len += snprintf(s_buf + len, n - (size_t)len, "  %s %" PRIu32 "/%" PRIu32 "/%" PRIu32, g_prof_name[i],
                            us(g_prof[i].last), us(g_prof[i].max), us(prof_avg((prof_id_t)i)));
        }
        if (len > 0 && (size_t)len < n) {
            len += snprintf(s_buf + len, n - (size_t)len, "\r\n");
        }
        return len;
    }
#endif
    case H_ATTENTION: {
        bool first = true;
        int len = snprintf(s_buf, n, "\r\nATTENTION: ");
        const int start = len;
        if (!g_app_baud_ok) {
            len = attn_add(len, &first, "UART baud out of tolerance");
        }
        static const char *const ename[] = {"RX overflow", "TX reject", "framing errors", "breaks",
                                            "HW overruns"};
        for (unsigned k = 0; k < 2u; k++) {
            for (unsigned e = 0; e < 5u; e++) {
                if (s_delta[DIAG_L0_RX_OVERFLOW + e + DIAG_LINK_STRIDE * k] != 0u) {
                    char t[40];
                    snprintf(t, sizeof t, "UART%u %s", k, ename[e]);
                    len = attn_add(len, &first, t);
                }
            }
        }
#if APP_MODE != APP_MODE_USER
        for (unsigned k = 0; k < 2u; k++) {
            if (s_d_pattern[k] != 0u) {
                char t[40];
                snprintf(t, sizeof t, "UART%u pattern errors", k);
                len = attn_add(len, &first, t);
            }
        }
        if (s_d_gaps != 0u) {
            len = attn_add(len, &first, "magnetometer sample gaps");
        }
#endif
        if (mag_fault()) {
            len = attn_add(len, &first, s_delta[DIAG_MAG_RUNTIME_FAULT] ? "magnetometer run-time fault"
                                                                        : "magnetometer not detected");
        } else {
            if (s_delta[DIAG_MAG_I2C_ERROR] | s_delta[DIAG_MAG_TIMEOUT] | s_delta[DIAG_MAG_MISSED_TICK]) {
                len = attn_add(len, &first, "magnetometer bus errors");
            }
            if (s_delta[DIAG_MAG_NOT_READY]) {
                len = attn_add(len, &first, "magnetometer samples not ready");
            }
        }
        if (max_isr_ns() > 5000u || ns(DIAG_T_CRITICAL) > 2000u || max_api_ns() > 2000u) {
            len = attn_add(len, &first, "handler/critical/API time over limit");
        }
        if (loop_or_epoch_max_us() > 999u) {
            len = attn_add(len, &first, "main loop over 1 ms");
        }
        if (s_delta[DIAG_TICK_OVERRUN] | s_delta[DIAG_EPOCH_OVERRUN]) {
            len = attn_add(len, &first, "tick/epoch overruns");
        }
        if (len == start) {
            return snprintf(s_buf, n, "\r\nSTATUS: all checks OK\r\n");
        }
        return len + snprintf(s_buf + len, n - (size_t)len, "\r\n");
    }
    default:
        return 0;
    }
}

static unsigned line_count(void) {
    return s_human ? (unsigned)H_COUNT : N_LINES;
}

void status_poll(void) {
    /* Format selection from the host: 'm' machine, 'h' readable. */
    int ch = getchar_timeout_us(0);
    if (ch == 'm' || ch == 'M') {
        s_want_human = false;
    } else if (ch == 'h' || ch == 'H') {
        s_want_human = true;
    }

    if (s_line >= line_count()) {
        if ((int64_t)(time_us_64() - s_next_us) < 0) {
            return;
        }
        s_next_us += STATUS_PERIOD_US;
        uint32_t snap[DIAG_COUNT];
        diag_take_all(snap); /* D-2 */
        for (unsigned i = 0; i < DIAG_COUNT; i++) {
            s_total[i] += snap[i];
            s_delta[i] = snap[i];
        }
#if APP_MODE != APP_MODE_USER
        uint32_t v = g_app_mag_valid, gp = g_app_mag_gaps;
        s_d_valid = v - s_prev_valid;
        s_prev_valid = v;
        s_d_gaps = gp - s_prev_gaps;
        s_prev_gaps = gp;
        for (unsigned k = 0; k < 2u; k++) {
            uint32_t pe = g_app_rx_pattern_errors[k];
            s_d_pattern[k] = pe - s_prev_pattern[k];
            s_prev_pattern[k] = pe;
        }
#endif
        s_human = s_want_human;
        s_seq++;
        s_line = 0;
        s_len = 0;
    }
    if (s_len == 0) {
        s_len = s_human ? format_human(s_line) : format_line(s_line);
        s_off = 0;
        if (s_len <= 0 || s_len >= (int)sizeof s_buf) {
            s_len = 0;
            s_line++;
            return;
        }
        return; /* format and send on separate iterations to bound loop time */
    }
    /* The host must have the port open with DTR asserted (most terminals
     * do this automatically); otherwise the report is dropped. */
    if (!tud_cdc_connected()) {
        s_len = 0;
        s_line = 0xFFFFu; /* drop this report */
        return;
    }
    /* Send as much of the line as the CDC TX buffer has room for now, and
     * the rest on later passes. Never waits: a line can be longer than the
     * whole buffer (64 bytes by default at USB full speed). */
    int avail = (int)tud_cdc_write_available();
    if (avail <= 0) {
        return; /* try again next pass */
    }
    int n = s_len - s_off;
    if (n > avail) {
        n = avail;
    }
    stdio_put_string(s_buf + s_off, n, false, false);
    s_off += n;
    if (s_off >= s_len) {
        s_len = 0;
        s_line++;
    }
}

#else

void status_init(void) {}
void status_poll(void) {}

#endif
