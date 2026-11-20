"""Reader for the firmware's USB CDC status lines (diag / diag-pattern builds).

The firmware prints a readable report by default, for any serial terminal.
This tool sends 'm' when it connects to switch the firmware to compact
machine lines, which it parses and shows as a dashboard. In machine
format, each second the firmware emits nine lines:

    STAT seq=.. mode=.. t_ms=.. baud0=.. baud1=.. baud_ok=.. loop_max_us=.. status_max_us=.. tick_overrun=..
    L0 rx_overflow=.. tx_reject=.. framing=.. break=.. overrun=.. rx_hwm=.. tx_hwm=.. pattern_err=..
    L1 ...
    MAG fault=.. i2c_error=.. missed_tick=.. timeout=.. pid_fault=.. no_ack=.. wrong_id=.. cause=.. id=0x.. bus_clear=.. ...
    MAGS sr_mode=.. sr_interval=.. recovered=.. not_ready=.. x=.. y=.. z=.. rate_hz=.. rate_req_hz=.. bw=..
         noise_n=.. nx_ug=.. ny_ug=.. nz_ug=..   (noise: test builds only)
    CAL offset_valid=.. offset_cal=.. saturated=.. ox=.. oy=.. oz=..
    WIRE ran=.. sda_low=.. scl_low=.. sda_pullup=.. scl_pullup=.. shorted=.. found=.. mag=.. mag_swapped=.. ...
    BOOT ran=.. id=0x.. status=0x.. cause=.. cal_ok=.. t_fast_us=.. t_slow_us=.. t_rs_us=.. t_200_us=.. ...
    TIM_NS isr_uart0=.. isr_uart1=.. isr_txdma=.. isr_i2c=.. isr_tick=.. crit=.. api_read=.. ...

User application builds (diag-user) add:

    EPOCH last_us=.. max_us=.. avg_us=.. overrun=.. start_max_us=.. count=..
    PROF mag=last/max/avg link0=last/max/avg link1=last/max/avg   (us)

Test application builds (diag, diag-pattern) also show pattern_err on L0/L1
and valid/invalid/gaps/stale on MAG.

Counters are cumulative since boot. Run standalone for a readable dashboard
that redraws each second, with per-report changes (+n) and plain-language
health checks (colour where the console supports it):

    py tools/fwstatus.py COM7
    py tools/fwstatus.py COM7 --scroll       # one report below the next
    py tools/fwstatus.py COM7 --raw          # the firmware's own lines
    py tools/fwstatus.py COM7 --log run.log  # also record every raw line
    py tools/fwstatus.py COM7 --rate 100     # set the magnetometer rate first
                                             # (1000, 200, 100 or 50 Hz)
"""

from __future__ import annotations

import argparse
import os
import sys
import threading
import time

import serial  # pyserial


def parse_line(line: str) -> tuple[str, dict[str, int]] | None:
    parts = line.strip().split()
    if not parts or parts[0] not in ("STAT", "L0", "L1", "MAG", "MAGS", "CAL", "WIRE", "BOOT", "TIM_NS", "EPOCH", "PROF"):
        return None
    values: dict[str, int] = {}
    for p in parts[1:]:
        if "=" in p:
            k, v = p.split("=", 1)
            try:
                values[k] = int(v, 0)
            except ValueError:
                # PROF entries are last/max/avg
                bits = v.split("/")
                if len(bits) == 3 and all(b.isdigit() for b in bits):
                    values[k + "_last"], values[k + "_max"], values[k + "_avg"] = (int(b) for b in bits)
    return parts[0], values


class StatusMonitor:
    """Background reader that keeps the latest value of every status group."""

    def __init__(self, port: str, log_path: str | None = None):
        self._ser = serial.Serial(port, 115200, timeout=0.2)
        self._lock = threading.Lock()
        self._latest: dict[str, dict[str, int]] = {}
        self._first: dict[str, dict[str, int]] = {}
        self._pending: dict[str, dict[str, int]] = {}
        self._report: dict[str, dict[str, int]] = {}  # last complete report
        self._reports = 0
        self._stop = threading.Event()
        self._log = open(log_path, "a", encoding="utf-8") if log_path else None
        # The firmware prints a readable report by default; ask for the
        # machine-readable "GROUP key=value" lines this tool parses.
        self._request_machine()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def send(self, data: bytes) -> None:
        self._ser.write(data)

    def _request_machine(self) -> None:
        try:
            self._ser.write(b"m")
        except serial.SerialException:
            pass

    def _run(self) -> None:
        buf = b""
        while not self._stop.is_set():
            buf += self._ser.read(4096)
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("ascii", "replace")
                if self._log:
                    self._log.write(f"{time.time():.3f} {line.strip()}\n")
                if line.startswith("=====") or line.startswith("\r====="):
                    # A readable-format header: the board missed (or was reset
                    # since) the request for machine format. Ask again.
                    self._request_machine()
                parsed = parse_line(line)
                if parsed:
                    group, values = parsed
                    with self._lock:
                        self._latest[group] = values
                        self._first.setdefault(group, dict(values))
                        if group == "STAT":
                            self._pending = {}
                        self._pending[group] = values
                        # A report ends with TIM_NS (test applications) or
                        # PROF (user application, mode=2).
                        last = "PROF" if self._pending.get("STAT", {}).get("mode") == 2 else "TIM_NS"
                        if group == last and "STAT" in self._pending:
                            self._report = dict(self._pending)
                            self._reports += 1

    def latest(self) -> dict[str, dict[str, int]]:
        with self._lock:
            return {k: dict(v) for k, v in self._latest.items()}

    def first(self) -> dict[str, dict[str, int]]:
        with self._lock:
            return {k: dict(v) for k, v in self._first.items()}

    def wait_for_report(self, timeout: float = 5.0) -> bool:
        """Wait until a new *complete* report (all of its lines) has arrived."""
        end = time.time() + timeout
        with self._lock:
            start = self._reports
        while time.time() < end:
            with self._lock:
                if self._reports != start:
                    return True
            time.sleep(0.05)
        return False

    def report(self) -> dict[str, dict[str, int]]:
        """The most recent complete report."""
        with self._lock:
            return {k: dict(v) for k, v in self._report.items()}

    def close(self) -> None:
        self._stop.set()
        self._thread.join(timeout=1)
        self._ser.close()
        if self._log:
            self._log.close()


def delta(after: dict[str, dict[str, int]], before: dict[str, dict[str, int]], group: str, key: str) -> int:
    return after.get(group, {}).get(key, 0) - before.get(group, {}).get(key, 0)


def format_report(s: dict[str, dict[str, int]]) -> str:
    out = []
    for g in ("STAT", "L0", "L1", "MAG", "MAGS", "CAL", "WIRE", "BOOT", "TIM_NS", "EPOCH", "PROF"):
        if g in s:
            out.append(g + " " + " ".join(f"{k}={v}" for k, v in s[g].items()))
    return "\n".join(out)


# ----------------------------------------------------------------- dashboard

_RESET, _RED, _YEL, _GRN, _DIM, _BOLD = "\x1b[0m", "\x1b[31m", "\x1b[33m", "\x1b[32m", "\x1b[2m", "\x1b[1m"
_MODES = {0: "echo test", 1: "pattern test", 2: "user application"}


def _cause_text(cause: int, pid: int) -> str:
    """Firmware mag_fault_cause_t (Rev 2.17) as text."""
    if cause == 1:
        return "no answer at address 0x30 (check power, SDA/SCL wiring, pull-ups); re-probing at 5 Hz"
    if cause == 2:
        hint = "likely a bus or wiring problem" if pid in (0x00, 0xFF) else "a different device at this address?"
        return f"wrong product ID: read 0x{pid:02X}, expected 0x30 ({hint}); re-probing at 5 Hz"
    if cause == 3:
        return "I2C bus error (line held low, noise, or no pull-ups); re-probing at 5 Hz"
    if cause == 4:
        return "factory calibration (OTP) not loaded after reset; re-probing"
    if cause == 5:
        return "configuration write failed; re-probing"
    if cause == 6:
        return "faulted at run time on not-ready samples; reconfiguring"
    return "fault flag set"


def _wire_text(w: dict[str, int]) -> str:
    """Start-up wiring check (Rev 2.18), same conclusions as the firmware."""
    def pin(name: str) -> str:
        if w.get(name + "_low"):
            return f"{name.upper()} LOW"
        return f"{name.upper()} " + ("pulled up" if w.get(name + "_pullup") else "no pull-up")
    facts = f"{pin('sda')}, {pin('scl')}, " + ("SDA-SCL SHORTED" if w.get("shorted") else "no short")
    if w.get("sda_low") or w.get("scl_low"):
        why = "a line is held low: shorted to GND, or pulled to an unpowered VDDIO"
    elif w.get("shorted"):
        why = "SDA and SCL are shorted together"
    elif w.get("mag"):
        why = "sensor answers at 0x30" + ("" if w.get("sda_pullup") and w.get("scl_pullup")
                                          else ", but external pull-ups are missing")
    elif w.get("mag_swapped"):
        why = "sensor answers only with SDA/SCL swapped: swap the D4/D5 wires"
    elif w.get("found") or w.get("found_swapped"):
        why = f"a device answers at 0x{w.get('first') or w.get('first_swapped'):02X}, not 0x30"
    elif not w.get("sda_pullup") and not w.get("scl_pullup"):
        why = "nothing answers, no pull-ups: sensor VDDIO not powered, or SDA/SCL not reaching it"
    else:
        why = "pull-ups present but nothing answers: check CS to VDDIO, VDD, GND"
    return facts + " -> " + why


def _paint(text: str, colour: str, use: bool) -> str:
    return f"{colour}{text}{_RESET}" if use else text


def render(cur: dict[str, dict[str, int]], prev: dict[str, dict[str, int]] | None, colour: bool) -> str:
    """Human-readable view of one report. `prev` (the previous report) gives
    per-report changes, shown as (+n)."""
    def g(group: str, key: str, default: int = 0) -> int:
        return cur.get(group, {}).get(key, default)

    def d(group: str, key: str) -> int:
        if prev is None:
            return 0
        return g(group, key) - prev.get(group, {}).get(key, 0)

    def count(group: str, key: str, bad_if_rising: bool = True) -> str:
        v, dv = g(group, key), d(group, key)
        txt = f"{v}" + (f" (+{dv})" if dv else "")
        if bad_if_rising and dv:
            return _paint(txt, _RED, colour)
        if bad_if_rising and v:
            return _paint(txt, _YEL, colour)
        return txt

    def limit(v: int, lim: int, unit: str) -> str:
        txt = f"{v} {unit}"
        return _paint(txt, _RED, colour) if v > lim else txt

    ok = _paint("OK", _GRN, colour)
    lines: list[str] = []
    problems: list[str] = []
    mode = g("STAT", "mode", -1)
    t_s = g("STAT", "t_ms") // 1000
    lines.append(_paint(f"DFC status  #{g('STAT', 'seq')}   up {t_s // 3600:02d}:{t_s // 60 % 60:02d}:{t_s % 60:02d}"
                        f"   {_MODES.get(mode, f'mode {mode}')}", _BOLD, colour))
    baud_ok = g("STAT", "baud_ok") == 1
    lines.append(f"  baud  UART0 {g('STAT', 'baud0')}  UART1 {g('STAT', 'baud1')}  "
                 + (ok if baud_ok else _paint("OUT OF TOLERANCE", _RED, colour)))
    if not baud_ok:
        problems.append("UART baud rate outside tolerance (U-2)")

    # UARTs
    lines.append("")
    lines.append(_paint(f"  {'UART':<14}{'UART0':>14}{'UART1':>14}", _BOLD, colour))
    rows = [("rx_overflow", "RX overflows"), ("tx_reject", "TX rejects"), ("framing", "framing err"),
            ("break", "breaks"), ("overrun", "HW overruns")]
    if "pattern_err" in cur.get("L0", {}):
        rows.append(("pattern_err", "pattern err"))
    for key, label in rows:
        lines.append(f"  {label:<14}{count('L0', key):>14}{count('L1', key):>14}")
        for k in ("L0", "L1"):
            if d(k, key):
                problems.append(f"{'UART0' if k == 'L0' else 'UART1'} {label} rising")
    for key, label in (("rx_hwm", "RX high-water"), ("tx_hwm", "TX high-water")):
        lines.append(f"  {label:<14}{g('L0', key):>14}{g('L1', key):>14}")
    lines.append(f"  {'TX interrupts':<14}{g('L0', 'tx_irq'):>14}{g('L1', 'tx_irq'):>14}")

    # Magnetometer
    lines.append("")
    fault = g("MAG", "fault") == 1
    if fault:
        why = _cause_text(g("MAG", "cause"), g("MAG", "id"))
        state = _paint("FAULT - " + why, _RED, colour)
        problems.append("magnetometer " + why)
    else:
        state = ok
    lines.append(_paint("  MAGNETOMETER  ", _BOLD, colour) + state)
    if "valid" in cur.get("MAG", {}):
        dv = d("MAG", "valid")
        hz = g("MAGS", "rate_hz") or 1000
        rate = (f"  (+{dv} this report" + ("" if fault else f", expect ~{hz}") + ")") if prev is not None else ""
        lines.append(f"  valid samples  {g('MAG', 'valid')}{rate}")
        lines.append(f"  invalid {count('MAG', 'invalid', False)}   gaps {count('MAG', 'gaps')}   "
                     f"stale {count('MAG', 'stale', False)}")
        if d("MAG", "gaps"):
            problems.append("magnetometer sample gaps")
    lines.append(f"  i2c errors {count('MAG', 'i2c_error')}   timeouts {count('MAG', 'timeout')}   "
                 f"missed ticks {count('MAG', 'missed_tick')}")
    lines.append(f"  no answer {count('MAG', 'no_ack', False)}   wrong ID {count('MAG', 'wrong_id', False)}   "
                 f"bus clears {g('MAG', 'bus_clear')}   "
                 f"recoveries {g('MAGS', 'recovered')}   run-time faults {count('MAGS', 'runtime_fault')}")
    lines.append(f"  not ready {count('MAGS', 'not_ready')}   saturated {count('CAL', 'saturated', False)}")
    if g("WIRE", "ran"):
        lines.append("  wiring (boot check)  " + _wire_text(cur.get("WIRE", {})))
    if "BOOT" in cur:
        b = cur["BOOT"]
        def t(v: int) -> str:
            return "error" if v == 0 else ("never" if v == 0xFFFF else f"{v / 1000:.2f} ms")
        lines.append(f"  start-up  ID 0x{b.get('id', 0):02X}  status 0x{b.get('status', 0):02X}  "
                     f"calibration {'OK' if b.get('cal_ok') else 'failed'}   meas. time 800 Hz {t(b.get('t_fast_us', 0))}"
                     f", 200 Hz {t(b.get('t_200_us', 0))}"
                     f", 100 Hz {t(b.get('t_slow_us', 0))}, after repeated START {t(b.get('t_rs_us', 0))}"
                     f", Status alone/after data 0x{b.get('direct_status', 0):02X}/0x{b.get('burst_status', 0):02X}")
    if not fault:
        for key, label in (("i2c_error", "I2C errors"), ("timeout", "timeouts"), ("missed_tick", "missed ticks")):
            if d("MAG", key):
                problems.append(f"magnetometer {label} rising")
        if d("MAGS", "not_ready"):
            problems.append("magnetometer samples read before the measurement finished (not ready)")
    m = cur.get("MAGS", {})
    if "rate_hz" in m:
        bw = {0: "100 Hz", 1: "200 Hz", 2: "400 Hz", 3: "800 Hz"}.get(m.get("bw"), "?")
        txt = f"  rate {m['rate_hz']} Hz (sensor BW {bw}, one measurement per sample)"
        if m.get("rate_req_hz", m["rate_hz"]) != m["rate_hz"]:
            txt += f"   {m['rate_req_hz']} Hz requested"
        lines.append(txt)
    if m.get("noise_n", 0) >= 2:
        lines.append(f"  noise x {m['nx_ug'] / 1000:.2f}  y {m['ny_ug'] / 1000:.2f}  z {m['nz_ug'] / 1000:.2f} mG RMS"
                     f" over {m['noise_n']} samples (board kept still)")
    sr = {0: "single cycle", 1: f"SET pulse every {g('MAGS', 'sr_interval')}"}.get(g("MAGS", "sr_mode"), "?")
    lines.append(f"  field x {g('MAGS', 'x'):>8}  y {g('MAGS', 'y'):>8}  z {g('MAGS', 'z'):>8}  counts (16384/G)"
                 f"   SET/RESET: {sr}")
    cal = "valid" if g("CAL", "offset_valid") == 1 else _paint("not calibrated", _YEL, colour)
    lines.append(f"  offset x {g('CAL', 'ox'):>8}  y {g('CAL', 'oy'):>8}  z {g('CAL', 'oz'):>8}   "
                 f"{cal}, {g('CAL', 'offset_cal')} calibrations")

    # Timing
    lines.append("")
    lines.append(_paint("  TIMING (worst case since boot)", _BOLD, colour))
    t = cur.get("TIM_NS", {})
    isr = max([t.get(k, 0) for k in ("isr_uart0", "isr_uart1", "isr_txdma", "isr_i2c", "isr_tick")] or [0])
    api = max([t.get(k, 0) for k in ("api_read", "api_write", "api_mag")] or [0])
    lines.append(f"  interrupt handler {limit(isr, 5000, 'ns')} (<= 5000)   critical section "
                 f"{limit(t.get('crit', 0), 2000, 'ns')} (<= 2000)   API {limit(api, 2000, 'ns')} (<= 2000)")
    if isr > 5000 or t.get("crit", 0) > 2000 or api > 2000:
        problems.append("a handler, critical-section or API time is over its limit (EX-3/4/7)")
    if mode == 2:
        e = cur.get("EPOCH", {})
        lines.append(f"  epoch work last {e.get('last_us', 0)} us  avg {e.get('avg_us', 0)} us  max "
                     f"{limit(e.get('max_us', 0), 999, 'us')} (< 1000)   start latency max {e.get('start_max_us', 0)} us")
        lines.append(f"  epoch overruns {count('EPOCH', 'overrun')}   tick overruns {count('STAT', 'tick_overrun')}")
        if e.get("max_us", 0) > 999 or d("EPOCH", "overrun"):
            problems.append("epoch work over 1 ms (EX-8)")
        p = cur.get("PROF", {})
        secs = sorted({k.rsplit("_", 1)[0] for k in p})
        if secs:
            lines.append("  sections (last/max/avg us): " + "   ".join(
                f"{n} {p.get(n + '_last', 0)}/{p.get(n + '_max', 0)}/{p.get(n + '_avg', 0)}" for n in secs))
    else:
        lines.append(f"  main-loop pass max {limit(g('STAT', 'loop_max_us'), 999, 'us')} (< 1000)   "
                     f"tick overruns {count('STAT', 'tick_overrun')}")
        if g("STAT", "loop_max_us") > 999:
            problems.append("main-loop pass over 1 ms (EX-8)")
    if d("STAT", "tick_overrun"):
        problems.append("tick overruns (M-6a)")

    lines.append("")
    if problems:
        lines.append(_paint("  ATTENTION", _RED, colour))
        lines.extend("   - " + x for x in problems)
    else:
        lines.append("  " + _paint("All checks OK", _GRN, colour))
    return "\n".join(lines)


def _enable_ansi() -> bool:
    if not sys.stdout.isatty():
        return False
    if os.name == "nt":
        os.system("")  # enables ANSI escape processing in the Windows console
    return True


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", help="USB CDC port of the firmware, e.g. COM7")
    ap.add_argument("--log", help="append every line, timestamped, to this file")
    ap.add_argument("--raw", action="store_true", help="print the firmware's machine-format lines unchanged")
    ap.add_argument("--scroll", action="store_true", help="print each report below the last "
                    "instead of redrawing the screen")
    ap.add_argument("--rate", type=int, choices=(1000, 200, 100, 50), help="set the magnetometer rate (Hz) "
                    "by sending 'r' until the firmware reports it; lasts until the next reset")
    ap.add_argument("--bootsel", action="store_true", help="send '~' to reboot the board into its USB "
                    "bootloader (RP2350 drive) for reflashing, then exit")
    args = ap.parse_args()
    if args.bootsel:
        with serial.Serial(args.port, 115200, timeout=0.2) as ser:
            ser.dtr = True
            ser.write(b"~")
            ser.flush()
            time.sleep(0.3)
        print("Sent '~': the board should now appear as the RP2350 drive.")
        return 0
    ansi = _enable_ansi()
    mon = StatusMonitor(args.port, args.log)
    prev = None
    try:
        while True:
            if not mon.wait_for_report(5.0):
                print("(no status report in 5 s: is the diag build running and the port opened with DTR?)",
                      file=sys.stderr)
                continue
            cur = mon.report()
            req = cur.get("MAGS", {}).get("rate_req_hz")
            if args.rate is not None and req is not None and req != args.rate:
                mon.send(b"r")  # one step per report: 1000 -> 200 -> 100 -> 50 -> 1000
            if args.raw:
                print(format_report(cur), "\n", flush=True)
            else:
                text = render(cur, prev, ansi)
                if ansi and not args.scroll:
                    print("\x1b[H\x1b[2J" + text, flush=True)  # redraw in place
                else:
                    print(text + "\n", flush=True)
            prev = cur
    except KeyboardInterrupt:
        pass
    finally:
        mon.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
