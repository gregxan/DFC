/*
 * User application (APP_MODE=user, the production build). Rev 2.14.
 *
 * app_poll() runs once per 1 ms epoch, started by main.c just after each
 * tick (EX-10). At that point:
 *   - every UART byte received during the previous epoch is in the RX ring
 *     (DMA, U-3..U-5);
 *   - the newest magnetometer sample was published during the previous
 *     epoch (~445-515 us after its tick, M-8) and is complete.
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
#include "prof.h"

/* Static, so they use no stack (B-23) and are allocated at build time. */
static uint8_t s_link0_buf[512];
static uint8_t s_link1_buf[512];

void app_init(void) {}

/* ---- Magnetometer state, shared by all handlers -------------------- */
/* Filled by mag_handler(), which runs first in app_poll(), so the link
 * handlers always see this epoch's values. File scope: visible to every
 * handler in this file, not exported. */
static mag_reading_t s_mag;  /* latest reading this epoch (may be invalid or stale) */
static int32_t s_mag_xyz[3]; /* last valid field: signed counts, 16384/G, offset removed */
static int16_t s_mag_xyz16[3]; /* same, 16-bit scale (4096/G), for compact messages */
static bool s_mag_new;       /* true if this epoch brought a new valid sample */

/* ---- Magnetometer: most recent sample ------------------------------- */
static void mag_handler(void) {
    static bool have_last;
    static uint32_t last_seq;

    s_mag_new = false;
    if (mag_read_latest(&s_mag) && s_mag.s.valid && !s_mag.stale) {
        if (have_last && s_mag.s.seq == last_seq) {
            return; /* same sample as last epoch: nothing new */
        }
        if (have_last && s_mag.s.seq != last_seq + 1u) {
            /* gap: samples were missed (abort / fault) */
        }
        have_last = true;
        last_seq = s_mag.s.seq;
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
        /* TODO: parse / process LINK_0 data here */
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
