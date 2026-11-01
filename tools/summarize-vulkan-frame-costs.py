#!/usr/bin/env python3
"""Summarize optional shared Vulkan per-frame costs without adding overlapping timers.

Windows use completed frame numbers: NAME:START:END, inclusive start/exclusive end.
CPU time is the calling thread only; wall minus CPU includes waits, scheduling,
and work on other threads. Zero pipeline-creation frames are a diagnostic subset,
not proof of a fully warm run (previous compilation can affect subsequent pacing).
"""
import argparse
import json
import math
from pathlib import Path
import re


def summarize(rows):
    fields = sorted(set().union(*(r.keys() for r in rows))) if rows else []
    metrics = {}
    for field in fields:
        if not field.endswith("_ms"):
            continue
        values = sorted(r[field] for r in rows if field in r)
        metrics[field] = dict(count=len(values), mean=sum(values)/len(values),
            p50=values[math.ceil(len(values)*.50)-1],
            p95=values[math.ceil(len(values)*.95)-1],
            p99=values[math.ceil(len(values)*.99)-1], max=values[-1])
    return dict(frames=len(rows), metrics=metrics)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--window", action="append", default=[], metavar="NAME:START:END")
    parser.add_argument("--tail-ms", type=float, default=20.0,
                        help="Compare frames above this interval with shorter frames (default: 20 ms)")
    args = parser.parse_args()
    if not math.isfinite(args.tail_ms) or args.tail_ms <= 0:
        parser.error("tail-ms must be finite and positive")
    rows = []
    for line in args.log.read_text(errors="replace").splitlines():
        if "[Vulkan frame costs]" not in line:
            continue
        row = {k: float(v) for k, v in re.findall(r"(\w+)=(-?[\d.]+)", line)}
        if "frame" not in row or "interval_ms" not in row:
            parser.error("incomplete frame-cost record")
        rows.append(row)
    if not rows:
        parser.error("no frame-cost records; enable CHIHIRO_VULKAN_FRAME_COSTS=1")
    windows = []
    for spec in args.window:
        try:
            name, start, end = spec.split(":")
            start, end = int(start), int(end)
            if start >= end:
                raise ValueError()
        except ValueError:
            parser.error("window must be NAME:START:END with START < END")
        windows.append((name, start, end))
    if not windows:
        windows = [("all", 0, math.inf)]
    output = dict(log=str(args.log.resolve()),
        caution="Pipeline/GPU/wait timers overlap phases. CPU covers the calling thread only. Zero-creation filtering is not a warm-cache acceptance test.",
        windows={})
    for name, start, end in windows:
        selected = [r for r in rows if start <= r["frame"] < end]
        warm = [r for r in selected if r.get("pipeline_ms") == 0]
        output["windows"][name] = dict(all=summarize(selected),
            zero_pipeline_creation=summarize(warm),
            zero_creation_tail_comparison=dict(threshold_ms=args.tail_ms,
                over=summarize([r for r in warm if r["interval_ms"] > args.tail_ms]),
                at_or_under=summarize([r for r in warm if r["interval_ms"] <= args.tail_ms])),
            thread_cpu_timed_frames=sum(r.get("thread_cpu_timing", 0) == 1 for r in selected),
            gpu_timed_frames=sum(r.get("gpu_timing", 0) == 1 for r in selected))
    print(json.dumps(output, indent=2))


if __name__ == "__main__":
    main()
