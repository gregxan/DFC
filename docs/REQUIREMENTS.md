# XIAO RP2350 Firmware Requirements: Dual UART + MMC5983MA

**Revision 2.28**. The changes since Revision 2 are listed in sections 11 to 38.

## 1. Scope

Firmware for a Seeed XIAO RP2350 that:

- Moves data on two UARTs at 921,600 baud with the lowest achievable latency, to the numeric targets in section 4.5.
- Samples an MMC5983MA magnetometer over I2C at 1 kHz, or at 500, 250, 200, 100 or 50 Hz for lower noise or load (M-3b).
- Exposes all data to a single bare-metal main loop without ever blocking it.
- Can be updated in the field over either UART, with A/B firmware slots, a trial boot and automatic fallback (section 6a).
- Keeps a small block of settings, such as the start-up magnetometer rate, in flash across power cycles and updates (section 6b).
- Builds, flashes, and debugs from a Windows host.

## 2. Hardware

- **HW-1** MCU board shall be the Seeed XIAO RP2350.
- **HW-2** UART0 shall use D6 (GPIO0, TX) and D7 (GPIO1, RX).
- **HW-3** UART1 shall use the back pads D12 (GPIO20, TX) and D11 (GPIO21, RX).
- **HW-4** The magnetometer shall connect to I2C1 on D4 (GPIO6, SDA) and D5 (GPIO7, SCL), with pull-ups to 3.3 V of at most 4.7 kΩ (2.7 kΩ recommended by the datasheet) if the breakout has none. Rise time on both lines shall be ≤ 300 ns (datasheet t_r, fast mode), verified on a scope. *(Amended in Rev 2.9.)*
- **HW-5** The MMC5983MA shall be powered at 3.3 V, I2C address 0x30. The INT pin is not used.
- **HW-5a** Per the datasheet (pp. 9–11): *(Added in Rev 2.9.)*
  - SPI_CS (pin 4) shall be tied to VDDIO to select I2C.
  - VDDIO shall be 2.8–3.6 V (3.3 V, the same rail as VDD, is acceptable).
  - A 10 µF low-ESR capacitor shall be placed close to the CAP pin, which supplies the SET/RESET current.
  - VDD shall have a 1 µF decoupling capacitor.
  - A breakout board that already provides these satisfies HW-5a. Its schematic shall be checked.
- **HW-6** Neither UART uses flow control; only TX and RX are connected.
- **HW-6a** The internal pull-up shall be enabled on each UART RX pin (GPIO1, GPIO21) so a disconnected line idles high.
- **HW-7** Firmware shall be written in C using the Pico SDK on the Arm Cortex-M33 cores.
- **HW-8** All firmware shall run on core 0. Core 1 shall remain idle.
- **HW-9** The logic-analyzer test pins (V-2, V-3) shall be on D8, D9 and D10 (GPIO2, 4, 3) and D3 (GPIO5), leaving the board's only analog inputs (D0–D2) free. They shall be driven only in builds that enable them: the diagnostic presets. Production builds leave them untouched. In the user application, D8 instead marks each epoch's work (high while `app_poll()` runs). *(Amended in Rev 2.14.)* Pins used by the board itself (GPIO19, 22, 23, 25, 29) shall not be assigned. *(Added in Rev 2.13.)*

## 3. Execution Model

- **EX-1** The application shall be a single bare-metal main loop. It is the only consumer of the RX queues and magnetometer data, and the only producer for the TX queues. The application code is supplied as `app_init()` / `app_poll()` (`src/app.h`), called from that loop. *(Clarified in Rev 2.14.)*
- **EX-2** No function called from the main loop shall block, busy-wait, or poll for completion of any UART, I2C, or DMA operation. The main loop may wait for the next epoch tick (EX-10), provided interrupts stay enabled: DMA and the interrupt handlers keep receiving and publishing data while it waits. *(Clarified in Rev 2.14.)*
- **EX-3** Critical sections (interrupts disabled) are allowed only around fixed-size copies, pointer/index updates, or counter read-and-clear, each lasting at most 2 µs.
- **EX-4** All interrupt handlers shall have a bounded execution time of at most 5 µs.
- **EX-5** Interrupt priority shall be: UART-related handlers highest (TX DMA completion, UART error), then magnetometer transaction completion (I2C STOP detect, M-12) and I2C abort (equal priority), then the magnetometer timer. *(Amended in Rev 2.1.)*
- **EX-6** Interrupt handlers and queue access functions shall execute from RAM, not flash (satisfied by B-18).
- **EX-7** Each RX read, TX write, and read-latest call shall take at most 3 µs plus payload copy time. The time is the call's own execution: in diagnostic builds, time spent in interrupt handlers that preempt the call (e.g. the USB stack) is measured and excluded. *(Amended in Rev 2.22; limit raised from 2 µs in Rev 2.25.)*
- **EX-8** The main loop shall keep real-time performance: its worst-case execution shall be less than 1 ms (D-4). *(Replaced in Rev 2.14.)*
  - **User application:** the work of one epoch, from the epoch start until `app_poll()` returns, shall be < 1 ms, so the next epoch never starts late. Epochs that do run late are counted (D-1).
  - **Test applications:** one main-loop pass shall be < 1 ms.
- **EX-9** The CPU shall not read or write a peripheral FIFO that a DMA channel is paced against, unless that channel's DREQ credit counter is resynchronised (DBG_CTDREQ) before its next transfer. The RP2350 datasheet (§12.6.4.2) warns that such access "causes the channel and peripheral to become desynchronised, and can cause corruption or loss of data". *(Added in Rev 2.12.)*
- **EX-10** The user application shall run on 1 ms epochs aligned to the tick grid (M-6). After each tick, the main loop calls `app_poll()` once. It consumes the data that arrived during the previous epoch: UART bytes received up to the tick (made available by DMA within U-16), and the newest magnetometer sample, published during the previous epoch (M-8). The test applications (`echo`, `pattern`) run `app_poll()` on every main-loop pass instead, so the latency tests (V-2, V-3) can measure how quickly a byte is noticed. *(Added in Rev 2.14.)*
  - At every magnetometer rate (M-3b) the epochs shall still run every 1 ms. Only the epochs that follow a sampling tick bring a new sample. They see it with the same timing as at 1 kHz: published during the previous epoch, about 0.45 ms after that tick. The application tells them apart by the sample count (M-13a). *(Added in Rev 2.27.)*
  - Epoch-to-epoch jitter from the rate: a sampling tick's handler starts a transaction (about 1–2 µs more than a non-sampling tick), and the transaction's completion interrupt (about 3 µs, M-12) preempts that epoch. Non-sampling epochs have neither. The start latency recorded by D-4 includes this. *(Added in Rev 2.27.)*

## 4. UART Requirements

### 4.1 Configuration

- **U-1** Both UARTs shall run at 921,600 baud, 8 data bits, no parity, 1 stop bit.
- **U-2** Baud rate error shall be ≤ 0.1%. The expected error at the 150 MHz peripheral clock is about 0.006%.

### 4.2 Receive

- **U-3** Received bytes shall be transferred by DMA directly into a RAM ring buffer, with no CPU involvement per byte.
- **U-4** RX DMA shall run continuously for the life of the application without stopping or needing CPU restarts (e.g. using RP2350 endless transfer mode).
- **U-4a** The RX stream shall start clean. Each RX pin shall have its pull-up and UART function selected before that UART's receiver is enabled, and anything received during set-up shall be discarded before the RX DMA starts, with the channel's DREQ counter resynchronised (EX-9). No byte shall appear in the stream that was not on the line. *(Added in Rev 2.15.)*
- **U-5** No interrupt shall be in the RX data path. A byte is available to the main loop as soon as DMA has written it, as indicated by the DMA write position.
- **U-6** The RX read function shall be non-blocking. It returns the bytes currently available (possibly zero) and returns immediately.
- **U-7** RX ring size shall be a compile-time parameter, a power of two, aligned as DMA ring mode requires. Default: 8 KB per UART (about 89 ms of saturated traffic).
- **U-8** The 1 ms timer handler (M-6) shall sample each UART's DMA write position and add the advance (modulo ring size) to a per-UART 32-bit total-bytes-written count. This shall add at most 0.5 µs to the timer handler. This bookkeeping is not part of the RX data path (U-5).
- **U-8a** The RX read function shall declare overflow when total bytes written minus total bytes read exceeds the ring size, using modulo-2^32 arithmetic. Overflow shall be detected no later than the first RX read call made 1 ms or more after the overwrite.
- **U-9** On RX overflow, the read function shall discard all unread data, set its read position to the current DMA write position, and increment the per-UART RX overflow counter once. No other action is taken.

### 4.3 Transmit

- **U-10** The TX write function shall be non-blocking. It copies bytes into a per-UART TX ring buffer and returns immediately. *(U-10a exception withdrawn in Rev 2.12.)*
- **U-10a** *(Withdrawn in Rev 2.12.)* Writing directly into the UART TX FIFO would desynchronise the TX DMA channel's DREQ credit counter (EX-9), and could make the next DMA transfer overfill the FIFO and lose bytes. The lazy completion interrupt (U-13) already removes the interrupt for non-overlapping writes.
- **U-11** If the TX ring lacks space for the entire write, the write shall be rejected in full, the per-UART TX reject counter (D-1) incremented, and an error returned. No partial message is queued.
- **U-12** If TX DMA is idle when data is queued in the ring, the write function shall start DMA transmission before returning. A DMA transfer that has finished but has not yet been retired by an interrupt counts as idle: the write function retires it and starts the next. *(Amended in Rev 2.10.)*
- **U-13** While the TX queue holds data, the UART TX FIFO shall be kept non-empty, within the gap limits of U-18. When a TX DMA transfer completes and the queue still holds data, the TX DMA completion handler shall start the next transfer. *(Amended in Rev 2.10.)*
  - Each transfer covers everything queued when it starts, so the completion interrupt is needed only when data is queued behind a running transfer.
  - The completion interrupt shall be enabled exactly when that happens, and disabled when a transfer starts.
  - A finished transfer that no interrupt retired shall be retired, and any data queued behind it started, by the next write or free-space query.
- **U-14** TX ring size shall be a compile-time parameter. Default: 4 KB per UART.

### 4.4 Line Errors

- **U-15** Framing errors, break conditions, and hardware FIFO overruns shall be detected by the UART error interrupt and counted per UART. RX DMA shall continue on error (DMA-on-error disabled), and the affected bytes remain in the RX stream unmarked. No other action is taken.
- **U-15a** After servicing an error interrupt, the handler shall mask all three of that UART's error interrupts (framing, break, overrun) together. The 1 ms timer handler shall re-enable them. Error counters are therefore lower bounds, capped at one count per error type per UART per millisecond. Masking all three loses no per-type counts compared with masking only the type that fired: the UART latches each type in its raw status while masked, and a type that occurred is counted once when the interrupts are re-enabled. *(Clarified in Rev 2.11.)*
- **U-15b** Worst-case CPU load from error interrupts shall be at most 1% (2 UARTs × 1,000 per second × 5 µs).

### 4.5 Latency

- **U-16** RX latency: with a reference main loop that only polls RX, the time from the end of the stop bit to the byte being visible to the main loop shall be at most 3 µs median and at most 20 µs maximum, with all subsystems active. "Visible" means available to the next `link_read()`. When the application consumes it is the application's choice: in the user application, at the next epoch (EX-10). *(Clarified in Rev 2.14.)*
- **U-17** TX latency: with TX idle and a write of at most 64 bytes, the time from the write call to the start bit on the TX line shall be at most 3 µs median and at most 20 µs maximum.
- **U-18** TX gaps: while the TX queue holds data, the gap between a stop bit and the next start bit shall be at most 1 bit time (1.1 µs). The single exception is the boundary where one TX DMA transfer ends and the completion handler starts the next; there the gap shall be at most 25 µs (about 2.3 byte times).

> Note: the 20 µs maxima allow for three 5 µs handlers running back to back plus margin. The 25 µs limit allows for one 2 µs critical section, three other 5 µs UART-priority handlers, the 5 µs completion handler itself, and 3 µs margin, with no credit taken for bytes still in the TX FIFO.

## 5. Magnetometer Requirements

### 5.1 Configuration

- **M-1** I2C1 shall run at 400 kHz.
- **M-2** At startup, firmware shall first perform one I2C bus clear (M-18), then a software reset (Internal Control 1, SW_RST) followed by the datasheet's 10 ms power-on time, then read the Product ID register (0x2F) and expect 0x30. On mismatch or no response, set the magnetometer fault flag and continue running the UARTs. The startup bus clear and probe may block, since they run before the main loop. *(Amended in Rev 2.2, 2.7, 2.8.)*
  - The startup check also requires the Status register's OTP_Rd_Done bit (bit 4) to be set after the software reset, confirming the factory calibration was loaded. If it is clear, the check fails. *(Added in Rev 2.9.)*
  - **M-2a** While the fault flag is set, the timer handler shall re-probe the Product ID every 200 ms (5 Hz), using the magnetometer DMA channels without blocking. *(Added in Rev 2.7.)*
    - On a match, the next tick shall configure the sensor: M-3, Internal Control 2 = 0, then a RESET followed by a TM_M trigger. The post-RESET measurement is the first sample (invalid, M-9) and the RESET half of an offset calibration (M-4b). The first sample transaction carries the SET. The firmware shall then clear the fault flag and resume sampling. *(Amended in Rev 2.9.)*
    - A failed probe or configuration returns to re-probing.
    - Probe and configuration transactions are not counted as missed ticks. A probe, configuration or startup check that is not acknowledged is counted as "no answer". One that reads a product ID other than 0x30 is counted as "wrong ID". Any other bus failure is counted as an I2C error, and a timeout as a timeout. *(Amended in Rev 2.17.)*
    - **M-2c** While the fault flag is set, firmware shall record why (the fault cause): no answer, wrong product ID (with the byte read), I2C bus error, OTP not loaded, configuration failed, or run-time not-ready fault (M-2b). Each new failure updates it, and it is cleared with the fault flag. *(Added in Rev 2.17.)*
    - The startup check uses blocking SDK calls, which report "not acknowledged" without telling address and data NACKs apart. Within 200 ms the first re-probe (M-2a) refines the cause.
    - The fault flag is set by the startup check (M-2) and by the run-time check of M-2b. A sensor that stops answering during sampling (NACK, bus fault) is handled by M-15/M-16 as before, at the full tick rate, and does not set the fault flag. *(Amended in Rev 2.16.)*
  - **M-2b** A sensor that loses power and returns answers at its default settings (8 ms measurements), so its samples are read before they finish (M-9, not ready). The firmware shall detect this and set the fault flag, so that the re-probe (M-2a) reconfigures the sensor. *(Added in Rev 2.16.)*
    - Each not-ready sample adds 2 to a score, and each ready sample subtracts 1 (never below 0). When the score reaches 20, the fault flag is set and a run-time fault is counted (D-1). The values are compile-time parameters; a threshold of 0 disables the check.
    - The first re-probe follows on the next tick, without waiting a full probe period.
    - Expected timing at 1 kHz: detection within about 13 ms of the sensor answering again, and valid samples about 5 ms after that. The score counts samples, so at the slower rates (M-3b) detection takes proportionally longer (about 65 ms at 200 Hz).
    - At 100 and 50 Hz the sensor's default bandwidth is the configured one, so a sensor that power-cycles keeps producing complete measurements and is not detected. Its samples stay valid, but the SET state and offset calibration (M-4b) may no longer match until the next RESET → SET cycle. *(Added in Rev 2.25.)*
    - Isolated not-ready samples decay away and never set the flag.
- **M-3** The sensor shall be operated in single (triggered) measurement mode, one trigger per output sample, at the rate of M-3b. Register settings: *(Replaced in Rev 2.8; amended in Rev 2.25.)*
  - Internal Control 1: BW as given by M-3b for the selected rate. At 1 kHz this is BW = 11 (0.5 ms measurement), the only bandwidth whose measurement time fits a 1 ms cycle, at the cost of 1.2 mG RMS noise. X-inhibit and YZ-inhibit stay 0: the channels are measured in parallel, so inhibiting them saves no time.
  - Internal Control 2 = 0x00: continuous mode, CM_Freq, periodic SET (Prd_set, En_prd_set) all off. Continuous mode runs from the sensor's own oscillator ("1000 Hz typical"). It would drift against the 1 ms tick, causing duplicated or skipped samples and losing the trigger timestamp (M-10). Periodic SET requires continuous mode.
  - Internal Control 0: Auto_SR_en, INT_meas_done_en, TM_T and OTP_Read are never set.
  - Internal Control 3 = 0x00 (self-test and 3-wire SPI off).
- **M-3b** The output rate shall be selectable from 1000, 500, 250, 200, 100 and 50 Hz *(500 and 250 Hz added in Rev 2.27)*. Each rate uses the sensor's own bandwidth setting, with one triggered measurement per output sample. The firmware does not average samples. *(Added in Rev 2.25.)*

  | Rate | Trigger every | BW | Measurement time (datasheet) | Time available (M-8) | Noise (datasheet, RMS) |
  |---|---|---|---|---|---|
  | 1000 Hz | 1 tick | 11 (800 Hz) | 0.5 ms | ~0.6 ms | 1.2 mG |
  | 500 Hz | 2 ticks | 11 (800 Hz) | 0.5 ms | ~1.6 ms | 1.2 mG |
  | 250 Hz | 4 ticks | 10 (400 Hz) | 2 ms | ~3.6 ms | 0.8 mG |
  | 200 Hz | 5 ticks | 01 (200 Hz) | 4 ms | ~4.6 ms | 0.6 mG |
  | 100 Hz | 10 ticks | 00 (100 Hz) | 8 ms | ~9.6 ms | 0.4 mG |
  | 50 Hz | 20 ticks | 00 (100 Hz) | 8 ms | ~19.6 ms | 0.4 mG |

  - The rate at startup is a build option (`MAG_RATE_DEFAULT_HZ`, default 1000), unless a rate is saved in the non-volatile settings (NV-11, *added in Rev 2.28*). At run time the application sets it with `mag_set_rate_hz(hz)`, which returns false and changes nothing for an unsupported rate *(added in Rev 2.27)*. `mag_rate_hz()` returns the rate in effect. The underlying global, `g_mag_rate_hz`, remains for the debugger. Diagnostic builds also step through the rates from the USB console (B-20).
  - A change shall take effect at the next sample transaction, which writes the new BW (Internal Control 1) after its read and before its trigger write. The sample read in that transaction belongs to the old rate and is valid. If the transaction is aborted, the next one writes BW again. The sample triggered right after a BW write (rate change or re-probe configuration) is read at least 2 ticks later, because the bench showed the first measurement after a BW write taking 0.72 ms. At 1 kHz this gives one 2 ms step in `seq`, recorded in the sample's period (M-10), so it is not a gap.
  - An unsupported value shall be ignored, and the previous rate remains in use.
  - The re-probe configuration (M-2a) uses the rate selected at that time.
  - 500 Hz has the same noise as 1000 Hz (same bandwidth): it halves the load, not the noise. At 50 Hz the noise is the same as at 100 Hz, because BW = 00 is the sensor's slowest setting.
  - Continuous mode remains off at every rate (M-3).
- **M-3a** Auto_SR_en shall not be used. The datasheet gives a maximum output rate of 580 Hz at BW = 11 outside continuous mode, consistent with two measurements per output when automatic SET/RESET is active, so it is not expected to sustain 1 kHz. *(Added in Rev 2.8.)*
- **M-4** The sensor shall be conditioned with SET/RESET pulses. At startup, firmware shall perform one RESET → SET cycle, ending in SET so measurements have normal polarity. The ongoing strategy shall be selectable at run time through a global variable, `g_mag_sr_mode`. It is currently changed only from the debugger. *(Replaced in Rev 2.4; automatic mode removed in Rev 2.8.)*
  - **Single cycle** (`MAG_SR_INIT_ONLY` = 0, the initial value): no further pulses. Selecting this mode again at run time performs one more RESET → SET cycle: a RESET in one transaction, then a SET in the next.
  - **Pulse** (`MAG_SR_PULSE` = 1): a SET pulse is inserted into the transaction, before the trigger write, every `g_mag_sr_interval` samples. The default is 50. Values are clamped to 1–100, so a pulse occurs at least every 100 samples.

  A change to either variable shall take effect at the next sample transaction (within one sample period, M-3b). The pulse interval counts samples, so at 50 Hz a SET every 50 samples is once a second. An invalid mode value shall be ignored, and the previous mode remains in use. Leaving a mode partway through a RESET → SET cycle shall still complete the SET. The single-cycle mode provides no ongoing SET; re-selecting it re-calibrates the offset (M-4b).
- **M-4b** Every RESET → SET cycle shall also calibrate the bridge offset, following the datasheet procedure (p. 18). *(Added in Rev 2.9.)*
- **M-4c** Whether the field helpers (`mag_field()`, `mag_field16()`) subtract the calibrated offset shall be selectable at run time with `mag_set_offset_apply(bool)`, and readable with `mag_offset_apply()`. The default is on (`MAG_OFFSET_APPLY_DEFAULT`), unless the saved settings say otherwise (NV-11). Off, the helpers return the field as measured, bridge offset included. The offset is calibrated and reported either way. The raw sample never has it removed. Each reading carries the choice in effect (`offset_apply`), so one reading is always converted consistently. *(Added in Rev 2.28.)*
  - Cycles occur at startup, when single-cycle mode is selected at run time, and after a successful re-probe.
  - Measure once after the RESET (−H + offset) and once after the SET (+H + offset). The offset is their per-axis mean, in the same signed counts as the converted field.
  - A calibration is used only if both measurements completed (Meas_M_Done set) and no abort intervened. Otherwise the previous offset is kept.
  - The offset and its validity flag are returned by the read-latest function (M-13). The main loop subtracts it from every axis. The firmware does not modify the raw sample bytes (M-10).
  - The calibration assumes the field is steady between the two measurements: ~2 ms at 1 kHz, two sample periods in general (40 ms at 50 Hz).
- **M-5** Every write to Internal Control 0 (0x09) shall include any persistent control bits along with TM_M. With Auto_SR_en not used (M-3a), there are none, so each write carries only its own bit (TM_M, SET or RESET). *(Amended in Rev 2.8.)*

### 5.2 Sampling

- **M-6** A hardware timer shall tick every 1 ms (1,000 Hz) and start one magnetometer sample transaction every N ticks, where N is the rate's tick count from M-3b (N = 1 at 1 kHz). The timer shall run for the life of the application at 1 kHz, whatever the magnetometer rate, and even if the magnetometer fault flag is set, because U-8 and U-15a depend on it. *(Amended in Rev 2.25.)*
  - A sample transaction starts on the first tick at or after N ticks from the previous one, so a skipped tick (M-6a) delays it by at most one tick.
  - Probe and configuration transactions (M-2a) are not tied to this schedule. After a configuration, the first sample transaction follows N ticks later.
- **M-6a** The tick period shall be 1,000 µs ± 20 µs, with no cumulative drift (each tick is scheduled from the previous target time, not from the current time). If the next target is already in the past when the timer is re-armed, the schedule shall skip forward on the same grid. Each skipped tick is counted as a tick overrun (D-1), not as a magnetometer missed tick. *(Amended in Rev 2.11.)*
- **M-7** Each transaction shall run entirely by DMA, using one channel to feed I2C command words and one to receive data. It consists of: *(Amended in Rev 2.8.)*
  - Reading Status (0x08), then 8 bytes from register 0x00: the 7 field bytes of the measurement triggered by the previous transaction and Tout (0x07, unused). Status is read first because, on the real part, reading the output registers clears Meas_M_Done (found at bring-up, Rev 2.21; the datasheet says the bit stays set until the next measurement). *(Amended in Rev 2.21.)*
  - Optionally, a SET or RESET write to register 0x09 (M-4).
  - Writing TM_M to register 0x09 to trigger the next measurement.

  The segments are joined by repeated STARTs, with a single STOP at the end:
  `START W(0x08) · RESTART R×1 · RESTART W(0x00) · RESTART R×8 · [RESTART W(0x09, SET|RESET)] · RESTART W(0x09, TM_M) · STOP`. *(Rev 2.21.)*
  The datasheet (MMC5983MA Rev A, p. 17) states that a data transfer "is terminated by a STOP condition or another START condition". Its example measurement sequence also chains read → START → write without a STOP. It also states that the sensor "initiate[s] a measurement" when it acknowledges the TM_M byte.
- **M-7a** Before starting each magnetometer transaction (sample, probe or configuration), firmware shall resynchronise the DREQ credit counters of both magnetometer DMA channels (DBG_CTDREQ). An abort flushes the I2C TX and RX FIFOs, and draining leftover RX data reads a DMA-serviced FIFO. Either leaves a counter that no longer matches its FIFO (EX-9), which could let the data channel read an empty RX FIFO (corrupting a sample) or starve the command channel (a stall). *(Added in Rev 2.12.)*
- **M-8** Each transaction shall complete within 470 µs of its tick, or 540 µs for a transaction that includes a SET or RESET pulse (M-4) or a bandwidth write (M-3b), or 610 µs if it includes both. *(Bandwidth write added in Rev 2.25.)* Expected durations are about 445 µs and 515 µs. Status is read about 60 µs after the next tick, so a measurement finishing within 0.5 ms of the trigger (datasheet) is complete when checked; bring-up measured about 0.1 ms. A late measurement is caught by the M-9 check. Both figures are to be confirmed by measurement. *(Amended in Rev 2.4, Rev 2.8, Rev 2.21.)* At the slower rates (M-3b) the measurement has about N ms − 0.4 ms; the tightest is 200 Hz, ~4.6 ms for a 4 ms measurement. D-8 measures it.
- **M-9** The first transaction after startup, and the first transaction after any aborted or timed-out transaction (M-15, M-16), has no prior measurement, and its data shall be marked invalid. A measurement triggered immediately after a RESET pulse has inverted polarity, and its sample shall also be marked invalid. *(Amended in Rev 2.4.)* A sample whose Status register shows Meas_M_Done = 0 was read before the measurement finished. It shall be marked invalid and counted as "not ready" (D-1). The datasheet says this bit "should be checked before reading the output". *(Amended in Rev 2.8.)*
  - Invalid samples shall still be published, so the main loop sees every event in order. *(Added in Rev 2.11.)*
  - Each sample carries a reason code: `OK` (valid), `NO_PRIOR` (first after startup or an abort), `AFTER_RESET` (measured right after a RESET pulse), or `NOT_READY` (Meas_M_Done clear). If several reasons apply, the first in the order `NO_PRIOR`, `NOT_READY`, `AFTER_RESET` is reported.
  - The valid flag is true exactly when the reason is `OK`.
  - An application that needs the last valid value keeps its own copy.
- **M-10** Each sample shall be stored as:
  - the 7 raw data bytes, plus the Status byte read with them (and the unused Tout byte), *(Status added in Rev 2.8)*
  - the sample period in ticks (N of M-3b) of the rate it was triggered at, *(added in Rev 2.25)*
  - a 64-bit microsecond timestamp of when that measurement was triggered. This is the time its trigger (TM_M) write completed, captured at the start of the completion interrupt (M-12) of the transaction that carried the trigger. The expected lag behind the actual trigger is about 3 µs plus interrupt latency. A sample with no prior trigger (M-9) carries the nominal time one sample period before its tick. *(Amended in Rev 2.6, Rev 2.25.)*
  - a 32-bit sequence number: the index of the tick whose transaction triggered the measurement, so consecutive samples differ by the sample period in ticks (1 at 1 kHz), *(amended in Rev 2.25)*
  - a valid flag and the reason code of M-9. *(Reason added in Rev 2.11.)*

  Conversion to 18-bit X/Y/Z values is performed by the main loop.

### 5.3 Double Buffering

- **M-11** Two sample slots shall be used. DMA shall always write into the slot not currently marked latest.
- **M-12** A transaction is complete when the I2C controller detects the STOP condition that ends the trigger write. On that event, the completion interrupt (I2C STOP detect) shall confirm that both magnetometer DMA channels have finished. It shall then publish the new slot as latest with a single atomic 32-bit write. DMA channel completion does not mark the end of a transaction and shall not be used for this purpose. *(Amended in Rev 2.1.)*
- **M-13** The read-latest function shall copy the latest slot inside a critical section of at most 2 µs. It returns the sample, timestamp, sequence number, valid flag, and the current offset calibration and its validity (M-4b). *(Amended in Rev 2.9.)* It also returns the sample count (M-13a) as of that sample, copied in the same critical section. *(Amended in Rev 2.27.)*
- **M-13a** Firmware shall count every published sample, valid or invalid, in a 32-bit count readable at any time through `mag_sample_count()`. The count changes exactly when a new sample is published, so comparing it with the previous epoch's value tells whether an epoch brought a new sample. It wraps after 2^32 samples, so compare for inequality. *(Added in Rev 2.27.)*
- **M-14** Only the latest sample is retained. No history buffer is required.

### 5.4 Errors

- **M-15** On I2C abort (e.g. NACK), the I2C abort handler shall stop both magnetometer DMA channels, clear the abort condition, and increment the I2C error counter if the controller reports any bus-detected cause (e.g. NACK, arbitration loss). An abort whose only cause is the firmware's own timeout abort (M-16) is counted only as a timeout, not as an I2C error. An unexpected STOP that ends a transaction before both DMA channels have finished is also an I2C error. If the abort was caused by arbitration loss or by a timeout abort (M-16), the handler shall also start a bus clear (M-18) as part of completing the abort. The slot shall not be published, and sampling continues on the next tick. *(Amended in Rev 2.2, Rev 2.3.)*
- **M-16** If a transaction is still in progress when the next tick fires, the timer handler shall skip that tick and increment the missed-tick counter. If no abort is already pending, it shall also initiate an abort of both DMA channels and the I2C transfer, without waiting for completion, and increment the timeout counter. The I2C controller's abort shall be requested only if the controller is active or has commands queued; if it is idle, stopping the DMA channels is sufficient. A later tick shall start a new transaction only once the channels, the I2C controller, and any bus clear in progress (M-18) are idle. Completing a bus clear shall not depend on the state of the I2C controller. *(Amended in Rev 2.2, Rev 2.15.)*
- **M-16a** Valid samples shall resume within 3 ms plus one sample period (4 ms at 1 kHz) of the tick on which a single hung transaction started. *(Amended in Rev 2.25.)* With a persistent bus fault, the abort shall repeat with no effect on UART traffic.
- **M-17** The read-latest function shall report the sample as stale if its timestamp is more than three sample periods old, using the longer of the sample's own period and the current rate's period: 3 ms at 1 kHz, 15 ms at 200 Hz, 30 ms at 100 Hz, 60 ms at 50 Hz. The current rate's period covers the last sample before a slow-down. *(Amended in Rev 2.25.)*
- **M-18** An I2C bus clear shall consist of nine open-drain SCL clocks, so a slave left mid-byte releases SDA, followed by a STOP condition. *(Added in Rev 2.2; STOP added in Rev 2.25.)* After an arbitration loss the I2C controller waits for a STOP before it starts again, so without one a single disturbance (bench: SCL reconnected mid-transfer) left every later transaction hung.
  - A bus clear started from an interrupt handler shall be non-blocking. It shall run without CPU involvement (e.g. a PIO state machine) and shall finish within 200 µs, so it completes before the next tick.
  - It shall not reconfigure, disable, or reset the I2C controller. It only lends the SDA and SCL pins to the clock generator and returns them to I2C afterwards.
  - It shall have no effect on UART traffic.
  - A firmware build option may disable bus clearing entirely. With it disabled, M-16a and the V-6 resume requirement are not guaranteed.

## 6. Status and Diagnostics

- **D-1** Firmware shall maintain these 32-bit counters:
  - per UART: RX overflows, TX rejects, framing errors, breaks, hardware overruns
  - magnetometer: I2C errors, missed ticks, timeouts, no-answer probes and wrong-ID probes (failed product-ID checks at startup and re-probe, M-2a), bus clears (startup and post-abort), recoveries (re-probes that cleared the fault flag), not-ready samples (Meas_M_Done clear, M-9), offset calibrations (M-4b), saturated samples (a valid sample with any axis within 72 counts of either end of the 18-bit range; informational only, no action is taken), run-time faults (M-2b) *(bus clears added in Rev 2.2; product ID fault redefined and recoveries added in Rev 2.7; not-ready added in Rev 2.8; offset calibrations and saturated added in Rev 2.9; run-time faults added in Rev 2.16; product ID faults split into no answer and wrong ID in Rev 2.17)*
  - scheduling: tick overruns (M-6a) *(added in Rev 2.11)*, epoch overruns (EX-8): user epochs that started late because the previous epoch's work ran past its tick, plus epochs skipped entirely *(added in Rev 2.14; skipped epochs included in Rev 2.15)*
- **D-2** Counters shall be incremented without blocking and cleared only by the main loop, using an atomic read-and-clear.
- **D-3** Each RX and TX queue shall record a high-water mark (the maximum fill level ever reached). *(Rev 2.10 note on direct FIFO writes withdrawn with U-10a in Rev 2.12.)*
- **D-3a** Firmware shall count TX DMA completion interrupts per UART, for verification of U-13. This count is cumulative and is not cleared. *(Added in Rev 2.10.)*
- **D-4** Firmware shall record the main loop's execution time in microseconds. *(Amended in Rev 2.11, Rev 2.14.)*
  - **User application:** the epoch work time (last, maximum and average) and the worst epoch start latency after its tick.
  - **Test applications:** the maximum main-loop pass time.

  In diagnostic builds the status reporter's time is excluded, since production builds have no reporter, and is recorded as a separate maximum.
- **D-5** No flag or counter shall trigger an automatic recovery action. All of the following are normal operation, not recovery:
  - aborting a failed or timed-out magnetometer transaction and resuming the schedule (M-15, M-16);
  - the I2C bus clear at startup and on completing an arbitration-loss or timeout abort (M-2, M-15, M-18);
  - re-probing and re-configuring the magnetometer at 5 Hz while its fault flag is set (M-2a, Rev 2.7);
  - setting the magnetometer fault flag on persistent not-ready samples, which leads to that re-probe (M-2b, Rev 2.16);
  - resynchronising the RX read position after overflow (U-9);
  - rebooting into the updater at the application's request, and the updater's own reboots (idle timeout, after an update, trial fallback) (F-7, F-10, F-20). *(Added in Rev 2.26.)*

  Resets, re-initialisation, and retries remain prohibited. *(Amended in Rev 2.2.)*
- **D-7** Diagnostic builds shall check the magnetometer wiring once at startup, before the I2C controller takes the pins, using no test equipment: each line's level with the internal pull-up (held low), each line's rise against the internal pull-down after being driven low (external pull-up present), SDA–SCL short, and an I2C address scan (0x08–0x77) with SDA/SCL as wired and swapped. The results and a one-line conclusion are reported over USB. Production builds shall not include it. *(Added in Rev 2.18.)*
- **D-8** Diagnostic builds shall report the magnetometer's startup results (product ID, Status after the software reset, fault cause, calibration result) and run a measurement-timing experiment after startup: the time from TM_M to Meas_M_Done at 800 Hz and at the default bandwidth with STOP-terminated writes, the same after a bandwidth write followed by a repeated START, and Status read at the end of the 9-byte burst compared with Status read alone. *(Added in Rev 2.19.)* It also times a measurement at BW = 01 (200 Hz), for the 200 Hz rate's margin (M-8). It ends by restoring the selected rate's bandwidth. *(Amended in Rev 2.25.)*
- **D-10** Diagnostic builds with a test application (`echo`, `pattern`) shall report, once per status report, the per-axis RMS deviation of the valid samples received since the previous report, in mG, and how many samples it covers. With the board kept still, this is the sensor noise at the selected rate (M-3b), so the rates can be compared on the bench. It is computed in the main loop from the converted field and does not change the samples. *(Added in Rev 2.25.)*
- **D-9** Diagnostic builds shall capture hangs and crashes:
  - **Watchdog:** a 2 s watchdog, fed by the main loop, resets the board if the main loop stops. Before that, the main loop's position, the interrupt running (if any) and the tick count are kept in watchdog scratch registers 0–3.
  - **Faults:** a HardFault records the faulting PC, LR and CFSR there, then resets.
  - **Report:** after the reset, the status report states what happened.
  - **Heartbeat:** the user LED blinks while the main loop runs.

  This is a diagnostic exception to D-5. Production builds never reset themselves. *(Added in Rev 2.23.)*
  - The watchdog shall be fed only once this capture has armed it, and shall not be armed during a trial boot (F-20). Otherwise feeding would hold off the bootrom's trial watchdog. A watchdog reset with an implausible tick gap (over 10 s) shall not be reported as a hang: that is a BOOTSEL reflash rebooting through the watchdog. *(Added in Rev 2.26.)*
- **D-6** Firmware shall provide section profiling: named code sections can be timed with negligible overhead (last, maximum and average, in µs). These are reported in diagnostic builds. The user application's handlers are profiled this way. *(Added in Rev 2.14.)*

## 6a. Firmware Update

*(Added in Rev 2.26. Design: DESIGN.md sections 4a–4c. Host tool: `tools/dfc_update.py`.)*

### 6a.1 Flash layout and provisioning

- **F-1** The 2 MB flash shall be divided by a partition table (`partition_table.json`): the table at 0 KB (8 KB), slot A (partition 0) at 8 KB, slot B (partition 1, linked to A as an A/B pair) at 1020 KB, each 1012 KB and accepting the `rp2350-arm-s` UF2 family. The last 16 KB shall stay unpartitioned. The build shall produce the table as `pt.uf2` next to `firmware.uf2`.
- **F-2** The same image shall run from either slot. The build shall fail if `firmware.bin` exceeds one slot.
- **F-3** Every `firmware.uf2` shall start with the RP2350-E10 workaround block, aimed at the last flash sector, which is unpartitioned. On A3 and later chips the block is ignored.
- **F-4** A unit shall be provisioned once over the USB BOOTSEL drive: `pt.uf2`, then `firmware.uf2`. On blank slots the first image goes into slot B. An image copied to the BOOTSEL drive shall not be a trial image (F-18): it is permanent at its first boot, and the bootrom erases the other slot's first sector. After provisioning, updates shall use the updater.

### 6a.2 Image identification and versions

- **F-5** Every image shall carry a 32-byte updater info block: a 64-bit magic, the block format, the updater protocol, the firmware version (major.minor.patch), the protocol floor, and a check word over the rest. It shall be kept in every image regardless of what else the linker removes. It shall be found by a 4-byte-step scan across sector boundaries.
- **F-6** Version rules:
  - `DFC_FW_VERSION` (CMake) shall be raised for every release.
  - `UPDATER_PROTOCOL` shall be raised whenever the host protocol changes in a way older tools cannot drive.
  - `UPDATER_PROTOCOL_FLOOR` shall be raised once the tools no longer support an older protocol.

### 6a.3 Entering the updater

- **F-7** The updater shall be entered only by a reboot requested through `updater_request(link, key)`. The function shall check the 64-bit request key (`UPDATER_REQUEST_KEY`) and the link (UART0, UART1, or USB in diagnostic builds only).
  - **Wrong key or link:** it returns false, counts a rejected call, and changes nothing.
  - **Right key and link:** it writes the boot flag, disarms the diagnostic watchdog, and reboots. It does not return.
- **F-8** Boot flag:
  - It shall be held in POWMAN scratch registers 0–3: a separate 64-bit boot key, the link, and a check word.
  - It shall be read and cleared first thing in `main()`, whatever it holds.
  - Only a flag valid in every bit shall start the updater; anything else boots normally.
  - A power cycle or the RESET button clears it.
- **F-9** Triggers:
  - **Diagnostic USB console:** the `!` key.
  - **Generic trigger:** 20 consecutive `!` characters on a UART, recognised by `updater_trigger_scan()`. The user application and the test applications shall act on it on both UARTs, calling `updater_request(link, UPDATER_REQUEST_KEY)`. The run carries no key, so any stream containing it enters the updater, which writes nothing and returns to the firmware after 90 s without a host (F-10).
  - **User application:** it may replace the generic trigger with its own "enter updater" message, calling `updater_request()` with the request key. Without some entry a deployed unit cannot be updated.
- **F-10** In updater mode:
  - The updater shall serve only the link that requested it (the link in the boot flag): UART0, UART1 (921,600 baud 8N1, polled, independent of the link drivers), or USB in diagnostic builds. The other links shall be ignored entirely. A request from UART0 ignores UART1, and the other way round.
  - The trial boot after an update shall come back on the same link.
  - Plain-text status lines shall go only to USB.
  - After 90 s without a valid command, it shall reboot into the current firmware.
  - The fast LED blink shows updater mode.

### 6a.4 Protocol

- **F-11** Framing:
  - **Frame:** `0xA5 0x5A`, command (1 byte), payload length (2 bytes, little-endian), payload, then a CRC-32 (zlib/IEEE) of command, length and payload.
  - **Replies:** a reply sets bit 7 of the command. An error reply is 0xFF with a code byte and a 32-bit detail. The REBOOT reply is also 0xFF, with an empty payload.
  - **Robustness:** bytes outside frames shall be ignored. A frame stalled for 500 ms partway shall be dropped.
  - **Flow:** the host shall wait for each reply before sending the next frame (stop-and-wait).
- **F-12** Commands:
  - **HELLO:** protocol, link in use, running slot, chip revision, flash and slot size, DATA size limits, board ID, build ID, the running image's info block, and the trial state.
  - **BEGIN:** image size and CRC, with optional bypass flags and key (F-21).
  - **DATA:** an offset and data.
  - **FINISH.**
  - **CONFIRM:** during a trial only.
  - **REBOOT.**
  - During a trial only HELLO, CONFIRM and REBOOT shall be accepted.
- **F-13** DATA payloads shall be accepted from 64 to 4096 bytes. A payload shall not cross a 4 KB sector. Each sector shall be assembled in order: a payload at the sector start (re)starts it, one at the next expected byte extends it, and a resend overwrites. An out-of-order payload shall be refused with the expected offset. HELLO advertises the limits.

### 6a.5 Writing and checking

- **F-14** The updater shall write only the slot that is not running, through the bootrom's flash operation in storage addressing, which enforces the partition table's permissions. BEGIN shall erase the target's first sector at once.
- **F-15** Each complete sector shall be erased, programmed and read back. A read-back mismatch shall be refused.
- **F-16** The image's first sector shall be held in RAM and written last, at FINISH, and only once all of the following hold:
  - every sector has arrived;
  - the CRC of the whole image matches;
  - the image has a valid info block;
  - its updater protocol is not below this firmware's floor.

  A refused image therefore never becomes bootable, and the running firmware is untouched.
- **F-17** The host tool shall refuse, before sending anything:
  - an image without an info block;
  - an image whose updater protocol it does not support;
  - unless overridden (F-21): an image whose updater protocol or firmware version is older than the running one.

  It shall report the image's and the running versions.

### 6a.6 Trial boot, confirm and fallback

- **F-18** At FINISH, the updater shall set the try-before-you-buy flag in the image type of the copy it writes. An image whose start block has a hash or signature shall be refused, since patching it would break it. The updater shall then set the boot flag for the same link and do a flash update boot of the new slot.
- **F-19** The new image shall come up in its own updater, under the bootrom's fixed 16.7 s watchdog. The host shall then:
  - check HELLO: the slot just written, on trial, and the same info block as the image sent;
  - send CONFIRM.

  CONFIRM shall make the image permanent (`explicit_buy()`, which erases the old slot's first sector) and reboot into the application.
- **F-20** Fallback:
  - Without CONFIRM, the watchdog shall reboot the board, and the bootrom shall boot the previous firmware.
  - The application shall never confirm itself.
  - Diagnostic builds shall not arm or feed their watchdog while a buy is pending.
  - A user application **should** not arm or feed a watchdog while `updater_trial_pending()` is true. If it does, an unconfirmed image stays up until the next reset, which still restores the previous firmware.

### 6a.7 Deliberate bypasses

- **F-21** Each protection may be switched off only deliberately, with its own host option:

  | Option | Switches off |
  |---|---|
  | `--allow-no-info` | the info block requirement (host and board) |
  | `--allow-unsupported-protocol` | the host's protocol support check |
  | `--allow-below-floor` | the board's protocol floor |
  | `--allow-older-updater` | the older updater protocol check |
  | `--allow-downgrade` | the older firmware version check |
  | `--no-trial` | trial boot, confirm and fallback |

  The host shall list every protection switched off and require a typed `yes` (or `--yes`). Board-side bypasses shall travel as BEGIN flags. The board shall refuse unknown flags and echo the flags it accepted, and the host shall stop if the echo differs. A host that sends no flags gets every protection.
- **F-22** `--no-trial` shall need the 64-bit no-trial key (`UPDATER_NO_TRIAL_KEY`), typed by the user (or given on the command line) and checked by the board in every bit before it writes a permanent image.

### 6a.8 Link interface

- **F-23** A device between the host and the DFC (pass-through) shall:
  - **Content:** pass all 256 byte values unchanged, with no flow control and no inserted or removed bytes.
  - **Throughput:** sustain bursts of one frame at the full line rate (92.16 kB/s at 921,600 baud). A 4096-byte DATA frame is 4109 bytes, or smaller with `--block-size`.
  - **Gaps:** not hold bytes for 500 ms or more within a frame.
  - **Reboots:** keep passing data while the DFC reboots (the link is idle for about a second), including during the 16.7 s trial window.

  Latency between frames only lengthens the update.
- **F-24** The secrets (request key, boot key, no-trial key) are 64-bit constants in `updater.h`, `updater.c` and the host tool. They guard against accidental use, not against an attacker. Authenticated updates would need signed images (RP2350 secure boot) and are not provided.

## 6b. Non-volatile Settings

*(Added in Rev 2.28.)*

- **NV-1** Firmware shall keep a 64-byte settings block in flash that survives power cycles, resets and firmware updates.
- **NV-2** The block shall be stored outside both firmware slots, in two 4 KB sectors of the unpartitioned flash at 2032 K and 2036 K (offsets 0x1FC000–0x1FDFFF). It shall not use the last sector (0x1FF000), at which the RP2350-E10 UF2 block is aimed.
- **NV-3** Each save shall append a record to the next blank 256-byte page. A record holds a 32-bit magic number, a 32-bit sequence number, the data length, a format number, the data and a CRC-32. The current settings are the valid record with the highest sequence number.
- **NV-4** A save shall never erase or overwrite the current record. A sector is erased only when the next save moves into it, and it then holds only older records.
- **NV-5** A save interrupted by a power loss or reset shall leave the previous settings in force. A half-written record or a half-erased sector fails its CRC check and is ignored, and the next save skips it.
- **NV-6** Each save shall be read back and checked. The save function returns 0 on success, or a negative error.
- **NV-7** A save blocks the main loop for about 1 ms, plus a sector erase (typically 45 ms, up to 400 ms) every 16th save. Interrupts and DMA keep running. Missed epochs during a save are expected. Saves shall be made from the main loop only, never from an interrupt.
- **NV-8** Saving settings equal to those already stored shall not write the flash.
- **NV-9** Wear: one sector erase per 16 saves. At the 100,000-cycle flash endurance this allows more than 3 million saves.
- **NV-10** The settings shall be a C struct of exactly 64 bytes (`dfc_settings_t`). New fields shall be added only at the end, from the reserved bytes. A record saved by older firmware is shorter, and the missing bytes read as 0, so every field shall treat 0 as "use the default".
- **NV-11** The fields shall be:
  - the start-up magnetometer rate (16-bit Hz, 0 = `MAG_RATE_DEFAULT_HZ`);
  - the offset choice (M-4c, 8-bit: 0 = `MAG_OFFSET_APPLY_DEFAULT`, 1 = offset removed, 2 = offset left in).

  `app_init()` shall load the settings and apply them (`settings_apply_startup()`) before the 1 ms tick starts, so the first sample uses them. A stored rate that `mag_rate_supported()` rejects, or an unknown offset value, shall be ignored, and the default is used. *(Offset choice added in Rev 2.28.)*
- **NV-12** The application shall save the rate with `settings_save_mag_rate(hz)` and the offset choice with `settings_save_mag_offset_apply(apply)`, for example on client commands. Diagnostic builds shall switch the offset choice with `o` on the USB console and save the current rate and offset choice together with `s`. The report shall show the stored values, the record number, and the result and duration of the last save. The Offset line and the machine `CAL` line (`offset_apply`) shall show the choice in effect.
- **NV-13** Erasing the whole chip (for example `picotool erase`) clears the settings. Updates over the updater and BOOTSEL drag-and-drop of firmware images do not touch them.

## 7. Resources

- **R-1** DMA channels: 6 of 16. UART0 RX, UART0 TX, UART1 RX, UART1 TX, I2C command, I2C data.
- **R-2** RAM: about 24 KB for queues with default sizes, plus magnetometer slots and counters.
- **R-3** Peripheral clock shall remain 150 MHz, or baud accuracy shall be re-verified if changed.
- **R-4** Flash: one image shall fit a 1012 KB slot (F-1, F-2). At Rev 2.26 a diagnostic image is about 82 KB and a release image about 25 KB. *(Added in Rev 2.26.)*

## 8. Build and Toolchain

### 8.1 Host Environment

- **B-1** The project shall build, flash, and debug on Windows 10 or 11 (x64) without WSL, MSYS2, or Cygwin.
- **B-2** The reference setup shall be the Raspberry Pi Pico extension for VS Code, which installs a matched Pico SDK, Arm GNU Toolchain, CMake, Ninja, picotool, and OpenOCD.
- **B-3** A manual setup (individually installed tools on PATH) shall also be documented, with the exact version of each tool pinned.
- **B-4** Host tools (picotool, pioasm) shall be the prebuilt Windows binaries, located through picotool_DIR and pioasm_DIR, so no native host compiler (MSVC or MinGW) is needed.
- **B-5** Required host software: Git for Windows, Python 3, and the tools in B-2.

### 8.2 Build System

- **B-6** The build system shall be CMake (≥ 3.21) with Ninja. The generator shall be set explicitly, since CMake's Windows default is Visual Studio.
- **B-7** Configure and build settings shall be defined in a checked-in CMakePresets.json, so `cmake --preset release` and `cmake --build --preset release` work identically in PowerShell, cmd, and VS Code.
- **B-8** No build step shall depend on bash, make, or other Unix-only shell tools. Any helper scripts shall be CMake scripts or Python.
- **B-9** The Pico SDK shall be pinned to a specific release (≥ 2.0), included as a git submodule, and initialized with `git submodule update --init --recursive` (needed for TinyUSB).
- **B-10** The toolchain shall be the Arm GNU Toolchain (arm-none-eabi-gcc), with the exact version pinned.
- **B-11** The build shall target `PICO_PLATFORM=rp2350-arm-s` and `PICO_BOARD=seeed_xiao_rp2350`.
- **B-11a** The build shall set `PICO_FLASH_SIZE_BYTES` to 2 MB, the XIAO RP2350's actual flash per the Seeed product page. The SDK board file's default of 4 MB would let an image too large for the real flash link successfully. *(Added in Rev 2.13.)*

### 8.3 Windows Path and File Handling

- **B-12** The project shall build from a path with no spaces. Documentation shall recommend a short root, e.g. `C:\<project>`, to avoid the Windows 260-character path limit.
- **B-13** Git shall be configured with `core.longpaths true`. A checked-in .gitattributes shall normalize text files to LF.
- **B-14** All `#include` filenames shall match the case of the files on disk exactly, so the project stays portable to case-sensitive hosts.

### 8.4 Flashing and Debugging

- **B-15** Flashing shall be supported by copying the UF2 to the board's BOOTSEL drive, which needs no driver. With the A/B partition table, the UF2 goes into the slot that is not running and is permanent (F-4). For routine flashing the updater is preferred, and BOOTSEL is kept for provisioning and recovery. *(Amended in Rev 2.26.)*
- **B-16** picotool flashing shall be supported. If Windows doesn't bind the RP2350 BOOTSEL interface automatically, installing the WinUSB driver (e.g. with Zadig) shall be documented.
- **B-17** SWD debugging shall be supported with a CMSIS-DAP probe (e.g. Raspberry Pi Debug Probe) and the Raspberry Pi OpenOCD fork. It shall be launchable from VS Code using the checked-in launch.json.

### 8.5 Build Configuration

- **B-18** The binary type shall be copy_to_ram, so all code executes from SRAM.
- **B-19** Debug printing over UART shall be disabled (`pico_enable_stdio_uart(<target> 0)`), since GPIO0/1 are used by UART0.
- **B-20** USB serial shall be disabled in production builds. If enabled for debugging, it uses the built-in Windows USB serial driver (shows up as a COM port), and its interrupt priority shall be below all UART-related interrupts.
  - Both USB interrupts shall have that priority: the USB controller IRQ and the IRQ in which the SDK runs `tud_task()`. The SDK assumes neither can preempt the other. *(Added in Rev 2.24.)*
  - In diagnostic builds, receiving `~` on the USB console shall reboot the board into the USB bootloader (BOOTSEL) for reflashing, about 100 ms later so a notice reaches the terminal. *(Added in Rev 2.20.)* The firmware shall first disconnect from USB and stay off the bus for 500 ms, so the host sees a clean unplug before the bootloader attaches. *(Added in Rev 2.28: without it, the drive often failed to appear on the bench.)*
  - Diagnostic console keys: `m` machine format, `h` readable format, `r` magnetometer rate (M-3b), `o` offset removal (M-4c), `s` save settings (NV-12), `~` BOOTSEL, `!` the updater on this port (F-9). *(Added in Rev 2.26.)*
- **B-21** Release builds shall use `CMAKE_BUILD_TYPE=Release` with `-g`.
- **B-22** Compiler warnings shall include `-Wall -Wextra -Werror -Wshadow -Wdouble-promotion -Wconversion`. B-22, B-23 and B-24 apply to every C source compiled into the image, including the Pico SDK's (B-25). *(Amended in Rev 2.5.)*
- **B-23** Stack usage shall be checked with `-fstack-usage` and `-Wstack-usage=1024`.
- **B-24** The C standard shall be C11 (`-std=c11`).
- **B-26** The build shall also:
  - generate `pt.uf2` from `partition_table.json` (F-1);
  - check the image against the slot size (F-2);
  - regenerate a build ID (date and time) on every build, shown in the diagnostic report and in HELLO;
  - take the firmware version from `DFC_FW_VERSION` (F-6).

  `DFC_TEST_NO_UPDATER` (off by default) builds an image whose updater never runs, for testing F-20 only. It shall never be shipped. *(Added in Rev 2.26.)*
- **B-25** Exceptions to B-22 … B-24 are permitted only for third-party (SDK) sources. Each exception shall apply to a single file, relax as little as possible, carry a written justification, and be listed in one place in the build definition. The list shall be reviewed whenever the SDK pin (B-9) changes. Exceptions in Rev 2.5: *(Added in Rev 2.5.)*
  - `pico_runtime_init/runtime_init.c`: compiled with `asm=__asm` defined, because it uses the GNU keyword `asm` once. The file remains strict C11 and no warning is relaxed.
  - `pico_stdio_usb/stdio_usb.c` (diagnostic USB builds only): `-Wsign-conversion` is reported but not treated as an error, because of one warning in SDK 2.2.0.

## 9. Verification

- **V-1** Both UARTs running saturated full-duplex at 921.6k, with the magnetometer at 1 kHz (and, for Rev 2.25, once more at each of 200, 100 and 50 Hz), for at least 10 minutes:
  - zero framing errors,
  - zero RX overflows,
  - zero TX rejects,
  - zero magnetometer sequence gaps,
  - zero I2C errors, missed ticks, and timeouts,
  - zero tick overruns, *(added in Rev 2.11)*
  - zero not-ready magnetometer samples, in both SET/RESET modes, *(added in Rev 2.8)*
  - byte-exact match of all data sent and received on both UARTs.
- **V-2** RX latency shall be measured with a logic analyzer: last stop bit on the RX line to a GPIO toggled when the main loop sees the byte. At least 100,000 measurements shall meet U-16.
- **V-3** TX latency shall be measured from a GPIO toggled at the write call to the start bit on the TX line. At least 100,000 measurements shall meet U-17.
- **V-4** Under continuous queueing for the full 10 minutes of V-1, captured at 20 MS/s or faster, the TX line shall show no gap longer than 25 µs. The number of gaps longer than 1 bit time shall be reported and shall be consistent with the number of TX DMA transfer boundaries (U-18).
- **V-5** Maximum interrupt handler duration and critical-section duration shall be measured and shown to meet EX-3 and EX-4. API call time and main-loop execution time shall be shown to meet EX-7 and EX-8. The figures from a diagnostic build (D-4, reporter excluded) are acceptable evidence for EX-8: the epoch work time from `diag-user`, and the loop pass time from `diag` / `diag-pattern`. *(Amended in Rev 2.11, Rev 2.14.)*
- **V-5a** The magnetometer timestamp (M-10) shall be compared, on a logic analyzer, with the ACK of the trigger byte on SDA/SCL. The measured lag shall be reported. Expected: about 3 µs typical, and under 20 µs maximum with all subsystems active. *(Added in Rev 2.6.)*
- **V-6** Fault injection:
  - Disconnecting the magnetometer increments I2C errors without affecting UART traffic.
  - Shorting SDA to ground for 1 s increases the sum of I2C errors and timeouts by at least 300, causes zero UART errors, and valid samples resume within 4 ms of release (at 1 kHz).
  - Changing `g_mag_sr_mode` and `g_mag_sr_interval` from the debugger, while data flows, switches strategy within 1 ms. Each strategy produces the expected pulse pattern on the bus, and there are no UART errors or magnetometer sequence gaps. The only invalid sample is the one after each RESET. *(Added in Rev 2.4.)*
  - Removing and restoring the magnetometer's power while sampling (with the bus still connected) produces one run-time fault and one recovery. Valid samples resume within 50 ms of the sensor answering again, with no UART errors. *(Added in Rev 2.16.)*
  - Powering up with the magnetometer disconnected sets the fault flag, the no-answer count rises by about 5 per second, and the reported fault cause is "no answer" *(Amended in Rev 2.17)*. Connecting the sensor clears the fault flag within 250 ms, after which valid samples flow. There are no UART errors throughout. *(Added in Rev 2.7.)*
  - Re-selecting single-cycle mode from the debugger with the sensor still produces one offset calibration (counter +1). Repeating it 10 times with the sensor still gives offsets within ±16 counts (±1 mG, the datasheet's SET/RESET repeatability) per axis. *(Added in Rev 2.9.)*
  - Placing a strong magnet (> 10 G) near the sensor raises the saturated count. After it is removed, valid readings with the offset subtracted return to their previous values within the datasheet's ±1 mG SET/RESET repeatability, once a SET has been applied (pulse mode, or re-selecting single-cycle mode). *(Added in Rev 2.9.)*
  - Resetting the MCU (e.g. from the debugger) while magnetometer transactions are running, repeated at least 100 times, never leaves the magnetometer fault flag set after boot. *(Added in Rev 2.2.)*
  - Holding one UART's RX line low, or leaving it disconnected, causes no data loss on the other UART and no magnetometer sequence gaps.
  - Deliberately stalling the main loop produces RX overflow counts and high-water marks consistent with the queue size.
- **V-8** A/B layout (F-1 … F-4): install `pt.uf2` and `firmware.uf2` over BOOTSEL. The first image boots from slot B. Further copies alternate slots and stay selected after a power cycle. *(Added in Rev 2.26.)*
- **V-9** Updating (F-7 … F-17), over USB and over UART0 (and UART1 once wired):
  - Enter, HELLO and REBOOT work.
  - A full update completes at DATA sizes 4096, 1024, 256 and 64.
  - An update interrupted midway leaves the previous firmware running.
  - An image without an info block is refused, by the host and (with the host check bypassed) by the board.
  - The 90 s idle timeout returns to the firmware.
  - A power cycle in updater mode boots the firmware.

  Record the update times. *(Added in Rev 2.26.)*
- **V-10** Trial (F-18 … F-20):
  - A normal update is confirmed and stays after a power cycle.
  - `--no-confirm` falls back within about 17 s.
  - A `DFC_TEST_NO_UPDATER` image falls back by itself.
  - A power cycle during a trial restores the previous firmware.

  *(Added in Rev 2.26.)*
- **V-11** Bypasses (F-21, F-22):
  - A wrong no-trial key is refused before anything is sent.
  - With the key, an image without an updater installs permanently, and is recovered over BOOTSEL.
  - With `--allow-no-info` alone, it falls back.

  The host side can be pre-checked against `tools/sim_board.py`. *(Added in Rev 2.26.)*
- **V-7** All verification tools (logic analyzer software, serial terminals, test scripts) shall run on Windows. Test scripts shall be written in Python.

## 10. Revision 2 Changes

- Added: HW-6a, EX-7, EX-8, U-8a, U-15a, U-15b, U-16, U-17, U-18, M-6a, M-16a.
- Replaced: U-8, U-9, U-15, M-8, M-15, M-16.
- Amended: EX-5, U-13, M-6, M-9, D-1, D-5, V-1, V-2, V-3, V-4, V-5, V-6.
- All new numeric limits are engineering estimates and shall be confirmed by the section 9 measurements.

## 11. Revision 2.1 Changes

- **Amended EX-5.** "Magnetometer DMA completion" becomes "magnetometer transaction completion (I2C STOP detect)". The priority order is unchanged.
- **Amended M-12.** A transaction is defined as complete at the I2C STOP after the trigger write. Publication happens in the I2C STOP-detect interrupt, after it checks that both DMA channels have finished.

  Rationale: neither magnetometer DMA channel finishes at the end of the transaction.
  - The command DMA finishes within a few µs, as soon as all command words fit in the I2C TX FIFO.
  - The data DMA finishes when the last read byte arrives, before the trigger write is on the bus. (This was about 230 µs with the 7-byte read of Rev 2.1. The read is 9 bytes since Rev 2.8; see M-8 for current timings.)

  The alternatives were rejected:
  - Publishing on data-DMA completion would publish before the transaction completes.
  - Adding a trailing dummy read costs about 50 µs of M-8 margin and reads a write-only register.

  See docs/DESIGN.md §6.

## 12. Revision 2.2 Changes

- **Added M-18 (I2C bus clear).** Without a bus clear, a slave left driving SDA low by an interrupted read blocks every later START. The RP2350 I2C controller has no hardware bus-clear feature, so M-16a and the V-6 resume requirement could not be guaranteed.
- **Amended M-2.** Perform one bus clear before the Product ID read, so an MCU reset during a read doesn't leave the magnetometer faulted.
- **Amended M-15 and M-16.** A bus clear is part of completing an arbitration-loss or timeout abort. New transactions wait for it to finish.
- **Amended D-1.** Added the bus-clear counter.
- **Amended D-5.** The bus clear is classified as normal operation, alongside aborts and the RX resync. It is not a recovery action.
- **Amended V-6.** Added the repeated MCU-reset test.

Alternatives considered (docs/DESIGN.md §6):
- No bus clear: strict reading of Rev 2 D-5, with V-6 resume left to chance.
- Clearing only when SDA reads low: resume ~4.35 ms, outside M-16a.
- Bit-banging from the tick: violates EX-4 or M-16a.
- PWM with pulse-counting interrupts: more interrupt load, no benefit.
- Switching sensor power: a hardware change and closer to a reset.

## 13. Revision 2.3 Changes

- **Amended M-15.** A timeout abort (M-16) raises the same abort interrupt as a bus error. Under the Rev 2 wording, one hung transaction would therefore be counted both as a timeout and as an I2C error. Each failure is now counted once:
  - **I2C error:** any bus-reported cause, including an unexpected STOP.
  - **Timeout:** the firmware abandoned the transaction.

  If both happen in the same abort (a bus error is reported alongside the firmware's abort), both counters are incremented, because two distinct events occurred.
- V-1 and V-6 are unchanged.

## 14. Revision 2.4 Changes

- **Replaced M-4.**
  - There are three SET/RESET strategies: a single cycle at startup (the default), automatic (Auto_SR_en), and a periodic SET pulse.
  - The strategy is selectable at run time through `g_mag_sr_mode`.
  - The pulse interval is set through `g_mag_sr_interval` (default 50, clamped to 1–100).
  - A RESET → SET cycle runs at startup in every mode.
  - Pulses are always SET, because a measurement after RESET reads with inverted polarity.
- **Amended M-8.** Expected duration of transactions that carry a pulse.
- **Amended M-9.** A sample measured after a RESET pulse is invalid.
- **Amended V-6.** Added the run-time strategy-switch test.

## 15. Revision 2.5 Changes

- **Amended B-22.** B-22, B-23 and B-24 apply to every C source in the image, not only to application code.
  - Measured against SDK 2.2.0: 46 of 47 SDK C files in the production build, and 70 of 72 in the diagnostic build, are already clean under all of the flags.
  - Applying the flags everywhere puts SDK code under the `-Werror` gate and gives `-fstack-usage` data for SDK functions called from interrupt handlers.
- **Added B-25.** Rules for exceptions, and the two current exceptions.

## 16. Revision 2.6 Changes

- **Amended M-10.** The timestamp is the time the trigger write completed, not the scheduled time of the tick.
  - The sensor starts measuring when the trigger is written. That happened ~330 µs after the tick, or ~400 µs with a SET/RESET pulse, at Rev 2.6. (Since Rev 2.8 the figures are ~375 µs and ~445 µs; see M-8.) The tick time was therefore off by a fixed ~0.3–0.4 ms.
  - The time is captured in the existing completion interrupt. The cost is one timer read per sample, about 0.01% of the CPU.
  - Timestamps are no longer exactly on the 1 ms grid. `seq` still identifies the tick.
- **Clarified M-10.** The sequence number is defined as the index of the triggering tick.
- **Added V-5a.** Measure the timestamp lag.

## 17. Revision 2.7 Changes

- **Amended M-2, added M-2a.** While the magnetometer fault flag is set, the product ID is re-probed by DMA at 5 Hz. On success the sensor is configured and sampling resumes. This supports a sensor that powers up late or is connected after boot.
- **Amended D-1.** "Product ID fault" now counts failed product-ID checks (startup and re-probe). Added a recoveries counter.
- **Amended D-5.** The re-probe is classified as normal operation.
- **Amended V-6.** Added the late-connection test.

## 18. Revision 2.8 Changes

These changes follow a review of the MMC5983MA datasheet (Rev A, 4/3/2019) for operation at 1 kHz.
- **Replaced M-3, added M-3a.** Full register settings for 1 kHz:
  - BW = 11, as the only bandwidth that fits;
  - continuous mode, CM_Freq and periodic SET off, because they are not synchronised to the tick;
  - Auto_SR_en not used, because the datasheet's 580 Hz limit is consistent with two measurements per output.
- **Amended M-2.** A software reset and 10 ms wait before the product-ID read, since an MCU reset does not reset the sensor.
- **Amended M-4.** The automatic SET/RESET mode is removed. The modes are now 0 = single cycle and 1 = pulse.
- **Amended M-5.** No persistent bits remain.
- **Amended M-7.**
  - The read is extended to 9 bytes to include the Status register.
  - The repeated-START bus sequence is specified, with its datasheet basis.
- **Amended M-8.** Limits of 400 µs (no pulse) and 450 µs (with pulse), reflecting the longer read.
- **Amended M-9.** Samples read with Meas_M_Done = 0 are invalid.
- **Amended M-10.** The Status byte is stored with the sample.
- **Amended D-1.** Added the not-ready counter.
- **Amended V-1.** Zero not-ready samples, in both modes.

## 19. Revision 2.9 Changes

These changes follow the second datasheet review.
- **Amended HW-4.** Pull-up limit and rise-time verification.
- **Added HW-5a.** SPI_CS to VDDIO, 10 µF on CAP, 1 µF on VDD.
- **Amended M-2.** Check OTP_Rd_Done after the software reset.
- **Amended M-2a.** The re-probe configuration ends with RESET + TM_M, so it yields a calibration.
- **Added M-4b.** Bridge-offset calibration from every RESET → SET cycle, per the datasheet's procedure. It is returned through M-13 for the main loop to subtract.
- **Amended M-13.** Returns the offset.
- **Amended D-1.** Added the offset-calibration and saturated-sample counters.
- **Amended V-6.** Added the calibration and saturation tests.
- **Bench checks.** Section 9 procedures in VERIFICATION.md were added for SET effectiveness at a pulse interval of 1, supply current at 1 kHz, and axis orientation.

## 20. Revision 2.10 Changes

These changes cut TX CPU overhead.
- **Before.** Each write started a DMA transfer whose completion raised an interrupt. With byte-at-a-time traffic on two saturated UARTs, that was up to ~184,000 interrupts per second, about 11% of the CPU.
- **Added U-10a; amended U-10 and U-12.** On an idle line, writes go directly into the 32-entry hardware FIFO, with no copy, no DMA set-up, no interrupt, and the lowest start-bit latency.
- **Amended U-13.** The completion interrupt is enabled only when data is queued behind a running transfer.

Together these remove the interrupt for non-overlapping writes. Bulk traffic still uses one transfer per chain.
- **Amended D-3.** The TX high-water mark covers the ring only.
- **Added D-3a.** TX interrupt counter.

## 21. Revision 2.11 Changes

These changes close the remaining design interpretations.
- **Clarified U-15a.** All three error types are masked together, with no loss of per-type counts.
- **Amended M-6a, D-1 and V-1.** A late tick is counted as a tick overrun, separate from magnetometer missed ticks, and V-1 requires zero.
- **Amended D-4 and V-5.** In diagnostic builds the status reporter is excluded from the loop-time maximum and measured separately, so EX-8 can be verified in any diagnostic run.
- **Amended M-9 and M-10.** Invalid samples are still published, now with a reason code.

## 22. Revision 2.12 Changes

These changes follow the RP2350 datasheet review (datasheet dated 1 October 2026).
- **Added EX-9.** No CPU access to a FIFO that a DMA channel is paced against, without resynchronising the channel's DREQ credit counter (datasheet §12.6.4.2).
- **Withdrew U-10a** (direct TX FIFO write) under EX-9. The lazy completion interrupt (U-13) keeps the interrupt saving.
- **Added M-7a.** Resynchronise both magnetometer DMA channels' DREQ counters before every transaction. This prevents corrupted samples or stalls after I2C aborts.
- **Implementation fixes** from the same review, with no requirement change:
  - RP2350-E5: clear EN before aborting a DMA channel.
  - Disarm the tick alarm before re-arming when skipping ahead.
- **Errata checked** (Appendix D, all 31 items). Only E5 and, in diagnostic builds, E12 apply. E12 is satisfied by `clk_sys` (150 MHz) being more than 10% faster than `clk_usb` (48 MHz).

## 23. Revision 2.13 Changes

These changes follow a check against the Seeed XIAO RP2350 product page.
- **Added HW-9.** The test pins move from D0–D2, the board's only analog inputs, to the SPI0 pads, and are enabled only in diagnostic builds.
- **Added B-11a.** Flash size is set to the board's actual 2 MB.
- **Pin-selection guidance** in `src/config.h`: XIAO pad-to-GPIO map, board-reserved pins, and the alternative pads for each UART and I2C from the RP2350 datasheet GPIO function table.

## 24. Revision 2.14 Changes

- **Application structure.** The application code is now separate from initialisation and the main loop:
  - `src/app_user.c` is the production application;
  - `src/app_test.c` holds the echo and pattern test applications;
  - the CMake option `APP_MODE` selects which is built.
- **Added EX-10.** The user application runs on 1 ms epochs aligned to the tick, consuming the data of the previous epoch.
- **Clarified EX-1, EX-2 and U-16.** Waiting for the epoch tick is allowed with interrupts enabled. "Visible" means available to read.
- **Replaced EX-8.** Real-time performance is defined as worst-case main-loop execution < 1 ms (epoch work for the user application).
- **Amended D-1.** Added the epoch-overrun counter.
- **Amended D-4.** Epoch work time (last, max, average) and start latency.
- **Added D-6.** Section profiling.
- **Amended HW-9.** D8 marks epoch work in the user application.
- **Amended V-5.** Evidence sources.

## 25. Revision 2.15 Changes

These changes follow an independent code review of Rev 2.14.
- **Amended M-16.**
  - The I2C controller's abort is requested only when the controller is active or has commands queued. The datasheet doesn't say whether `IC_ENABLE.ABORT` clears on an idle controller; if it didn't, the magnetometer could never restart.
  - Completing a bus clear no longer depends on the controller's state. A controller that became active during a bus clear could otherwise stall the magnetometer permanently.
- **Added U-4a.** The RX pin is configured before the receiver is enabled, and the RX stream starts clean. Previously the receiver was briefly enabled with no pin selected, which reads as a low line and could insert a spurious first byte.
- **Amended D-1.** The epoch-overrun counter also counts epochs skipped entirely.
- **Wording.**
  - U-11: "TX reject counter".
  - B-12: example path.
  - §11 and §16: superseded timing figures annotated.
  - EX-9/EX-10 and D-5/D-6 put in order.
- **Implementation only, no requirement change:**
  - the TX completion interrupt is enabled only when the running transfer doesn't already cover everything queued (U-13 "exactly");
  - Internal Control 3 is now written explicitly (M-3);
  - the toolchain fallback ignores non-Arm toolchains and warns (B-10);
  - the profiler's run count is 64-bit.
- **Open decision, not changed.** A sensor that power-cycles at run time returns at its defaults and is not reconfigured (M-2a sets the fault flag only at startup). Whether repeated not-ready or NACK samples should enter the fault state is a D-5 question.

## 26. Revision 2.16 Changes

This closes the open decision from the Rev 2.15 review.
- **Added M-2b.** Persistent not-ready samples set the magnetometer fault flag at run time, so the existing re-probe reconfigures a sensor that power-cycled. Previously such a sensor stayed at its defaults until the MCU was reset.
- **Amended M-2a.** The fault flag can now also be set at run time.
- **Amended D-1.** Added the run-time fault counter.
- **Amended D-5.** M-2b is classified as normal operation.
- **Amended V-6.** Added the sensor power-cycle test.
- **Not changed:** NACKs and bus faults still retry at the full tick rate and never set the fault flag. Making them do so would break M-16a (resume within 4 ms) and the V-6 SDA-short test (≥ 300 errors and timeouts in 1 s).

## 27. Revision 2.17 Changes

- **Added M-2c.** The magnetometer fault records its cause, so the status report says whether the sensor isn't answering, answered with the wrong product ID (with the byte read), or hit another bus error, OTP or configuration failure.
- **Amended M-2a and D-1.** The product-ID fault counter is split into "no answer" (address not acknowledged) and "wrong ID". Other bus failures during a probe now count as I2C errors. The USB machine format keeps `pid_fault` as the sum of the two.
- **Amended V-6.** The disconnected-sensor test checks the no-answer count and the fault cause.
- **USB status report (diagnostic builds).** The report is readable by default in any terminal. Sending `m` switches to the machine format and `h` switches back.

## 28. Revision 2.18 Changes

- **Added D-7.** A start-up wiring self-test in diagnostic builds: held-low lines, external pull-ups, an SDA–SCL short, and address scans as wired and swapped. It reports a conclusion such as "swap the D4/D5 wires" or "sensor VDDIO not powered", for bring-up without a meter. It takes about 110 ms before the main loop, and only in diagnostic builds.

## 29. Revision 2.19 Changes

- **Added D-8.** A startup measurement-timing experiment in diagnostic builds. On first hardware bring-up the sensor answered, but every sample was "not ready" and the startup calibration failed. The experiment tells apart: the bandwidth setting not taking effect, register writes followed by a repeated START being ignored (as in the DMA configuration), the TM_M trigger not working, and Status reading wrong at the end of the 9-byte burst.

## 30. Revision 2.20 Changes

- **Amended B-20.** In diagnostic builds, `~` on the USB console reboots into the USB bootloader for reflashing. `fwstatus.py --bootsel` sends it.

## 31. Revision 2.21 Changes

Found at first hardware bring-up with the D-8 experiment: measurements complete (about 0.1 ms at 800 Hz, 7.5 ms at the default), and bandwidth writes take effect whether ended by STOP or a repeated START. But Meas_M_Done reads clear once the output registers have been read. The datasheet says the bit "will remain 1 till next measurement". The 9-byte burst read Status last, so every sample was "not ready", the startup calibration failed, and the M-2b run-time fault fired about 77 times a second.
- **Amended M-7.** Status is read first, then 0x00–0x07. The data DMA destination is now `status, raw[7], tout`.
- **Amended M-8.** The limits rise by 70 µs for the extra address phases.
- **Startup calibration (M-4b)** reads Status on its own before the data.
- **D-8 experiment** compares Status read alone with Status at the end of a burst, each 2 ms after a fresh trigger.

## 32. Revision 2.22 Changes

- **Amended EX-7.** At bring-up, mag_read_latest measured 14.6 µs against the 2 µs limit. In diagnostic builds, every enabled interrupt is now routed through a wrapper that adds up time spent in handlers, nested ones counted once. The API timings subtract it, so they show only the call itself. Production builds keep the plain vector table and are unchanged. The handler timings (V-5) were already measured inside each handler.
- **USB status report (diagnostic builds).**
  - The readable format fits an 80-column terminal: at most 78 characters per line, with long values and conclusions wrapped.
  - ATTENTION lists one item per line and names what is over its limit, with the value, e.g. "mag_read_latest 14600 ns, over the 2000 ns API limit".
  - The timing section names the worst handler and the worst API call.
  - The machine format is unchanged.

## 33. Revision 2.23 Changes

- **Added D-9.** Hang and crash capture in diagnostic builds. During bring-up the board occasionally stopped responding: no output, and `~` was ignored, so the main loop or the USB stack had stopped. The watchdog report now shows whether the main loop was stuck (and where), an interrupt never returned (and which one), or the processor faulted (PC, LR and fault type). The `~` reboot disarms the watchdog first, and writes its notice through stdio rather than calling TinyUSB directly.

## 34. Revision 2.24 Changes

Found at bring-up with D-9: the diagnostic build reset with a HardFault in the USB controller interrupt (IRQ 14) about every 1–2 minutes. CFSR was 0, the signature of the SDK's `panic()`, which ends in a breakpoint. This was the earlier "stops responding".
- **Cause.** B-20 lowered the USB controller IRQ to 0xC0, but the SDK's `tud_task()` IRQ (a spare IRQ) stayed at the default 0x80. `tud_task()` could therefore interrupt the controller handler while it was updating an endpoint, start a transfer on the same endpoint, and trip the SDK's consistency checks (e.g. "ep .. was already available").
- **Amended B-20.** The `tud_task()` IRQ is fixed at SPARE_IRQ_5 (51) and given the same priority as the controller IRQ.
- **Amended D-9.** In diagnostic builds a `panic()` records its message and first two arguments, then resets. The report prints the message.
- **D-7 wiring check.** A sensor caught mid-read by an MCU reset can hold SDA low. The check now clocks SCL nine times and sends a STOP before deciding that SDA is held low.

## 35. Revision 2.25 Changes

At bring-up, lower sample rates gave less noise. This revision makes the rate selectable. The lower noise comes from the sensor's own slower bandwidth settings rather than from averaging in firmware.
- **Added M-3b.** Rates of 1000, 200, 100 and 50 Hz, each with one triggered measurement per sample at BW 11, 01, 00 and 00. The datasheet noise is 1.2, 0.6, 0.4 and 0.4 mG RMS. The rate is set at build time (`MAG_RATE_DEFAULT_HZ`) and at run time through `g_mag_rate_hz`. The bandwidth write is part of the next sample transaction.
- **Amended M-3, M-4, M-4b.** BW follows the rate. Mode changes take effect within one sample period, and the calibration spans two sample periods.
- **Amended M-6.** The tick stays at 1 kHz for the UARTs. A sample transaction starts every N ticks.
- **Amended M-8.** A transaction with a bandwidth write has the same limit as one with a pulse, and one with both has 610 µs. The time available for each rate's measurement is stated.
- **Amended M-10.** Each sample carries its period in ticks. Consecutive sequence numbers differ by it.
- **Amended M-16a, M-17.** The resume and stale limits scale with the sample period. They are unchanged at 1 kHz.
- **Amended M-2b.** Detection is slower at lower rates. At 100 and 50 Hz a power-cycled sensor is not detected, because its default bandwidth is the configured one.
- **Amended D-8, added D-10.** The start-up experiment also times BW = 01. Test-application diagnostic builds report the per-axis noise (RMS over each report) for comparing rates.
- **Amended B-20 (diagnostic builds).** `r` on the USB console steps the rate 1000 → 200 → 100 → 50 → 1000 Hz. `fwstatus.py --rate HZ` sets it.
- **Bench findings (first run).** Measured noise per axis: 1000 Hz 0.76 / 1.05 / 1.32 mG, 200 Hz 0.29 / 0.43 / 0.53 mG, 100 Hz 0.21 / 0.32 / 0.41 mG, 50 Hz the same as 100 Hz. Two transition defects were fixed. A slow-down reported ~590 main-loop passes as stale, so M-17 now uses the longer period. Switching to 1 kHz gave one not-ready sample, because the first measurement after a BW write takes 0.72 ms, so that read now waits 2 ticks. The field also drifts for several seconds after a rate change, most likely self-heating changing with the measurement duty cycle. Allow ~10 s of settling before comparing noise or trusting the offset.
- **Amended M-18 (bench finding).** After SCL was disconnected and reconnected, sampling never resumed. Missed ticks, timeouts, bus clears and stale counts rose without end. The run-time bus clear now ends with a STOP, as the start-up clear already did. Diagnostic builds also report what the magnetometer is waiting on when it cannot restart.
- **Known limitation (bench, accepted).** A sensor whose VDD is connected only after boot, with SDA/SCL already connected, does not come online through the re-probe (M-2a), but an MCU reset brings it up. Partly powered through the I2C lines, it needs the software reset that only start-up (M-2) sends. This is not handled. Power the sensor with the board.
- **Amended EX-7 (bench).** The limit is now 3 µs, up from 2. Measured worst cases under pattern-mode load (both UARTs transmitting at line rate, magnetometer at 1 kHz): link_write 2.6 µs, mag_read_latest 2.05 µs. The excess is most likely interrupt entry and exit overhead (~0.3–0.5 µs per interrupt) that the exclusion cannot remove, rather than the calls themselves.
- **Firmware updater (in progress at the time; specified in Rev 2.26, section 6a).** Step 2: a boot flag in POWMAN scratch 0–3 (64-bit key, link, check word, cleared at once on boot), `updater_request(link, key)`, the diag `!` key, and an updater mode on USB with HELLO, REBOOT and a 90 s idle timeout. Step 3 adds BEGIN, DATA and FINISH:
  - The image goes into the slot that is not running, through the bootrom's partition-checked flash operation.
  - Each 4 KB block is erased, programmed and read back.
  - The image's first sector is written last, and only after every block arrived and the whole-image CRC matched, so a partial image is never bootable.
  - The board then does a flash update boot into the new slot. Trial boot and confirm follow in step 5.
- Build ID (date and time) on the Firmware line and in HELLO. `updater_request()` reboots the board at the application's request. That is not a recovery action under D-5.
- **Not changed:** the 1 kHz tick, UART timing, continuous mode (still off), and Auto_SR_en (still unused, M-3a). Production builds run at 1 kHz unless `MAG_RATE_DEFAULT_HZ` is set.

## 36. Revision 2.26 Changes

The firmware updater, built and bench-tested during Rev 2.25, is now specified.
- **Added section 6a, F-1 … F-24:**
  - A/B flash layout and provisioning.
  - The updater info block and version rules.
  - Entry by reboot only: request key, boot flag, triggers.
  - The protocol, with selectable DATA sizes.
  - Writing and checks, with the first sector written last.
  - Trial boot, confirm through the new image's own updater, and automatic fallback.
  - Deliberate, keyed bypasses.
  - The interface a pass-through device on the link must meet.
- **Added R-4** (flash per slot), **B-26** (pt.uf2, slot-size check, build ID, firmware version, test-only `DFC_TEST_NO_UPDATER`), and **V-8 … V-11**.
- **Amended D-5:** reboots for the updater are normal operation.
- **Amended B-15:** BOOTSEL images go into the inactive slot and are permanent; the updater is preferred for routine flashing.
- **F-10 (bench request):** the updater serves only the requesting link. It was "the first port that delivers a valid frame".
- **Amended B-20 and D-9:** the diagnostic console's keys are listed. The watchdog is fed only when armed and is not armed during a trial. An implausible watchdog record is not reported as a hang.
- **Not covered:** authenticated (signed) updates (F-24).

## 37. Revision 2.27 Changes

- **Amended M-3b.** 500 Hz (BW 11, every 2 ticks) and 250 Hz (BW 10, every 4 ticks, 0.8 mG) added. The application sets the rate with `mag_set_rate_hz()`.
- **Added M-13a, amended M-13.** A count of published samples, through `mag_sample_count()` and in every reading, to tell epochs with a new sample from those without.
- **Amended EX-10.** Epochs run every 1 ms at every rate, and sample epochs keep the 1 kHz timing. The rate-dependent jitter (1–2 µs tick handler, about 3 µs completion interrupt in sample epochs) is stated.

## 38. Revision 2.28 Changes

- **Added section 6b (NV-1 … NV-13).** A 64-byte non-volatile settings block in two reserved flash sectors, written as append-only records with CRC, safe against power loss. Its first field is the start-up magnetometer rate, applied in `app_init()`.
- **Amended M-3b.** The start-up rate can also come from the saved settings (NV-11).
- **Amended B-20.** `~` detaches from USB for 500 ms before entering the bootloader. Keys `o` and `s` added.
- **Added M-4c.** A run-time choice of whether the field helpers remove the calibrated offset. It is also kept in the saved settings (NV-11).
