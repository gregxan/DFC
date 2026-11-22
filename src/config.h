/*
 * Compile-time configuration. Every value can be overridden with -D on the
 * command line or in CMakeLists.txt (each is wrapped in #ifndef).
 *
 * Requirement IDs refer to docs/REQUIREMENTS.md (Revision 2.16).
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

/* ======================================================================
 * Pin selection (XIAO RP2350). Read before changing any pin below.
 * ======================================================================
 *
 * XIAO pad -> GPIO, front edge (from the Seeed pinout and the SDK board file
 * seeed_xiao_rp2350.h):
 *   D0 = GPIO26 (A0)   D4 = GPIO6         D8  = GPIO2
 *   D1 = GPIO27 (A1)   D5 = GPIO7         D9  = GPIO4
 *   D2 = GPIO28 (A2)   D6 = GPIO0         D10 = GPIO3
 *   D3 = GPIO5         D7 = GPIO1
 * Back pads used here: D11 = GPIO21, D12 = GPIO20. The other back pads
 * (D13..D18) carry further GPIOs; take their numbers from the Seeed pinout,
 * not from this comment.
 *
 * Used by the board itself; do not assign: GPIO19 (battery-sense enable),
 * GPIO22/23 (RGB LED data/power), GPIO25 (user LED), GPIO29 (battery ADC).
 * Only D0..D2 (GPIO26..28) can be analog inputs (ADC); keep them free if
 * the application may need one.
 *
 * Alternatives on exposed pads, from the RP2350 datasheet GPIO function
 * table (F2 = UART/I2C function, AUX = F11 "UART_AUX": TX/RX on the CTS/RTS
 * positions; UART_FUNCSEL_NUM() picks the right one automatically):
 *
 *   UART0 (LINK_0)  TX/RX  GPIO0/1  = D6/D7   (current)
 *                          GPIO2/3  = D8/D10  (AUX)  - conflicts with SPI0
 *                                                       and the test pins
 *   UART1 (LINK_1)  TX/RX  GPIO20/21 = D12/D11 (current, back pads)
 *                          GPIO4/5  = D9/D3   (F2)  - conflicts with SPI0
 *                                                       and the test pins
 *                          GPIO26/27 = D0/D1  (AUX) - uses analog pins
 *                          GPIO6/7  = D4/D5   (AUX) - conflicts with I2C1
 *   I2C1 (magnetometer) SDA/SCL  GPIO6/7 = D4/D5 (current)
 *                                GPIO2/3 = D8/D10, GPIO26/27 = D0/D1
 *   I2C0 (would need MAG_I2C/MAG_I2C_IRQ changed in mag.c)
 *                                GPIO0/1 = D6/D7 (UART0), GPIO4/5 = D9/D3
 *
 * Rules:
 *  - TX and RX must belong to the same UART instance, and LINK_0 is
 *    hard-wired to uart0, LINK_1 to uart1 (link_init()).
 *  - SDA must be the even and SCL the odd pin of the same I2C instance. The
 *    PIO bus clear (M-18) works on any SCL pin.
 *  - Test pins (below) can be any free GPIO; they use plain SIO outputs.
 *  - UART RX and I2C pins get internal pull-ups (HW-6a, HW-4), which also
 *    keeps them clear of erratum RP2350-E9 (pull-down leakage, A2 only).
 *  - Update README.md and docs/VERIFICATION.md wiring tables after a change.
 */

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

/* M-3b: magnetometer output rate at start-up, in Hz. One of 1000, 500, 250,
 * 200, 100 or 50 (see the table in mag.c); changed at run time with
 * mag_set_rate_hz() (mag.h). Each rate uses the sensor's own bandwidth
 * setting, one triggered measurement per output sample, with no averaging in
 * firmware. */
#ifndef MAG_RATE_DEFAULT_HZ
#define MAG_RATE_DEFAULT_HZ 1000u
#endif

/* M-4c (Rev 2.28): whether mag_field() / mag_field16() subtract the
 * calibrated bridge offset (M-4b), unless the saved settings say otherwise
 * (NV-11a). Changed at run time with mag_set_offset_apply(). */
#ifndef MAG_OFFSET_APPLY_DEFAULT
#define MAG_OFFSET_APPLY_DEFAULT 1
#endif

/* M-17: stale limit per sample period (3 ms at 1 kHz, 60 ms at 50 Hz). */
#ifndef MAG_STALE_US
#define MAG_STALE_US 3000u
#endif

/* M-4 (Rev 2.12): SET/RESET strategy, selectable at run time through the
 * global g_mag_sr_mode (see mag.h; currently changed only from a debugger).
 *   MAG_SR_INIT_ONLY (0) - one RESET -> SET cycle at start-up, then none. If
 *                          the mode is selected again at run time, one more
 *                          cycle runs (RESET in one transaction, SET in the
 *                          next).
 *   MAG_SR_PULSE     (1) - a SET pulse inserted before the TM_M trigger
 *                          every g_mag_sr_interval samples (clamped 1..100).
 * A RESET -> SET start-up cycle runs in every mode. Auto_SR_en is never used:
 * at BW=11 it is not expected to sustain 1 kHz (docs/DESIGN.md section 6). */
#define MAG_SR_INIT_ONLY 0u
#define MAG_SR_PULSE 1u
#ifndef MAG_SR_MODE_DEFAULT
#define MAG_SR_MODE_DEFAULT MAG_SR_INIT_ONLY
#endif
#ifndef MAG_SR_INTERVAL_DEFAULT
#define MAG_SR_INTERVAL_DEFAULT 50u
#endif
#define MAG_SR_INTERVAL_MAX 100u /* M-4: at least every 100 samples */

/* D-1 (Rev 2.12): a valid sample counts as saturated when any axis is within
 * this many counts of either end of the 18-bit range (+/-8 G full scale;
 * fields above ~10 G can upset the sensor until the next SET). */
#ifndef MAG_SAT_MARGIN
#define MAG_SAT_MARGIN 72
#endif

/* M-2 (Rev 2.7): while the magnetometer fault flag is set, re-probe the
 * product ID every this many ticks (200 = 5 Hz). */
#ifndef MAG_REPROBE_PERIOD_TICKS
#define MAG_REPROBE_PERIOD_TICKS 200u
#endif

/* M-2b (Rev 2.16): run-time fault on persistent not-ready samples. A sensor
 * that lost power and came back answers at its defaults (8 ms measurements),
 * so most samples are read before they finish. Each not-ready sample adds
 * MAG_NOT_READY_STEP to a score and each ready sample subtracts 1 (never
 * below 0); reaching MAG_NOT_READY_FAULT_SCORE sets the fault flag, and the
 * re-probe (M-2a) then reconfigures the sensor. With the defaults a sensor
 * that is never ready faults after 10 samples (10 ms); one that is ready
 * only 1 time in 8 faults after about 13 ms; isolated not-ready samples
 * decay away. Set MAG_NOT_READY_FAULT_SCORE to 0 to disable. */
#ifndef MAG_NOT_READY_FAULT_SCORE
#define MAG_NOT_READY_FAULT_SCORE 20u
#endif
#define MAG_NOT_READY_STEP 2u

/* I2C bus clear (D-5, M-2, M-15, M-16, Rev 2.2): nine SCL clocks so a
 * sensor left mid-byte releases SDA.
 *   - once at start-up, before the product-ID read (blocking, ~110 us);
 *   - after an arbitration-loss or timeout abort, by a PIO state machine
 *     (non-blocking, ~100 us, done before the next tick).
 * Set to 0 to disable both. See docs/DESIGN.md section 4, "Bus clear". */
#ifndef MAG_BUS_CLEAR
#define MAG_BUS_CLEAR 1
#endif

/* ---------------------------------------------------- Interrupt priority */

/* EX-5. RP2350 implements 4 priority bits; lower value = higher priority. */
#define IRQ_PRIO_UART 0x00u /* UART error, TX DMA completion            */
#define IRQ_PRIO_MAG 0x40u  /* I2C STOP detect (completion) + abort    */
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

/* HW-9 (Rev 2.13): test GPIOs for logic-analyzer measurements (V-2, V-3),
 * toggled by the test application (app_test.c). In the user application,
 * D8 instead goes high for the duration of each epoch's app_poll().
 * Off by default; the CMake option APP_TEST_PINS turns them on (the diag
 * and diag-pattern presets do).
 * They sit on the SPI0 pads, keeping all three analog inputs (D0..D2)
 * free. If the application needs SPI0, move them to free back pads. */
#ifndef APP_TEST_PINS
#define APP_TEST_PINS 0
#endif
#define TEST_PIN_LINK0_RX_SEEN 2u  /* D8  */
#define TEST_PIN_LINK0_TX_WRITE 4u /* D9  */
#define TEST_PIN_LINK1_RX_SEEN 3u  /* D10 */
#define TEST_PIN_LINK1_TX_WRITE 5u /* D3  */

/* Application selection (app.h; Rev 2.14). Set by the CMake option
 * APP_MODE: user (production, app_user.c), echo or pattern (app_test.c). */
#define APP_MODE_ECHO 0
#define APP_MODE_PATTERN 1
#define APP_MODE_USER 2
#ifndef APP_MODE
#define APP_MODE APP_MODE_USER
#endif

/* Periodic status lines over USB CDC (B-20). Off in production builds. */
#ifndef APP_USB_STATUS
#define APP_USB_STATUS 0
#endif

#endif /* APP_CONFIG_H */
