"""Analyse logic-analyzer captures for V-2, V-3 and V-4.

Input: a CSV with a time column (seconds) followed by one column per
digital channel holding 0/1, either one row per transition (Saleae Logic 2
"Export Data -> CSV") or one row per sample (sigrok/PulseView). The file is
streamed, so captures of any length can be processed.

Channels are selected by header name or by column number (0 = first column
after the time column).

  latency  V-2 and V-3. Needs --rx, --rx-seen, and optionally --tx and
           --tx-write. Use with latency_stimulus.py (isolated bytes).
  gaps     V-4. Needs --tx. Use with the firmware in pattern mode (TX queue
           never empty). Reports every stop-bit-to-start-bit gap.

Examples:
    py tools/analyze_capture.py latency cap.csv --rx 0 --tx 1 --rx-seen 2 --tx-write 3
    py tools/analyze_capture.py gaps cap.csv --tx 1
"""

from __future__ import annotations

import argparse
import csv
import sys
from collections import Counter
from typing import Iterator

BAUD_DEFAULT = 921600


def edges(path: str, wanted: dict[str, str]) -> Iterator[tuple[float, str, int]]:
    """Yield (time_s, role, new_level) for every change on the wanted channels."""
    with open(path, newline="", encoding="utf-8-sig") as f:
        rd = csv.reader(f)
        header = next(rd)
        cols: dict[str, int] = {}
        for role, sel in wanted.items():
            if sel.isdigit():
                cols[role] = int(sel) + 1
            else:
                try:
                    cols[role] = header.index(sel)
                except ValueError:
                    sys.exit(f"channel '{sel}' not in header {header}")
        last: dict[str, int] = {}
        for row in rd:
            if not row:
                continue
            t = float(row[0])
            for role, c in cols.items():
                v = 1 if row[c].strip() in ("1", "1.0", "high", "HIGH") else 0
                if role not in last:
                    last[role] = v
                    continue
                if v != last[role]:
                    last[role] = v
                    yield t, role, v


def stats(name: str, vals_s: list[float], med_limit_us: float, max_limit_us: float, need: int) -> bool:
    if not vals_s:
        print(f"{name}: no measurements")
        return False
    v = sorted(x * 1e6 for x in vals_s)
    n = len(v)

    def pct(p: float) -> float:
        return v[min(n - 1, int(p * n))]

    med = v[n // 2]
    ok = med <= med_limit_us and v[-1] <= max_limit_us and n >= need
    print(f"{name}: n={n} min={v[0]:.3f} median={med:.3f} p99={pct(0.99):.3f} p99.9={pct(0.999):.3f} max={v[-1]:.3f} us")
    print(f"  limits: median <= {med_limit_us} us, max <= {max_limit_us} us, n >= {need}  ->  {'PASS' if ok else 'FAIL'}")
    return ok


def run_latency(args: argparse.Namespace) -> int:
    bit = 1.0 / args.baud
    wanted = {"rx": args.rx, "rx_seen": args.rx_seen}
    if args.tx and args.tx_write:
        wanted["tx"] = args.tx
        wanted["tx_write"] = args.tx_write

    rx_lat: list[float] = []
    tx_lat: list[float] = []
    rx_frame_busy_until = -1.0
    rx_pending: list[tuple[float, float]] = []  # (data latched time, end of stop bit)
    rx_missing = 0
    rx_unmatched = 0
    tx_frame_busy_until = -1.0
    tx_last_frame_end = -1.0
    tx_pending: float | None = None
    tx_missing = 0

    for t, role, v in edges(args.csv, wanted):
        if role == "rx" and v == 0 and t >= rx_frame_busy_until:
            # start bit; the UART has the byte at mid-stop-bit (9.5 bits)
            rx_frame_busy_until = t + 9.5 * bit
            while rx_pending and t - rx_pending[0][1] > args.window_us * 1e-6:
                rx_pending.pop(0)
                rx_missing += 1
            rx_pending.append((t + 9.0 * bit, t + 10.0 * bit))
        elif role == "rx_seen":
            matched = False
            while rx_pending and t >= rx_pending[0][0]:
                latched, stop_end = rx_pending.pop(0)
                if not rx_pending or t < rx_pending[0][0]:
                    rx_lat.append(t - stop_end)
                    matched = True
            if not matched:
                rx_unmatched += 1
        elif role == "tx_write":
            idle = t >= tx_last_frame_end
            if tx_pending is not None:
                tx_missing += 1
            tx_pending = t if idle else None
        elif role == "tx" and v == 0 and t >= tx_frame_busy_until:
            tx_frame_busy_until = t + 9.5 * bit
            tx_last_frame_end = t + 10.0 * bit
            if tx_pending is not None:
                tx_lat.append(t - tx_pending)
                tx_pending = None

    ok = stats("V-2 RX latency (end of stop bit -> RX_SEEN)", rx_lat, 3.0, 20.0, args.min_count)
    print(f"  RX frames with no RX_SEEN toggle within {args.window_us} us: {rx_missing}; unmatched toggles: {rx_unmatched}")
    if "tx" in wanted:
        ok &= stats("V-3 TX latency (TX_WRITE -> start bit)", tx_lat, 3.0, 20.0, args.min_count)
        print(f"  TX_WRITE toggles with no start bit: {tx_missing}")
    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


def run_gaps(args: argparse.Namespace) -> int:
    bit = 1.0 / args.baud
    busy_until = -1.0
    prev_end: float | None = None
    frames = 0
    gaps_over_bit = 0
    gaps_over_limit = 0
    max_gap = 0.0
    max_gap_at = 0.0
    hist: Counter[int] = Counter()
    tol = args.tolerance_ns * 1e-9

    for t, _role, v in edges(args.csv, {"tx": args.tx}):
        if v != 0 or t < busy_until:
            continue
        frames += 1
        if prev_end is not None:
            gap = max(0.0, t - prev_end)
            if gap > max_gap:
                max_gap, max_gap_at = gap, t
            if gap > bit + tol:
                gaps_over_bit += 1
                hist[int(gap * 1e6)] += 1
            if gap > args.limit_us * 1e-6:
                gaps_over_limit += 1
        busy_until = t + 9.5 * bit
        prev_end = t + 10.0 * bit

    print(f"frames decoded: {frames}")
    print(f"gaps > 1 bit ({bit * 1e6:.3f} us + {args.tolerance_ns} ns): {gaps_over_bit}")
    print(f"gaps > {args.limit_us} us: {gaps_over_limit}")
    print(f"max gap: {max_gap * 1e6:.3f} us at t={max_gap_at:.6f} s")
    if hist:
        print("histogram of gaps > 1 bit (whole microseconds: count):")
        for k in sorted(hist):
            print(f"  {k:4d}: {hist[k]}")
    print("Compare 'gaps > 1 bit' with the number of TX DMA transfer boundaries (U-18);")
    print("in pattern mode that is at most one per 256-byte chunk written.")
    ok = gaps_over_limit == 0 and frames > 0
    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="mode", required=True)

    lat = sub.add_parser("latency", help="V-2 / V-3")
    lat.add_argument("csv")
    lat.add_argument("--rx", required=True)
    lat.add_argument("--rx-seen", required=True)
    lat.add_argument("--tx")
    lat.add_argument("--tx-write")
    lat.add_argument("--window-us", type=float, default=200.0)
    lat.add_argument("--min-count", type=int, default=100_000)
    lat.add_argument("--baud", type=float, default=BAUD_DEFAULT)

    gp = sub.add_parser("gaps", help="V-4")
    gp.add_argument("csv")
    gp.add_argument("--tx", required=True)
    gp.add_argument("--limit-us", type=float, default=25.0)
    gp.add_argument("--tolerance-ns", type=float, default=100.0, help="sampling tolerance on the 1-bit limit")
    gp.add_argument("--baud", type=float, default=BAUD_DEFAULT)

    args = ap.parse_args()
    return run_latency(args) if args.mode == "latency" else run_gaps(args)


if __name__ == "__main__":
    sys.exit(main())
