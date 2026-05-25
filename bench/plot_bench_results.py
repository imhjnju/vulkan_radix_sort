#!/usr/bin/env python3
import argparse
import csv
from pathlib import Path

import matplotlib.pyplot as plt


def read_rows(path: Path):
    rows = []
    with path.open(newline="") as f:
        filtered = (line for line in f if not line.startswith("#"))
        for row in csv.DictReader(filtered):
            row["n"] = int(row["n"])
            row["gpu_ms"] = float(row["gpu_ms"])
            row["gpu_gitems_s"] = float(row["gpu_gitems_s"])
            rows.append(row)
    return rows


def main():
    parser = argparse.ArgumentParser(description="Plot radix sort benchmark CSV files.")
    parser.add_argument("--csv", action="append", required=True, help="CSV file produced by bench")
    parser.add_argument("--out", required=True, help="Output PNG path")
    args = parser.parse_args()

    rows = []
    for csv_path in args.csv:
        rows.extend(read_rows(Path(csv_path)))

    fig, axes = plt.subplots(1, 2, figsize=(12, 5), constrained_layout=True)
    for sort in ["keys", "kv"]:
        ax = axes[0]
        for backend in sorted({row["backend"] for row in rows if row["sort"] == sort}):
            series = sorted([row for row in rows if row["backend"] == backend and row["sort"] == sort], key=lambda r: r["n"])
            if series:
                ax.plot([r["n"] for r in series], [r["gpu_ms"] for r in series], marker="o", label=f"{backend} {sort}")

        ax = axes[1]
        for backend in sorted({row["backend"] for row in rows if row["sort"] == sort}):
            series = sorted([row for row in rows if row["backend"] == backend and row["sort"] == sort], key=lambda r: r["n"])
            if series:
                ax.plot([r["n"] for r in series], [r["gpu_gitems_s"] for r in series], marker="o", label=f"{backend} {sort}")

    axes[0].set_title("GPU time")
    axes[0].set_xlabel("N")
    axes[0].set_ylabel("ms")
    axes[0].set_xscale("log", base=2)
    axes[0].grid(True, which="both", linestyle="--", alpha=0.35)
    axes[0].legend(fontsize=8)

    axes[1].set_title("Throughput")
    axes[1].set_xlabel("N")
    axes[1].set_ylabel("GItems/s")
    axes[1].set_xscale("log", base=2)
    axes[1].grid(True, which="both", linestyle="--", alpha=0.35)
    axes[1].legend(fontsize=8)

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out, dpi=180)
    print(out)


if __name__ == "__main__":
    main()
