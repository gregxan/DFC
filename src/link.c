#include "link.h"

#include <string.h>

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/uart.h"

#include "diag.h"

#define RX_SIZE LINK_RX_RING_SIZE
#define RX_MASK (RX_SIZE - 1u)
#define TX_SIZE LINK_TX_RING_SIZE
#define TX_MASK (TX_SIZE - 1u)

_Static_assert(LINK_RX_RING_BITS >= 8 && LINK_RX_RING_BITS <= 15,
               "RX ring must be 256 B .. 32 KB (DMA ring mode limit, and > 1 ms of data for U-8)");
_Static_assert(LINK_TX_RING_BITS >= 6 && LINK_TX_RING_BITS <= 15,
               "TX ring must be 64 B .. 32 KB (DMA ring mode limit)");

#define UART_ERR_BITS (UART_UARTIMSC_FEIM_BITS | UART_UARTIMSC_BEIM_BITS | UART_UARTIMSC_OEIM_BITS)

/* U-7: rings aligned to their size, as DMA ring mode requires. */
static uint8_t s_rx_ring0[RX_SIZE] __attribute__((aligned(RX_SIZE)));
static uint8_t s_rx_ring1[RX_SIZE] __attribute__((aligned(RX_SIZE)));
static uint8_t s_tx_ring0[TX_SIZE] __attribute__((aligned(TX_SIZE)));
static uint8_t s_tx_ring1[TX_SIZE] __attribute__((aligned(TX_SIZE)));

typedef struct {
    uart_inst_t *uart;
    uint8_t *rx_ring;
    uint8_t *tx_ring;
    uint rx_ch;
    uint tx_ch;
    uint tx_pin;
    uint rx_pin;

    /* RX. Byte k of the stream (k counted from 0, modulo 2^32) lives at
     * rx_ring[k & RX_MASK]. */
    uint32_t rx_read_total;              /* main loop only               */
    volatile uint32_t rx_written_total;  /* tick ISR only (U-8)           */
    volatile uint32_t rx_sampled_pos;    /* tick ISR only                 */

    /* TX. Byte k of the stream lives at tx_ring[k & TX_MASK]. */
    volatile uint32_t tx_head;     /* bytes queued; main loop only        */
    volatile uint32_t tx_tail;     /* bytes fully handed to the UART FIFO */
    volatile uint32_t tx_inflight; /* length of the running DMA transfer  */
    volatile bool tx_busy;
} link_t;

static link_t s_links[LINK_COUNT];

static inline uart_hw_t *hw_of(const link_t *l) {
    return uart_get_hw(l->uart);
}

static inline diag_ctr_t ctr(link_id_t id, diag_ctr_t base) {
    return (diag_ctr_t)((unsigned)base + DIAG_LINK_STRIDE * (unsigned)id);
}

/* ---------------------------------------------------------------- RX --- */

/* Ring index of the next byte the RX DMA will write. */
static inline uint32_t __not_in_flash_func(rx_dma_pos)(const link_t *l) {
    return (dma_hw->ch[l->rx_ch].write_addr - (uint32_t)(uintptr_t)l->rx_ring) & RX_MASK;
}

/* Total bytes written by DMA so far: the tick's 32-bit count (U-8) plus the
 * advance since the tick sampled the position. The tick runs every 1 ms,
 * during which at most ~93 bytes arrive, so the advance is < RX_SIZE. */
static inline uint32_t __not_in_flash_func(rx_total_now)(const link_t *l) {
    cs_t cs = cs_enter();
    uint32_t total = l->rx_written_total;
    uint32_t sampled = l->rx_sampled_pos;
    uint32_t pos = rx_dma_pos(l);
    cs_exit(cs);
    return total + ((pos - sampled) & RX_MASK);
}

/* U-9: discard all unread data and resynchronise to the DMA position. */
static void __not_in_flash_func(rx_overflow)(link_t *l, link_id_t id) {
    l->rx_read_total = rx_total_now(l);
    diag_inc(ctr(id, DIAG_L0_RX_OVERFLOW));
    diag_hwm(&g_diag_rx_hwm[id], RX_SIZE);
}

size_t __not_in_flash_func(link_rx_available)(link_id_t id) {
    const link_t *l = &s_links[id];
    uint32_t unread = rx_total_now(l) - l->rx_read_total;
    return unread > RX_SIZE ? RX_SIZE : unread;
}

size_t __not_in_flash_func(link_read)(link_id_t id, uint8_t *dst, size_t max) {
    uint32_t t0 = diag_cycles();
    link_t *l = &s_links[id];

    /* U-8a: modulo-2^32 comparison of bytes written vs bytes read. */
    uint32_t unread = rx_total_now(l) - l->rx_read_total;
    if (unread > RX_SIZE) {
        rx_overflow(l, id);
        diag_max(DIAG_T_API_READ, diag_cycles() - t0);
        return 0;
    }
    diag_hwm(&g_diag_rx_hwm[id], unread);
    if (unread == 0u || max == 0u) {
        diag_max(DIAG_T_API_READ, diag_cycles() - t0);
        return 0;
    }

    uint32_t n = unread < max ? unread : (uint32_t)max;
    uint32_t idx = l->rx_read_total & RX_MASK;
    uint32_t first = RX_SIZE - idx;
    if (first > n) {
        first = n;
    }
    uint32_t tc = diag_cycles();
    memcpy(dst, &l->rx_ring[idx], first);
    if (n > first) {
        memcpy(dst + first, &l->rx_ring[0], n - first);
    }
    uint32_t copy = diag_cycles() - tc;

    /* The DMA may have lapped the reader while copying. The oldest copied
     * byte (stream index rx_read_total) is overwritten once more than
     * RX_SIZE bytes past it have been written. */
    if (rx_total_now(l) - l->rx_read_total > RX_SIZE) {
        rx_overflow(l, id);
        diag_max(DIAG_T_API_READ, diag_cycles() - t0 - copy);
        return 0;
    }
    l->rx_read_total += n;
    diag_max(DIAG_T_API_READ, diag_cycles() - t0 - copy);
    return n;
}

/* ---------------------------------------------------------------- TX --- */

/* Start a transfer of everything queued. Caller is the TX completion ISR,
 * or the main loop with interrupts disabled. */
static inline void __not_in_flash_func(tx_start)(link_t *l) {
    uint32_t tail = l->tx_tail;
    uint32_t n = l->tx_head - tail;
    if (n == 0u) {
        l->tx_busy = false;
        return;
    }
    l->tx_inflight = n;
    l->tx_busy = true;
    dma_channel_hw_t *ch = &dma_hw->ch[l->tx_ch];
    /* Read-ring wrapping lets one transfer cross the end of the ring. */
    ch->read_addr = (uint32_t)(uintptr_t)&l->tx_ring[tail & TX_MASK];
    ch->al1_transfer_count_trig = n;
}

size_t __not_in_flash_func(link_tx_free)(link_id_t id) {
    const link_t *l = &s_links[id];
    return TX_SIZE - (l->tx_head - l->tx_tail);
}

bool __not_in_flash_func(link_write)(link_id_t id, const uint8_t *src, size_t len) {
    uint32_t t0 = diag_cycles();
    link_t *l = &s_links[id];
    if (len == 0u) {
        return true;
    }

    uint32_t head = l->tx_head;
    /* tx_tail only grows, so a stale read underestimates free space. */
    uint32_t used = head - l->tx_tail;
    if (len > TX_SIZE - used) {
        diag_inc(ctr(id, DIAG_L0_TX_REJECT)); /* U-11 */
        diag_max(DIAG_T_API_WRITE, diag_cycles() - t0);
        return false;
    }

    uint32_t n = (uint32_t)len;
    uint32_t idx = head & TX_MASK;
    uint32_t first = TX_SIZE - idx;
    if (first > n) {
        first = n;
    }
    uint32_t tc = diag_cycles();
    memcpy(&l->tx_ring[idx], src, first);
    if (n > first) {
        memcpy(&l->tx_ring[0], src + first, n - first);
    }
    uint32_t copy = diag_cycles() - tc;

    __dmb(); /* data visible before the new head */
    l->tx_head = head + n;
    diag_hwm(&g_diag_tx_hwm[id], used + n);

    /* U-12. If the completion ISR ran after our head update it has already
     * picked up the data; if it found nothing it cleared tx_busy, and we
     * start the transfer here. */
    cs_t cs = cs_enter();
    if (!l->tx_busy) {
        tx_start(l);
    }
    cs_exit(cs);

    diag_max(DIAG_T_API_WRITE, diag_cycles() - t0 - copy);
    return true;
}

/* U-13, EX-4: TX DMA completion for both links (DMA_IRQ_0). */
static void __not_in_flash_func(tx_dma_isr)(void) {
    uint32_t t0 = diag_cycles();
    uint32_t ints = dma_hw->ints0;
    for (unsigned i = 0; i < LINK_COUNT; i++) {
        link_t *l = &s_links[i];
        uint32_t bit = 1u << l->tx_ch;
        if (ints & bit) {
            dma_hw->ints0 = bit;
            l->tx_tail = l->tx_tail + l->tx_inflight;
            tx_start(l);
        }
    }
    diag_max(DIAG_T_ISR_TX_DMA, diag_cycles() - t0);
}

/* ------------------------------------------------------- line errors --- */

/* U-15, U-15a: count, clear and mask; the tick re-enables. */
static inline void __not_in_flash_func(uart_err_service)(link_id_t id) {
    uart_hw_t *hw = hw_of(&s_links[id]);
    uint32_t mis = hw->mis;
    if (mis & UART_UARTMIS_FEMIS_BITS) {
        diag_inc(ctr(id, DIAG_L0_FRAMING));
    }
    if (mis & UART_UARTMIS_BEMIS_BITS) {
        diag_inc(ctr(id, DIAG_L0_BREAK));
    }
    if (mis & UART_UARTMIS_OEMIS_BITS) {
        diag_inc(ctr(id, DIAG_L0_OVERRUN));
    }
    hw->icr = UART_ERR_BITS;
    hw_clear_bits(&hw->imsc, UART_ERR_BITS);
}

static void __not_in_flash_func(uart0_err_isr)(void) {
    uint32_t t0 = diag_cycles();
    uart_err_service(LINK_0);
    diag_max(DIAG_T_ISR_UART0_ERR, diag_cycles() - t0);
}

static void __not_in_flash_func(uart1_err_isr)(void) {
    uint32_t t0 = diag_cycles();
    uart_err_service(LINK_1);
    diag_max(DIAG_T_ISR_UART1_ERR, diag_cycles() - t0);
}

/* -------------------------------------------------------------- tick --- */

void __not_in_flash_func(link_tick_isr)(void) {
    for (unsigned i = 0; i < LINK_COUNT; i++) {
        link_t *l = &s_links[i];
        /* U-8: accumulate the DMA advance since the previous tick. */
        uint32_t pos = rx_dma_pos(l);
        l->rx_written_total = l->rx_written_total + ((pos - l->rx_sampled_pos) & RX_MASK);
        l->rx_sampled_pos = pos;
        /* U-15a: re-enable error interrupts. An error that occurred while
         * masked is still latched in RIS and fires once, immediately. */
        hw_set_bits(&hw_of(l)->imsc, UART_ERR_BITS);
    }
}

/* -------------------------------------------------------------- init --- */

static bool link_setup(link_id_t id, uart_inst_t *uart, uint tx_pin, uint rx_pin,
                       uint8_t *rx_ring, uint8_t *tx_ring, irq_handler_t err_isr) {
    link_t *l = &s_links[id];
    memset(l, 0, sizeof *l);
    l->uart = uart;
    l->rx_ring = rx_ring;
    l->tx_ring = tx_ring;
    l->tx_pin = tx_pin;
    l->rx_pin = rx_pin;

    /* U-1, U-2 */
    uint actual = uart_init(uart, LINK_BAUD);
    uart_set_format(uart, 8, 1, UART_PARITY_NONE);
    uart_set_hw_flow(uart, false, false); /* HW-6 */
    uart_set_fifo_enabled(uart, true);
    g_diag_actual_baud[id] = actual;

    uart_hw_t *hw = uart_get_hw(uart);
    /* U-15: DMA continues on error. uart_init enables TX/RX DMA requests. */
    hw_clear_bits(&hw->dmacr, UART_UARTDMACR_DMAONERR_BITS);
    hw_set_bits(&hw->dmacr, UART_UARTDMACR_TXDMAE_BITS | UART_UARTDMACR_RXDMAE_BITS);

    gpio_set_function(tx_pin, UART_FUNCSEL_NUM(uart, tx_pin));
    gpio_set_function(rx_pin, UART_FUNCSEL_NUM(uart, rx_pin));
    gpio_pull_up(rx_pin); /* HW-6a */

    /* RX DMA: UART DR -> ring, endless (U-3, U-4). */
    l->rx_ch = (uint)dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(l->rx_ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_ring(&c, true, LINK_RX_RING_BITS);
    channel_config_set_dreq(&c, uart_get_dreq_num(uart, false));
    channel_config_set_high_priority(&c, true);
    dma_channel_configure(l->rx_ch, &c, rx_ring, &hw->dr, dma_encode_endless_transfer_count(), true);
    l->rx_sampled_pos = 0u;
    l->rx_written_total = 0u;
    l->rx_read_total = 0u;

    /* TX DMA: ring -> UART DR, started on demand. */
    l->tx_ch = (uint)dma_claim_unused_channel(true);
    c = dma_channel_get_default_config(l->tx_ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_ring(&c, false, LINK_TX_RING_BITS);
    channel_config_set_dreq(&c, uart_get_dreq_num(uart, true));
    channel_config_set_high_priority(&c, true);
    dma_channel_configure(l->tx_ch, &c, &hw->dr, tx_ring, 0, false);
    dma_irqn_set_channel_enabled(LINK_TX_DMA_IRQ_INDEX, l->tx_ch, true);

    /* U-15: framing, break and overrun interrupts only. RX and RX-timeout
     * interrupts stay off: nothing interrupt-driven is in the data path. */
    hw->icr = 0x7ffu;
    hw->imsc = UART_ERR_BITS;
    uint irq = UART_IRQ_NUM(uart);
    irq_set_exclusive_handler(irq, err_isr);
    irq_set_priority(irq, IRQ_PRIO_UART);
    irq_set_enabled(irq, true);

    uint32_t err = actual > LINK_BAUD ? actual - LINK_BAUD : LINK_BAUD - actual;
    return (uint64_t)err * 1000000u <= (uint64_t)LINK_BAUD * LINK_BAUD_TOLERANCE_PPM;
}

bool link_init(void) {
    bool ok = true;
    ok &= link_setup(LINK_0, uart0, LINK0_TX_PIN, LINK0_RX_PIN, s_rx_ring0, s_tx_ring0, uart0_err_isr);
    ok &= link_setup(LINK_1, uart1, LINK1_TX_PIN, LINK1_RX_PIN, s_rx_ring1, s_tx_ring1, uart1_err_isr);

    uint irq = DMA_IRQ_0 + LINK_TX_DMA_IRQ_INDEX;
    irq_set_exclusive_handler(irq, tx_dma_isr);
    irq_set_priority(irq, IRQ_PRIO_UART);
    irq_set_enabled(irq, true);
    return ok;
}
