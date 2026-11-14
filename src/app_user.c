/*
 * User application (APP_MODE=user, the production build). Rev 2.14.
 *
 * app_poll() runs once per 1 ms epoch, started by main.c just after each
 * tick (EX-10). At that point:
 *   - every UART byte received during the previous epoch is in the RX ring
 *     (DMA, U-3..U-5);
 *   - the newest magnetometer sample was published during the previous
 *     epoch (~375-450 us after its tick, M-8) and is complete.
 * The whole of app_poll() must finish in < 1 ms (EX-8); main.c measures it
 * (PROF_EPOCH) and counts epochs that run late (DIAG_EPOCH_OVERRUN).
 *
 * The handlers below are stubs to be filled in: for now each UART's data is
 * echoed onto the other UART.
 */
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

/* ---- Magnetometer: most recent sample ------------------------------- */
static void mag_handler(void) {
    static uint32_t last_seq;
    mag_reading_t r;
    if (mag_read_latest(&r) && r.s.valid && !r.stale) {
        if (r.s.seq != last_seq + 1u) {
            /* gap: samples were missed (abort / fault) */
        }
        last_seq = r.s.seq;
        int32_t xyz[3];
        mag_field(&r, xyz); /* signed counts, 16384/G, offset removed */
        (void)xyz;
        /* TODO: use xyz[], r.s.timestamp_us */
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
