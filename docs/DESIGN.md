# Design

## 1. Overview

```
             +--------------------------- core 0 ----------------------------+
 UART0 RX -->| DMA (endless, write ring 8 KB) --> rx ring 0 --+              |
 UART1 RX -->| DMA (endless, write ring 8 KB) --> rx ring 1 --+              |
             |                                                v              |
             |                       main loop:  link_read()  link_write()   |
             |                                   mag_read_latest()           |
             |                                                |              |
 UART0 TX <--| DMA (read ring 4 KB) <-- tx ring 0 <-----------+              |
 UART1 TX <--| DMA (read ring 4 KB) <-- tx ring 1 <-----------+              |
             |                                                               |
 I2C1     <->| DMA cmd (s_cmd -> IC_DATA_CMD) / DMA data (IC_DATA_CMD -> slot)
             |      slots[2]  --latest-->  mag_read_latest()                 |
             |                                                               |
 TIMER0   -->| 1 kHz tick ISR: U-8 bookkeeping, U-15a re-enable, mag start   |
 PIO0 SM  -->| I2C bus clear (9 SCL clocks) after an arbitration/timeout abort
             +---------------------------------------------------------------+
```

The firmware is linked as `copy_to_ram`, so all code, every ISR included,
executes from SRAM (B-18, EX-6). Core 1 is never started (HW-8).

## 2. Interrupts (EX-4, EX-5)

| IRQ | Priority | Handler | Work |
|---|---|---|---|
| `UART0_IRQ`, `UART1_IRQ` | 0x00 | `uart{0,1}_err_isr` | Count FE/BE/OE, clear, mask the error interrupts (U-15, U-15a) |
| `DMA_IRQ_0` | 0x00 | `tx_dma_isr` | Retire the finished TX transfer, start the next one if data is queued (U-13) |
| `I2C1_IRQ` | 0x40 | `i2c_isr` | `STOP_DET`: publish the sample (M-12). `TX_ABRT`: abort both DMA channels, count, start a bus clear (M-15) |
| `TIMER0_IRQ_n` | 0x80 | `tick_isr` | Rearm on a fixed grid (M-6a), U-8, U-15a re-enable, start or abort a magnetometer transaction (M-6, M-16) |
| `USBCTRL_IRQ` | 0xC0 | SDK | `diag` builds only (B-20) |

No interrupt is in the RX data path (U-5). RX and RX-timeout interrupts are
never enabled.

Every handler is straight-line code with no loops that wait on hardware. The
only loop drains up to 16 entries from the I2C RX FIFO after an abort.
Durations are recorded with the DWT cycle counter into `g_diag_max_cycles[]`
(`DIAG_TIMING`, V-5).

## 3. UART links (`src/link.c`)

### RX (U-3 … U-9)

- Each UART has a DMA channel. It reads `UARTDR` (8-bit, DREQ = UART RX) and
  writes into an 8 KB ring with write-address ring wrapping. Its transfer
  count is `dma_encode_endless_transfer_count()`, so it never completes and
  the CPU never restarts it (U-4).
- `DMAONERR` is cleared, so DMA continues through line errors. Bad bytes
  stay in the stream unmarked (U-15).
- The 1 ms tick samples each channel's `WRITE_ADDR` and adds the advance
  (modulo the ring size) to a 32-bit `rx_written_total` (U-8). That is a
  handful of loads and stores per UART.
- `link_read()` computes, inside one critical section of a few dozen
  cycles:
  `total_now = rx_written_total + ((dma_pos - sampled_pos) & mask)`.
  Then `unread = total_now - rx_read_total`, using modulo-2^32
  arithmetic (U-8a).
  - If `unread > ring size`, the read declares overflow (U-9). It sets
    `rx_read_total = total_now`, increments the overflow counter once, and
    returns 0.
  - Otherwise it copies `min(unread, max)` bytes, in at most two `memcpy`s.
    It then re-checks that the DMA did not lap the oldest copied byte during
    the copy. If it did, that is also overflow.
- So overflow is detected on the **first** read after the overwrite, not
  just within 1 ms of it. This is stronger than U-8a requires.
- The RX high-water mark is taken at every read, before the copy. Between
  reads the fill level only grows, so the maximum is always observed (D-3).

### TX (U-10 … U-14, U-18)

- A 4 KB ring is aligned to its size. The TX DMA uses **read-address ring
  wrapping**, so one transfer can cross the end of the ring. Every transfer
  sends *everything* queued at the moment it starts.
- `tx_head` (written only by the main loop) and `tx_tail` (advanced only
  by the completion ISR) are free-running 32-bit byte counts.
- `link_write()` checks free space, `TX_SIZE - (head - tail)`. If the
  message doesn't fit, the whole write is rejected and counted (U-11).
  Otherwise it copies, issues `__dmb()`, publishes `head`, and starts the DMA
  inside a critical section of about 10 instructions if `tx_busy` is clear
  (U-12). The ISR and the writer can't both start a transfer, and neither
  can miss queued data. The ISR either sees the new head, or it clears
  `tx_busy` first and the writer sees that.
- The completion ISR fires when the last byte has been written into the
  32-entry UART FIFO, not when it has left the wire. The next transfer
  therefore normally starts while about 32 bytes (~350 µs) are still
  queued in hardware. The ≤ 25 µs boundary limit of U-18 is a worst case
  that takes no credit for this FIFO.

### Line errors (U-15 … U-15b)

The error ISR reads `UARTMIS`, increments the framing / break / overrun
counters, clears the flags in `UARTICR` and masks all three error
interrupts. The tick unmasks them. An error that occurs while masked stays
latched in `UARTRIS` and fires once as soon as the mask is cleared. That
caps the count at one per type per UART per millisecond, and the interrupt
rate at ≤ 1 kHz per UART (U-15b).

## 4. Magnetometer (`src/mag.c`)

### Transaction (M-7, M-8)

One command list goes to `IC_DATA_CMD` (16-entry TX FIFO, so the whole list
fits). The DW I2C issues RESTARTs where the `RESTART` bit is set:

```
START 0x30W 0x00   RESTART 0x30R  b0..b6 (7 reads)   RESTART 0x30W 0x09 (ctrl0|TM_M)  STOP
```

That is about 121 SCL bit times, ~300 µs at 400 kHz, or ~330 µs with
controller overheads (M-8: ≤ 400 µs). With `MAG_SR_MODE = MAG_SR_PULSE` an
extra `RESTART 0x30W 0x09 SET` is inserted before the trigger every
`MAG_SET_INTERVAL` (default 50) samples. That adds ~70 µs, so the trigger
write finishes ≲ 400 µs after the tick and the 0.5 ms measurement ends
≲ 900 µs after it, before the next read (M-8: ≥ 50 µs margin).

- Command DMA channel: `s_cmd` → `IC_DATA_CMD`, 32-bit, DREQ = I2C TX.
- Data DMA channel: `IC_DATA_CMD` → `slots[!latest].raw`, 8-bit, 7
  transfers, DREQ = I2C RX (M-11).
- Every write to Internal Control 0 carries `CTRL0_PERSIST`, which is
  `Auto_SR_en` in `MAG_SR_AUTO` mode (M-5).

### Completion and publication (M-9 … M-14)

Completion is the I2C **STOP_DET** interrupt (M-12, Rev 2.1). The data
channel's DMA finishes earlier, before the trigger write, so its completion
can't mark the end of the transaction (see §6). The ISR checks that both DMA channels have
finished. It then fills in `valid`, `seq` and `timestamp_us` and issues
`__dmb()`. Finally it publishes the slot with a single 32-bit store to
`s_latest` (M-12).

- `seq` is the index of the tick that **triggered** the measurement, and
  `timestamp_us` is that tick's scheduled time on the fixed 1 ms grid
  (M-10). Consecutive valid samples have consecutive `seq`, so any lost
  sample shows up as a gap.
- The first transaction after start-up or after an abort has no prior
  trigger. Its sample is published with `valid = false` (M-9).
- `mag_read_latest()` copies the 24-byte slot inside a critical section
  (M-13) and sets `stale` when the sample is more than 3 ms old (M-17).

### Errors and timeouts (M-15 … M-16a)

The tick runs a state machine: `IDLE → BUSY → (IDLE | ABORTING → ABORTED → IDLE)`.
The I2C IRQ is masked in the NVIC while the tick runs it, so the
higher-priority I2C ISR can't change state halfway through. That masks one
IRQ only; global interrupts stay on.

- **TX_ABRT** (NACK, arbitration loss, user abort) is handled in the I2C
  ISR. It writes `DMA_ABORT` for both channels without waiting, reads
  `IC_CLR_TX_ABRT` and counts an I2C error unless the abort was the
  firmware's own timeout abort. The state becomes `ABORTED` and the slot
  is not published (M-15).
- **Tick while BUSY** counts a missed tick and a timeout. It then writes
  `DMA_ABORT` and sets `IC_ENABLE.ABORT`, without waiting (M-16).
- **Tick while ABORTING/ABORTED** starts a new transaction only if both
  channels, the I2C controller (`MST_ACTIVITY`, `TFE`, `ABORT` bit) and
  the bus clear are idle. Otherwise it counts a missed tick. If the DMA
  has finished and no abort is pending, but the controller still holds the
  bus (for example, a command word reached the FIFO after an abort), the
  tick issues a fresh abort and counts a timeout. This is the M-16 rule
  applied again.
- **Timing for one hung transaction** started on tick N:
  - N+1: abort.
  - N+2: restart, with invalid data.
  - N+3: valid sample published ≈ N+3.35 ms, which is under the M-16a
    limit of 4 ms.
- **Persistent bus fault.** The tick only ever writes a few registers, so
  UART traffic is unaffected (M-16a).

### Bus clear (M-18, Rev 2.2; `MAG_BUS_CLEAR`, default on)

If a read is cut off while the MMC5983MA is sending a 0 bit, the sensor
keeps driving SDA low. Every later START then loses arbitration, and the
RP2350 I2C block has no hardware bus-clear feature. The firmware clears the
bus in two places:

- **At startup (M-2).** `startup_bus_clear()` runs before the Product ID
  read. It drives SCL as an open-drain GPIO for 9 clocks at ~100 kHz, then
  sends START + STOP. It blocks for ~110 µs, which is allowed before the
  main loop. This covers an MCU reset or reflash that interrupted a read.
- **After an arbitration-loss or timeout abort (M-15, M-16).** The I2C ISR
  hands SCL to a PIO state machine. The state machine sends 9 open-drain
  clocks (~100 µs) and pushes a word when it's done. The next tick sees
  that word, gives SCL back to I2C and starts the transaction. That START
  resets the sensor. No CPU time is used while it runs. NACK aborts
  (sensor absent) don't trigger a clear, because they can't leave SDA
  stuck.

Neither path touches the I2C controller's configuration. Every clear is
counted in `DIAG_MAG_BUS_CLEAR` (D-1).

## 5. Resources (R-1 … R-3)

| Resource | Use |
|---|---|
| DMA | 6 channels: UART0 RX/TX, UART1 RX/TX, I2C command, I2C data (claimed dynamically) |
| DMA IRQ | `DMA_IRQ_0` (UART TX completion only) |
| PIO | PIO0, 1 state machine, 7 instructions (post-abort bus clear, M-18) |
| Timer | TIMER0, 1 alarm |
| RAM | 2 × 8 KB RX + 2 × 4 KB TX rings (24 KB) + ~2 KB state. Total `.bss` is ~26 KB. Code (~17 KB) also runs from RAM. |
| Clock | `clk_peri` = `clk_sys` = 150 MHz (SDK default). The UART divisor gives 921,659 baud, an error of +0.0064 % (U-2). `link_init()` checks the actual rate against `LINK_BAUD_TOLERANCE_PPM` (1000 ppm) and reports it in the status (R-3). |

## 6. Deviations and interpretations (please review)

1. **Magnetometer completion interrupt = I2C `STOP_DET`. Resolved:
   accepted and written into the spec as Rev 2.1 (EX-5, M-12; see
   [REQUIREMENTS.md](REQUIREMENTS.md) §11).** The data DMA finishes before
   the trigger write is on the bus, and the command DMA finishes when its
   last word enters the FIFO, so neither marks the end of the transaction.
   `STOP_DET` and `TX_ABRT` share `I2C1_IRQ` at the EX-5 magnetometer
   priority. The DMA completion interrupts of the two magnetometer channels
   are not used.
2. **One STOP per transaction.** The read and the trigger write are joined
   by a repeated START, not STOP + START. This is standard I2C and saves a
   few bit times. Confirm it on the bench with the MMC5983MA.
3. **Timeout aborts aren't counted as I2C errors.** An abort issued by the
   tick (M-16) is counted as a *timeout*. Its `ABRT_USER_ABRT` is not also
   counted as an I2C error. NACKs, arbitration loss and unexpected STOPs
   are I2C errors. V-6's "sum of I2C errors and timeouts" is unaffected.
4. **Bus clear: resolved, written into the spec as Rev 2.2** (M-18, plus
   amendments to M-2, M-15, M-16, D-1, D-5 and V-6; see
   [REQUIREMENTS.md](REQUIREMENTS.md) §12). It runs at startup and after
   arbitration-loss and timeout aborts, as described in §4.
   `MAG_BUS_CLEAR=0` removes both. With it removed, M-16a and V-6 resume
   are not guaranteed.
5. **Startup product-ID failure.** The fault flag is set and the
   product-ID-fault counter is set to 1. No magnetometer transactions are
   attempted. The tick still runs (M-6).
6. **Missed alarm.** If the tick ISR is ever held off for a whole period
   (only possible with a debugger and `DBGPAUSE` disabled), the schedule
   skips forward on the same grid and counts each skipped tick as a missed
   tick.
7. **C standard.** `-std=c11` and the B-22/B-23 warning flags apply to the
   application sources. The SDK's own sources need GNU extensions (`asm`),
   so the target as a whole compiles as `gnu11` and the SDK is compiled with
   its own flags.
8. **U-15a masking.** After any error, all three error interrupts of that
   UART are masked, matching the 1 % budget in U-15b.
9. **Auto_SR_en (M-4).** `MAG_SR_AUTO` is the default, and the M-4 condition
   ("if verified effective with triggered measurements") has **not** been
   verified. If the bench test in VERIFICATION.md fails, build with
   `-DMAG_SR_MODE=MAG_SR_PULSE`.
10. **Diagnostic builds.** In `diag` builds, `stdio_usb` runs `tud_task()`
    inline when a status line is written. The loop time reported there
    therefore includes USB work. EX-8 is shown on the `release` build by
    reading `g_diag_max_cycles[DIAG_T_LOOP]` over SWD (VERIFICATION.md,
    V-5).

## 7. Requirement traceability

| Req | Where |
|---|---|
| HW-1, B-11 | `CMakePresets.json`, `CMakeLists.txt` (`PICO_BOARD`, `PICO_PLATFORM`) |
| HW-2, HW-3, HW-6, HW-6a | `config.h` pins; `link_setup()`: `uart_set_hw_flow(false)`, `gpio_pull_up(rx)` |
| HW-4, HW-5 | `config.h`; `mag_init()` pins, pull-ups, address 0x30 |
| HW-7, HW-8 | C + Pico SDK; core 1 never launched |
| EX-1, EX-2 | `main.c`; every API in `link.c` / `mag.c` / `diag.c` returns without waiting |
| EX-3 | `cs_enter()` / `cs_exit()` in `diag.h`; max recorded as `DIAG_T_CRITICAL` |
| EX-4, EX-5 | §2; `irq_set_priority` calls in `link.c`, `mag.c`, `tick.c`, `status.c` |
| EX-6, B-18 | `pico_set_binary_type(firmware copy_to_ram)`, `__not_in_flash_func` |
| EX-7 | `DIAG_T_API_READ/WRITE/MAG` (copy time excluded) |
| EX-8, D-4 | `diag_loop_mark()`, `DIAG_T_LOOP` |
| U-1, U-2, R-3 | `link_setup()`; actual baud in `g_diag_actual_baud[]` |
| U-3 … U-7 | `link_setup()` RX DMA; `LINK_RX_RING_BITS` |
| U-8, U-8a, U-9 | `link_tick_isr()`, `rx_total_now()`, `link_read()`, `rx_overflow()` |
| U-10 … U-14 | `link_write()`, `tx_start()`, `tx_dma_isr()`; `LINK_TX_RING_BITS` |
| U-15 … U-15b | `uart_err_service()`, `link_tick_isr()` |
| U-16 … U-18 | by design (§3); measured by V-2 … V-4 |
| M-1 … M-5 | `mag_init()`, `build_cmds()`, `MAG_SR_MODE` |
| M-6, M-6a | `tick.c` |
| M-7, M-8 | `mag_start()`, `build_cmds()` |
| M-9 … M-14 | `publish()`, `mag_read_latest()` |
| M-15 … M-17 | `i2c_isr()`, `mag_tick_isr()`, `hw_idle()`, `mag_read_latest()` |
| M-18 | `startup_bus_clear()`, `bus_clear_start()`, `bus_clear_done()`, `src/i2c_bus_clear.pio` |
| D-1, D-2 | `diag.h` counters, `diag_take()`, `diag_take_all()` |
| D-3 | `g_diag_rx_hwm[]`, `g_diag_tx_hwm[]` |
| D-5 | No counter or flag triggers anything; only M-15/M-16 aborts and U-9 resync |
| R-1, R-2 | §5 |
| B-1 … B-17 | README.md, `CMakePresets.json`, `cmake/host_tools.cmake`, `.gitattributes`, `.gitmodules`, `.vscode/` |
| B-19, B-20 | `CMakeLists.txt` (`pico_enable_stdio_uart 0`, USB only with `APP_USB_STATUS`), `status_init()` |
| B-21 … B-24 | `CMakeLists.txt` (`-g` in Release, `APP_WARNINGS`) |
| V-1 … V-7 | docs/VERIFICATION.md, `tools/` |
