#!/usr/bin/env python3
"""Benchmark `git repack -adfq` with two git binaries.

For each iteration, both binaries are run exactly once, in randomized
order, against a fresh copy of a template repository. Results are
written to a CSV file with columns iteration,position,variant,seconds.
"""

import argparse
import csv
import os
import random
import shutil
import statistics
import subprocess
import sys
import time


def robust_rmtree(path, attempts=20, delay=0.5):
    """`shutil.rmtree` with retries.

    Windows may briefly retain a file lock on objects (e.g. an mmap'd
    commit-graph) even after the process that opened them has exited,
    causing `rmtree` to raise `PermissionError`. Retry a few times.
    """
    for i in range(attempts):
        try:
            shutil.rmtree(path)
            return
        except (PermissionError, OSError):
            if i == attempts - 1:
                raise
            time.sleep(delay)


def time_one_repack(binary, template, work):
    """Run `<binary> -C <work> repack -adfq` once and return elapsed seconds.

    The work directory is overwritten with a fresh copy of `template`
    before the timed command runs.
    """
    if os.path.exists(work):
        robust_rmtree(work)
    shutil.copytree(template, work)
    cmd = [binary, "-C", work, "-c", "pack.threads=4", "repack", "-adfq"]
    t0 = time.monotonic_ns()
    subprocess.check_call(cmd)
    t1 = time.monotonic_ns()
    return (t1 - t0) / 1e9


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--vanilla", required=True, help="path to vanilla git binary")
    p.add_argument("--mimalloc", required=True, help="path to USE_MIMALLOC git binary")
    p.add_argument("--template", required=True,
                   help="path to bare template repository (read-only)")
    p.add_argument("--work", default="_bench/run.git",
                   help="working copy of the template (overwritten each run)")
    p.add_argument("--results", default="_bench/results.csv",
                   help="CSV output file")
    p.add_argument("--iterations", type=int, default=5,
                   help="number of iterations; each runs both variants once")
    p.add_argument("--seed", type=int, default=None,
                   help="seed for the per-iteration order shuffle "
                        "(default: nondeterministic)")
    args = p.parse_args()

    binaries = {"vanilla": args.vanilla, "mimalloc": args.mimalloc}
    for name, path in binaries.items():
        if not os.path.exists(path):
            sys.exit(f"missing {name} binary: {path}")
    if not os.path.isdir(args.template):
        sys.exit(f"missing template repo: {args.template}")

    rng = random.Random(args.seed)
    os.makedirs(os.path.dirname(args.results) or ".", exist_ok=True)

    with open(args.results, "w", newline="") as f:
        writer = csv.DictWriter(
            f, fieldnames=["iteration", "position", "variant", "seconds"])
        writer.writeheader()
        for it in range(1, args.iterations + 1):
            order = list(binaries.keys())
            rng.shuffle(order)
            print(f"=== iteration {it}: order = {order} ===", flush=True)
            for pos, variant in enumerate(order, start=1):
                seconds = time_one_repack(binaries[variant], args.template, args.work)
                writer.writerow({
                    "iteration": it,
                    "position": pos,
                    "variant": variant,
                    "seconds": f"{seconds:.6f}",
                })
                f.flush()
                print(f"  pos={pos} variant={variant} seconds={seconds:.3f}",
                      flush=True)

    # Summary
    with open(args.results) as f:
        rows = list(csv.DictReader(f))
    print("\n=== summary ===")
    for v in binaries:
        xs = [float(r["seconds"]) for r in rows if r["variant"] == v]
        if xs:
            print(f"{v:8s}  n={len(xs)}  mean={statistics.fmean(xs):.3f}s  "
                  f"stdev={statistics.pstdev(xs):.3f}s  "
                  f"min={min(xs):.3f}s  max={max(xs):.3f}s")


if __name__ == "__main__":
    main()
