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

### TX (U-10 … U-14, U-18; Rev 2.10, 2.12)

- **Ring and DMA, as before.** A 4 KB ring is aligned to its size. The TX
  DMA uses **read-address ring wrapping**, so one transfer can cross the
  end of the ring. Every transfer sends *everything* queued at the moment it
  starts.
- **Pointers.** `tx_head` is advanced only by the main loop. `tx_tail` is
  advanced when a transfer is retired, by the ISR or by a reap. Both are
  free-running 32-bit byte counts. `tx_busy` means "a transfer was started
  and has not been retired".
- **`link_write()`** does the following:
  1. **Reap** (critical section, a few instructions). If `tx_busy` is set
     but the channel's BUSY bit is clear, the transfer finished without an
     interrupt. The function acknowledges the latched status, advances
     `tx_tail`, and starts any data queued behind it.
  2. **Reject** the whole message if it doesn't fit in the ring (U-11).
     Nothing has been sent at this point.
  3. **Copy** the message into the ring, then `__dmb()` and publish
     `tx_head`. The CPU never writes the UART FIFO itself (EX-9; the
     direct-write fast path U-10a was withdrawn in Rev 2.12).
  4. **Critical section:** reap again. If the DMA is idle, start a transfer
     of everything queued, with the completion interrupt **disabled**.
     Otherwise **enable** the channel's completion interrupt, so the ISR
     chains the next transfer (U-13). If the transfer already finished, the
     latched status makes the interrupt fire as soon as the critical section
     ends. It can't be missed.
- **`tx_dma_isr()`** retires the transfer and starts the next one, which
  disables the interrupt again. It runs only when data was queued behind a
  running transfer, so non-overlapping writes produce **no interrupts**.
  `g_diag_tx_irq[]` counts the interrupts that do fire (D-3a, on the `L0`
  and `L1` status lines).
- **`link_tx_free()`** also reaps, so free space is never under-reported
  for long.
- **Gaps.** The completion ISR fires when the last byte has been written
  into the 32-entry UART FIFO, not when it has left the wire. The next
  transfer therefore normally starts while about 32 bytes (~350 µs) are
  still queued in hardware. The ≤ 25 µs boundary limit of U-18 is a worst
  case that takes no credit for this FIFO.
- **Model check.** The algorithm was checked with a randomised model:
  3000 runs, ~1.45 M bytes, with DMA progress, UART draining and the ISR
  interleaved between every step of `link_write()`. Every run kept byte
  order with no loss, and the queue drained with no further API calls. The
  check found, and led to the fix of, a stall: a reap that didn't start
  data queued behind the retired transfer. The model was re-run after the
  direct-write path was removed (Rev 2.12).

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
START 0x30W 0x00   RESTART 0x30R  b0..b8 (9 reads: X,Y,Z, Tout, Status)   RESTART 0x30W 0x09 TM_M  STOP
```

That is about 139 SCL bit times, ~350 µs at 400 kHz, or ~375 µs with
controller overheads (M-8: ≤ 400 µs). When the SET/RESET strategy calls for
a pulse (see below), an extra `RESTART 0x30W 0x09 SET` (or `RESET`) is
inserted before the trigger. That adds ~70 µs, for ~445 µs in total
(M-8: ≤ 450 µs with a pulse). The 0.5 ms measurement then ends ≲ 950 µs
after the tick, before the next read reaches the data registers at
~1050 µs. The repeated STARTs follow the datasheet (p. 17; M-7).

- Command DMA channel: `s_cmd` → `IC_DATA_CMD`, 32-bit, DREQ = I2C TX.
- Data DMA channel: `IC_DATA_CMD` → `slots[!latest].raw`, 8-bit, 9
  transfers, DREQ = I2C RX (M-11). The 9 bytes land in `raw[7]`, `tout`
  and `status`, which a `_Static_assert` keeps adjacent.
- Three sample command lists are prebuilt: no pulse, SET, RESET. The
  longest is 14 words, which fits the 16-entry FIFO. No persistent
  Internal Control 0 bits are used (M-5).
- **Completeness check (M-9, Rev 2.8).** When publishing, the ISR checks
  `Meas_M_Done` (Status bit 0), which was read just before this
  transaction's own trigger. If it is clear, the measurement was read
  before it finished: the sample is published invalid and counted in
  `DIAG_MAG_NOT_READY`.

### Register settings (M-3, Rev 2.8)

| Register | Value | Why |
|---|---|---|
| Internal Control 1 (0x0A) | `SW_RST` once at startup, then BW = 11 | Known state after an MCU-only reset. BW = 11 is the only 0.5 ms option, at 1.2 mG RMS. The inhibit bits stay 0: the channels run in parallel. |
| Internal Control 2 (0x0B) | 0x00 | Continuous mode, CM_Freq and periodic SET are off. They would run from the sensor's own oscillator and drift against the tick. |
| Internal Control 0 (0x09) | TM_M, SET or RESET only | `Auto_SR_en` is not used: the datasheet gives 580 Hz max at BW = 11, consistent with two measurements per output (M-3a). INT, TM_T and OTP stay off. |
| Internal Control 3 (0x0C) | 0x00 (reset default) | No self-test, I2C (not 3-wire SPI) |

### SET/RESET strategy (M-4, Rev 2.4; automatic mode removed in Rev 2.8)

- **At startup**, `mag_init()` writes RESET and then SET, waiting 1 ms after
  each. This happens in every mode.
- **Mode selection.** At the start of every transaction, `sr_select()` reads
  `g_mag_sr_mode` and `g_mag_sr_interval`. You can write either from the
  debugger, and the change applies within 1 ms. An invalid mode value is
  ignored.

| `g_mag_sr_mode` | Behaviour |
|---|---|
| `0` `MAG_SR_INIT_ONLY` (default) | No pulses. Selecting it at run time runs one cycle: RESET in one transaction, SET in the next. |
| `1` `MAG_SR_PULSE` | SET before the trigger every `g_mag_sr_interval` transactions (clamped 1–100, default 50). The first pulse comes immediately on entering the mode. |

- **Pulses are always SET.** After a RESET the sensor reads with inverted
  polarity, so the sample measured after a RESET is published with
  `valid = false` (M-9). The cycle always ends in SET, even if the mode
  changes partway through.
- **Retries.** If a pulse transaction is aborted, the cycle step or pulse
  is retried on the next transaction.
- **Status.** The `diag` builds report the current values on the `MAGS`
  status line.

### Offset calibration (M-4b, Rev 2.9)

This follows the datasheet procedure (p. 18). After RESET a measurement
reads −H + offset, and after SET it reads +H + offset. The mean of the two
is the bridge offset.

- **Where the pairs come from.** Every RESET → SET cycle produces one:
  - **Startup.** `mag_init()` blocks: RESET, then trigger and read, then
    SET, then trigger and read.
  - **Re-selecting mode 0 at run time.** The RESET goes in one
    transaction, the SET in the next.
  - **Re-probe configuration.** The configuration ends with RESET + TM_M.
- **At run time,** `publish()` → `calibrate()` stores the post-RESET
  reading. The next sample, whose measurement followed the cycle's SET,
  completes the calibration, and `DIAG_MAG_OFFSET_CAL` is incremented.
- **A pair is discarded** if either measurement wasn't complete
  (`Meas_M_Done` clear), or if an abort or any other sample came in
  between. The previous offset is then kept.
- **Using the offset.** `mag_read_latest()` copies the offset and its
  validity flag in the same critical section as the sample.
  `mag_field()` subtracts it. The firmware never alters `raw[]`.
- **Assumption.** The field is steady over the ~2 ms between the two
  measurements, so re-zero with the sensor still.
- **Saturation counter.** `count_saturation()` counts valid samples with
  any axis within `MAG_SAT_MARGIN` (72) counts of full scale, in
  `DIAG_MAG_SATURATED`. This is informational only (D-5). A field above
  ~10 G can upset the sensor until the next SET, which happens in pulse
  mode, or when mode 0 is re-selected.

### Completion and publication (M-9 … M-14)

Completion is the I2C **STOP_DET** interrupt (M-12, Rev 2.1). The data
channel's DMA finishes earlier, before the trigger write, so its completion
can't mark the end of the transaction (see §6). The ISR checks that both DMA channels have
finished. It then fills in `valid`, `seq` and `timestamp_us` and issues
`__dmb()`. Finally it publishes the slot with a single 32-bit store to
`s_latest` (M-12).

- `seq` is the index of the tick whose transaction **triggered** the
  measurement. Consecutive valid samples have consecutive `seq`, so any
  lost sample shows up as a gap.
- `timestamp_us` (M-10, Rev 2.6) is when the trigger write completed. The
  STOP-detect ISR reads the timer as its first action. That is one bit time
  (~2.5 µs) after the trigger byte's ACK, plus interrupt latency, which is
  typically < 1 µs and up to ~15 µs behind UART handlers. The value is
  saved and attached to the sample when the *next* transaction reads it
  out. An invalid sample with no prior trigger carries the nominal
  previous-tick time instead.
- The first transaction after start-up or after an abort has no prior
  trigger. Its sample is published with `valid = false` (M-9).
- Every invalid sample is still published, with a reason code (M-9,
  Rev 2.11). `NO_PRIOR` takes priority over `NOT_READY`, which takes
  priority over `AFTER_RESET`. `valid` is `reason == MAG_SAMPLE_OK`.
- `mag_read_latest()` copies the 24-byte slot inside a critical section
  (M-13) and sets `stale` when the sample is more than 3 ms old (M-17).

### Errors and timeouts (M-15 … M-16a)

The tick runs a state machine: `IDLE → BUSY → (IDLE | ABORTING → ABORTED → IDLE)`.
The I2C IRQ is masked in the NVIC while the tick runs it, so the
higher-priority I2C ISR can't change state halfway through. That masks one
IRQ only; global interrupts stay on.

- **TX_ABRT** (NACK, arbitration loss, user abort) is handled in the I2C
  ISR. It writes `DMA_ABORT` for both channels without waiting, reads
  `IC_CLR_TX_ABRT` and counts an I2C error for any bus-reported cause. An
  abort caused only by the firmware's own timeout is counted only as a
  timeout (M-15, Rev 2.3). The state becomes `ABORTED` and the slot
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

### Re-probe (M-2a, Rev 2.7)

If the blocking startup probe fails, `mag_init()` still sets up the DMA
channels, the PIO and the interrupt. The magnetometer enters `MAG_FAULT`.
Every `MAG_REPROBE_PERIOD_TICKS` (200) ticks, the tick starts a DMA probe
once the hardware is idle:

```
START 0x30W 0x2F  RESTART 0x30R b0  STOP                        (~100 us)
```

- **On a product-ID match:** the STOP-detect ISR sets `s_config_pending`.
  The next tick then sends:

  ```
  START 0x30W 0x0A BW  RESTART 0x30W 0x0B 0x00  RESTART 0x30W 0x09 RESET  RESTART 0x30W 0x09 TM_M  STOP
  ```

  The completion of that transaction clears the fault flag, counts a
  recovery, and sets the SET/RESET cycle step to "SET pending". The first
  sample transaction reads the post-RESET measurement and carries the SET.
  That sample is invalid (M-9) and is the RESET half of an offset
  calibration (M-4b). Normal samples follow, and the next one completes
  the calibration.
- **NACK, mismatch, abort or timeout:** the firmware returns to
  `MAG_FAULT`. A failed probe counts as a product-ID fault. Neither probes
  nor configurations count as missed ticks. A configuration failure, or a
  timeout of either kind, uses the normal I2C-error and timeout counters.
- **Recovery time:** the fault flag clears within 200 ms of the sensor
  becoming reachable, plus two ticks.
- **Unplugging during sampling** does not set the fault flag. It produces
  I2C errors at 1 kHz (M-15), as before.

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

## 6. Design decisions (all reviewed and written into the spec)

1. **Magnetometer completion interrupt = I2C `STOP_DET`. Resolved:
   accepted and written into the spec as Rev 2.1 (EX-5, M-12; see
   [REQUIREMENTS.md](REQUIREMENTS.md) §11).** The data DMA finishes before
   the trigger write is on the bus, and the command DMA finishes when its
   last word enters the FIFO, so neither marks the end of the transaction.
   `STOP_DET` and `TX_ABRT` share `I2C1_IRQ` at the EX-5 magnetometer
   priority. The DMA completion interrupts of the two magnetometer channels
   are not used.
2. **One STOP per transaction: resolved, written into the spec as Rev 2.8
   (M-7).** This is supported by the datasheet (p. 17): a transfer "is
   terminated by a STOP condition or another START condition", and its
   example chains read → START → write. The read and the trigger write are
   joined by a repeated START, not STOP + START. This is standard I2C and saves a
   few bit times. Confirm it on the bench with the MMC5983MA.
3. **Timeout aborts are counted only as timeouts. Resolved, written into
   the spec as Rev 2.3 (M-15; see [REQUIREMENTS.md](REQUIREMENTS.md) §13).**
   An abort whose only cause is the firmware's own timeout abort
   (`ABRT_USER_ABRT`) counts as a timeout, not an I2C error. Any other cause
   reported in `IC_TX_ABRT_SOURCE` bits 16:0 counts as an I2C error, even
   alongside a timeout abort. Unexpected STOPs also count as I2C errors.
4. **Bus clear: resolved, written into the spec as Rev 2.2** (M-18, plus
   amendments to M-2, M-15, M-16, D-1, D-5 and V-6; see
   [REQUIREMENTS.md](REQUIREMENTS.md) §12). It runs at startup and after
   arbitration-loss and timeout aborts, as described in §4.
   `MAG_BUS_CLEAR=0` removes both. With it removed, M-16a and V-6 resume
   are not guaranteed.
5. **Magnetometer absent at boot: resolved, written into the spec as Rev 2.7**
   (M-2 amended, M-2a added, D-1, D-5 and V-6 amended; see
   [REQUIREMENTS.md](REQUIREMENTS.md) §17). The fault flag is set, and the
   tick re-probes at 5 Hz using the same DMA channels. A successful
   re-probe is followed by a configuration transaction, then sampling.
   See §4, "Re-probe".
6. **Missed alarm: resolved, written into the spec as Rev 2.11** (M-6a,
   D-1, V-1). If the tick ISR is ever held off for a whole period, the
   schedule skips forward on the same grid. Each skipped tick counts as a
   **tick overrun** (`DIAG_TICK_OVERRUN`), separate from magnetometer
   missed ticks. That's only possible with a debugger and `DBGPAUSE`
   disabled, or a violation of EX-3/EX-4.
7. **C standard and warnings: resolved, written into the spec as Rev 2.5**
   (B-22 amended, B-25 added; see [REQUIREMENTS.md](REQUIREMENTS.md) §15).
   Every C source in the image, the SDK's included, is compiled as
   `-std=c11` with the full B-22/B-23 flags and `-Werror`. Two single-file
   SDK exceptions are listed in `CMakeLists.txt`:
   - `runtime_init.c`: `asm=__asm`.
   - `stdio_usb.c`: `-Wsign-conversion` not fatal, diag builds only.
8. **U-15a masking: resolved, clarified in the spec as Rev 2.11.** After
   any error, all three error interrupts of that UART are masked until the
   next tick, matching the 1 % budget in U-15b. Per-type counts are not
   lost: the raw status latches each type while masked, and it is counted
   when the tick re-enables the interrupts.
9. **SET/RESET strategy: resolved, written into the spec as Rev 2.4**
   (M-4 replaced; M-8, M-9 and V-6 amended; see
   [REQUIREMENTS.md](REQUIREMENTS.md) §14). The strategy is selectable at
   run time (§4). The default is a single cycle at startup. The automatic
   mode (`Auto_SR_en`) was removed in Rev 2.8 (M-3a). See "Register
   settings" in §4.
10. **Loop time in diagnostic builds: resolved, written into the spec as
    Rev 2.11** (D-4, V-5). The main loop brackets `status_poll()` with
    `diag_excluded_begin()`/`diag_excluded_end()`. Its time is subtracted
    from the iteration time (`DIAG_T_LOOP`, `loop_max_us`) and recorded
    separately (`DIAG_T_STATUS`, `status_max_us`). Interrupt time still
    counts in the loop figure, as it should. EX-8 is visible in every diag
    run; SWD readout on release remains valid.
11. **Invalid samples are published, with a reason: resolved, written into
    the spec as Rev 2.11** (M-9, M-10). `mag_sample_t.reason` is one of
    `MAG_SAMPLE_OK`, `_NO_PRIOR`, `_AFTER_RESET`, `_NOT_READY`, and `valid`
    is `reason == OK`. The field sits in existing padding, so the slot stays
    24 bytes (checked by a `_Static_assert`). The reference loop counts
    invalid samples by reason in `g_app_mag_invalid_by_reason[]`.

## 7. Requirement traceability

| Req | Where |
|---|---|
| HW-1, B-11 | `CMakePresets.json`, `CMakeLists.txt` (`PICO_BOARD`, `PICO_PLATFORM`) |
| HW-2, HW-3, HW-6, HW-6a | `config.h` pins (and its pin-selection notes); `link_setup()`: `uart_set_hw_flow(false)`, `gpio_pull_up(rx)` |
| HW-9 | `config.h` `TEST_PIN_*`, CMake option `APP_TEST_PINS` (on in the diag presets) |
| B-11a | `CMakeLists.txt` `PICO_FLASH_SIZE_BYTES` |
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
| U-10 … U-14 | `link_write()`, `tx_reap()`, `tx_start()`, `tx_dma_isr()`; `LINK_TX_RING_BITS` |
| M-7a, EX-9 | `start_dma()` (`dma_debug_hw->ch[].dbg_ctdreq`) |
| D-3a | `g_diag_tx_irq[]` |
| D-4 (diag exclusion) | `diag_excluded_begin()`, `diag_excluded_end()`, `DIAG_T_STATUS` |
| M-6a (overrun) | `tick_isr()`, `DIAG_TICK_OVERRUN` |
| U-15 … U-15b | `uart_err_service()`, `link_tick_isr()` |
| U-16 … U-18 | by design (§3); measured by V-2 … V-4 |
| M-4b, D-1 (calibration, saturation) | `calibrate()`, `count_saturation()`, `blocking_measure()`, `mag_field()` |
| M-2a | `probe_start()`, `config_start()`, `mag_tick_isr()` (`MAG_FAULT`), `i2c_isr()` |
| M-1 … M-5 | `mag_init()` (SW_RST, BW, CTRL2), `build_cmds()`, `sr_select()`, `g_mag_sr_mode`, `g_mag_sr_interval` |
| M-9 (not ready) | `publish()` (`Meas_M_Done` check), `DIAG_MAG_NOT_READY` |
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
| B-21 … B-25 | `CMakeLists.txt` (`-g` in Release, target-wide C flags, per-file SDK exceptions) |
| V-1 … V-7 | docs/VERIFICATION.md, `tools/` |

## 8. RP2350 datasheet cross-check

The implementation was checked first against the register documentation
in the Pico SDK 2.2.0 headers and `RP2350.svd`, then against the full RP2350
datasheet (1 October 2026 edition): the DMA, UART, I2C, timer and GPIO
chapters, and Appendix D (errata).

**Fixed as a result**

- **RP2350-E5 (DMA abort).** The datasheet requires an aborted channel's EN
  bit to be cleared before `CHAN_ABORT`, or the channel may re-trigger.
  `mag_dma_abort()` now clears EN on both magnetometer channels before
  aborting, and `start_dma()` sets it again. The UART channels are never
  aborted.
- **Timer alarm race.** An alarm armed at, or just before, its matching time
  might fire after the tick has already skipped past it. The skip-ahead
  path in `tick_isr()` now disarms the alarm (`ARMED`) and clears its
  pending interrupt before re-arming.

**Confirmed**

| Design assumption | Register documentation |
|---|---|
| Lazy TX interrupt (U-13): completion is latched while the interrupt is disabled | `INTR`: raw status "ignores any masking"; writing `INTS0` clears it |
| RX byte is in RAM before the position advances (U-5) | `WRITE_ADDR` "updates automatically each time a write completes" |
| Endless RX DMA, ring sizes (U-4, U-7) | `TRANS_COUNT.MODE = 0xF`: endless, no interrupts; ring 2–32768 B |
| No restart before an abort completes (M-16) | `CHAN_ABORT` must read all-zero before restart (`hw_idle()`) |
| Break inserts one 0 byte (U-15) | `UARTDR.BE`: "only one 0 character is loaded into the FIFO" |
| Error interrupts latch while masked (U-15a) | `UARTRIS` raw state; `UARTIMSC` only masks |
| M-7 command list | `IC_DATA_CMD` RESTART/STOP semantics; no STOP with an empty FIFO stalls the bus (`controller_hung()`) |
| Abort (M-15, M-16) | `IC_ENABLE.ABORT`: STOP, flush, then TX_ABRT; settable only while enabled |
| Abort clear ordering | TX_ABRT flushes TX and RX FIFOs until `IC_CLR_TX_ABRT` is read |
| Target address reuse | `IC_TAR` writable only while disabled; left at 0x30 by the SDK's blocking calls |
| DMA thresholds | `IC_DMA_TDLR` (≤ 8 entries) and `IC_DMA_RDLR` (≥ 1 entry) |
| Tick (M-6, M-6a) | `ALARMx` fires on equality with `TIMELR`; `DBGPAUSE` resets to 7 |
| Priorities (EX-5) | `__NVIC_PRIO_BITS = 4`: 0x00/0x40/0x80/0xC0 are distinct |
| PIO handover of SCL (M-18) | `gpio_set_function` sets IE, clears OD and pad isolation (`ISO`) |
| RP2350-E2 (SIO writes above +0x180 alias spinlocks) | Not affected: only `gpio_togl` (+0x28) is written |

- **DREQ credit counters (datasheet §12.6.4.2, Rev 2.12).** The DMA keeps a
  per-channel count of how many accesses it believes the peripheral FIFO
  can take, and the CPU "must not access a FIFO currently being serviced by
  the DMA". Two consequences:
  - The UART direct-FIFO write (U-10a) was **removed**. It would have let
    the next TX DMA transfer overfill the FIFO.
  - `start_dma()` now writes `DBG_CTDREQ` on both magnetometer channels
    before every transaction (M-7a). After an I2C abort the controller
    flushes its FIFOs, and the RX drain reads the FIFO with the CPU, so the
    counters no longer matched. That could corrupt the first sample after
    an abort, or stall the command channel.

**Errata (Appendix D, all 31 items)**

| Erratum | Applies | Status |
|---|---|---|
| E5 DMA abort / CHAIN_TO | Yes | Fixed: EN cleared before abort (`mag_dma_abort()`), set again in `start_dma()` |
| E8 CHAIN_TO with zero-length transfers | No | No chaining, and no zero-length triggers |
| E9 GPIO pad leakage | No | A2 silicon only, and only pins relying on pull-downs; all inputs here use pull-ups |
| E27 bus-priority miswiring | No | `BUS_PRIORITY` left at its default |
| E12 USB status synchronisation | Diag builds | Satisfied: `clk_sys` 150 MHz is more than 10% above `clk_usb` 48 MHz |
| E2 SIO spinlock aliasing | No | Only `gpio_togl` is written; the SDK applies its workaround |
| E4, E6, E7, E29, E31 | No | Hazard3 (RISC-V) only |
| Bootrom, OTP, XIP, ACCESSCTRL, RCP, E1 | No | Unused, or not affected by a copy-to-RAM Arm build |

**Previously open, now closed**

- **PIO reads SCL while its function is I2C.** Confirmed: "the input is
  always connected, so the PIOs can always see the state of all pins".
- **UART RX DMA request.** The single-character request is asserted when
  the RX FIFO "contains at least one character". With the credit scheme the
  DMA keeps "as many transfers in-flight as the peripheral has capacity
  for", so each byte should move on arrival. This supports U-16. V-2 still
  confirms it, because the datasheet doesn't spell out the request wiring.
- **Silicon revision.** E9 is fixed from A3 onwards. Nothing else here
  depends on the revision.
