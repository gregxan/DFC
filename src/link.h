/*
 * Two DMA-driven UART links at 921,600 baud (section 4).
 *
 * RX: a DMA channel in endless mode writes every received byte into a RAM
 *     ring (U-3, U-4). No interrupt is in the data path (U-5); link_read()
 *     reads the DMA write address directly.
 * TX: link_write() copies into a TX ring and, if the DMA is idle, starts it
 *     (U-10..U-12). The TX DMA completion interrupt starts the next transfer
 *     while data remains (U-13).
 *
 * All functions except link_init() are non-blocking (EX-2) and may only be
 * called from the main loop (EX-1).
 */
#ifndef LINK_H
#define LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"

typedef enum { LINK_0 = 0, LINK_1 = 1, LINK_COUNT = 2 } link_id_t;

#define LINK_RX_RING_SIZE (1u << LINK_RX_RING_BITS)
#define LINK_TX_RING_SIZE (1u << LINK_TX_RING_BITS)

/* Configure both UARTs, their pins and DMA. Returns false if a UART's
 * actual baud rate is outside LINK_BAUD_TOLERANCE_PPM (U-2); the links are
 * still started. */
bool link_init(void);

/* U-6: copy up to max received bytes into dst. Returns the number copied,
 * possibly 0. On overflow (U-8a, U-9) all unread data is discarded, the RX
 * overflow counter is incremented once, and 0 is returned. */
size_t link_read(link_id_t id, uint8_t *dst, size_t max);

/* Bytes currently waiting to be read (no overflow check). */
size_t link_rx_available(link_id_t id);

/* U-10, U-11: queue len bytes for transmission. Returns false, queues
 * nothing and increments the TX reject counter if the whole message does
 * not fit. */
bool link_write(link_id_t id, const uint8_t *src, size_t len);

/* Free space in the TX ring. */
size_t link_tx_free(link_id_t id);

/* Bytes received (written into the RX ring by DMA) and transmitted (handed
 * to the UART TX FIFO) since start-up. Both wrap at 2^32 (~13 h at full
 * rate); take differences. Main loop only. */
uint32_t link_rx_bytes(link_id_t id);
uint32_t link_tx_bytes(link_id_t id);

/* Called from the 1 ms tick ISR only: U-8 bookkeeping, U-15a re-enable. */
void link_tick_isr(void);

#endif /* LINK_H */
