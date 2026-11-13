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
| RX_SEEN test pin | D0 / GPIO26 | D2 / GPIO28 |
| TX_WRITE test pin | D1 / GPIO27 | D3 / GPIO5 |

- MMC5983MA on D4/D5, plus the USB cable for the `diag` builds' status port.

**Host baud accuracy.** Measure each adapter's real bit time with the
analyzer. An adapter that runs faster than the XIAO's 921,659 baud makes an
echo test pile up data in the XIAO's TX queue. `soak_test.py --rate`
(default 0.99) paces the host below line rate to avoid that.

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

Use the `diag` build (echo mode), or `release` with `-DAPP_MODE=echo`, which
is the default.

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
| `DIAG_T_LOOP` | main-loop iteration | 100 µs (EX-8) |

The `diag` builds print these in nanoseconds on the `TIM_NS` status line.
The `release` build has no USB, so read them over SWD. In the VS Code debug
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

## V-6 Fault injection

Use the `diag` build and watch the status port (`py tools\fwstatus.py COM7`)
while UART traffic runs (`soak_test.py`).

| Test | Expect |
|---|---|
| Disconnect the magnetometer (SDA/SCL) | `MAG i2c_error` rises (~1000/s, NACK). L0/L1 counters and the soak data stay clean. |
| Short SDA to GND for 1 s, then release | `i2c_error + timeout` rises by ≥ 300; zero UART errors. After release, `valid` resumes within 4 ms. Capture SDA, SCL and a test pin to time it (see below). `bus_clear` shows how many PIO bus clears ran. |
| Reset the MCU from the debugger (or tap RESET) while running, ≥ 100 times. A script can loop `openocd ... -c "init; reset run; exit"` with random delays. | After every boot, `MAG fault=0` and `valid` rises. The startup bus clear shows as `bus_clear` = 1 right after boot. |
| Hold one UART's RX low (break), or leave it unconnected | That UART's framing/break counts rise, at most 1/ms per type. The other UART's soak data is byte-exact and `MAG gaps` stays 0. |
| Stall the main loop (break in the debugger, or add a temporary `busy_wait_ms(200)` to a test build) | Each UART's `rx_overflow` increments once per overrun episode, and `rx_hwm` = 8192 (the ring size). |

**Timing the resume after the SDA short.** Trigger the analyzer on SDA
rising (the release). Find the first transaction on the bus after it whose
read phase returns non-0xFF data; the sample read by the transaction after
that one is valid. Alternatively, toggle a spare pin in `mag_consume()` when
`r.s.valid` first becomes true again.

## SET/RESET strategies (M-4, Rev 2.4)

Use the `diag` build in a debug session, with the `MAGS` status line on
screen. Set `g_mag_sr_mode` and `g_mag_sr_interval` in *Watch*, or with
OpenOCD `mww`.

1. **Switching.** Watch SDA/SCL on the analyzer. Start in mode 0 (no pulse
   writes). Switch to 2 with an interval of 10: a 4-write transaction
   (read, SET, trigger) appears every 10 ms. Set the interval to 100: every
   100 ms. Switch to 1: no pulse writes, and every trigger byte is `0x21`.
   Switch back to 0: one transaction carries RESET, the next carries SET, and
   `invalid` rises by exactly 1. Throughout, the soak test stays clean and
   `gaps` stays 0.
2. **Pulse transaction duration (M-8).** Measure from the tick (START) to the
   final STOP on a SET or RESET transaction. Expected ~400 µs; the limit is
   400 µs. If it is over, raise it so M-8 can be revisited.
3. **Auto_SR_en effectiveness.** Do this before relying on mode 1.
   1. Hold the sensor still and log Z for 10 s in mode 1.
   2. Apply a strong magnet briefly. This magnetises the sensor and shifts
      the offset.
   3. Remove the magnet and log again.
   4. Repeat in mode 2.

   Mode 1 is effective if the offset returns to its original value, as it
   does in mode 2.
