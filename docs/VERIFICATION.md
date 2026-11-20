# Verification (section 9)

Every numeric limit in the requirements is an engineering estimate that
these measurements must confirm. Results so far are logged in the next
section. Procedures not listed there have not been run.

## Bench results

**Magnetometer, `diag` build, board and sensor only (Rev 2.25, firmware `eeb7a72`).**

| Check | Result |
|---|---|
| Wiring check, start-up (ID, OTP, calibration), measurement timing | Pass |
| Cold boot: `Last reset` normal, all checks OK | Pass |
| 1 kHz completeness: `not ready` 0 over several minutes, single-cycle mode | Pass |
| Rate changes 1000 / 200 / 100 / 50 Hz: no `stale`, `not ready`, `gaps` or `invalid` at the changes | Pass (after `737b12e`) |
| Noise (mG RMS, x / y / z) | 1000 Hz 0.76 / 1.05 / 1.32; 200 Hz 0.29 / 0.43 / 0.53; 100 Hz 0.21 / 0.32 / 0.41; 50 Hz about the same as 100 Hz. Allow ~10 s of settling after a change |
| SDA / SCL disconnect and reconnect while running (V-6, M-7a) | Pass (after the bus-clear STOP fix, `897909b`) |
| Sensor powered after boot (V-6) | Accepted limitation: needs an MCU reset (see V-6) |
| RESET button, 20 times | Pass |
| Offset removal (180° turn), axis orientation, SDA short to GND, sensor power-cycle at run time (M-2b) | Not run |
| SET/RESET modes, saturation, M-8 durations, V-5a, supply current | Not run (need a debugger, analyzer or meter) |

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
   py C:\dfc\tools\fwstatus.py COM7 --log loopback.log
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
| `MAG` without a sensor | `fault=1`; `no_ack` (and `pid_fault`) rising ~5/s, `wrong_id` not rising; `cause=1`; `i2c_error`, `timeout` and `missed_tick` not rising; `MAGS recovered=0` | the 5 Hz re-probe (M-2a), with **no effect on any UART counter** |
| `MAG` with a sensor | `fault=0`; `valid` rising ~1000/s (~`MAGS rate_hz` per second at other rates, M-3b); `not_ready=0`; `gaps=0`; `CAL offset_valid=1` | normal sampling |

While `fault=1`, `MAG cause=` says why (Rev 2.17). The readable report
spells it out:

| `cause` | Meaning | Look at |
|---|---|---|
| 1 | no answer at address 0x30 | sensor power, SDA/SCL wiring, pull-ups |
| 2 | wrong product ID; `id=` is the byte read | 0x00 or 0xFF: bus or wiring problem. Anything else: a different device at 0x30 |
| 3 | I2C bus error (data NACK, arbitration lost, unexpected STOP, timeout) | a line held low, noise, missing pull-ups |
| 4 | OTP not loaded after the start-up reset | the sensor itself |
| 5 | a start-up configuration write failed | an intermittent connection |
| 6 | run-time fault: persistent not-ready samples (M-2b) | sensor power dropouts |

A UART with nothing wired to its RX shows all zero counters and
`pattern_err=0`, because it receives nothing (the pull-up holds the line
idle).

### Magnetometer wiring check without test equipment (Rev 2.18)

Diagnostic builds test the SDA/SCL wiring at every start-up, using the
RP2350's own pull resistors and a slow address scan. The readable report
shows it under the magnetometer:

```
  Wiring      SDA pulled up, SCL pulled up, no short (boot check)
              answers: none   with SDA/SCL swapped: 0x30
              -> sensor answers only with SDA/SCL swapped: swap the D4/D5
                 wires
```

| Conclusion | What to do |
|---|---|
| sensor answers at 0x30 as wired: wiring OK | wiring is fine; any remaining fault is in the sensor or its configuration |
| answers at 0x30, but SDA/SCL lack external pull-ups | add 4.7 kΩ from SDA and from SCL to 3V3 (the internal pull-ups only work at the scan's slow speed) |
| answers only with SDA/SCL swapped | swap the wires on D4 and D5 (or at the sensor end) |
| nothing answers and no pull-ups seen | the sensor board's VDDIO is not powered (its pull-ups run from VDDIO), or SDA/SCL don't reach the sensor |
| pull-up on one line only | that other line's wire or crimp is open, or on the wrong pin |
| pull-ups present but nothing answers | VDDIO is powered: check CS is tied to VDDIO, VDD is powered, and GND is connected |
| a line held low | that line is shorted to GND, or its pull-up goes to an unpowered VDDIO |
| SDA and SCL are shorted together | look for a solder bridge or two wires on one pin |
| a device answers, but not at 0x30 | a different chip is on the bus |

The check runs only at start-up. After rewiring (with the USB unplugged),
plug back in, or press **R**, and reconnect the terminal.

### Hangs and crashes (Rev 2.23)

In diagnostic builds the yellow user LED blinks about twice a second while
the main loop runs. If the main loop stops for 2 s, the watchdog resets the
board. A processor fault also resets it. Reconnect the terminal: the
**Last reset** line under Baud says what happened. ATTENTION flags it until
the next normal reset.

```
Last reset    WATCHDOG - main loop stopped for 2 s
              Main loop was sending to USB.
              Stuck inside interrupt IRQ 14 (USB controller).
```

| Line | Meaning |
|---|---|
| Stuck inside interrupt IRQ n | that handler never returned and starved the main loop |
| Interrupts kept running | the main loop itself was stuck (in the step named) |
| The tick stopped at the same time | interrupts were disabled, or everything stalled |
| HARD FAULT … PC / LR / CFSR | a crash. Run the `arm-none-eabi-addr2line` command shown, from the repository folder, with the toolchain from the Pico extension (`%USERPROFILE%\.pico-sdk\toolchain\<version>\bin`), to get the function and line |

### Magnetometer start-up and timing check (Rev 2.19)

Below the wiring check, diagnostic builds show what happened at start-up
and how long real measurements take:

```
  Start-up    ID 0x30, status 0x10 (OTP loaded), result OK
              calibration OK (status 0x11)
  Meas. time  800 Hz 0.10 ms   100 Hz 7.50 ms
              800 Hz set before a repeated START 0.72 ms
              Status read alone / after data: 0x11 / 0x10
              -> sensor timing OK (reading the data clears Meas_M_Done, so
                 Status is read first)
```

These are the values from the first bring-up. Expected: well under 1 ms
at 800 Hz (the datasheet says 0.5 ms), about 8 ms at 100 Hz, and the
repeated-START case well under 2 ms. Status read alone shows bit 0 set;
after the data it reads clear on the real part (Rev 2.21). The conclusion
line names the first thing that differs.

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

Use the `diag` build (echo test application, test pins on), or
`release-echo` for release-optimised figures.

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
| `DIAG_T_LOOP` | test applications: main-loop pass, status reporter excluded | < 1 ms (EX-8) |
| `DIAG_T_STATUS` | status reporter (diag builds only) | information only |

The `diag` builds print these in nanoseconds on the `TIM_NS` status line.
The `STAT` line also shows `loop_max_us` (EX-8 evidence, reporter
excluded), `status_max_us` and `tick_overrun`, which must be 0.
The epoch work time of the user application (EX-8) is covered separately
below ("Epoch timing"). The `release` and `release-echo` builds have no USB,
so read them over SWD if you also want the production figures. In the VS Code debug
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
| Power up with the magnetometer unplugged, then plug it in after a few seconds | `MAG fault=1`, `cause=1` (readable report: "no answer at address 0x30") and `no_ack` rising ~5/s. Within 250 ms of plugging in: `fault=0`, `MAGS recovered` +1, `valid` rising ~1000/s. The UARTs stay clean throughout. **Known limitation (bench, Rev 2.25):** if only the sensor's VDD is disconnected at boot and SDA/SCL stay connected, the sensor does not come online when VDD is connected, but an MCU reset brings it up. It is half-powered through the I2C lines and needs the software reset that only the start-up sequence sends. Accepted: power the sensor with the board, or reset after connecting it. |
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

## Rate and noise comparison (M-3b, D-10, Rev 2.25)

Use the `diag` build, with the board and sensor kept still and away from
moving steel and cables carrying current.

1. Read the start-up `Meas. time` line (D-8). The 200 Hz figure must be well
   under 4.6 ms (expected about 3.8 ms). Otherwise 200 Hz will show
   `not_ready`.
2. For each rate, type `r` until the `Rate` line shows it (or run
   `fwstatus.py --rate HZ`). Wait for one report to pass the change, then
   record the `Noise` line over at least 10 reports.

   | Rate | `valid` per report | `Noise` (datasheet) | `not_ready`, `gaps` |
   |---|---|---|---|
   | 1000 Hz | ~1000 | ~1.2 mG per axis | 0 |
   | 200 Hz | ~200 | ~0.6 mG | 0 |
   | 100 Hz | ~100 | ~0.4 mG | 0 |
   | 50 Hz | ~50 | ~0.4 mG (same BW as 100 Hz) | 0 |

3. A change causes no glitch: the report that spans it shows no new `gaps`,
   no rise in `invalid`, and `not_ready` stays 0.
4. In `MAG`, `i2c_error`, `timeout` and `missed_tick` stay 0 at every rate.
   On the analyzer, the transaction carrying a change contains
   `W(0x0A, BW)` before the trigger and lasts ≤ 540 µs (≤ 610 µs if it also
   carries a pulse, M-8).
5. With mode 1 (SET pulses) at 50 Hz, a pulse appears every
   `sr_interval` samples, i.e. once a second at the default 50.

The figures to compare are the measured ones. If noise falls with the
rate as the datasheet says, the configuration holds. If 100 and 50 Hz are no
quieter than 200 Hz, the floor is something else (supply, wiring,
environment), and the faster rate can be kept.

At rates below 1 kHz, the counts that this document gives "per second at
1 kHz" (`valid`, and `i2c_error` with the sensor disconnected) scale with the
rate.

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

Since Rev 2.16, persistent not-ready samples also set the fault flag
(M-2b). If triggered 1 kHz sampling can't complete in time, you'll see
`runtime_fault` and `recovered` both climbing, about once every 15–20 ms,
as the firmware reconfigures and faults again.

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

## Epoch timing (EX-8, EX-10, D-4, D-6; Rev 2.14)

Use the **diag-user** build: your application, with USB status and the epoch
pin. It works with or without UART traffic. For the full-load figure, feed
both UARTs, e.g. with `soak_test.py --mode echo`; the user application
forwards each UART to the other, so cross-connect the host adapters or run
the soak on one port.

1. Watch the `EPOCH` line (`fwstatus.py`):
   - `count` rises by ~1000 per second;
   - `overrun` = **0**;
   - `max_us` < **1000** (EX-8). Expected: tens of µs with the stub handlers;
   - `start_max_us` small. Expected a few µs, up to ~20 µs if UART
     interrupts coincide with a tick.
2. Watch the `PROF` line: each handler's `last/max/avg` in µs. Their sum
   should be close to the `EPOCH` figures.
3. **Optional, logic analyzer.** Capture D8 for at least 10 s. Check:
   - the pulse period is 1000 µs (± tick jitter);
   - the pulse width matches `EPOCH` `last_us`;
   - the longest pulse is < 1000 µs.

## Review follow-ups (Rev 2.15)

1. **Abort on an idle controller (M-16).** The firmware no longer sets
   `IC_ENABLE.ABORT` when the controller is idle, because the datasheet
   doesn't say whether the bit clears in that case. To learn what the
   hardware does (for the record, not required for operation):
   1. In a debug session with the magnetometer idle (`MAG fault=1`, between
      probes), set `IC_ENABLE` bit 1 from the debugger.
   2. Read it back. Record whether it clears, and whether `TX_ABRT` is
      raised.
2. **Clean RX start (U-4a).** With a USB-UART adapter connected and idle,
   reset the board 20 times. After each boot the host must receive nothing
   in echo mode, and `L0`/`L1` must show no `framing` or `break` counts.
   In pattern mode, `pattern_err` must stay 0 from the first report.
3. **Bus clear with an active controller (M-16).** During the V-6 SDA-short
   test, `missed_tick` must stop rising within 4 ms of release every time;
   a stall would show as `missed_tick` rising ~1000/s indefinitely.

## Sensor power-cycle at run time (M-2b, Rev 2.16)

This needs a way to switch the magnetometer's VDD while leaving SDA and SCL
connected, e.g. a jumper in its supply wire.

1. With sampling running normally (`MAG fault=0`, `valid` rising ~1000/s),
   note `runtime_fault`, `recovered`, `not_ready` and the x/y/z values.
2. Remove the sensor's power for about a second, then restore it.
3. While power is off: `i2c_error` rises ~1000/s (NACKs), and `fault`
   stays 0.
4. After power returns:
   - `runtime_fault` +1 and `recovered` +1;
   - `not_ready` rises by roughly 10–15, then stops;
   - `valid` resumes within 50 ms, and x/y/z return to their earlier
     values;
   - `CAL offset_cal` +1 (the recovery re-calibrates);
   - no UART counters change.
5. Repeat 20 times.
