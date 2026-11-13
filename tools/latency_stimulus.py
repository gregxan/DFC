"""Stimulus for V-2 (RX latency) and V-3 (TX latency).

Sends isolated single bytes to one firmware UART (firmware built in echo
mode). Each byte arrives with the main loop polling RX and the TX path idle,
so a single logic-analyzer capture gives both measurements:

  V-2: end of the stop bit on the RX line  ->  RX_SEEN test pin toggle
  V-3: TX_WRITE test pin toggle            ->  start bit on the TX line

Capture RX, TX, RX_SEEN and TX_WRITE for the UART under test (see
docs/VERIFICATION.md for pins), export as CSV and run analyze_capture.py.

Example:
    py tools/latency_stimulus.py COM5 --count 100000
"""

from __future__ import annotations

import argparse
import random
import sys
import time

import serial  # pyserial


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", help="host port wired to the UART under test")
    ap.add_argument("--count", type=int, default=100_000)
    ap.add_argument("--gap-ms", type=float, default=0.5, help="minimum spacing between bytes")
    args = ap.parse_args()

    ser = serial.Serial(args.port, 921600, timeout=0)
    ser.reset_input_buffer()
    rng = random.Random(0)
    echoed = 0
    t_report = time.time()
    for i in range(args.count):
        ser.write(bytes([rng.randrange(256)]))
        ser.flush()
        # Spacing guarantees an idle TX path and an isolated RX frame.
        # Windows sleep granularity makes the real spacing ~1-2 ms.
        time.sleep(args.gap_ms / 1000.0)
        echoed += len(ser.read(4096))
        if time.time() - t_report > 5:
            print(f"{i + 1}/{args.count} sent, {echoed} echoed", flush=True)
            t_report = time.time()
    time.sleep(0.1)
    echoed += len(ser.read(4096))
    print(f"done: {args.count} sent, {echoed} echoed")
    return 0 if echoed == args.count else 1


if __name__ == "__main__":
    sys.exit(main())
