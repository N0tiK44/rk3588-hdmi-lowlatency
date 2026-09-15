#!/usr/bin/env python3
"""Analyze consolidated V3.4/V3.5 timing CSV traces.

Usage:
  tools/analyze.py TRACE.csv
  tools/analyze.py NORMAL.csv ASYNC.csv
"""
from __future__ import annotations

import csv
import io
import math
import statistics
import sys
from collections import defaultdict
from pathlib import Path

WARMUP_FRAMES = 60


def median(xs):
    return statistics.median(xs) if xs else math.nan


def percentile(xs, p):
    if not xs:
        return math.nan
    xs = sorted(xs)
    k = (len(xs) - 1) * p
    lo, hi = math.floor(k), math.ceil(k)
    if lo == hi:
        return xs[lo]
    return xs[lo] * (hi - k) + xs[hi] * (k - lo)


def integer(value):
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def delta_us(a, b):
    if a is None or b is None or b < a:
        return None
    return (b - a) / 1000.0


def load_csv(path):
    raw = Path(path).read_text(errors="replace")
    # Compatibility with early V3 traces that accidentally wrote literal "\\n".
    if raw.count("\n") < 3 and raw.count("\\n") > 3:
        raw = raw.replace("\\n", "\n")
    reader = csv.DictReader(io.StringIO(raw))
    rows = list(reader)
    if not rows:
        raise RuntimeError(f"{path}: no CSV rows")
    required = {"sequence", "index", "dq_ns", "commit_begin_ns", "commit_end_ns", "out_signal_ns"}
    missing = sorted(required - set(reader.fieldnames or []))
    if missing:
        raise RuntimeError(f"{path}: missing columns: {', '.join(missing)}")
    if len(rows) > WARMUP_FRAMES + 10:
        rows = rows[WARMUP_FRAMES:]
    return rows


def analyze(path):
    rows = load_csv(path)
    metrics = defaultdict(list)
    seq_gaps = 0
    missing_fences = 0
    per_index_last_dq = {}
    per_index_reuse = defaultdict(list)
    prev = None

    for row in rows:
        seq = integer(row.get("sequence"))
        idx = integer(row.get("index"))
        fence = integer(row.get("fence_present"))
        dq = integer(row.get("dq_ns"))
        cb = integer(row.get("commit_begin_ns"))
        ce = integer(row.get("commit_end_ns"))
        out = integer(row.get("out_signal_ns"))
        qbuf = integer(row.get("qbuf_ns"))
        vts = integer(row.get("v4l2_ts_ns"))
        if None in (seq, idx, dq, cb, ce, out):
            continue

        if fence == 0:
            missing_fences += 1

        for key, val in (
            ("dq_to_commit_begin", delta_us(dq, cb)),
            ("commit_ioctl", delta_us(cb, ce)),
            ("commit_to_out", delta_us(ce, out)),
            ("dq_to_out", delta_us(dq, out)),
            ("own_out_to_qbuf", delta_us(out, qbuf)),
        ):
            if val is not None:
                metrics[key].append(val)

        if prev is not None:
            if seq > prev["seq"] + 1:
                seq_gaps += seq - prev["seq"] - 1
            for key, val in (
                ("dq_period", delta_us(prev["dq"], dq)),
                ("out_period", delta_us(prev["out"], out)),
                ("prev_out_to_dq", delta_us(prev["out"], dq)),
                ("prev_out_to_commit", delta_us(prev["out"], cb)),
                ("v4l2_period", delta_us(prev["vts"], vts) if prev["vts"] and vts else None),
            ):
                if val is not None:
                    metrics[key].append(val)

        if idx in per_index_last_dq:
            val = delta_us(per_index_last_dq[idx], dq)
            if val is not None:
                per_index_reuse[idx].append(val)
                metrics["same_buffer_reuse"].append(val)
        per_index_last_dq[idx] = dq
        prev = {"seq": seq, "dq": dq, "out": out, "vts": vts}

    if not metrics["commit_to_out"]:
        raise RuntimeError(f"{path}: no usable timing rows")

    return {
        "path": str(path),
        "rows": len(rows),
        "sequence_gaps": seq_gaps,
        "missing_fences": missing_fences,
        "metrics": metrics,
        "per_index_reuse": per_index_reuse,
    }


def fmt_us(us):
    return "n/a" if math.isnan(us) else f"{us / 1000.0:.3f} ms"


def metric_line(label, xs):
    return (
        f"  {label:<28} "
        f"p50 {fmt_us(median(xs)):>10}   "
        f"p95 {fmt_us(percentile(xs, 0.95)):>10}"
    )


def show(label, result):
    m = result["metrics"]
    print(f"{label}: {result['path']} ({result['rows']} post-warmup rows)")
    print(f"  sequence gaps: {result['sequence_gaps']}   missing acquire fences: {result['missing_fences']}")
    print(metric_line("V4L2 timestamp cadence", m["v4l2_period"]))
    print(metric_line("DQ -> next DQ", m["dq_period"]))
    print(metric_line("OUT -> next OUT", m["out_period"]))
    print(metric_line("previous OUT -> DQ", m["prev_out_to_dq"]))
    print(metric_line("previous OUT -> commit", m["prev_out_to_commit"]))
    print(metric_line("DQ -> commit call", m["dq_to_commit_begin"]))
    print(metric_line("atomic commit ioctl", m["commit_ioctl"]))
    print(metric_line("commit return -> OUT", m["commit_to_out"]))
    print(metric_line("DQ -> OUT", m["dq_to_out"]))
    print(metric_line("own OUT -> QBUF", m["own_out_to_qbuf"]))
    print(metric_line("same-buffer DQ reuse", m["same_buffer_reuse"]))
    if result["per_index_reuse"]:
        parts = []
        for idx in sorted(result["per_index_reuse"]):
            parts.append(f"b{idx}={fmt_us(median(result['per_index_reuse'][idx]))}")
        print("  per-index reuse p50:          " + ", ".join(parts))


def main(argv):
    if len(argv) not in (2, 3):
        raise SystemExit("usage: analyze.py TRACE.csv [ASYNC.csv]")
    normal = analyze(argv[1])
    print("=== RK3588 HDMI-RX / KMS TIMING REPORT ===")
    show("NORMAL" if len(argv) == 3 else "TRACE", normal)

    if len(argv) == 3:
        async_result = analyze(argv[2])
        print()
        show("ASYNC", async_result)
        n = median(normal["metrics"]["commit_to_out"])
        a = median(async_result["metrics"]["commit_to_out"])
        gain = n - a
        print()
        print(f"Async commit->OUT median improvement: {gain / 1000.0:.3f} ms")
        if a < 0.5 * n:
            print("Interpretation: major display-phase bypass detected.")
        elif gain > 1000.0:
            print("Interpretation: measurable async benefit, but not a full vblank bypass.")
        else:
            print("Interpretation: no meaningful phase bypass; proceed to phase/refresh-control work.")


if __name__ == "__main__":
    try:
        main(sys.argv)
    except (OSError, RuntimeError) as exc:
        raise SystemExit(f"ERROR: {exc}")
