# Verification (section 9)

None of the procedures below has been run yet. Every numeric limit in the
requirements is an engineering estimate that these measurements must
confirm.

All tools run on Windows (V-7). Install the Python dependency once:

```bash
py -m pip install -r tools/requirements.txt
```

## Test setup

- Two USB-UART adapters, 3.3 V logic, capable of 921,600 baud. Connect one
  to UART0 (D6 TX, D7 RX) and one to UART1 (D12 TX, D11 RX), with a common
  ground.
- A logic analyzer at ≥ 20 MS/s (V-4) on these signals:

| Signal | UART0 | UART1 |
|---|---|---|
| RX line (into the XIAO) | D7 / GPIO1 | D11 / GPIO21 |
| TX line (out of the XIAO) | D6 / GPIO0 | D12 / GPIO20 |
| RX_SEEN test pin | D8 / GPIO2 | D10 / GPIO3 |
| TX_WRITE test pin | D9 / GPIO4 | D3 / GPIO5 |

The test pins are driven only in builds with `APP_TEST_PINS` on: the
`diag` and `diag-pattern` presets, or `-DAPP_TEST_PINS=ON`.

- MMC5983MA on D4/D5, plus the USB cable for the `diag` builds' status port.

**Host baud accuracy.** Measure each adapter's real bit time with the
analyzer. An adapter that runs faster than the XIAO's 921,659 baud makes an
echo test pile up data in the XIAO's TX queue. `soak_test.py --rate`
(default 0.99) paces the host below line rate to avoid that.

## Loopback self-test (board only, no external equipment)

This is a first check of the UART path (both RX and TX DMA, the RX overflow
logic, the lazy TX interrupt) and of the magnetometer fault handling, before
any USB-UART adapters, logic analyzer or magnetometer are available. It
uses the **diag-pattern** build: each UART transmits a continuous 8-bit
counting sequence at full rate and checks that what it receives is the same
sequence. The echo build does nothing here, because nothing starts the
traffic.

### Wiring

Use **jumper wires**. Don't bridge neighbouring pads: on the XIAO, D6 and
D7 are on opposite edges (bottom of each row), and the pad next to D6 is
D5 (I2C SCL).

Preferred, crossed: each direction passes through both UARTs.

| From | To |
|---|---|
| D6 (UART0 TX, GPIO0) | D11 (UART1 RX, GPIO21, back pad) |
| D12 (UART1 TX, GPIO20, back pad) | D7 (UART0 RX, GPIO1) |

Alternative, each UART looped to itself: D6 → D7 and D12 → D11. With no
access to the back pads yet, wire just D6 → D7. UART1 then receives
nothing, which is a valid "disconnected RX" test of its own (HW-6a).

Leave the magnetometer unconnected (or connect it; see "Expected values").

### Run

1. Build the **diag-pattern** preset ("Release + USB status, TX pattern
   mode").
2. Hold **B**, tap **R**, release **B**, and copy
   `build\diag-pattern\firmware.uf2` to the `RP2350` drive.
3. Find the board's COM port (Device Manager → Ports), then run:
   ```bash
   py C:\dev\dfc\tools\fwstatus.py COM7 --log loopback.log
   ```
4. Leave it running for at least 10 minutes. Counters are cumulative since
   boot, so compare a report from the first minute with the last one.

### Expected values

| Status field | Expected | What it shows |
|---|---|---|
| `L0` / `L1` `pattern_err` | **0** (on each UART that has a wire into its RX) | received data matched the transmitted sequence, byte for byte, at line rate |
| `rx_overflow`, `tx_reject` | **0** | the main loop keeps up; TX never had to reject a write |
| `framing`, `break`, `overrun` | **0** | clean line |
| `rx_hwm` | small (tens of bytes) | little RX data waits between main-loop reads |
| `tx_hwm` | close to 4096 | the pattern keeps the TX queue full, as designed |
| `tx_irq` | rising roughly 20–30 per second per UART | one completion interrupt per chained DMA transfer (U-13), not one per byte |
| `STAT loop_max_us` | **≤ 100** | EX-8 |
| `STAT tick_overrun` | **0** | M-6a |
| `TIM_NS` `isr_*` | **≤ 5000** | EX-4 (nanoseconds) |
| `TIM_NS` `crit` | **≤ 2000** | EX-3 |
| `TIM_NS` `api_read`, `api_write`, `api_mag` | **≤ 2000** | EX-7 (copy time excluded) |
| `MAG` without a sensor | `fault=1`; `pid_fault` rising ~5/s; `i2c_error`, `timeout` and `missed_tick` not rising; `MAGS recovered=0` | the 5 Hz re-probe (M-2a), with **no effect on any UART counter** |
| `MAG` with a sensor | `fault=0`; `valid` rising ~1000/s; `not_ready=0`; `gaps=0`; `CAL offset_valid=1` | normal sampling |

A UART with nothing wired to its RX shows all zero counters and
`pattern_err=0`, because it receives nothing (the pull-up holds the line
idle).

### Optional fault checks (same wiring)

- **Break and line errors.** Unplug the jumper from one RX pin and briefly
  connect that **RX** pin to GND with a separate wire. Don't connect a TX
  pin to GND. Reconnect the jumper. On that UART:
  - `break` and/or `framing` rise, by at most one per millisecond (U-15a);
  - `pattern_err` rises by a small number (the injected 0 byte and the
    re-sync), then stays constant;
  - the other UART's counters are unaffected.
- **Open RX line.** Unplug one RX jumper and leave it open for a minute. No
  counts rise on that UART (HW-6a pull-up).

### What loopback doesn't cover

- **Baud-rate mismatch** against a real adapter (V-1): both ends share one
  clock here.
- **Latency and gaps** (V-2 to V-4): these need a logic analyzer on the
  test pins.
- **Host-side byte checking:** `pattern_err` is the firmware's own check.
  The soak test with USB-UART adapters (V-1) adds an independent check from
  the PC.
- **Magnetometer data quality.**

## V-1 Soak (10 min, saturated full duplex)

Run it twice: once in echo mode and once in pattern mode. Pattern mode is the
one that saturates the XIAO→host direction.

```bash
cmake --preset diag
```
```bash
cmake --build --preset diag
```
Flash `build\diag\firmware.uf2`, then:
```bash
py tools\soak_test.py --port0 COM5 --port1 COM6 --status COM7 --mode echo --duration 600
```

Then build and flash `diag-pattern` and run:

```bash
py tools\soak_test.py --port0 COM5 --port1 COM6 --status COM7 --mode pattern --duration 600
```

The script checks every byte (echo: a pseudo-random stream; pattern: the
counting sequence in both directions). It also reads the firmware counters
before and after the run and fails on any non-zero delta in these: framing,
RX overflow, TX reject, pattern error, magnetometer sequence gaps, I2C
errors, missed ticks or timeouts. It also prints the number of valid
magnetometer samples (expect ~1000/s) and checks the fault flag.

## V-2 / V-3 Latency

Use the `diag` build (echo mode, test pins on). A `release` build needs
`-DAPP_TEST_PINS=ON` for these measurements.

1. Capture RX, TX, RX_SEEN and TX_WRITE of one UART. Make the capture long
   enough for ≥ 100,000 bytes (~3–4 min at Windows sleep granularity).
2. Start the stimulus:
   ```bash
   py tools\latency_stimulus.py COM5 --count 110000
   ```
3. Run the magnetometer and traffic on the *other* UART at the same time
   (U-16: "with all subsystems active"). For example, run `soak_test.py` on
   the other port only, or loop it back.
4. Export the capture as CSV (Saleae Logic 2: *File → Export Data → CSV*)
   and analyse it:
   ```bash
   py tools\analyze_capture.py latency cap.csv --rx "Channel 0" --tx "Channel 1" --rx-seen "Channel 2" --tx-write "Channel 3"
   ```

The script takes end of stop bit = start-bit falling edge + 10 bit times. It
reports n, median, p99 and max for both paths and checks U-16 and U-17
(median ≤ 3 µs, max ≤ 20 µs, n ≥ 100,000).

## V-4 TX gaps

Use the `diag-pattern` build. Its TX queue is never empty, because the main
loop tops it up in 256-byte writes whenever ≥ 256 bytes are free. Capture the
TX line at ≥ 20 MS/s for the full 10 minutes of V-1 (split the capture into
several files if your analyzer needs to), then run:

```bash
py tools\analyze_capture.py gaps tx.csv --tx "Channel 1"
```

The output gives the number of gaps longer than 1 bit, a histogram and the
maximum gap. It passes if no gap exceeds 25 µs. Compare the count of gaps
longer than 1 bit with the number of TX DMA transfer boundaries. With a full
queue each transfer carries most of the 4 KB ring, so expect roughly one
boundary per ~4 KB sent. Most boundaries should show no gap at all, because
the 32-byte UART FIFO is still full when the next transfer starts.

## V-5 Handler, critical-section, API and loop timing

The firmware records the maximum of each in CPU cycles (150 cycles = 1 µs)
in `g_diag_max_cycles[]`:

| Index | Quantity | Limit |
|---|---|---|
| `DIAG_T_ISR_UART0_ERR`, `_UART1_ERR` | UART error ISR | 5 µs (EX-4) |
| `DIAG_T_ISR_TX_DMA` | TX DMA completion ISR | 5 µs |
| `DIAG_T_ISR_I2C` | I2C completion/abort ISR | 5 µs |
| `DIAG_T_ISR_TICK` | 1 kHz tick ISR | 5 µs |
| `DIAG_T_CRITICAL` | longest critical section | 2 µs (EX-3) |
| `DIAG_T_API_READ`, `_WRITE`, `_MAG` | API call minus payload copy | 2 µs (EX-7) |
| `DIAG_T_LOOP` | main-loop iteration, status reporter excluded | 100 µs (EX-8) |
| `DIAG_T_STATUS` | status reporter (diag builds only) | information only |

The `diag` builds print these in nanoseconds on the `TIM_NS` status line.
The `STAT` line also shows `loop_max_us` (EX-8 evidence, reporter
excluded), `status_max_us` and `tick_overrun`, which must be 0.
The `release` build has no USB, so read them over SWD if you also want the production figures. In the VS Code debug
session, add `g_diag_max_cycles` to *Watch*. With OpenOCD alone:

```bash
arm-none-eabi-nm build\release\firmware.elf | findstr g_diag_max_cycles
```
```bash
openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg -c "init" -c "mdw <address> 10" -c "exit"
```

Notes:

- An ISR's figure runs from its first to its last instruction, so it
  excludes the ~12-cycle hardware entry and exit. It *includes* time spent
  preempted by a higher-priority ISR, which makes it conservative for the
  I2C and tick ISRs.
- For an independent cross-check, add test-pin toggles at ISR entry and
  exit and measure them on the analyzer.
- Run V-5 during the V-1 soak and during V-6, so the figures cover the
  error and abort paths.

## V-5a Magnetometer timestamp lag (Rev 2.6)

The timestamp is the time the trigger write completed, so its lag can be
measured against the bus.

1. Make a temporary build that toggles a spare GPIO in `i2c_isr()`,
   immediately after `now_us` is read. Use a free back pad; see the
   pin-selection notes in `src/config.h`.
2. On the analyzer, measure from the 9th SCL clock (ACK) of the trigger data
   byte to that toggle, for ≥ 10,000 transactions with UART traffic running.
3. Report the median and the maximum. Expected: ~3 µs typical, < 20 µs
   maximum. The median can be subtracted as a fixed correction if wanted.

## V-6 Fault injection

Use the `diag` build and watch the status port (`py tools\fwstatus.py COM7`)
while UART traffic runs (`soak_test.py`).

| Test | Expect |
|---|---|
| Disconnect the magnetometer (SDA/SCL) | `MAG i2c_error` rises (~1000/s, NACK). L0/L1 counters and the soak data stay clean. |
| Short SDA to GND for 1 s, then release | `i2c_error + timeout` rises by ≥ 300; zero UART errors. After release, `valid` resumes within 4 ms. Capture SDA, SCL and a test pin to time it (see below). `bus_clear` shows how many PIO bus clears ran. |
| Power up with the magnetometer unplugged, then plug it in after a few seconds | `MAG fault=1` and `pid_fault` rising ~5/s. Within 250 ms of plugging in: `fault=0`, `MAGS recovered` +1, `valid` rising ~1000/s. The UARTs stay clean throughout. |
| Reset the MCU from the debugger (or tap RESET) while running, ≥ 100 times. A script can loop `openocd ... -c "init; reset run; exit"` with random delays. | After every boot, `MAG fault=0` and `valid` rises. The startup bus clear shows as `bus_clear` = 1 right after boot. |
| Hold one UART's RX low (break), or leave it unconnected | That UART's framing/break counts rise, at most 1/ms per type. The other UART's soak data is byte-exact and `MAG gaps` stays 0. |
| Stall the main loop (break in the debugger, or add a temporary `busy_wait_ms(200)` to a test build) | Each UART's `rx_overflow` increments once per overrun episode, and `rx_hwm` = 8192 (the ring size). |

**Timing the resume after the SDA short.** Trigger the analyzer on SDA
rising (the release). Find the first transaction on the bus after it whose
read phase returns non-0xFF data; the sample read by the transaction after
that one is valid. Alternatively, toggle a spare pin in `mag_consume()` when
`r.s.valid` first becomes true again.

## SET/RESET strategies (M-4)

Use the `diag` build in a debug session, with the `MAGS` status line on
screen. Set `g_mag_sr_mode` and `g_mag_sr_interval` in *Watch*, or with
OpenOCD `mww`. The modes are 0 = single cycle and 1 = pulse.

1. **Switching.** Watch SDA/SCL on the analyzer.
   1. Start in mode 0: no pulse writes.
   2. Switch to mode 1 with an interval of 10: a transaction carrying a SET
      write appears every 10 ms.
   3. Set the interval to 100: the SET transaction appears every 100 ms.
   4. Switch back to mode 0: one transaction carries RESET, the next carries
      SET, and `invalid` rises by exactly 1.

   Throughout, the soak test stays clean, and `gaps` and `not_ready` stay 0.
2. **Transaction duration (M-8).** Measure from the tick (START) to the
   final STOP. Expected ~375 µs without a pulse (limit 400 µs) and ~445 µs
   with a SET or RESET (limit 450 µs).

## 1 kHz measurement completeness (M-3, M-9, Rev 2.8)

The datasheet lists 580 Hz as the maximum output rate at BW = 11 outside
continuous mode. The firmware assumes this limit doesn't apply without
`Auto_SR_en`, i.e. that a triggered measurement takes the stated 0.5 ms.
To check that:

1. Run the V-1 soak in mode 0 and in mode 1 (interval 1, the worst case:
   every transaction is ~445 µs).
2. `MAGS not_ready` must stay 0 and `MAG valid` must rise by ~1000/s.
3. On the analyzer, confirm the 9th byte of each read (Status) is `0x01`
   (Meas_M_Done set).

If `not_ready` is non-zero, the 1 kHz triggered design doesn't hold. The
fallback is continuous mode (`CM_Freq = 111`), which needs a spec change.

## Software reset at startup (M-2, Rev 2.8)

Put the sensor into an unusual state in a test build: for example, write
`CTRL2 = 0x0F` (continuous mode, 1 kHz) from a debug session, then reset
only the MCU. After boot, the bus shows:

1. `W(0x0A, 0x80)`, then a 10 ms gap;
2. the product-ID and Status reads (Status bit 4, OTP_Rd_Done, = 1);
3. the configuration;
4. the RESET, measure, SET, measure calibration.

Then sampling is normal, with `CAL offset_valid=1 offset_cal=1`.

## Offset calibration and saturation (M-4b, D-1, Rev 2.9)

1. **Repeatability.** Keep the sensor still, in mode 0. Re-select mode 0
   from the debugger 10 times (write 0 after briefly writing 1).
   - Each re-select adds 1 to `offset_cal`.
   - `ox/oy/oz` should agree within ±16 counts. That is the datasheet's
     ±1 mG SET/RESET repeatability at 16384 counts/G. Record the actual
     spread.
2. **Offset removal.** Rotate the sensor 180° about Z and compare X and Y
   before and after. With the offset subtracted, they should be equal and
   opposite to within noise.
3. **Saturation and SET recovery.** Bring a strong magnet (> 10 G) close,
   so that `saturated` rises. Remove it.
   - In mode 0, the readings may stay shifted.
   - Re-select mode 0, or run mode 1. The readings should return to their
     pre-magnet values.

## SET effectiveness at pulse interval 1

In mode 1 with `g_mag_sr_interval = 1`, there are 1000 SET pulses per
second, drawn from the 10 µF CAP capacitor. The datasheet gives no
recharge time.

1. Magnetise the sensor with a magnet, remove it, and note how quickly
   the readings recover with interval 1 compared with interval 50.
2. On a scope, check the VDD/CAP ripple at each pulse.

If interval 1 is less effective than interval 50, raise the lower clamp
on `g_mag_sr_interval`.

## Supply current at 1 kHz

Measure the sensor's VDD current in mode 0 and in mode 1 (interval 1).
The estimate, scaled from the datasheet's 32 µA at 7 measurements per
second, is about 4.5 mA in mode 0, plus the SET pulses in mode 1. Record
the measured values.

## Axis orientation

The datasheet defines +X, +Y and +Z after a SET (p. 9, marking
illustration). Point each board axis at magnetic north in turn and record
which sensor axis, and which sign, responds. Document the mapping for
the main-loop application.

## TX interrupt economy (U-13, D-3a; Rev 2.10, 2.12)

Use the `diag` build, echo mode. Read the `tx_irq=` field on the `L0` and
`L1` status lines.

1. **Isolated bytes.** Run `latency_stimulus.py`, one byte per ~1 ms.
   - `tx_irq` should not increase. Each echo runs as its own DMA transfer,
     which the next write retires without an interrupt.
2. **Saturated echo.** Run `soak_test.py --mode echo`.
   - `tx_irq` should stay far below the byte rate (≪ 92,000/s per UART).
   - Record the rate. Soak results must be unchanged (byte-exact, no
     rejects).
3. **Bulk.** Run `diag-pattern` with `soak_test.py --mode pattern`.
   - `tx_irq` rises by roughly one per transfer chain.
   - V-4 gaps are unchanged.

## Magnetometer after aborts (M-7a, Rev 2.12)

This checks that the DREQ counter resynchronisation keeps samples intact
after I2C aborts.

1. With the sensor still, note `x/y/z` on the `MAGS` line.
2. Run the V-6 disconnect test, and then the SDA-short test, 20 times each.
3. After each reconnect or release:
   - the first **valid** sample's `x/y/z` must match the noted values to
     within noise. Byte-shifted data would be off by orders of magnitude;
   - its Status byte (`r.s.status`, or the 9th byte on the analyzer) must be
     `0x01`;
   - the `MAG` line must show `timeout` not climbing once the bus is
     healthy. A climbing count would mean a starved command channel.
