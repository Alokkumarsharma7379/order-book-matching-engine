"""Run the native benchmark and retain raw trials plus reproducibility metadata."""
import argparse
import csv
from datetime import datetime, timezone
import hashlib
import io
import json
from pathlib import Path
import platform
import statistics
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True, type=Path)
    parser.add_argument("--commands", type=int, default=50000)
    parser.add_argument("--trials", type=int, default=5)
    parser.add_argument("--warmup", type=int, default=5000)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    output = args.output or Path("benchmark-results") / datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    output.mkdir(parents=True, exist_ok=False)
    executable = args.executable.resolve()
    command = [str(executable), str(args.commands), str(args.trials), str(args.warmup)]
    result = subprocess.run(command, capture_output=True, text=True, check=True)
    rows = list(csv.DictReader(io.StringIO(result.stdout)))
    if len(rows) != args.trials * 5:
        raise RuntimeError("unexpected benchmark output; no summary generated")
    for row in rows:
        if float(row["seconds"]) <= 0 or float(row["mean_ns"]) <= 0:
            raise RuntimeError("invalid measured duration; no summary generated")
        if not 0 <= int(row["median_ns"]) <= int(row["p95_ns"]) <= int(row["p99_ns"]):
            raise RuntimeError("invalid latency percentiles; no summary generated")
    (output / "raw.csv").write_text(result.stdout, encoding="utf-8")
    (output / "metadata.json").write_text(json.dumps({
        "timestamp_utc": datetime.now(timezone.utc).isoformat(), "command": command,
        "platform": platform.platform(), "processor": platform.processor(),
        "compiler": result.stderr.strip(),
        "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
        "sources_sha256": {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
                           for directory in ("engine/src", "engine/include", "engine/benchmarks")
                           for path in sorted(Path(directory).rglob("*")) if path.is_file()},
    }, indent=2), encoding="utf-8")
    text = ["# Measured local benchmark\n", "Median across trials; latency columns are medians of per-trial statistics.\n",
            "| Scenario | Commands/s | Submitted orders/s | Mean ns | Median ns | p95 ns | p99 ns |",
            "| --- | ---: | ---: | ---: | ---: | ---: | ---: |"]
    for scenario in dict.fromkeys(row["scenario"] for row in rows):
        subset = [row for row in rows if row["scenario"] == scenario]
        values = [statistics.median(float(row[key]) for row in subset) for key in (
            "commands_per_second", "orders_per_second", "mean_ns", "median_ns", "p95_ns", "p99_ns")]
        text.append(f"| {scenario} | " + " | ".join(f"{value:,.1f}" for value in values) + " |")
    text.extend(["", "Includes per-command clock and harness overhead. Core only; excludes Python, HTTP, SQL, and Redis.",
                 "Single developer-machine run; not a production capacity claim. See raw.csv and metadata.json."])
    (output / "summary.md").write_text("\n".join(text) + "\n", encoding="utf-8")
    print("\n".join(text))
    print(f"\nArtifacts: {output.resolve()}")


if __name__ == "__main__":
    main()
