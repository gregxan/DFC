# XIAO RP2350 Firmware Requirements: Dual UART + MMC5983MA

**Revision 2.10**. The changes since Revision 2 are listed in sections 11 to 20.

## 1. Scope

Firmware for a Seeed XIAO RP2350 that:

- Moves data on two UARTs at 921,600 baud with the lowest achievable latency, to the numeric targets in section 4.5.
- Samples an MMC5983MA magnetometer at 1 kHz over I2C.
- Exposes all data to a single bare-metal main loop without ever blocking it.
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

## 3. Execution Model

- **EX-1** The application shall be a single bare-metal main loop. It is the only consumer of the RX queues and magnetometer data, and the only producer for the TX queues.
- **EX-2** No function called from the main loop shall block, busy-wait, or poll for completion of any UART, I2C, or DMA operation.
- **EX-3** Critical sections (interrupts disabled) are allowed only around fixed-size copies, pointer/index updates, or counter read-and-clear, each lasting at most 2 µs.
- **EX-4** All interrupt handlers shall have a bounded execution time of at most 5 µs.
- **EX-5** Interrupt priority shall be: UART-related handlers highest (TX DMA completion, UART error), then magnetometer transaction completion (I2C STOP detect, M-12) and I2C abort (equal priority), then the magnetometer timer. *(Amended in Rev 2.1.)*
- **EX-6** Interrupt handlers and queue access functions shall execute from RAM, not flash (satisfied by B-18).
- **EX-7** Each RX read, TX write, and read-latest call shall take at most 2 µs plus payload copy time.
- **EX-8** The maximum main-loop iteration time (D-4) shall be at most 100 µs. This gives a worst-case in-application RX latency of about 120 µs.

## 4. UART Requirements

### 4.1 Configuration

- **U-1** Both UARTs shall run at 921,600 baud, 8 data bits, no parity, 1 stop bit.
- **U-2** Baud rate error shall be ≤ 0.1%. The expected error at the 150 MHz peripheral clock is about 0.006%.

### 4.2 Receive

- **U-3** Received bytes shall be transferred by DMA directly into a RAM ring buffer, with no CPU involvement per byte.
- **U-4** RX DMA shall run continuously for the life of the application without stopping or needing CPU restarts (e.g. using RP2350 endless transfer mode).
- **U-5** No interrupt shall be in the RX data path. A byte is available to the main loop as soon as DMA has written it, as indicated by the DMA write position.
- **U-6** The RX read function shall be non-blocking. It returns the bytes currently available (possibly zero) and returns immediately.
- **U-7** RX ring size shall be a compile-time parameter, a power of two, aligned as DMA ring mode requires. Default: 8 KB per UART (about 89 ms of saturated traffic).
- **U-8** The 1 ms timer handler (M-6) shall sample each UART's DMA write position and add the advance (modulo ring size) to a per-UART 32-bit total-bytes-written count. This shall add at most 0.5 µs to the timer handler. This bookkeeping is not part of the RX data path (U-5).
- **U-8a** The RX read function shall declare overflow when total bytes written minus total bytes read exceeds the ring size, using modulo-2^32 arithmetic. Overflow shall be detected no later than the first RX read call made 1 ms or more after the overwrite.
- **U-9** On RX overflow, the read function shall discard all unread data, set its read position to the current DMA write position, and increment the per-UART RX overflow counter once. No other action is taken.

### 4.3 Transmit

- **U-10** The TX write function shall be non-blocking. It copies bytes into a per-UART TX ring buffer and returns immediately, except as allowed by U-10a. *(Amended in Rev 2.10.)*
- **U-10a** When TX is completely idle (no DMA transfer outstanding and the ring empty), the write function shall write the leading bytes of the message directly into the UART TX FIFO, up to its free space (32 entries when the FIFO is empty). The rest goes through the ring and DMA (U-12), after those bytes, so order is preserved. The U-11 accept/reject decision is made on the whole message before any byte is written. *(Added in Rev 2.10.)*
- **U-11** If the TX ring lacks space for the entire write, the write shall be rejected in full, the per-UART TX overflow counter incremented, and an error returned. No partial message is queued.
- **U-12** If TX DMA is idle when data is queued in the ring, the write function shall start DMA transmission before returning. A DMA transfer that has finished but has not yet been retired by an interrupt counts as idle: the write function retires it and starts the next. *(Amended in Rev 2.10.)*
- **U-13** While the TX queue holds data, the UART TX FIFO shall be kept non-empty, within the gap limits of U-18. When a TX DMA transfer completes and the queue still holds data, the TX DMA completion handler shall start the next transfer. *(Amended in Rev 2.10.)*
  - Each transfer covers everything queued when it starts, so the completion interrupt is needed only when data is queued behind a running transfer.
  - The completion interrupt shall be enabled exactly when that happens, and disabled when a transfer starts.
  - A finished transfer that no interrupt retired shall be retired, and any data queued behind it started, by the next write or free-space query.
- **U-14** TX ring size shall be a compile-time parameter. Default: 4 KB per UART.

### 4.4 Line Errors

- **U-15** Framing errors, break conditions, and hardware FIFO overruns shall be detected by the UART error interrupt and counted per UART. RX DMA shall continue on error (DMA-on-error disabled), and the affected bytes remain in the RX stream unmarked. No other action is taken.
- **U-15a** After servicing an error interrupt, the handler shall mask that UART's error interrupts. The 1 ms timer handler shall re-enable them. Error counters are therefore lower bounds, capped at one count per error type per UART per millisecond.
- **U-15b** Worst-case CPU load from error interrupts shall be at most 1% (2 UARTs × 1,000 per second × 5 µs).

### 4.5 Latency

- **U-16** RX latency: with a reference main loop that only polls RX, the time from the end of the stop bit to the byte being visible to the main loop shall be at most 3 µs median and at most 20 µs maximum, with all subsystems active.
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
    - Probe and configuration transactions are not counted as missed ticks. A failed probe is counted as a product-ID fault, not as an I2C error.
    - The fault flag is set only by the startup check. Losing the sensor during sampling is handled by M-15/M-16 as before.
- **M-3** The sensor shall be operated in single (triggered) measurement mode at 1 kHz. Register settings: *(Replaced in Rev 2.8.)*
  - Internal Control 1: BW = 11 (0.5 ms measurement). It is the only bandwidth whose measurement time fits a 1 ms cycle, at the cost of 1.2 mG RMS noise. X-inhibit and YZ-inhibit stay 0: the channels are measured in parallel, so inhibiting them saves no time.
  - Internal Control 2 = 0x00: continuous mode, CM_Freq, periodic SET (Prd_set, En_prd_set) all off. Continuous mode runs from the sensor's own oscillator ("1000 Hz typical"). It would drift against the 1 ms tick, causing duplicated or skipped samples and losing the trigger timestamp (M-10). Periodic SET requires continuous mode.
  - Internal Control 0: Auto_SR_en, INT_meas_done_en, TM_T and OTP_Read are never set.
  - Internal Control 3 = 0x00 (self-test and 3-wire SPI off).
- **M-3a** Auto_SR_en shall not be used. The datasheet gives a maximum output rate of 580 Hz at BW = 11 outside continuous mode, consistent with two measurements per output when automatic SET/RESET is active, so it is not expected to sustain 1 kHz. *(Added in Rev 2.8.)*
- **M-4** The sensor shall be conditioned with SET/RESET pulses. At startup, firmware shall perform one RESET → SET cycle, ending in SET so measurements have normal polarity. The ongoing strategy shall be selectable at run time through a global variable, `g_mag_sr_mode`. It is currently changed only from the debugger. *(Replaced in Rev 2.4; automatic mode removed in Rev 2.8.)*
  - **Single cycle** (`MAG_SR_INIT_ONLY` = 0, the initial value): no further pulses. Selecting this mode again at run time performs one more RESET → SET cycle: a RESET in one transaction, then a SET in the next.
  - **Pulse** (`MAG_SR_PULSE` = 1): a SET pulse is inserted into the transaction, before the trigger write, every `g_mag_sr_interval` samples. The default is 50. Values are clamped to 1–100, so a pulse occurs at least every 100 samples.

  A change to either variable shall take effect at the next transaction (within 1 ms). An invalid mode value shall be ignored, and the previous mode remains in use. Leaving a mode partway through a RESET → SET cycle shall still complete the SET. The single-cycle mode provides no ongoing SET; re-selecting it re-calibrates the offset (M-4b).
- **M-4b** Every RESET → SET cycle shall also calibrate the bridge offset, following the datasheet procedure (p. 18). *(Added in Rev 2.9.)*
  - Cycles occur at startup, when single-cycle mode is selected at run time, and after a successful re-probe.
  - Measure once after the RESET (−H + offset) and once after the SET (+H + offset). The offset is their per-axis mean, in the same signed counts as the converted field.
  - A calibration is used only if both measurements completed (Meas_M_Done set) and no abort intervened. Otherwise the previous offset is kept.
  - The offset and its validity flag are returned by the read-latest function (M-13). The main loop subtracts it from every axis. The firmware does not modify the raw sample bytes (M-10).
  - The calibration assumes the field is steady over the ~2 ms between the two measurements.
- **M-5** Every write to Internal Control 0 (0x09) shall include any persistent control bits along with TM_M. With Auto_SR_en not used (M-3a), there are none, so each write carries only its own bit (TM_M, SET or RESET). *(Amended in Rev 2.8.)*

### 5.2 Sampling

- **M-6** A hardware timer shall start one magnetometer transaction every 1 ms (1,000 Hz). The timer shall run for the life of the application, even if the magnetometer fault flag is set, because U-8 and U-15a depend on it.
- **M-6a** The tick period shall be 1,000 µs ± 20 µs, with no cumulative drift (each tick is scheduled from the previous target time, not from the current time).
- **M-7** Each transaction shall run entirely by DMA, using one channel to feed I2C command words and one to receive data. It consists of: *(Amended in Rev 2.8.)*
  - Reading 9 bytes from register 0x00: the 7 field bytes of the measurement triggered by the previous transaction, Tout (0x07, unused), and Status (0x08).
  - Optionally, a SET or RESET write to register 0x09 (M-4).
  - Writing TM_M to register 0x09 to trigger the next measurement.

  The segments are joined by repeated STARTs, with a single STOP at the end:
  `START W(0x00) · RESTART R×9 · [RESTART W(0x09, SET|RESET)] · RESTART W(0x09, TM_M) · STOP`.
  The datasheet (MMC5983MA Rev A, p. 17) states that a data transfer "is terminated by a STOP condition or another START condition". Its example measurement sequence also chains read → START → write without a STOP. It also states that the sensor "initiate[s] a measurement" when it acknowledges the TM_M byte.
- **M-8** Each transaction shall complete within 400 µs of its tick, or 450 µs for a transaction that includes a SET or RESET pulse (M-4). Expected durations are about 375 µs and 445 µs. The trigger write shall finish within 450 µs of the tick, leaving at least 50 µs between measurement completion (trigger + 0.5 ms) and the next read. Both figures are to be confirmed by measurement. *(Amended in Rev 2.4, Rev 2.8.)*
- **M-9** The first transaction after startup, and the first transaction after any aborted or timed-out transaction (M-15, M-16), has no prior measurement, and its data shall be marked invalid. A measurement triggered immediately after a RESET pulse has inverted polarity, and its sample shall also be marked invalid. *(Amended in Rev 2.4.)* A sample whose Status register shows Meas_M_Done = 0 was read before the measurement finished. It shall be marked invalid and counted as "not ready" (D-1). The datasheet says this bit "should be checked before reading the output". *(Amended in Rev 2.8.)*
- **M-10** Each sample shall be stored as:
  - the 7 raw data bytes, plus the Status byte read with them (and the unused Tout byte), *(Status added in Rev 2.8)*
  - a 64-bit microsecond timestamp of when that measurement was triggered. This is the time its trigger (TM_M) write completed, captured at the start of the completion interrupt (M-12) of the transaction that carried the trigger. The expected lag behind the actual trigger is about 3 µs plus interrupt latency. A sample with no prior trigger (M-9) carries the nominal time of the previous tick. *(Amended in Rev 2.6.)*
  - a 32-bit sequence number: the index of the tick whose transaction triggered the measurement, so consecutive samples differ by 1,
  - a valid flag.

  Conversion to 18-bit X/Y/Z values is performed by the main loop.

### 5.3 Double Buffering

- **M-11** Two sample slots shall be used. DMA shall always write into the slot not currently marked latest.
- **M-12** A transaction is complete when the I2C controller detects the STOP condition that ends the trigger write. On that event, the completion interrupt (I2C STOP detect) shall confirm that both magnetometer DMA channels have finished. It shall then publish the new slot as latest with a single atomic 32-bit write. DMA channel completion does not mark the end of a transaction and shall not be used for this purpose. *(Amended in Rev 2.1.)*
- **M-13** The read-latest function shall copy the latest slot inside a critical section of at most 2 µs. It returns the sample, timestamp, sequence number, valid flag, and the current offset calibration and its validity (M-4b). *(Amended in Rev 2.9.)*
- **M-14** Only the latest sample is retained. No history buffer is required.

### 5.4 Errors

- **M-15** On I2C abort (e.g. NACK), the I2C abort handler shall stop both magnetometer DMA channels, clear the abort condition, and increment the I2C error counter if the controller reports any bus-detected cause (e.g. NACK, arbitration loss). An abort whose only cause is the firmware's own timeout abort (M-16) is counted only as a timeout, not as an I2C error. An unexpected STOP that ends a transaction before both DMA channels have finished is also an I2C error. If the abort was caused by arbitration loss or by a timeout abort (M-16), the handler shall also start a bus clear (M-18) as part of completing the abort. The slot shall not be published, and sampling continues on the next tick. *(Amended in Rev 2.2, Rev 2.3.)*
- **M-16** If a transaction is still in progress when the next tick fires, the timer handler shall skip that tick and increment the missed-tick counter. If no abort is already pending, it shall also initiate an abort of both DMA channels and the I2C transfer, without waiting for completion, and increment the timeout counter. A later tick shall start a new transaction only once the channels, the I2C controller, and any bus clear in progress (M-18) are idle. *(Amended in Rev 2.2.)*
- **M-16a** Valid samples shall resume within 4 ms of the tick on which a single hung transaction started. With a persistent bus fault, the abort shall repeat with no effect on UART traffic.
- **M-17** The read-latest function shall report the sample as stale if its timestamp is more than 3 ms old.
- **M-18** An I2C bus clear shall consist of nine open-drain SCL clocks, so a slave left mid-byte releases SDA. *(Added in Rev 2.2.)*
  - A bus clear started from an interrupt handler shall be non-blocking. It shall run without CPU involvement (e.g. a PIO state machine) and shall finish within 200 µs, so it completes before the next tick.
  - It shall not reconfigure, disable, or reset the I2C controller. It only lends the SCL pin to the clock generator and returns it to I2C afterwards.
  - It shall have no effect on UART traffic.
  - A firmware build option may disable bus clearing entirely. With it disabled, M-16a and the V-6 resume requirement are not guaranteed.

## 6. Status and Diagnostics

- **D-1** Firmware shall maintain these 32-bit counters:
  - per UART: RX overflows, TX rejects, framing errors, breaks, hardware overruns
  - magnetometer: I2C errors, missed ticks, timeouts, product ID faults (failed product-ID checks, startup and re-probe), bus clears (startup and post-abort), recoveries (re-probes that cleared the fault flag), not-ready samples (Meas_M_Done clear, M-9), offset calibrations (M-4b), saturated samples (a valid sample with any axis within 72 counts of either end of the 18-bit range; informational only, no action is taken) *(bus clears added in Rev 2.2; product ID fault redefined and recoveries added in Rev 2.7; not-ready added in Rev 2.8; offset calibrations and saturated added in Rev 2.9)*
- **D-2** Counters shall be incremented without blocking and cleared only by the main loop, using an atomic read-and-clear.
- **D-3** Each RX and TX queue shall record a high-water mark (the maximum fill level ever reached). For TX, this covers the ring only. Bytes written directly to the FIFO under U-10a are not included. *(Amended in Rev 2.10.)*
- **D-3a** Firmware shall count TX DMA completion interrupts per UART, for verification of U-13. This count is cumulative and is not cleared. *(Added in Rev 2.10.)*
- **D-4** Firmware shall record the maximum main-loop iteration time in microseconds.
- **D-5** No flag or counter shall trigger an automatic recovery action. All of the following are normal operation, not recovery:
  - aborting a failed or timed-out magnetometer transaction and resuming the schedule (M-15, M-16);
  - the I2C bus clear at startup and on completing an arbitration-loss or timeout abort (M-2, M-15, M-18);
  - re-probing and re-configuring the magnetometer at 5 Hz while its fault flag is set (M-2a, Rev 2.7);
  - resynchronising the RX read position after overflow (U-9).

  Resets, re-initialisation, and retries remain prohibited. *(Amended in Rev 2.2.)*

## 7. Resources

- **R-1** DMA channels: 6 of 16. UART0 RX, UART0 TX, UART1 RX, UART1 TX, I2C command, I2C data.
- **R-2** RAM: about 24 KB for queues with default sizes, plus magnetometer slots and counters.
- **R-3** Peripheral clock shall remain 150 MHz, or baud accuracy shall be re-verified if changed.

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

### 8.3 Windows Path and File Handling

- **B-12** The project shall build from a path with no spaces. Documentation shall recommend a short root, e.g. `C:\dev\<project>`, to avoid the Windows 260-character path limit.
- **B-13** Git shall be configured with `core.longpaths true`. A checked-in .gitattributes shall normalize text files to LF.
- **B-14** All `#include` filenames shall match the case of the files on disk exactly, so the project stays portable to case-sensitive hosts.

### 8.4 Flashing and Debugging

- **B-15** Flashing shall be supported by copying the UF2 to the board's BOOTSEL drive, which needs no driver.
- **B-16** picotool flashing shall be supported. If Windows doesn't bind the RP2350 BOOTSEL interface automatically, installing the WinUSB driver (e.g. with Zadig) shall be documented.
- **B-17** SWD debugging shall be supported with a CMSIS-DAP probe (e.g. Raspberry Pi Debug Probe) and the Raspberry Pi OpenOCD fork. It shall be launchable from VS Code using the checked-in launch.json.

### 8.5 Build Configuration

- **B-18** The binary type shall be copy_to_ram, so all code executes from SRAM.
- **B-19** Debug printing over UART shall be disabled (`pico_enable_stdio_uart(<target> 0)`), since GPIO0/1 are used by UART0.
- **B-20** USB serial shall be disabled in production builds. If enabled for debugging, it uses the built-in Windows USB serial driver (shows up as a COM port), and its interrupt priority shall be below all UART-related interrupts.
- **B-21** Release builds shall use `CMAKE_BUILD_TYPE=Release` with `-g`.
- **B-22** Compiler warnings shall include `-Wall -Wextra -Werror -Wshadow -Wdouble-promotion -Wconversion`. B-22, B-23 and B-24 apply to every C source compiled into the image, including the Pico SDK's (B-25). *(Amended in Rev 2.5.)*
- **B-23** Stack usage shall be checked with `-fstack-usage` and `-Wstack-usage=1024`.
- **B-24** The C standard shall be C11 (`-std=c11`).
- **B-25** Exceptions to B-22 … B-24 are permitted only for third-party (SDK) sources. Each exception shall apply to a single file, relax as little as possible, carry a written justification, and be listed in one place in the build definition. The list shall be reviewed whenever the SDK pin (B-9) changes. Exceptions in Rev 2.5: *(Added in Rev 2.5.)*
  - `pico_runtime_init/runtime_init.c`: compiled with `asm=__asm` defined, because it uses the GNU keyword `asm` once. The file remains strict C11 and no warning is relaxed.
  - `pico_stdio_usb/stdio_usb.c` (diagnostic USB builds only): `-Wsign-conversion` is reported but not treated as an error, because of one warning in SDK 2.2.0.

## 9. Verification

- **V-1** Both UARTs running saturated full-duplex at 921.6k, with the magnetometer at 1 kHz, for at least 10 minutes:
  - zero framing errors,
  - zero RX overflows,
  - zero TX rejects,
  - zero magnetometer sequence gaps,
  - zero I2C errors, missed ticks, and timeouts,
  - zero not-ready magnetometer samples, in both SET/RESET modes, *(added in Rev 2.8)*
  - byte-exact match of all data sent and received on both UARTs.
- **V-2** RX latency shall be measured with a logic analyzer: last stop bit on the RX line to a GPIO toggled when the main loop sees the byte. At least 100,000 measurements shall meet U-16.
- **V-3** TX latency shall be measured from a GPIO toggled at the write call to the start bit on the TX line. At least 100,000 measurements shall meet U-17.
- **V-4** Under continuous queueing for the full 10 minutes of V-1, captured at 20 MS/s or faster, the TX line shall show no gap longer than 25 µs. The number of gaps longer than 1 bit time shall be reported and shall be consistent with the number of TX DMA transfer boundaries (U-18).
- **V-5** Maximum interrupt handler duration and critical-section duration shall be measured and shown to meet EX-3 and EX-4. API call time and main-loop iteration time shall be shown to meet EX-7 and EX-8.
- **V-5a** The magnetometer timestamp (M-10) shall be compared, on a logic analyzer, with the ACK of the trigger byte on SDA/SCL. The measured lag shall be reported. Expected: about 3 µs typical, and under 20 µs maximum with all subsystems active. *(Added in Rev 2.6.)*
- **V-6** Fault injection:
  - Disconnecting the magnetometer increments I2C errors without affecting UART traffic.
  - Shorting SDA to ground for 1 s increases the sum of I2C errors and timeouts by at least 300, causes zero UART errors, and valid samples resume within 4 ms of release.
  - Changing `g_mag_sr_mode` and `g_mag_sr_interval` from the debugger, while data flows, switches strategy within 1 ms. Each strategy produces the expected pulse pattern on the bus, and there are no UART errors or magnetometer sequence gaps. The only invalid sample is the one after each RESET. *(Added in Rev 2.4.)*
  - Powering up with the magnetometer disconnected sets the fault flag, and the product-ID fault count rises by about 5 per second. Connecting the sensor clears the fault flag within 250 ms, after which valid samples flow. There are no UART errors throughout. *(Added in Rev 2.7.)*
  - Re-selecting single-cycle mode from the debugger with the sensor still produces one offset calibration (counter +1). Repeating it 10 times with the sensor still gives offsets within ±16 counts (±1 mG, the datasheet's SET/RESET repeatability) per axis. *(Added in Rev 2.9.)*
  - Placing a strong magnet (> 10 G) near the sensor raises the saturated count. After it is removed, valid readings with the offset subtracted return to their previous values within the datasheet's ±1 mG SET/RESET repeatability, once a SET has been applied (pulse mode, or re-selecting single-cycle mode). *(Added in Rev 2.9.)*
  - Resetting the MCU (e.g. from the debugger) while magnetometer transactions are running, repeated at least 100 times, never leaves the magnetometer fault flag set after boot. *(Added in Rev 2.2.)*
  - Holding one UART's RX line low, or leaving it disconnected, causes no data loss on the other UART and no magnetometer sequence gaps.
  - Deliberately stalling the main loop produces RX overflow counts and high-water marks consistent with the queue size.
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
  - The data DMA finishes at about 230 µs, when the 7th byte arrives, before the trigger write is on the bus.

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
  - The sensor starts measuring when the trigger is written. That happens ~330 µs after the tick, or ~400 µs with a SET/RESET pulse. The tick time was therefore off by a fixed ~0.3–0.4 ms.
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
