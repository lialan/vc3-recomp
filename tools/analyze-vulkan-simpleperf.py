#!/usr/bin/env python3
"""Analyze a canonical offscreen recording locally using its exact libmain.so.

No device access. Percentages describe sampled user CPU, not GPU or frame time.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import shutil
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("recording", type=Path)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--ndk", type=Path, default=Path.home() / "Library/Android/sdk/ndk/27.3.13750724")
    parser.add_argument("--last-seconds", type=float, default=0, help="Optional trailing wall-time window within the recording")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.last_seconds < 0:
        parser.error("last-seconds must be nonnegative")
    recording = args.recording.resolve()
    report = json.loads((recording / "report.json").read_text())
    binary = args.build_dir.resolve() / "libmain.so"
    digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    if digest != report["binary_hashes"]["libmain.so"]:
        parser.error("libmain.so hash differs from the recorded binary")
    remote = report["environment"]["LD_LIBRARY_PATH"]
    if not re.fullmatch(r"/data/local/tmp/vc3-offscreen-[0-9a-f]+", remote):
        parser.error("unexpected recording remote path")
    symfs = recording / "symfs"
    destination = symfs / remote.lstrip("/") / "libmain.so"
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(binary, destination)
    sys.path.insert(0, str(args.ndk / "simpleperf"))
    from simpleperf_report_lib import ReportLib
    lib = ReportLib()
    lib.SetRecordFile(str(recording / "perf.data"))
    lib.SetSymfs(str(symfs))
    samples = []
    try:
        while sample := lib.GetNextSample():
            symbol = lib.GetSymbolOfCurrentSample().symbol_name
            chain = lib.GetCallChainOfCurrentSample()
            callers = tuple(chain.entries[i].symbol.symbol_name for i in range(min(chain.nr, 8))) if "memcmp" in symbol else ()
            samples.append((sample.time, sample.period, symbol, callers))
    finally:
        lib.Close()
    if not samples:
        parser.error("recording contains no samples")
    first, last = min(s[0] for s in samples), max(s[0] for s in samples)
    cutoff = max(first, last - int(args.last_seconds * 1e9)) if args.last_seconds else first
    self_time, memcmp_callers = Counter(), Counter()
    selected = [s for s in samples if s[0] >= cutoff]
    for _, period, symbol, callers in selected:
        self_time[symbol] += period
        if "memcmp" in symbol:
            memcmp_callers[" -> ".join(callers)] += period
    total = sum(self_time.values())

    def rows(counter):
        return [{"symbol_or_chain": name, "sampled_cpu_percent": 100 * count / total,
                 "event_count": count} for name, count in counter.most_common(30)]

    result = {"scope": "Sampled user CPU only; no GPU or frame-time attribution",
              "libmain_sha256": digest, "recorded_samples": len(samples), "selected_samples": len(selected),
              "selected_wall_seconds": (last - cutoff) / 1e9, "event_count": total,
              "self": rows(self_time), "memcmp_callchains": rows(memcmp_callers)}
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({k: result[k] for k in ("selected_samples", "selected_wall_seconds")}))
    for row in result["memcmp_callchains"][:8]:
        print(f"{row['sampled_cpu_percent']:.2f}% {row['symbol_or_chain']}")


if __name__ == "__main__":
    main()
