"""V-1 soak test: both UARTs full duplex at 921.6 kbaud with the magnetometer
running, for 10 minutes (default), with byte-exact data checking.

Two firmware modes are supported (choose with --mode, build with the
matching preset):

  echo     (preset "diag")          the host sends a pseudo-random stream on
           each UART, the firmware echoes it, and the host checks every byte.
  pattern  (preset "diag-pattern")  the firmware sends a continuous counting
           pattern at full line rate (TX queue never empty) and checks the
           host's counting pattern; the host checks the firmware's pattern.

Host transmit rate: a USB-UART adapter's 921.6k may be faster than the
RP2350's (e.g. FTDI 923,077 baud vs RP2350 921,659 baud, +0.15 %). In echo
mode that excess accumulates in the firmware's TX queue and will eventually
cause TX rejects that say nothing about the firmware. --rate (default 0.99)
paces the host below line rate; use 1.0 only with an adapter whose baud
error is known to be below the firmware's.

Example (PowerShell):
    py tools/soak_test.py --port0 COM5 --port1 COM6 --status COM7 --mode echo
"""

from __future__ import annotations

import argparse
import random
import sys
import threading
import time

import serial  # pyserial

from fwstatus import StatusMonitor, delta, format_report

BAUD = 921600
CHUNK = 1024


class Stream:
    """Deterministic byte stream: pseudo-random (echo) or counting (pattern)."""

    def __init__(self, kind: str, seed: int):
        self.kind = kind
        self.rng = random.Random(seed)
        self.counter = 0

    def take(self, n: int) -> bytes:
        if self.kind == "random":
            return self.rng.randbytes(n)
        out = bytes((self.counter + i) & 0xFF for i in range(n))
        self.counter = (self.counter + n) & 0xFF
        return out


class LinkTest:
    def __init__(self, name: str, port: str, mode: str, rate: float, seed: int):
        self.name = name
        self.mode = mode
        self.ser = serial.Serial(port, BAUD, timeout=0.05, write_timeout=2)
        if hasattr(self.ser, "set_buffer_size"):  # Windows only
            self.ser.set_buffer_size(rx_size=1 << 20, tx_size=1 << 16)
        self.rate_bps = rate * BAUD / 10.0
        kind = "random" if mode == "echo" else "count"
        self.tx_stream = Stream(kind, seed)
        self.rx_expect = Stream(kind, seed)  # echo: same stream comes back
        self.sent = 0
        self.received = 0
        self.mismatches = 0
        self.first_mismatch: int | None = None
        self.synced = mode == "echo"
        self.next_count = 0
        self.stop_tx = threading.Event()
        self.stop_rx = threading.Event()

    def flush(self) -> None:
        self.ser.reset_input_buffer()
        time.sleep(0.3)
        self.ser.reset_input_buffer()

    def tx_loop(self) -> None:
        t0 = time.perf_counter()
        while not self.stop_tx.is_set():
            allowed = self.rate_bps * (time.perf_counter() - t0) - self.sent
            if allowed < CHUNK:
                time.sleep(0.001)
                continue
            data = self.tx_stream.take(CHUNK)
            self.ser.write(data)
            self.sent += len(data)

    def rx_loop(self) -> None:
        while not self.stop_rx.is_set():
            data = self.ser.read(max(1, min(self.ser.in_waiting, 65536)))
            if not data:
                continue
            if self.mode == "echo":
                expect = self.rx_expect.take(len(data))
                if data != expect:
                    for i, (a, b) in enumerate(zip(data, expect)):
                        if a != b:
                            self.mismatches += 1
                            if self.first_mismatch is None:
                                self.first_mismatch = self.received + i
            else:
                for i, b in enumerate(data):
                    if self.synced and b != self.next_count:
                        self.mismatches += 1
                        if self.first_mismatch is None:
                            self.first_mismatch = self.received + i
                    self.synced = True
                    self.next_count = (b + 1) & 0xFF
            self.received += len(data)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port0", required=True, help="host port wired to UART0 (D6/D7)")
    ap.add_argument("--port1", required=True, help="host port wired to UART1 (D12/D11)")
    ap.add_argument("--status", help="firmware USB CDC port (diag builds)")
    ap.add_argument("--mode", choices=("echo", "pattern"), default="echo")
    ap.add_argument("--duration", type=float, default=600.0, help="seconds (default 600)")
    ap.add_argument("--rate", type=float, default=0.99, help="host TX rate as a fraction of line rate")
    ap.add_argument("--log", help="status log file")
    args = ap.parse_args()

    mon = StatusMonitor(args.status, args.log) if args.status else None
    links = [LinkTest("UART0", args.port0, args.mode, args.rate, 1), LinkTest("UART1", args.port1, args.mode, args.rate, 2)]
    for lk in links:
        lk.flush()

    before = {}
    if mon:
        if not mon.wait_for_report(5.0):
            print("no status report from firmware", file=sys.stderr)
            return 2
        before = mon.latest()

    threads = []
    for lk in links:
        threads.append(threading.Thread(target=lk.rx_loop, daemon=True))
        threads.append(threading.Thread(target=lk.tx_loop, daemon=True))
    for t in threads:
        t.start()

    t_end = time.time() + args.duration
    try:
        while time.time() < t_end:
            time.sleep(min(10.0, max(0.0, t_end - time.time())))
            left = max(0, int(t_end - time.time()))
            print(" | ".join(f"{lk.name} tx={lk.sent} rx={lk.received} err={lk.mismatches}" for lk in links), f"| {left}s left", flush=True)
    except KeyboardInterrupt:
        print("interrupted")

    for lk in links:
        lk.stop_tx.set()
    time.sleep(1.0)  # let echoes drain
    for lk in links:
        lk.stop_rx.set()
    time.sleep(0.2)

    ok = True
    print("\n=== host side ===")
    for lk in links:
        line = f"{lk.name}: sent={lk.sent} received={lk.received} mismatches={lk.mismatches}"
        if lk.first_mismatch is not None:
            line += f" first_mismatch_at={lk.first_mismatch}"
        print(line)
        if lk.mismatches:
            ok = False
        if args.mode == "echo" and lk.received != lk.sent:
            print(f"  FAIL: {lk.name} echo length differs by {lk.sent - lk.received}")
            ok = False

    if mon:
        mon.wait_for_report(3.0)
        after = mon.latest()
        print("\n=== firmware counters during the run (deltas) ===")
        checks = [
            ("L0", "framing"), ("L0", "rx_overflow"), ("L0", "tx_reject"), ("L0", "pattern_err"),
            ("L1", "framing"), ("L1", "rx_overflow"), ("L1", "tx_reject"), ("L1", "pattern_err"),
            ("MAG", "gaps"), ("MAG", "i2c_error"), ("MAG", "missed_tick"), ("MAG", "timeout"),
            ("MAGS", "not_ready"),
        ]
        for g, k in checks:
            d = delta(after, before, g, k)
            print(f"{g}.{k} = {d}{'' if d == 0 else '   FAIL'}")
            ok &= d == 0
        valid = delta(after, before, "MAG", "valid")
        secs = (after.get("STAT", {}).get("t_ms", 0) - before.get("STAT", {}).get("t_ms", 0)) / 1000.0
        print(f"MAG.valid = {valid} over {secs:.1f} s ({valid / secs if secs else 0:.1f}/s)")
        if after.get("MAG", {}).get("fault", 1):
            print("MAG fault flag set   FAIL")
            ok = False
        print("\n=== final status ===")
        print(format_report(after))
        mon.close()

    print("\nRESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
