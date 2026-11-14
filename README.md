# XIAO RP2350 — dual UART + MMC5983MA firmware

Bare-metal firmware for the Seeed XIAO RP2350 that implements
*"XIAO RP2350 Firmware Requirements: Dual UART + MMC5983MA"*, Revision 2.12
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
| [docs/REQUIREMENTS.md](docs/REQUIREMENTS.md) | The requirements specification (Rev 2.12), the baseline for this firmware |
| [docs/DESIGN.md](docs/DESIGN.md) | Architecture, interrupt map, requirement traceability, deviations |
| [docs/VERIFICATION.md](docs/VERIFICATION.md) | Wiring and procedures for V-1 … V-7, including a board-only loopback self-test |

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
| Test pins (diag builds only): UART0 RX_SEEN / TX_WRITE | D8 / D9 | 2 / 4 |
| Test pins (diag builds only): UART1 RX_SEEN / TX_WRITE | D10 / D3 | 3 / 5 |

Fit pull-ups to 3.3 V on SDA and SCL if the breakout has none: 4.7 kΩ at
most, 2.7 kΩ recommended. Check that the rise time is ≤ 300 ns (HW-4).
The MMC5983MA also needs the following (HW-5a); most breakouts provide
them, but check the schematic:
- SPI_CS tied to VDDIO;
- a 10 µF low-ESR capacitor on CAP;
- 1 µF on VDD.

Leave MMC5983MA INT unconnected.

The test pins sit on the SPI0 pads, so all three analog inputs (D0–D2) stay
free. They're driven only in the `diag` and `diag-pattern` builds
(`APP_TEST_PINS`). Before moving any pin, read the pin-selection notes at
the top of `src/config.h`. They list the alternative pads for each UART and
I2C, the pins the board itself uses, and the rules.

Flash is set to the board's actual 2 MB. The SDK board file defaults to
4 MB, which `CMakeLists.txt` overrides (B-11a). The UARTs have no flow control, and both RX pins
have their internal pull-ups enabled.

## Setup on Windows (reference: VS Code extension)

1. Install **Git for Windows**, **Python 3.12** and **VS Code**.
2. Turn on long paths and clone into a short path with no spaces (B-12, B-13):
   ```bash
   git config --global core.longpaths true
   ```
   ```bash
   git clone --recurse-submodules <repo-url> C:\dfc
   ```
   If you already cloned without submodules:
   ```bash
   git submodule update --init --recursive
   ```
3. **Install the toolchain and tools with the Raspberry Pi Pico extension.**
   The extension only downloads tools when it creates or imports a project,
   so let it create a throwaway project once:
   1. In VS Code, install the **Raspberry Pi Pico** extension
      (`raspberry-pi.raspberry-pi-pico`) and the **CMake Tools** extension.
   2. Open the Raspberry Pi Pico view (its icon in the left Activity Bar)
      and choose **New C/C++ Project**.
   3. Fill it in like this:
      - Name: anything, e.g. `pico-tools-bootstrap`.
      - Board: **Seeed XIAO RP2350**, or Pico 2 if it isn't listed.
      - Location: a scratch folder such as `C:\scratch` (not this
        repo).
      - SDK version: **2.2.0**.
      - Under advanced options, if shown: toolchain **14.2.Rel1** and
        picotool **2.2.0**.
   4. Click **Create** and wait until the downloads finish. This takes a few
      minutes the first time.
   5. Check that `%USERPROFILE%\.pico-sdk` now contains `sdk`, `toolchain`,
      `picotool`, `tools`, `openocd`, `cmake` and `ninja` folders.
   6. Close and delete the throwaway project. The tools stay installed.
   7. Put the extension's **CMake** and **Ninja** on `PATH`. The extension
      only uses them inside its own projects. List their folders in
      PowerShell:
      ```bash
      Get-ChildItem $env:USERPROFILE\.pico-sdk\cmake, $env:USERPROFILE\.pico-sdk\ninja
      ```
      Add the folders that **contain the `.exe` files**, not their parent
      `cmake` and `ninja` folders. For example:
      - `C:\Users\<you>\.pico-sdk\cmake\v3.31.5\bin` (contains `cmake.exe`)
      - `C:\Users\<you>\.pico-sdk\ninja\v1.12.1` (contains `ninja.exe`;
        there is no `bin` subfolder)

      Use the version folder names the listing showed. To add them: Start →
      "Edit environment variables for your account" → `Path` → Edit → New.
      Then open a **new** terminal (existing ones keep the old `PATH`),
      fully restart VS Code, and check:
      ```bash
      cmake --version
      ```
      ```bash
      ninja --version
      ```
      If you already have CMake ≥ 3.21 and Ninja installed, skip this step.

   This project doesn't use the SDK copy the extension downloads: it builds
   against its own pinned submodule in `lib/pico-sdk`. It needs the
   extension's **toolchain and host tools**, which `cmake/host_tools.cmake`
   finds under `%USERPROFILE%\.pico-sdk` automatically.

   **Don't** use the extension's *Import Project* on this repository. Import
   rewrites `CMakeLists.txt` and `.vscode/` with the extension's own settings,
   which would replace this project's presets, launch configuration and
   tool discovery. If VS Code offers to import or configure the folder for
   the Pico extension, decline.

   Menu names vary slightly between extension versions. If anything differs,
   the goal is simply to end up with the tools in
   `%USERPROFILE%\.pico-sdk`.
4. **Open this repository's folder** in VS Code: *File → Open Folder…* and
   choose the folder that contains `CMakeLists.txt` and `CMakePresets.json`
   (e.g. `C:\dfc`).
5. **Trust the folder.** When VS Code asks *"Do you trust the authors of the
   files in this folder?"*, choose **Yes, I trust the authors**. In
   *Restricted Mode* (shown in the title bar) the CMake Tools and Pico
   extensions are disabled, and none of the commands below exist. If you
   dismissed the prompt, use *Manage → Trust* from the Restricted Mode
   banner, or run **Workspaces: Manage Workspace Trust** from the Command
   Palette.
6. **Select the preset and build.** CMake Tools reads `CMakePresets.json`.
   Use either route:
   - **Command Palette** (**Ctrl+Shift+P**): run **CMake: Select Configure
     Preset** and pick **Release (production)**. Then run **CMake: Build**,
     or press **F7**.
   - **CMake side panel:** click the **CMake** icon (a triangle) in the left
     Activity Bar. Under **Project Status → Configure**, click the preset
     line ("No configure preset selected") and pick **Release
     (production)**. Then click the **Build** button in the same panel.

   Recent CMake Tools versions show these controls in the side panel rather
   than the status bar. The first configure takes a minute or two. The
   output is `build\release\firmware.uf2`.

   If the CMake commands don't appear:
   - check that **CMake Tools** (`ms-vscode.cmake-tools`) is installed and
     enabled;
   - check that the folder is trusted;
   - check that you opened the top folder (the one with `CMakeLists.txt`).

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
