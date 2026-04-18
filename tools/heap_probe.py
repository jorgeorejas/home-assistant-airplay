#!/usr/bin/env python3
"""
heap_probe.py — Phase 0 gate harness.

Reads the ESP32-S3 USB CDC console, parses lines matching the format emitted
by `heap_probe_task` in main/app_main.c, and writes a CSV with the per-sample
free-heap numbers. The CSV is the evidence we attach to docs/phase0-report.md.

Log line format (from app_main.c):
  I (12345) heap: t_ms=12340 free_internal=234567 free_psram=4123456 ...
                  min_free_internal=234000 state=idle

Usage:
  python3 tools/heap_probe.py --port /dev/cu.usbmodem* --out phase0.csv \
      [--duration 1800] [--tag idle|playing|paused]

The --tag column is written into each row so 10-min idle / 10-min playing /
10-min pause-resume sessions can be concatenated and plotted together.
"""

from __future__ import annotations

import argparse
import csv
import glob
import os
import re
import signal
import sys
import time
from dataclasses import dataclass

try:
    import serial  # pyserial
except ImportError:
    sys.stderr.write("pyserial not installed. Run: pip install pyserial\n")
    sys.exit(1)


LINE_RE = re.compile(
    r"heap:\s+t_ms=(?P<t>\d+)\s+"
    r"free_internal=(?P<fi>\d+)\s+"
    r"free_psram=(?P<fp>\d+)\s+"
    r"free_total=(?P<ft>\d+)\s+"
    r"min_free_internal=(?P<mfi>\d+)\s+"
    r"state=(?P<state>\S+)"
)


@dataclass
class Row:
    wall_ts: float
    device_t_ms: int
    free_internal: int
    free_psram: int
    free_total: int
    min_free_internal: int
    state: str
    tag: str


def resolve_port(pattern: str) -> str:
    matches = glob.glob(pattern)
    if not matches:
        raise SystemExit(f"no serial device matched pattern {pattern!r}")
    return matches[0]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/cu.usbmodem*",
                    help="serial device glob (default: /dev/cu.usbmodem*)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--out", required=True, help="output CSV path")
    ap.add_argument("--duration", type=int, default=0,
                    help="seconds to record (0 = until ctrl-c)")
    ap.add_argument("--tag", default="run",
                    help="tag column written on every row (e.g. idle, playing)")
    args = ap.parse_args()

    port = resolve_port(args.port)
    print(f"[heap_probe] opening {port} @ {args.baud}", file=sys.stderr)

    ser = serial.Serial(port, args.baud, timeout=1)
    deadline = time.time() + args.duration if args.duration > 0 else None

    # Track the min across the whole run (useful in the summary).
    run_min_fi = None

    out_new = not os.path.exists(args.out)
    f = open(args.out, "a", newline="")
    writer = csv.writer(f)
    if out_new:
        writer.writerow(["wall_ts", "device_t_ms", "tag", "state",
                         "free_internal", "free_psram", "free_total",
                         "min_free_internal_session"])
        f.flush()

    def handle_sigint(_sig, _frm):
        print("\n[heap_probe] ctrl-c, closing", file=sys.stderr)
        f.flush(); f.close(); ser.close()
        sys.exit(0)

    signal.signal(signal.SIGINT, handle_sigint)

    try:
        while True:
            if deadline and time.time() >= deadline:
                break
            line = ser.readline().decode("utf-8", errors="replace").strip()
            if not line:
                continue
            m = LINE_RE.search(line)
            if not m:
                continue
            fi = int(m["fi"])
            run_min_fi = fi if run_min_fi is None else min(run_min_fi, fi)
            writer.writerow([
                f"{time.time():.3f}",
                m["t"], args.tag, m["state"],
                fi, m["fp"], m["ft"], m["mfi"],
            ])
            f.flush()
    finally:
        f.close(); ser.close()

    if run_min_fi is not None:
        print(f"[heap_probe] session min free_internal = {run_min_fi} bytes "
              f"({run_min_fi / 1024:.1f} KiB)", file=sys.stderr)
        gate = 100 * 1024
        verdict = "PASS" if run_min_fi >= gate else "FAIL"
        print(f"[heap_probe] Phase 0 gate (>={gate}): {verdict}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
