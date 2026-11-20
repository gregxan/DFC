#include "status.h"

#include "config.h"

#if APP_USB_STATUS

#include <inttypes.h>
#include <stdarg.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>

#include "hardware/irq.h"
#include "pico/stdio.h"
#include "pico/bootrom.h"
#include "pico/stdlib.h"
#include "tusb.h"

#include "crash.h"
#include "diag.h"
#include "mag.h"
#include "wirecheck.h"

#include "prof.h"

extern volatile bool g_app_baud_ok;

#if APP_MODE == APP_MODE_USER
extern volatile uint32_t g_epoch_start_max_us;
#define N_LINES 11u /* adds EPOCH and PROF */
#else
/* Test application (app_test.c) results. */
extern volatile uint32_t g_app_mag_gaps;
extern volatile uint32_t g_app_mag_valid;
extern volatile uint32_t g_app_mag_invalid;
extern volatile uint32_t g_app_mag_stale;
extern volatile uint32_t g_app_rx_pattern_errors[2];
#define N_LINES 9u
#endif

#define STATUS_PERIOD_US 1000000u

/* Two output formats (Rev 2.16):
 *   readable (default) - a labelled block for a person at a terminal;
 *   machine            - compact "GROUP key=value" lines (N_LINES of them),
 *                        parsed by tools/fwstatus.py and tools/soak_test.py.
 * Send 'm' over the same USB port for machine lines, 'h' for readable. The
 * switch takes effect at the next report. '~' reboots into the USB
 * bootloader for reflashing (Rev 2.20). */
static bool s_human = true;
static bool s_want_human = true;

static uint32_t s_total[DIAG_COUNT]; /* cumulative since boot */
static uint32_t s_delta[DIAG_COUNT]; /* change since the previous report */
static uint64_t s_next_us;
static unsigned s_line = 0xFFFFu;    /* >= line count: nothing pending */
static char s_buf[1200]; /* one report item; may hold several lines */
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
                        " pid_fault=%" PRIu32 " no_ack=%" PRIu32 " wrong_id=%" PRIu32 " cause=%u id=0x%02X"
                        " bus_clear=%" PRIu32
#if APP_MODE != APP_MODE_USER
                        " valid=%" PRIu32 " invalid=%" PRIu32 " gaps=%" PRIu32 " stale=%" PRIu32
#endif
                        "\r\n",
                        (unsigned)mag_fault(), s_total[DIAG_MAG_I2C_ERROR], s_total[DIAG_MAG_MISSED_TICK],
                        s_total[DIAG_MAG_TIMEOUT], s_total[DIAG_MAG_NO_ACK] + s_total[DIAG_MAG_WRONG_ID],
                        s_total[DIAG_MAG_NO_ACK], s_total[DIAG_MAG_WRONG_ID], (unsigned)mag_fault_cause(),
                        (unsigned)mag_fault_id(), s_total[DIAG_MAG_BUS_CLEAR]
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
    case 6: { /* Rev 2.18: start-up wiring check */
        const wirecheck_t *w = wirecheck_result();
        return snprintf(s_buf, n, "WIRE ran=%u sda_low=%u scl_low=%u sda_pullup=%u scl_pullup=%u shorted=%u"
                        " found=%u found_swapped=%u mag=%u mag_swapped=%u first=0x%02X first_swapped=0x%02X\r\n",
                        (unsigned)w->ran, (unsigned)w->held_low[WIRE_SDA], (unsigned)w->held_low[WIRE_SCL],
                        (unsigned)w->ext_pullup[WIRE_SDA], (unsigned)w->ext_pullup[WIRE_SCL], (unsigned)w->shorted,
                        (unsigned)w->n_found[0], (unsigned)w->n_found[1],
                        (unsigned)wirecheck_found(0u, (uint8_t)MAG_I2C_ADDR),
                        (unsigned)wirecheck_found(1u, (uint8_t)MAG_I2C_ADDR),
                        w->n_found[0] ? (unsigned)w->found[0][0] : 0u, w->n_found[1] ? (unsigned)w->found[1][0] : 0u);
    }
    case 7: { /* Rev 2.19: start-up results and measurement timing */
        const mag_boot_t *bt = mag_boot();
        return snprintf(s_buf, n, "BOOT ran=%u id=0x%02X status=0x%02X cause=%u cal_ok=%u cal_status=0x%02X"
                        " t_fast_us=%u t_slow_us=%u t_rs_us=%u burst_status=0x%02X direct_status=0x%02X"
                        " rst=%u rst_crumb=%u rst_irq=%" PRIu32 " rst_exc=%u rst_ticks=%" PRIu32
                        " rst_pc=0x%08" PRIX32 " rst_lr=0x%08" PRIX32 " rst_cfsr=0x%08" PRIX32 "\r\n",
                        (unsigned)bt->ran, (unsigned)bt->id, (unsigned)bt->status, (unsigned)bt->cause,
                        (unsigned)bt->cal_ok, (unsigned)bt->cal_status, (unsigned)bt->t_fast_us,
                        (unsigned)bt->t_slow_us, (unsigned)bt->t_rs_us, (unsigned)bt->burst_status,
                        (unsigned)bt->direct_status, (unsigned)crash_last()->kind, (unsigned)crash_last()->crumb,
                        crash_last()->irq, (unsigned)crash_last()->exception,
                        crash_last()->isr_tick - crash_last()->loop_tick, crash_last()->pc, crash_last()->lr,
                        crash_last()->cfsr);
    }
#if APP_MODE == APP_MODE_USER
    case 9: /* EX-8, D-4: 1 ms epoch work time */
        return snprintf(s_buf, n, "EPOCH last_us=%" PRIu32 " max_us=%" PRIu32 " avg_us=%" PRIu32
                        " overrun=%" PRIu32 " start_max_us=%" PRIu32 " count=%" PRIu32 "\r\n",
                        us(g_prof[PROF_EPOCH].last), us(g_prof[PROF_EPOCH].max), us(prof_avg(PROF_EPOCH)),
                        s_total[DIAG_EPOCH_OVERRUN], g_epoch_start_max_us, (uint32_t)g_prof[PROF_EPOCH].count);
    case 10: { /* D-6: sections, as last/max/avg in us */
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
    H_RESET,
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
    H_WIRE,
    H_WIRE_VERDICT,
    H_BOOT,
    H_BOOT_TIME,
    H_BOOT_VERDICT,
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

/* Measurement time for the start-up experiment (Rev 2.19). */
static const char *ms_text(char *b, size_t n, uint16_t us_val) {
    if (us_val == 0u) {
        snprintf(b, n, "error");
    } else if (us_val == MAG_BOOT_NEVER) {
        snprintf(b, n, "never (>20 ms)");
    } else {
        snprintf(b, n, "%u.%02u ms", us_val / 1000u, (us_val % 1000u) / 10u);
    }
    return b;
}

static const char *boot_verdict(const mag_boot_t *bt) {
    if (!bt->ran) {
        return "not run (sensor did not answer with ID 0x30 at start-up)";
    }
    bool fast = bt->t_fast_us != 0u && bt->t_fast_us < 2000u;
    bool slow_ok = bt->t_slow_us != 0u && bt->t_slow_us != MAG_BOOT_NEVER;
    if (bt->t_fast_us == MAG_BOOT_NEVER && bt->t_slow_us == MAG_BOOT_NEVER) {
        return "measurements never finish: the TM_M trigger is not taking effect";
    }
    if (!fast && slow_ok) {
        return "800 Hz setting not taking effect even with STOP-ended writes";
    }
    if ((bt->direct_status & MMC_STATUS_MEAS_M_DONE) && !(bt->burst_status & MMC_STATUS_MEAS_M_DONE)) {
        /* Expected on the real part (Rev 2.21): handled by reading Status first. */
        if (!(bt->status & MMC_STATUS_OTP_RD_DONE)) {
            return "OTP_Rd_Done was clear after the software reset";
        }
        return "sensor timing OK (reading the data clears Meas_M_Done, so Status is read first)";
    }
    if (fast && (bt->t_rs_us == MAG_BOOT_NEVER || bt->t_rs_us >= 2000u)) {
        return "writes followed by a repeated START are ignored: the DMA configuration needs a STOP after each write";
    }
    if (!(bt->status & MMC_STATUS_OTP_RD_DONE)) {
        return "OTP_Rd_Done was clear after the software reset (start-up check failed); timing otherwise OK";
    }
    return "sensor timing OK";
}

/* ---- 80-column layout helpers (Rev 2.22) ------------------------------ */

/* Every physical line of the readable report fits an 80-column terminal:
 * at most WIDTH characters before the CR LF. One report item may span
 * several physical lines. */
#define WIDTH 78
#define LABEL "              " /* value column: 14 characters in */

/* Append formatted text to s_buf at len; returns the new length, clamped so
 * the buffer always stays NUL-terminated. */
static int put(int len, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static int put(int len, const char *fmt, ...) {
    const int cap = (int)sizeof s_buf - 1;
    if (len < 0 || len >= cap) {
        return len;
    }
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(s_buf + len, (size_t)(cap + 1 - len), fmt, ap);
    va_end(ap);
    if (w < 0) {
        return len;
    }
    return (len + w > cap) ? cap : len + w;
}

/* Word-wrap text to WIDTH: the first line starts with prefix, the others
 * with indent spaces. Every line ends in CR LF. */
static int wrap(int len, const char *prefix, unsigned indent, const char *text) {
    len = put(len, "%s", prefix);
    unsigned col = (unsigned)strlen(prefix);
    const char *p = text;
    while (*p != '\0') {
        while (*p == ' ') {
            p++;
        }
        size_t wl = strcspn(p, " ");
        if (wl == 0u) {
            break;
        }
        bool at_start = (col == (unsigned)strlen(prefix)) || (col == indent && p != text);
        if (!at_start && col + 1u + wl > WIDTH) {
            len = put(len, "\r\n%*s", (int)indent, "");
            col = indent;
            at_start = true;
        }
        if (!at_start) {
            len = put(len, " ");
            col++;
        }
        len = put(len, "%.*s", (int)wl, p);
        col += (unsigned)wl;
        p += wl;
    }
    return put(len, "\r\n");
}

/* "label value" items separated by three spaces, wrapped as whole items
 * onto LABEL-indented lines when the next would pass WIDTH. */
static int items(int len, const char *prefix, const char *const *it, unsigned count) {
    len = put(len, "%s", prefix);
    unsigned col = (unsigned)strlen(prefix);
    for (unsigned i = 0; i < count; i++) {
        unsigned w = (unsigned)strlen(it[i]);
        if (i != 0u && col + 3u + w > WIDTH) {
            len = put(len, "\r\n" LABEL);
            col = 14u;
        } else if (i != 0u) {
            len = put(len, "   ");
            col += 3u;
        }
        len = put(len, "%s", it[i]);
        col += w;
    }
    return put(len, "\r\n");
}

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
    char a[28], b[28];
    unsigned i0 = (unsigned)base, i1 = (unsigned)base + DIAG_LINK_STRIDE;
    return snprintf(s_buf, sizeof s_buf, "  %-18s%14s%16s\r\n", label, cell(a, sizeof a, s_total[i0], s_delta[i0]),
                    cell(b, sizeof b, s_total[i1], s_delta[i1]));
}

/* Interrupt handlers and API calls with their limits (V-5, EX-7). */
static const diag_timing_t k_isr_slot[] = {DIAG_T_ISR_UART0_ERR, DIAG_T_ISR_UART1_ERR, DIAG_T_ISR_TX_DMA,
                                          DIAG_T_ISR_I2C, DIAG_T_ISR_TICK};
static const char *const k_isr_name[] = {"UART0 error", "UART1 error", "TX DMA", "I2C", "tick"};
static const diag_timing_t k_api_slot[] = {DIAG_T_API_READ, DIAG_T_API_WRITE, DIAG_T_API_MAG};
static const char *const k_api_name[] = {"link_read", "link_write", "mag_read_latest"};
#define ISR_LIMIT_NS 5000u
#define CRIT_LIMIT_NS 2000u
#define API_LIMIT_NS 2000u

static unsigned worst(const diag_timing_t *slot, unsigned count) {
    unsigned w = 0;
    for (unsigned i = 1; i < count; i++) {
        if (ns(slot[i]) > ns(slot[w])) {
            w = i;
        }
    }
    return w;
}

static uint32_t loop_or_epoch_max_us(void) {
#if APP_MODE == APP_MODE_USER
    return us(g_prof[PROF_EPOCH].max);
#else
    return diag_cycles_to_us(g_diag_max_cycles[DIAG_T_LOOP]);
#endif
}

/* D-9: what the last reset left (diagnostic builds). */
static const char *const k_crumb[CRUMB_COUNT] = {"unknown", "in app_poll()", "reading USB input",
                                                 "formatting a report line", "sending to USB", "in the main loop"};

static const char *irq_name(uint32_t irq) {
    if (irq <= 7u) {
        return "timer (tick or SDK alarm)";
    }
    if (irq >= 10u && irq <= 13u) {
        return "DMA (UART TX)";
    }
    if (irq == 14u) {
        return "USB controller";
    }
    if (irq >= 15u && irq <= 20u) {
        return "PIO";
    }
    if (irq == 33u || irq == 34u) {
        return "UART error";
    }
    if (irq == 36u || irq == 37u) {
        return "I2C (magnetometer)";
    }
    if (irq >= 46u && irq <= 51u) {
        return "USB stack task";
    }
    return "other";
}

static const char *cfsr_text(uint32_t cfsr) {
    static const struct {
        uint32_t bit;
        const char *text;
    } k[] = {{1u << 25, "divide by zero"},           {1u << 24, "unaligned access"},
             {1u << 20, "stack overflow"},           {1u << 19, "coprocessor (FPU) not enabled"},
             {1u << 18, "invalid PC on return"},     {1u << 17, "invalid state (Thumb bit)"},
             {1u << 16, "undefined instruction"},    {1u << 9, "bus error on data access"},
             {1u << 10, "imprecise bus error"},      {1u << 8, "bus error on instruction fetch"},
             {1u << 12, "bus error stacking"},       {1u << 11, "bus error unstacking"},
             {1u << 1, "memory protection: data"},   {1u << 0, "memory protection: instruction"},
             {1u << 4, "memory protection stacking"}};
    for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++) {
        if (cfsr & k[i].bit) {
            return k[i].text;
        }
    }
    return cfsr ? "see CFSR" : "no fault status (escalated or debug)";
}

static int reset_text(int len) {
    const crash_t *c = crash_last();
    const char *crumb = k_crumb[c->crumb < CRUMB_COUNT ? c->crumb : 0u];
    char t[160];
    if (c->kind == CRASH_KIND_WATCHDOG) {
        len = put(len, "Last reset    WATCHDOG - main loop stopped for 2 s\r\n");
        snprintf(t, sizeof t, "Main loop was %s.", crumb);
        len = wrap(len, LABEL, 14u, t);
        if (c->irq != 0u) {
            snprintf(t, sizeof t, "Stuck inside interrupt IRQ %" PRIu32 " (%s).", c->irq - 1u, irq_name(c->irq - 1u));
        } else if (c->isr_tick != c->loop_tick) {
            snprintf(t, sizeof t, "Interrupts kept running: tick %" PRIu32 " ms past the main loop's last pass.",
                     c->isr_tick - c->loop_tick);
        } else {
            snprintf(t, sizeof t, "The tick stopped at the same time (interrupts off or stalled).");
        }
        return wrap(len, LABEL, 14u, t);
    }
    if (c->kind == CRASH_KIND_FAULT) {
        if (c->exception >= 16u) {
            snprintf(t, sizeof t, "HARD FAULT in interrupt IRQ %u (%s)", c->exception - 16u,
                     irq_name(c->exception - 16u));
        } else if (c->exception != 0u) {
            snprintf(t, sizeof t, "HARD FAULT in exception %u", c->exception);
        } else {
            snprintf(t, sizeof t, "HARD FAULT in the main loop (%s)", crumb);
        }
        len = wrap(len, "Last reset    ", 14u, t);
        len = put(len, LABEL "PC 0x%08" PRIX32 "   LR 0x%08" PRIX32 "   CFSR 0x%08" PRIX32 "\r\n", c->pc, c->lr,
                  c->cfsr);
        snprintf(t, sizeof t, "%s. Look up PC and LR with: arm-none-eabi-addr2line -f -e build\\diag\\firmware.elf "
                 "0x%08" PRIX32 " 0x%08" PRIX32, cfsr_text(c->cfsr), c->pc, c->lr);
        return wrap(len, LABEL, 14u, t);
    }
    return put(len, "Last reset    normal (power-on, reset button or reflash)\r\n");
}

/* One ATTENTION item: the heading before the first. */
static int attn_item(int len, unsigned *count, const char *text) {
    if (*count == 0u) {
        len = put(len, "\r\nATTENTION:\r\n");
    }
    (*count)++;
    return wrap(len, "  - ", 4u, text);
}

static int format_human(unsigned line) {
    const size_t n = sizeof s_buf;
    char a[28], b[28], c[28], d[28];
    char t[96];
    switch (line) {
    case H_HEADER: {
        uint32_t tm = (uint32_t)(time_us_64() / 1000000u);
        static const char *const mode[] = {"echo test", "pattern test", "user application"};
        return snprintf(s_buf, n,
                        "\r\n===== DFC status #%" PRIu32 " | up %02" PRIu32 ":%02" PRIu32 ":%02" PRIu32
                        " | %s =====\r\n(type 'm' machine format, 'h' readable, '~' reflash)\r\n",
                        s_seq, tm / 3600u, (tm / 60u) % 60u, tm % 60u, mode[APP_MODE]);
    }
    case H_BAUD:
        return snprintf(s_buf, n, "Baud          UART0 %" PRIu32 "   UART1 %" PRIu32 "   %s\r\n", g_diag_actual_baud[0],
                        g_diag_actual_baud[1], g_app_baud_ok ? "OK" : "OUT OF TOLERANCE");
    case H_RESET:
        return reset_text(0);
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
    case H_MAG_STATE: {
        /* Read the latest sample once per report (also used below). */
        s_xyz[0] = s_xyz[1] = s_xyz[2] = 0;
        if (mag_read_latest(&s_mag) && s_mag.s.valid) {
            mag_field(&s_mag, s_xyz);
        }
        if (!mag_fault()) {
            return snprintf(s_buf, n, "MAGNETOMETER  OK\r\n");
        }
        int len = 0;
        switch (mag_fault_cause()) {
        case MAG_CAUSE_NO_ACK:
            len = put(len, "MAGNETOMETER  FAULT - no answer at address 0x%02X\r\n", (unsigned)MAG_I2C_ADDR);
            return wrap(len, LABEL, 14u, "Check power, SDA/SCL wiring and pull-ups. Re-probing 5 times a second.");
        case MAG_CAUSE_WRONG_ID: {
            unsigned id = mag_fault_id();
            len = put(len, "MAGNETOMETER  FAULT - wrong product ID: read 0x%02X, expected 0x%02X\r\n", id,
                      (unsigned)MMC_PRODUCT_ID);
            return wrap(len, LABEL, 14u,
                        (id == 0x00u || id == 0xFFu)
                            ? "Likely a bus or wiring problem. Re-probing 5 times a second."
                            : "A different device at this address? Re-probing 5 times a second.");
        }
        case MAG_CAUSE_BUS_ERROR:
            len = put(len, "MAGNETOMETER  FAULT - I2C bus error\r\n");
            return wrap(len, LABEL, 14u, "Line held low, noise, or no pull-ups. Re-probing 5 times a second.");
        case MAG_CAUSE_OTP_NOT_LOADED:
            len = put(len, "MAGNETOMETER  FAULT - factory calibration (OTP) not loaded\r\n");
            return wrap(len, LABEL, 14u, "OTP_Rd_Done was clear after the software reset. Re-probing.");
        case MAG_CAUSE_CONFIG_FAILED:
            return put(len, "MAGNETOMETER  FAULT - configuration write failed; re-probing\r\n");
        case MAG_CAUSE_NOT_READY:
            len = put(len, "MAGNETOMETER  FAULT - samples kept arriving unfinished (not ready)\r\n");
            return wrap(len, LABEL, 14u, "Reconfiguring the sensor (it may have lost power).");
        case MAG_CAUSE_NONE:
        default:
            return put(len, "MAGNETOMETER  FAULT - re-probing 5 times a second\r\n");
        }
    }
#if APP_MODE != APP_MODE_USER
    case H_MAG_SAMPLES: {
        char it[4][40];
        snprintf(it[0], sizeof it[0], "valid %s", cell(a, sizeof a, g_app_mag_valid, s_d_valid));
        snprintf(it[1], sizeof it[1], "invalid %" PRIu32, g_app_mag_invalid);
        snprintf(it[2], sizeof it[2], "gaps %s", cell(b, sizeof b, g_app_mag_gaps, s_d_gaps));
        snprintf(it[3], sizeof it[3], "stale %" PRIu32, g_app_mag_stale);
        const char *const p[] = {it[0], it[1], it[2], it[3]};
        return items(0, "  Samples     ", p, 4u);
    }
#endif
    case H_MAG_BUS: {
        char it[4][44];
        snprintf(it[0], sizeof it[0], "I2C errors %s",
                 cell(a, sizeof a, s_total[DIAG_MAG_I2C_ERROR], s_delta[DIAG_MAG_I2C_ERROR]));
        snprintf(it[1], sizeof it[1], "timeouts %s", cell(b, sizeof b, s_total[DIAG_MAG_TIMEOUT], s_delta[DIAG_MAG_TIMEOUT]));
        snprintf(it[2], sizeof it[2], "missed ticks %s",
                 cell(c, sizeof c, s_total[DIAG_MAG_MISSED_TICK], s_delta[DIAG_MAG_MISSED_TICK]));
        snprintf(it[3], sizeof it[3], "bus clears %" PRIu32, s_total[DIAG_MAG_BUS_CLEAR]);
        const char *const p[] = {it[0], it[1], it[2], it[3]};
        return items(0, "  Bus         ", p, 4u);
    }
    case H_MAG_HEALTH: {
        char it[5][44];
        snprintf(it[0], sizeof it[0], "no answer %s", cell(a, sizeof a, s_total[DIAG_MAG_NO_ACK], s_delta[DIAG_MAG_NO_ACK]));
        snprintf(it[1], sizeof it[1], "wrong ID %s", cell(b, sizeof b, s_total[DIAG_MAG_WRONG_ID], s_delta[DIAG_MAG_WRONG_ID]));
        snprintf(it[2], sizeof it[2], "not ready %s",
                 cell(c, sizeof c, s_total[DIAG_MAG_NOT_READY], s_delta[DIAG_MAG_NOT_READY]));
        snprintf(it[3], sizeof it[3], "run-time faults %s",
                 cell(d, sizeof d, s_total[DIAG_MAG_RUNTIME_FAULT], s_delta[DIAG_MAG_RUNTIME_FAULT]));
        snprintf(it[4], sizeof it[4], "recoveries %" PRIu32, s_total[DIAG_MAG_RECOVERED]);
        const char *const p[] = {it[0], it[1], it[2], it[3], it[4]};
        return items(0, "  Health      ", p, 5u);
    }
    case H_MAG_FIELD:
        return snprintf(s_buf, n, "  Field       x %7" PRId32 "   y %7" PRId32 "   z %7" PRId32 "   (16384 counts/G)\r\n",
                        s_xyz[0], s_xyz[1], s_xyz[2]);
    case H_MAG_CAL:
        if (g_mag_sr_mode == MAG_SR_PULSE) {
            snprintf(a, sizeof a, "SET every %" PRIu32, g_mag_sr_interval);
        } else {
            snprintf(a, sizeof a, "single cycle");
        }
        return snprintf(s_buf, n,
                        "  Offset      x %7" PRId32 "   y %7" PRId32 "   z %7" PRId32 "   %s (%" PRIu32 ")\r\n" LABEL
                        "SET/RESET %s   saturated %" PRIu32 "\r\n",
                        s_mag.offset[0], s_mag.offset[1], s_mag.offset[2],
                        s_mag.offset_valid ? "calibrated" : "not calibrated", s_total[DIAG_MAG_OFFSET_CAL], a,
                        s_total[DIAG_MAG_SATURATED]);
    case H_WIRE: { /* Rev 2.18: start-up wiring check */
        const wirecheck_t *w = wirecheck_result();
        char f[2][32];
        for (unsigned s = 0; s < 2u; s++) {
            unsigned k = w->n_found[s] < WIRE_MAX_FOUND ? w->n_found[s] : WIRE_MAX_FOUND;
            int m = snprintf(f[s], sizeof f[s], k ? "" : "none");
            for (unsigned j = 0; j < k && m >= 0 && (unsigned)m < sizeof f[s]; j++) {
                m += snprintf(f[s] + m, sizeof f[s] - (unsigned)m, "%s0x%02X", j ? " " : "", (unsigned)w->found[s][j]);
            }
        }
        int len = put(0, "  Wiring      SDA %s, SCL %s, %s (boot check)\r\n",
                      w->held_low[WIRE_SDA] ? "LOW" : (w->ext_pullup[WIRE_SDA] ? "pulled up" : "no pull-up"),
                      w->held_low[WIRE_SCL] ? "LOW" : (w->ext_pullup[WIRE_SCL] ? "pulled up" : "no pull-up"),
                      w->shorted ? "SDA-SCL SHORTED" : "no short");
        char it[2][64];
        snprintf(it[0], sizeof it[0], "answers: %s", f[0]);
        snprintf(it[1], sizeof it[1], "with SDA/SCL swapped: %s", f[1]);
        const char *const p[] = {it[0], it[1]};
        return items(len, LABEL, p, 2u);
    }
    case H_WIRE_VERDICT:
        return wrap(0, LABEL "-> ", 17u, wirecheck_verdict());
    case H_BOOT: { /* Rev 2.19 */
        const mag_boot_t *bt = mag_boot();
        static const char *const k_cause[] = {"OK", "no answer", "wrong ID", "bus error", "OTP not loaded",
                                              "config failed", "not ready"};
        return snprintf(s_buf, n,
                        "  Start-up    ID 0x%02X, status 0x%02X (OTP %s), result %s\r\n" LABEL
                        "calibration %s (status 0x%02X)\r\n",
                        (unsigned)bt->id, (unsigned)bt->status,
                        (bt->status & MMC_STATUS_OTP_RD_DONE) ? "loaded" : "NOT loaded",
                        k_cause[bt->cause < 7u ? bt->cause : 0u], bt->cal_ok ? "OK" : "failed",
                        (unsigned)bt->cal_status);
    }
    case H_BOOT_TIME: {
        const mag_boot_t *bt = mag_boot();
        return snprintf(s_buf, n,
                        "  Meas. time  800 Hz %s   100 Hz %s\r\n" LABEL "800 Hz set before a repeated START %s\r\n" LABEL
                        "Status read alone / after data: 0x%02X / 0x%02X\r\n",
                        ms_text(a, sizeof a, bt->t_fast_us), ms_text(b, sizeof b, bt->t_slow_us),
                        ms_text(c, sizeof c, bt->t_rs_us), (unsigned)bt->direct_status,
                        (unsigned)bt->burst_status);
    }
    case H_BOOT_VERDICT:
        return wrap(0, LABEL "-> ", 17u, boot_verdict(mag_boot()));
    case H_TIM_HEAD:
        return snprintf(s_buf, n, "TIMING (worst case since boot; API time excludes interrupts)\r\n");
    case H_TIM_ISR: {
        unsigned wi = worst(k_isr_slot, 5u);
        unsigned wa = worst(k_api_slot, 3u);
        return snprintf(s_buf, n,
                        "  Handlers    %6" PRIu32 " ns  (limit %u)  worst: %s\r\n"
                        "  Critical    %6" PRIu32 " ns  (limit %u)\r\n"
                        "  API calls   %6" PRIu32 " ns  (limit %u)  worst: %s\r\n",
                        ns(k_isr_slot[wi]), ISR_LIMIT_NS, k_isr_name[wi], ns(DIAG_T_CRITICAL), CRIT_LIMIT_NS,
                        ns(k_api_slot[wa]), API_LIMIT_NS, k_api_name[wa]);
    }
    case H_TIM_LOOP:
#if APP_MODE == APP_MODE_USER
    {
        char it[3][48];
        snprintf(it[0], sizeof it[0], "last %" PRIu32 " us", us(g_prof[PROF_EPOCH].last));
        snprintf(it[1], sizeof it[1], "avg %" PRIu32 " us", us(prof_avg(PROF_EPOCH)));
        snprintf(it[2], sizeof it[2], "max %" PRIu32 " us (limit 1000)", us(g_prof[PROF_EPOCH].max));
        const char *const p[] = {it[0], it[1], it[2]};
        int len = items(0, "  Epoch work  ", p, 3u);
        char jt[3][48];
        snprintf(jt[0], sizeof jt[0], "start latency max %" PRIu32 " us", g_epoch_start_max_us);
        snprintf(jt[1], sizeof jt[1], "overruns %s",
                 cell(a, sizeof a, s_total[DIAG_EPOCH_OVERRUN], s_delta[DIAG_EPOCH_OVERRUN]));
        snprintf(jt[2], sizeof jt[2], "tick overruns %s",
                 cell(b, sizeof b, s_total[DIAG_TICK_OVERRUN], s_delta[DIAG_TICK_OVERRUN]));
        const char *const q[] = {jt[0], jt[1], jt[2]};
        return items(len, LABEL, q, 3u);
    }
#else
    {
        char it[2][48];
        snprintf(it[0], sizeof it[0], "%6" PRIu32 " us  (limit 1000)", loop_or_epoch_max_us());
        snprintf(it[1], sizeof it[1], "tick overruns %s",
                 cell(a, sizeof a, s_total[DIAG_TICK_OVERRUN], s_delta[DIAG_TICK_OVERRUN]));
        const char *const p[] = {it[0], it[1]};
        return items(0, "  Main loop   ", p, 2u);
    }
#endif
#if APP_MODE == APP_MODE_USER
    case H_TIM_PROF: {
        int len = put(0, "  Sections    last/max/avg us\r\n");
        for (unsigned i = 1; i < PROF_COUNT; i++) {
            len = put(len, LABEL "%-16s %" PRIu32 " / %" PRIu32 " / %" PRIu32 "\r\n", g_prof_name[i],
                      us(g_prof[i].last), us(g_prof[i].max), us(prof_avg((prof_id_t)i)));
        }
        return len;
    }
#endif
    case H_ATTENTION: {
        unsigned count = 0;
        int len = 0;
        if (!g_app_baud_ok) {
            len = attn_item(len, &count, "UART baud rate out of tolerance");
        }
        if (crash_last()->kind != CRASH_KIND_NONE) {
            len = attn_item(len, &count, crash_last()->kind == CRASH_KIND_FAULT
                                             ? "the board reset after a HARD FAULT (see Last reset)"
                                             : "the board reset after a HANG: watchdog (see Last reset)");
        }
        static const char *const ename[] = {"RX overflows", "TX rejects", "framing errors", "breaks",
                                            "HW overruns"};
        for (unsigned k = 0; k < 2u; k++) {
            for (unsigned e = 0; e < 5u; e++) {
                uint32_t dv = s_delta[DIAG_L0_RX_OVERFLOW + e + DIAG_LINK_STRIDE * k];
                if (dv != 0u) {
                    snprintf(t, sizeof t, "UART%u %s (+%" PRIu32 ")", k, ename[e], dv);
                    len = attn_item(len, &count, t);
                }
            }
        }
#if APP_MODE != APP_MODE_USER
        for (unsigned k = 0; k < 2u; k++) {
            if (s_d_pattern[k] != 0u) {
                snprintf(t, sizeof t, "UART%u pattern errors (+%" PRIu32 ")", k, s_d_pattern[k]);
                len = attn_item(len, &count, t);
            }
        }
        if (s_d_gaps != 0u) {
            snprintf(t, sizeof t, "magnetometer sample gaps (+%" PRIu32 ")", s_d_gaps);
            len = attn_item(len, &count, t);
        }
#endif
        if (mag_fault()) {
            static const char *const k_attn[] = {
                [MAG_CAUSE_NONE] = "magnetometer fault",
                [MAG_CAUSE_NO_ACK] = "magnetometer not answering",
                [MAG_CAUSE_WRONG_ID] = "magnetometer wrong product ID",
                [MAG_CAUSE_BUS_ERROR] = "magnetometer I2C bus error",
                [MAG_CAUSE_OTP_NOT_LOADED] = "magnetometer OTP not loaded",
                [MAG_CAUSE_CONFIG_FAILED] = "magnetometer configuration failed",
                [MAG_CAUSE_NOT_READY] = "magnetometer run-time fault",
            };
            unsigned cause = (unsigned)mag_fault_cause();
            len = attn_item(len, &count, k_attn[cause < sizeof k_attn / sizeof k_attn[0] ? cause : 0u]);
        } else {
            if (s_delta[DIAG_MAG_I2C_ERROR] | s_delta[DIAG_MAG_TIMEOUT] | s_delta[DIAG_MAG_MISSED_TICK]) {
                len = attn_item(len, &count, "magnetometer bus errors, timeouts or missed ticks");
            }
            if (s_delta[DIAG_MAG_NOT_READY]) {
                len = attn_item(len, &count, "magnetometer samples read before they finished (not ready)");
            }
        }
        unsigned wi = worst(k_isr_slot, 5u);
        if (ns(k_isr_slot[wi]) > ISR_LIMIT_NS) {
            snprintf(t, sizeof t, "%s handler %" PRIu32 " ns, over the %u ns limit", k_isr_name[wi],
                     ns(k_isr_slot[wi]), ISR_LIMIT_NS);
            len = attn_item(len, &count, t);
        }
        if (ns(DIAG_T_CRITICAL) > CRIT_LIMIT_NS) {
            snprintf(t, sizeof t, "critical section %" PRIu32 " ns, over the %u ns limit", ns(DIAG_T_CRITICAL),
                     CRIT_LIMIT_NS);
            len = attn_item(len, &count, t);
        }
        for (unsigned i = 0; i < 3u; i++) {
            if (ns(k_api_slot[i]) > API_LIMIT_NS) {
                snprintf(t, sizeof t, "%s %" PRIu32 " ns, over the %u ns API limit", k_api_name[i],
                         ns(k_api_slot[i]), API_LIMIT_NS);
                len = attn_item(len, &count, t);
            }
        }
        if (loop_or_epoch_max_us() > 999u) {
            snprintf(t, sizeof t, "%s %" PRIu32 " us, over 1 ms",
                     APP_MODE == APP_MODE_USER ? "epoch work" : "main loop pass", loop_or_epoch_max_us());
            len = attn_item(len, &count, t);
        }
        if (s_delta[DIAG_TICK_OVERRUN] | s_delta[DIAG_EPOCH_OVERRUN]) {
            snprintf(t, sizeof t, "overruns: %" PRIu32 " tick, %" PRIu32 " epoch", s_delta[DIAG_TICK_OVERRUN],
                     s_delta[DIAG_EPOCH_OVERRUN]);
            len = attn_item(len, &count, t);
        }
        if (count == 0u) {
            return snprintf(s_buf, n, "\r\nSTATUS: all checks OK\r\n");
        }
        return len;
    }
    default:
        return 0;
    }
}

static unsigned line_count(void) {
    return s_human ? (unsigned)H_COUNT : N_LINES;
}

/* Rev 2.20: '~' from the host reboots into the USB bootloader (BOOTSEL)
 * for reflashing, after a short delay so the notice reaches the terminal. */
#define BOOTSEL_DELAY_US 100000u
static uint64_t s_bootsel_at; /* 0 = not requested */

void status_poll(void) {
    crash_feed(); /* D-9: watchdog, heartbeat LED */
    if (s_bootsel_at != 0u) {
        if ((int64_t)(time_us_64() - s_bootsel_at) >= 0) {
            reset_usb_boot(0u, 0u); /* does not return */
        }
        return;
    }

    /* From the host: 'm' machine format, 'h' readable, '~' bootloader. */
    crash_crumb(CRUMB_STATUS_INPUT);
    int ch = getchar_timeout_us(0);
    crash_crumb(CRUMB_IDLE);
    if (ch == 'm' || ch == 'M') {
        s_want_human = false;
    } else if (ch == 'h' || ch == 'H') {
        s_want_human = true;
    } else if (ch == '~') {
        static const char k_msg[] = "\r\n\r\n*** '~' received: rebooting into the USB bootloader. The COM port "
                                    "will close; the board appears as the RP2350 drive. ***\r\n";
        if (tud_cdc_write_available() >= sizeof k_msg - 1u) {
            stdio_put_string(k_msg, (int)(sizeof k_msg - 1u), false, false);
        }
        crash_disarm(); /* D-9: an intentional reboot, not a hang */
        s_bootsel_at = time_us_64() + BOOTSEL_DELAY_US;
        return;
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
        crash_crumb(CRUMB_STATUS_FORMAT);
        s_len = s_human ? format_human(s_line) : format_line(s_line);
        crash_crumb(CRUMB_IDLE);
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
    crash_crumb(CRUMB_STATUS_SEND);
    stdio_put_string(s_buf + s_off, n, false, false);
    crash_crumb(CRUMB_IDLE);
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
