/*
 * User application (APP_MODE=user, the production build). Rev 2.14.
 *
 * app_poll() runs once per 1 ms epoch, started by main.c just after each
 * tick (EX-10). At that point:
 *   - every UART byte received during the previous epoch is in the RX ring
 *     (DMA, U-3..U-5);
 *   - the newest magnetometer sample was published during the previous
 *     epoch (~445-515 us after its tick, M-8) and is complete. At 1 kHz
 *     every epoch brings a new sample. At a lower rate (M-3b,
 *     mag_set_rate_hz(): 500/250/200/100/50 Hz) only every 2nd/4th/5th/
 *     10th/20th epoch does, with the same timing, and s_mag_new is false in
 *     between. The epochs themselves still run every 1 ms.
 * The whole of app_poll() must finish in < 1 ms (EX-8); main.c measures it
 * (PROF_EPOCH) and counts epochs that run late (DIAG_EPOCH_OVERRUN).
 *
 * The handlers below are stubs to be filled in: for now each UART's data is
 * echoed onto the other UART.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app.h"
#include "link.h"
#include "mag.h"
#include "nvstore.h"
#include "prof.h"
#include "updater.h"

/* Static, so they use no stack (B-23) and are allocated at build time. */
static uint8_t s_link0_buf[512];
static uint8_t s_link1_buf[512];

/* Firmware update entry (Rev 2.25): 20 consecutive '!' on either UART
 * reboots into the updater (updater.h, updater_trigger_scan()). Without an
 * entry like this a deployed unit cannot be updated over its UARTs. Replace
 * it with your own protocol's "enter updater" message if you prefer, calling
 * updater_request(link, UPDATER_REQUEST_KEY) when it arrives. */
static updater_trigger_t s_upd_trigger[LINK_COUNT];

static void check_update_request(link_id_t id, const uint8_t *p, size_t n) {
    if (updater_trigger_scan(&s_upd_trigger[id], p, n)) {
        /* Reboots into the updater (UART0, UART1, and USB in diag builds);
         * does not return. */
        (void)updater_request(id == LINK_0 ? UPDATER_LINK_UART0 : UPDATER_LINK_UART1, UPDATER_REQUEST_KEY);
    }
}

void app_init(void) {
    /* Non-volatile settings (NV-x, Rev 2.28): load them, then apply the
     * saved start-up magnetometer rate (M-3b). app_init() runs before the
     * 1 ms tick starts, so the first sample already uses it. With nothing
     * saved the rate stays at 1000 Hz.
     *
     * To change and keep a rate from your client protocol (main loop only;
     * the flash write stalls the loop, ~1 ms, sometimes up to ~400 ms, and
     * is skipped if the value is unchanged):
     *     if (mag_set_rate_hz(hz)) {
     *         (void)settings_save_mag_rate(hz);
     *     }
     * Supported: 1000, 500, 250, 200, 100, 50 Hz. */
    (void)settings_load();
    uint32_t hz = settings_mag_rate_hz();
    if (hz != 0u) {
        (void)mag_set_rate_hz(hz);
    }
}

/* ---- Magnetometer state, shared by all handlers -------------------- */
/* Filled by mag_handler(), which runs first in app_poll(), so the link
 * handlers always see this epoch's values. File scope: visible to every
 * handler in this file, not exported. */
static mag_reading_t s_mag;  /* latest reading this epoch (may be invalid or stale) */
static int32_t s_mag_xyz[3]; /* last valid field: signed counts, 16384/G, offset removed */
static int16_t s_mag_xyz16[3]; /* same, 16-bit scale (4096/G), for compact messages */
static bool s_mag_new;       /* true if this epoch brought a new valid sample */
static bool s_mag_arrived;   /* true if this epoch brought a new sample, valid or not */

/* ---- Magnetometer: most recent sample ------------------------------- */
static void mag_handler(void) {
    static bool have_last;
    static uint32_t last_seq;
    static uint32_t last_period; /* M-3b: ticks to the next sample */
    static uint32_t last_count;  /* M-13a: mag_sample_count() last epoch */

    s_mag_new = false;
    s_mag_arrived = false;
    if (!mag_read_latest(&s_mag) || s_mag.count == last_count) {
        return; /* no sample yet, or none new this epoch (lower rates) */
    }
    last_count = s_mag.count;
    s_mag_arrived = true;
    if (s_mag.s.valid && !s_mag.stale) {
        if (have_last && s_mag.s.seq != last_seq + last_period) {
            /* gap: samples were missed (abort / fault) */
        }
        have_last = true;
        last_seq = s_mag.s.seq;
        last_period = s_mag.s.period_ticks;
        mag_field(&s_mag, s_mag_xyz);
        mag_field16(&s_mag, s_mag_xyz16);
        s_mag_new = true;
        /* TODO: use s_mag_xyz[] / s_mag_xyz16[], s_mag.s.timestamp_us */
    }
}

/* ---- LINK_0 (UART0) receive handler: echo to LINK_1 (UART1) ---------- */
static void link0_handler(void) {
    size_t avail = link_rx_available(LINK_0); /* bytes waiting now */
    while (avail != 0u) {
        size_t n = avail < sizeof s_link0_buf ? avail : sizeof s_link0_buf;
        size_t room = link_tx_free(LINK_1);
        if (n > room) {
            n = room; /* never trigger a TX reject */
        }
        if (n == 0u) {
            break; /* LINK_1 full: the rest waits for the next epoch */
        }
        n = link_read(LINK_0, s_link0_buf, n); /* also handles RX overflow */
        if (n == 0u) {
            break;
        }
        check_update_request(LINK_0, s_link0_buf, n); /* firmware update entry */
        /* TODO: parse / process LINK_0 data here. */
        (void)link_write(LINK_1, s_link0_buf, n);
        avail -= n;
    }

    /* TODO: once per epoch, send the magnetometer values if this epoch
     * brought a new sample (s_mag / s_mag_xyz are filled by mag_handler()):
     *   if (s_mag_new) {
     *       encode s_mag_xyz16[] (or s_mag_xyz[]) and s_mag.s.timestamp_us
     *       into a message;
     *       link_write(<link>, msg, len);
     *   } */
}

/* ---- LINK_1 (UART1) receive handler: echo to LINK_0 (UART0) ---------- */
static void link1_handler(void) {
    size_t avail = link_rx_available(LINK_1); /* bytes waiting now */
    while (avail != 0u) {
        size_t n = avail < sizeof s_link1_buf ? avail : sizeof s_link1_buf;
        size_t room = link_tx_free(LINK_0);
        if (n > room) {
            n = room; /* never trigger a TX reject */
        }
        if (n == 0u) {
            break; /* LINK_0 full: the rest waits for the next epoch */
        }
        n = link_read(LINK_1, s_link1_buf, n); /* also handles RX overflow */
        if (n == 0u) {
            break;
        }
        check_update_request(LINK_1, s_link1_buf, n); /* firmware update entry */
        /* TODO: parse / process LINK_1 data here */
        (void)link_write(LINK_0, s_link1_buf, n);
        avail -= n;
    }
}

/* ---- 1 ms epoch ------------------------------------------------------ */
void app_poll(void) {
    PROF_BEGIN(PROF_APP_MAG);
    mag_handler();
    PROF_END(PROF_APP_MAG);

    PROF_BEGIN(PROF_APP_LINK0);
    link0_handler();
    PROF_END(PROF_APP_LINK0);

    PROF_BEGIN(PROF_APP_LINK1);
    link1_handler();
    PROF_END(PROF_APP_LINK1);
}
