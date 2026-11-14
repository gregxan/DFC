# XIAO RP2350 — dual UART + MMC5983MA firmware

Bare-metal firmware for the Seeed XIAO RP2350 that implements
*"XIAO RP2350 Firmware Requirements: Dual UART + MMC5983MA"*, Revision 2.9
([docs/REQUIREMENTS.md](docs/REQUIREMENTS.md)):

- **UART0 and UART1 at 921,600 baud.** RX goes by DMA in endless mode, with no
  interrupt in the data path. TX is DMA-driven from a ring buffer.
- **MMC5983MA at 1 kHz on I2C1**, run entirely by DMA and published through a
  double buffer.
- **A single non-blocking main loop on core 0.** Core 1 stays idle.
- **Builds on Windows** with CMake presets and Ninja. It needs no WSL, MSYS2,
  bash or make.

| Document | Contents |
|---|---|
| [docs/REQUIREMENTS.md](docs/REQUIREMENTS.md) | The requirements specification (Rev 2.9), the baseline for this firmware |
| [docs/DESIGN.md](docs/DESIGN.md) | Architecture, interrupt map, requirement traceability, deviations |
| [docs/VERIFICATION.md](docs/VERIFICATION.md) | Wiring and procedures for V-1 … V-7 |

## Repository layout

```
CMakeLists.txt          build definition (B-6 … B-24)
CMakePresets.json       release / diag / diag-pattern / debug presets (B-7)
cmake/host_tools.cmake  finds the toolchain, picotool and pioasm (B-2, B-4)
lib/pico-sdk/           Pico SDK 2.2.0, git submodule (B-9)
src/link.[ch]           UART links: RX DMA ring, TX DMA queue, line errors
src/mag.[ch]            MMC5983MA: DMA transactions, double buffer, aborts
src/tick.[ch]           1 kHz hardware-timer tick
src/diag.[ch]           counters, high-water marks, timing instrumentation
src/status.[ch]         optional USB CDC status lines (diag builds only)
src/main.c              reference main loop (echo or pattern mode)
src/i2c_bus_clear.pio   PIO program: 9 SCL clocks after a bus fault (M-18)
tools/*.py              Windows test scripts (V-1 … V-4)
.vscode/                launch.json (SWD debug), tasks.json, settings
```

## Wiring

| Function | XIAO pad | GPIO |
|---|---|---|
| UART0 TX / RX | D6 / D7 | 0 / 1 |
| UART1 TX / RX | D12 / D11 (back pads) | 20 / 21 |
| I2C1 SDA / SCL → MMC5983MA (3.3 V, addr 0x30) | D4 / D5 | 6 / 7 |
| Test pins: UART0 RX_SEEN / TX_WRITE | D0 / D1 | 26 / 27 |
| Test pins: UART1 RX_SEEN / TX_WRITE | D2 / D3 | 28 / 5 |

Fit pull-ups to 3.3 V on SDA and SCL if the breakout has none: 4.7 kΩ at
most, 2.7 kΩ recommended. Check that the rise time is ≤ 300 ns (HW-4).
The MMC5983MA also needs the following (HW-5a); most breakouts provide
them, but check the schematic:
- SPI_CS tied to VDDIO;
- a 10 µF low-ESR capacitor on CAP;
- 1 µF on VDD.

Leave MMC5983MA INT unconnected. The UARTs have no flow control, and both RX pins
have their internal pull-ups enabled.

## Setup on Windows (reference: VS Code extension)

1. Install **Git for Windows**, **Python 3.12** and **VS Code**.
2. Turn on long paths and clone into a short path with no spaces (B-12, B-13):
   ```bash
   git config --global core.longpaths true
   ```
   ```bash
   git clone --recurse-submodules <repo-url> C:\dev\dfc
   ```
   If you already cloned without submodules:
   ```bash
   git submodule update --init --recursive
   ```
3. In VS Code, install the **Raspberry Pi Pico** extension
   (`raspberry-pi.raspberry-pi-pico`). Use it to install SDK **2.2.0**. It
   installs the matched toolchain (14.2.Rel1), CMake, Ninja, picotool,
   pioasm and OpenOCD under `%USERPROFILE%\.pico-sdk`.
   `cmake/host_tools.cmake` finds those tools there automatically. The
   project itself still builds against the submodule SDK in `lib/pico-sdk`.
4. Open the folder. CMake Tools picks up `CMakePresets.json`. Choose the
   **release** configure preset and build.

### Command line (PowerShell or cmd)

The same commands work in either shell and in VS Code (B-7):

```bash
cmake --preset release
```
```bash
cmake --build --preset release
```

The output is `build\release\firmware.uf2` (and `.elf`, `.bin`, `.dis`). The
presets are:

| Preset | Purpose |
|---|---|
| `release` | Production build: `-O3 -g`, no USB |
| `diag` | Release plus USB CDC status lines, echo mode (V-1, V-2, V-3, V-5, V-6) |
| `diag-pattern` | Release plus USB status, TX counting-pattern mode (V-1, V-4) |
| `debug` | `CMAKE_BUILD_TYPE=Debug`, only for stepping. Don't use it for timing. |

## Manual setup (tools installed individually)

Install these versions. They match what the VS Code extension installs for
SDK 2.2.0 (B-3, B-10):

| Tool | Version | Notes |
|---|---|---|
| Pico SDK | **2.2.0** (submodule, commit `a1438dff1d38bd9c65dbd693f0e5db4b9ae91779`) | already pinned in `lib/pico-sdk` |
| Arm GNU Toolchain | **14.2.Rel1**, Windows-hosted, `arm-none-eabi` | developer.arm.com |
| CMake | **3.31.6** (≥ 3.21 required) | |
| Ninja | **1.12.1** | |
| picotool | **2.2.0**, prebuilt Windows binary | github.com/raspberrypi/pico-sdk-tools / picotool releases |
| pioasm | **2.2.0**, prebuilt Windows binary | github.com/raspberrypi/pico-sdk-tools releases |
| OpenOCD | **0.12.0+dev**, Raspberry Pi fork, prebuilt | github.com/raspberrypi/pico-sdk-tools releases |
| Python | **3.12.x**, plus `pip install -r tools/requirements.txt` (pyserial 3.5) | |
| Git for Windows | **2.47** or later | |

Put `cmake`, `ninja` and the toolchain's `bin` directory on `PATH`. Then point
the build at the prebuilt host tools, so no MSVC or MinGW host compiler is
needed (B-4). Either set environment variables:

```bash
setx PICO_TOOLCHAIN_PATH C:\tools\arm-gnu-toolchain-14.2.rel1
```
```bash
setx picotool_DIR C:\tools\picotool-2.2.0\picotool
```
```bash
setx pioasm_DIR C:\tools\pico-sdk-tools-2.2.0\pioasm
```

…or put the same three as `cacheVariables` in a `CMakeUserPresets.json`
(git-ignored) that inherits from `release`. Each `*_DIR` is the directory
that contains `picotoolConfig.cmake` / `pioasmConfig.cmake`.

## Flashing

- **BOOTSEL drive (B-15, no driver needed).** Hold **B** (BOOT), tap **R**
  (RESET) and release B. Copy `firmware.uf2` to the `RP2350` drive.
- **picotool (B-16).** Put the board in BOOTSEL as above, then run:
  ```bash
  picotool load -x build\release\firmware.uf2
  ```
  If picotool can't see the device, Windows hasn't bound a driver to the
  RP2350 BOOTSEL vendor interface. Run **Zadig**, choose *Options → List All
  Devices*, select **RP2350 Boot (Interface 1)**, pick **WinUSB** and click
  *Install Driver*. Don't replace the driver on the mass-storage interface
  (Interface 0). `picotool load -f` (force reboot into BOOTSEL) only works
  with the `diag` builds, because only they have USB.
- **Debug probe.** Run the VS Code task *Flash with OpenOCD / debug probe*.

## Debugging (B-17)

Connect a Raspberry Pi Debug Probe (CMSIS-DAP) to the XIAO's SWD pads
(SWDIO, SWCLK, GND). In VS Code choose the **Pico Debug (extension
toolchain)** configuration and press F5. Use the second configuration for a
manual setup with `openocd` and `arm-none-eabi-gdb` on `PATH`. The counters
are in `g_diag_ctr[]`, the maximum timings (in CPU cycles at 150 MHz) are
in `g_diag_max_cycles[]`, and the high-water marks are in
`g_diag_rx_hwm[]` / `g_diag_tx_hwm[]`. You can watch them live.

The `diag` builds enable USB serial. It shows up as a standard COM port with
the built-in Windows driver. Its interrupt priority sits below every
UART-related interrupt (B-20). Read it with:

```bash
py tools\fwstatus.py COM7
```

## Using the API from your own main loop

```c
#include "link.h"
#include "mag.h"
#include "diag.h"

uint8_t buf[256];
size_t n = link_read(LINK_0, buf, sizeof buf);    // never blocks, may return 0
if (!link_write(LINK_1, buf, n)) { /* rejected in full, counted */ }

mag_reading_t r;
if (mag_read_latest(&r) && r.s.valid && !r.stale) {
    int32_t xyz[3];
    mag_field(&r, xyz);      // signed counts (16384/G), calibrated offset removed
}

uint32_t ovf = diag_take(DIAG_L0_RX_OVERFLOW);      // atomic read-and-clear
diag_loop_mark();                                    // once per iteration (D-4)
```

The compile-time parameters (ring sizes, SET/RESET defaults, bus clear,
test pins) are in `src/config.h`. You can change the SET/RESET strategy at
run time from the debugger through `g_mag_sr_mode` (0 single cycle, 1
pulse) and `g_mag_sr_interval`. See docs/DESIGN.md §4.
