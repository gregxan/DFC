"""Reader for the firmware's USB CDC status lines (diag / diag-pattern builds).

Each second the firmware emits seven lines:

    STAT seq=.. mode=.. t_ms=.. baud0=.. baud1=.. baud_ok=.. loop_max_us=.. status_max_us=.. tick_overrun=..
    L0 rx_overflow=.. tx_reject=.. framing=.. break=.. overrun=.. rx_hwm=.. tx_hwm=.. pattern_err=..
    L1 ...
    MAG fault=.. i2c_error=.. missed_tick=.. timeout=.. pid_fault=.. bus_clear=.. valid=.. ...
    MAGS sr_mode=.. sr_interval=.. recovered=.. not_ready=.. x=.. y=.. z=..
    CAL offset_valid=.. offset_cal=.. saturated=.. ox=.. oy=.. oz=..
    TIM_NS isr_uart0=.. isr_uart1=.. isr_txdma=.. isr_i2c=.. isr_tick=.. crit=.. api_read=.. ...

User application builds (diag-user) add:

    EPOCH last_us=.. max_us=.. avg_us=.. overrun=.. start_max_us=.. count=..
    PROF mag=last/max/avg link0=last/max/avg link1=last/max/avg   (us)

Test application builds (diag, diag-pattern) also show pattern_err on L0/L1
and valid/invalid/gaps/stale on MAG.

Counters are cumulative since boot. Run standalone to print them:

    py tools/fwstatus.py COM7
"""

from __future__ import annotations

import argparse
import sys
import threading
import time

import serial  # pyserial


def parse_line(line: str) -> tuple[str, dict[str, int]] | None:
    parts = line.strip().split()
    if not parts or parts[0] not in ("STAT", "L0", "L1", "MAG", "MAGS", "CAL", "TIM_NS", "EPOCH", "PROF"):
        return None
    values: dict[str, int] = {}
    for p in parts[1:]:
        if "=" in p:
            k, v = p.split("=", 1)
            try:
                values[k] = int(v)
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
        self._stop = threading.Event()
        self._log = open(log_path, "a", encoding="utf-8") if log_path else None
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def _run(self) -> None:
        buf = b""
        while not self._stop.is_set():
            buf += self._ser.read(4096)
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("ascii", "replace")
                if self._log:
                    self._log.write(f"{time.time():.3f} {line.strip()}\n")
                parsed = parse_line(line)
                if parsed:
                    group, values = parsed
                    with self._lock:
                        self._latest[group] = values
                        self._first.setdefault(group, dict(values))

    def latest(self) -> dict[str, dict[str, int]]:
        with self._lock:
            return {k: dict(v) for k, v in self._latest.items()}

    def first(self) -> dict[str, dict[str, int]]:
        with self._lock:
            return {k: dict(v) for k, v in self._first.items()}

    def wait_for_report(self, timeout: float = 5.0) -> bool:
        end = time.time() + timeout
        start_seq = self.latest().get("STAT", {}).get("seq")
        while time.time() < end:
            seq = self.latest().get("STAT", {}).get("seq")
            if seq is not None and seq != start_seq and "TIM_NS" in self.latest():
                return True
            time.sleep(0.1)
        return False

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
    for g in ("STAT", "L0", "L1", "MAG", "MAGS", "CAL", "TIM_NS", "EPOCH", "PROF"):
        if g in s:
            out.append(g + " " + " ".join(f"{k}={v}" for k, v in s[g].items()))
    return "\n".join(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", help="USB CDC port of the firmware, e.g. COM7")
    ap.add_argument("--log", help="append every line, timestamped, to this file")
    args = ap.parse_args()
    mon = StatusMonitor(args.port, args.log)
    try:
        while True:
            if mon.wait_for_report(5.0):
                print(format_report(mon.latest()), "\n", flush=True)
            else:
                print("(no status report in 5 s)", file=sys.stderr)
    except KeyboardInterrupt:
        pass
    finally:
        mon.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
