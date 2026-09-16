#!/usr/bin/env python3
"""Analyze RK3588 HDMI-RX/KMS V3.4-V3.8 timing CSV traces."""
from __future__ import annotations

import csv
import io
import math
import statistics
import sys
from collections import defaultdict
from pathlib import Path

WARMUP_FRAMES = 60
V4L2_TS_MASK = 0x0000E000
V4L2_TS_MONOTONIC = 0x00002000
V4L2_SRC_MASK = 0x00070000
V4L2_SRC_SOE = 0x00010000


def median(xs):
    return statistics.median(xs) if xs else math.nan


def percentile(xs, p):
    if not xs:
        return math.nan
    xs = sorted(xs)
    k = (len(xs) - 1) * p
    lo, hi = math.floor(k), math.ceil(k)
    return xs[lo] if lo == hi else xs[lo] * (hi - k) + xs[hi] * (k - lo)


def integer(value):
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def delta_us(a, b):
    if a is None or b is None or b < a:
        return None
    return (b - a) / 1000.0


def phase_us(event_ns, vblank_ns, period_ns):
    if None in (event_ns, vblank_ns, period_ns) or period_ns <= 0:
        return None
    return ((event_ns - vblank_ns) % period_ns) / 1000.0


def next_vblank_us(event_ns, vblank_ns, period_ns):
    phase = phase_us(event_ns, vblank_ns, period_ns)
    if phase is None:
        return None
    remaining = period_ns / 1000.0 - phase
    return 0.0 if abs(remaining - period_ns / 1000.0) < 0.001 else remaining


def load_csv(path):
    raw = Path(path).read_text(errors="replace")
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
    return rows[WARMUP_FRAMES:] if len(rows) > WARMUP_FRAMES + 10 else rows


def analyze(path):
    rows = load_csv(path)
    sync_rows = sum(integer(r.get("async_commit")) == 0 for r in rows)
    async_rows = sum(integer(r.get("async_commit")) == 1 for r in rows)
    early_rows = sum(integer(r.get("early_submit")) == 1 for r in rows)
    overlap_rows = sum(integer(r.get("overlap_commit")) == 1 for r in rows)
    intentional_drops = sum(integer(r.get("intentional_drops_before")) or 0 for r in rows)
    if async_rows:
        rows = [r for r in rows if integer(r.get("async_commit")) == 1]

    metrics = defaultdict(list)
    seq_gaps = missing_fences = phase_rows = monotonic_rows = soe_rows = 0
    per_index_last_dq = {}
    per_index_reuse = defaultdict(list)
    cadence_wrap_frames = []
    two_vblank_frames = []
    previous_vts_to_dq = None
    prev = None

    for row in rows:
        seq, idx = integer(row.get("sequence")), integer(row.get("index"))
        fence = integer(row.get("fence_present"))
        dq, cb = integer(row.get("dq_ns")), integer(row.get("commit_begin_ns"))
        ce, out = integer(row.get("commit_end_ns")), integer(row.get("out_signal_ns"))
        qbuf, vts = integer(row.get("qbuf_ns")), integer(row.get("v4l2_ts_ns"))
        if None in (seq, idx, dq, cb, ce, out):
            continue
        missing_fences += fence == 0
        for key, val in (
            ("dq_to_commit_begin", delta_us(dq, cb)), ("commit_ioctl", delta_us(cb, ce)),
            ("commit_to_out", delta_us(ce, out)), ("dq_to_out", delta_us(dq, out)),
            ("own_out_to_qbuf", delta_us(out, qbuf)),
        ):
            if val is not None:
                metrics[key].append(val)
        if prev is not None:
            if seq > prev["seq"] + 1:
                seq_gaps += seq - prev["seq"] - 1
            for key, val in (
                ("dq_period", delta_us(prev["dq"], dq)), ("out_period", delta_us(prev["out"], out)),
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

        period = integer(row.get("mode_period_ns"))
        flags = integer(row.get("v4l2_flags"))
        phase_values = (
            integer(row.get("dq_vblank_sequence")), integer(row.get("dq_vblank_ns")),
            integer(row.get("commit_vblank_sequence")), integer(row.get("commit_vblank_ns")),
            integer(row.get("out_vblank_sequence")), integer(row.get("out_vblank_ns")),
        )
        if integer(row.get("phase_valid")) == 1 and period and None not in phase_values:
            phase_rows += 1
            dq_seq, dq_vb, commit_seq, commit_vb, out_seq, out_vb = phase_values
            row_monotonic = (
                flags is not None and
                (flags & V4L2_TS_MASK) == V4L2_TS_MONOTONIC
            )
            if row_monotonic:
                monotonic_rows += 1
                val = delta_us(vts, dq)
                if val is not None:
                    metrics["v4l2_timestamp_to_dq"].append(val)
            if flags is not None and (flags & V4L2_SRC_MASK) == V4L2_SRC_SOE:
                soe_rows += 1
            for key, val in (
                ("dq_since_vblank", phase_us(dq, dq_vb, period)),
                ("dq_to_next_vblank", next_vblank_us(dq, dq_vb, period)),
                ("commit_since_vblank", phase_us(cb, commit_vb, period)),
                ("commit_to_next_vblank", next_vblank_us(cb, commit_vb, period)),
                ("out_since_vblank", phase_us(out, out_vb, period)),
            ):
                if val is not None:
                    metrics[key].append(val)
            metrics["dq_to_commit_vblank_advance"].append(commit_seq - dq_seq)
            out_steps = out_seq - commit_seq
            metrics["commit_to_out_vblank_advance"].append(out_steps)
            metrics["mode_period_ns"].append(period)
            if out_steps > 1:
                two_vblank_frames.append(integer(row.get("frame")))
            if row_monotonic and metrics["v4l2_timestamp_to_dq"]:
                current_vts_to_dq = metrics["v4l2_timestamp_to_dq"][-1]
                if (previous_vts_to_dq is not None and
                    current_vts_to_dq - previous_vts_to_dq > period / 2000.0):
                    cadence_wrap_frames.append(integer(row.get("frame")))
                previous_vts_to_dq = current_vts_to_dq
        prev = {"seq": seq, "dq": dq, "out": out, "vts": vts}

    if not metrics["commit_to_out"]:
        raise RuntimeError(f"{path}: no usable timing rows")
    v4l2_timestamps = [
        integer(row.get("v4l2_ts_ns")) for row in rows
        if integer(row.get("v4l2_ts_ns")) is not None
    ]
    input_period_us = (
        (v4l2_timestamps[-1] - v4l2_timestamps[0]) /
        (len(v4l2_timestamps) - 1) / 1000.0
        if len(v4l2_timestamps) >= 2 else math.nan
    )
    mode_period_ns = median(metrics["mode_period_ns"])
    input_hz = 1_000_000.0 / input_period_us if input_period_us else math.nan
    output_hz = 1_000_000_000.0 / mode_period_ns if mode_period_ns else math.nan
    cadence_delta_hz = abs(output_hz - input_hz)
    slip_seconds = (
        1.0 / cadence_delta_hz
        if not math.isnan(cadence_delta_hz) and cadence_delta_hz >= 1e-9
        else math.inf
    )
    two_vblank_spacings = [
        b - a for a, b in zip(two_vblank_frames, two_vblank_frames[1:])
        if a is not None and b is not None
    ]
    return {
        "path": str(path), "rows": len(rows), "sync_rows": sync_rows,
        "async_rows": async_rows, "early_rows": early_rows,
        "overlap_rows": overlap_rows, "seq_gaps": seq_gaps,
        "intentional_drops": intentional_drops,
        "unexpected_gaps": max(0, seq_gaps - intentional_drops),
        "missing_fences": missing_fences, "metrics": metrics,
        "per_index_reuse": per_index_reuse, "phase_rows": phase_rows,
        "monotonic_rows": monotonic_rows, "soe_rows": soe_rows,
        "input_hz": input_hz, "output_hz": output_hz,
        "slip_seconds": slip_seconds,
        "two_vblank_frames": two_vblank_frames,
        "two_vblank_spacings": two_vblank_spacings,
        "cadence_wrap_frames": cadence_wrap_frames,
    }


def fmt_us(us):
    return "n/a" if math.isnan(us) else f"{us / 1000.0:.3f} ms"


def metric_line(label, xs, unit="time"):
    p50, p95 = median(xs), percentile(xs, 0.95)
    if unit == "count":
        a = "n/a" if math.isnan(p50) else f"{p50:.1f}"
        b = "n/a" if math.isnan(p95) else f"{p95:.1f}"
    else:
        a, b = fmt_us(p50), fmt_us(p95)
    return f"  {label:<28} p50 {a:>10}   p95 {b:>10}"


def show(label, result):
    m = result["metrics"]
    print(f"{label}: {result['path']} ({result['rows']} post-warmup rows)")
    print(f"  commit modes: normal={result['sync_rows']} async={result['async_rows']}")
    print(
        "  sequence gaps: "
        f"raw={result['seq_gaps']} intentional={result['intentional_drops']} "
        f"unexpected={result['unexpected_gaps']}   "
        f"missing acquire fences: {result['missing_fences']}"
    )
    if result["early_rows"]:
        print(f"  early-window profiler rows: {result['early_rows']}")
        if result["overlap_rows"] or result["intentional_drops"]:
            print(
                f"  V3.8 overlap rows: {result['overlap_rows']}   "
                f"intentional drops: {result['intentional_drops']}"
            )
    for title, key in (
        ("V4L2 timestamp cadence", "v4l2_period"), ("DQ -> next DQ", "dq_period"),
        ("OUT -> next OUT", "out_period"), ("previous OUT -> DQ", "prev_out_to_dq"),
        ("previous OUT -> commit", "prev_out_to_commit"), ("DQ -> commit call", "dq_to_commit_begin"),
        ("atomic commit ioctl", "commit_ioctl"), ("commit -> completion", "commit_to_out"),
        ("DQ -> completion", "dq_to_out"), ("own completion -> QBUF", "own_out_to_qbuf"),
        ("same-buffer DQ reuse", "same_buffer_reuse"),
    ):
        print(metric_line(title, m[key]))
    if result["per_index_reuse"]:
        values = ", ".join(f"b{i}={fmt_us(median(result['per_index_reuse'][i]))}" for i in sorted(result["per_index_reuse"]))
        print("  per-index reuse p50:          " + values)
    if result["phase_rows"]:
        print("\n  V3.6 phase profiler:")
        print(f"  valid rows: {result['phase_rows']}   monotonic timestamps: {result['monotonic_rows']}   SOE timestamps: {result['soe_rows']}")
        for title, key in (
            ("V4L2 timestamp -> DQ", "v4l2_timestamp_to_dq"),
            ("DQ phase after vblank", "dq_since_vblank"), ("DQ -> predicted vblank", "dq_to_next_vblank"),
            ("commit phase after vblank", "commit_since_vblank"),
            ("commit -> predicted vblank", "commit_to_next_vblank"), ("OUT after vblank", "out_since_vblank"),
        ):
            print(metric_line(title, m[key]))
        print(metric_line("DQ -> commit vblank steps", m["dq_to_commit_vblank_advance"], "count"))
        print(metric_line("commit -> OUT vblank steps", m["commit_to_out_vblank_advance"], "count"))
        print("\n  V3.7 cadence diagnosis:")
        print(f"  measured input cadence:      {result['input_hz']:.6f} Hz")
        print(f"  active output cadence:       {result['output_hz']:.6f} Hz")
        beat = (
            f"{result['slip_seconds']:.3f} seconds"
            if math.isfinite(result["slip_seconds"])
            else "matched within measurement precision"
        )
        print(f"  theoretical beat period:    {beat}")
        print(f"  timestamp sawtooth wraps:    {len(result['cadence_wrap_frames'])}")
        print(f"  two-vblank completions:      {len(result['two_vblank_frames'])} / {result['phase_rows']}")
        if result["two_vblank_spacings"]:
            print(f"  two-vblank spacing p50:      {median(result['two_vblank_spacings']):.1f} frames")


def main(argv):
    if len(argv) not in (2, 3):
        raise SystemExit("usage: analyze.py TRACE.csv [ASYNC.csv]")
    normal = analyze(argv[1])
    print("=== RK3588 HDMI-RX / KMS TIMING REPORT ===")
    show("REFERENCE" if len(argv) == 3 else "TRACE", normal)
    if len(argv) == 3:
        comparison = analyze(argv[2])
        print()
        is_async = comparison["async_rows"] > 0
        is_early = comparison["early_rows"] > 0
        show("ASYNC" if is_async else ("EARLY" if is_early else "ALIGNED"), comparison)
        gain = median(normal["metrics"]["commit_to_out"]) - median(comparison["metrics"]["commit_to_out"])
        print(f"\nCompletion median improvement: {gain / 1000.0:.3f} ms")
        print(
            "Two-vblank completions: "
            f"{len(normal['two_vblank_frames'])} -> "
            f"{len(comparison['two_vblank_frames'])}"
        )


if __name__ == "__main__":
    try:
        main(sys.argv)
    except (OSError, RuntimeError) as exc:
        raise SystemExit(f"ERROR: {exc}")
