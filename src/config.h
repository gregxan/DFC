/*
 * Compile-time configuration. Every value can be overridden with -D on the
 * command line or in CMakeLists.txt (each is wrapped in #ifndef).
 *
 * Requirement IDs refer to "XIAO RP2350 Firmware Requirements, Revision 2".
 */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/* ---------------------------------------------------------------- UARTs */

/* U-1 */
#ifndef LINK_BAUD
#define LINK_BAUD 921600u
#endif

/* U-7: RX ring size = 2^LINK_RX_RING_BITS bytes. 13 -> 8 KB. DMA ring mode
 * supports up to 2^15. Must hold well over 1 ms of traffic (U-8). */
#ifndef LINK_RX_RING_BITS
#define LINK_RX_RING_BITS 13
#endif

/* U-14: TX ring size = 2^LINK_TX_RING_BITS bytes. 12 -> 4 KB. A power of two
 * is used so the TX DMA can use read-ring wrapping (one transfer may span the
 * end of the ring, halving the number of transfer boundaries, U-18). */
#ifndef LINK_TX_RING_BITS
#define LINK_TX_RING_BITS 12
#endif

/* HW-2, HW-3 */
#define LINK0_TX_PIN 0u  /* D6  */
#define LINK0_RX_PIN 1u  /* D7  */
#define LINK1_TX_PIN 20u /* D12 (back pad) */
#define LINK1_RX_PIN 21u /* D11 (back pad) */

/* U-2: accepted baud error, in parts per million. */
#define LINK_BAUD_TOLERANCE_PPM 1000u

/* --------------------------------------------------------- Magnetometer */

/* HW-4, HW-5, M-1 */
#define MAG_SDA_PIN 6u /* D4 */
#define MAG_SCL_PIN 7u /* D5 */
#define MAG_I2C_ADDR 0x30u
#ifndef MAG_I2C_HZ
#define MAG_I2C_HZ 400000u
#endif

/* M-6 */
#define TICK_PERIOD_US 1000u

/* M-17 */
#ifndef MAG_STALE_US
#define MAG_STALE_US 3000u
#endif

/* M-4: SET/RESET strategy.
 *   MAG_SR_AUTO  - Auto_SR_en is set in every write to Internal Control 0.
 *   MAG_SR_PULSE - a SET pulse is written in the same transaction, before the
 *                  TM_M trigger, every MAG_SET_INTERVAL samples (<= 100).
 * Auto_SR_en with single (TM_M) measurements must be confirmed on the bench
 * before MAG_SR_AUTO is accepted (see docs/VERIFICATION.md). */
#define MAG_SR_AUTO 0
#define MAG_SR_PULSE 1
#ifndef MAG_SR_MODE
#define MAG_SR_MODE MAG_SR_AUTO
#endif
#ifndef MAG_SET_INTERVAL
#define MAG_SET_INTERVAL 50u
#endif

/* After an arbitration-loss or user (timeout) abort, clock SCL nine times
 * with a PIO state machine so a slave left mid-byte releases SDA. Without it
 * a slave holding SDA low after a bus fault can block the bus permanently
 * and V-6 ("valid samples resume within 4 ms of release") cannot be met.
 * Set to 0 to disable. See docs/DESIGN.md, "Deviations and interpretations". */
#ifndef MAG_BUS_CLEAR
#define MAG_BUS_CLEAR 1
#endif

/* ---------------------------------------------------- Interrupt priority */

/* EX-5. RP2350 implements 4 priority bits; lower value = higher priority. */
#define IRQ_PRIO_UART 0x00u /* UART error, TX DMA completion            */
#define IRQ_PRIO_MAG 0x40u  /* I2C completion (STOP_DET) and I2C abort  */
#define IRQ_PRIO_TICK 0x80u /* 1 kHz timer                              */
#define IRQ_PRIO_USB 0xC0u  /* B-20: USB (diagnostic builds only)       */

/* DMA_IRQ_0 is used for UART TX completion. */
#define LINK_TX_DMA_IRQ_INDEX 0u

/* ------------------------------------------------------ Instrumentation */

/* Record maximum ISR, critical-section and API durations with the DWT
 * cycle counter (V-5). Costs a few cycles per measured section. */
#ifndef DIAG_TIMING
#define DIAG_TIMING 1
#endif

/* Test GPIOs for logic-analyzer measurements (V-2, V-3). Toggled by the
 * reference main loop. Set APP_TEST_PINS to 0 to leave them untouched. */
#ifndef APP_TEST_PINS
#define APP_TEST_PINS 1
#endif
#define TEST_PIN_LINK0_RX_SEEN 26u  /* D0 */
#define TEST_PIN_LINK0_TX_WRITE 27u /* D1 */
#define TEST_PIN_LINK1_RX_SEEN 28u  /* D2 */
#define TEST_PIN_LINK1_TX_WRITE 5u  /* D3 */

/* Reference application behaviour (see main.c). */
#define APP_MODE_ECHO 0
#define APP_MODE_PATTERN 1
#ifndef APP_MODE
#define APP_MODE APP_MODE_ECHO
#endif

/* Periodic status lines over USB CDC (B-20). Off in production builds. */
#ifndef APP_USB_STATUS
#define APP_USB_STATUS 0
#endif

#endif /* APP_CONFIG_H */
