#!/usr/bin/env python3
"""
Run the UasnDbr scenario over every attack x placement x fraction x seed and
collect one CSV line per run.

Typical use (inside the VM, from the project repo):

  # quick check: one attack, two placements, 3 seeds
  python3 scripts/run_experiments.py --attacks blackhole --placements random shallow \
      --fractions 0.1 0.2 --seeds 1-3 --out results/test.csv

  # the full experiment from the proposal
  python3 scripts/run_experiments.py --attacks blackhole \
      --placements random shallow middle deep --fractions 0.05 0.1 0.2 0.3 \
      --seeds 1-20 --out results/blackhole.csv

  # extensions
  python3 scripts/run_experiments.py --attacks grayhole --drop-probs 0.25 0.5 0.75 ...
  python3 scripts/run_experiments.py --attacks depthspoof sinkhole --fractions 0.01 0.02 0.05 ...

  # pick a box size / range: how often is a random layout DBR-connected?
  python3 scripts/run_experiments.py connectivity --boxes 300x300x500 250x250x500 \
      --ranges 100 120 --seeds 1-20

A run with no bad nodes (the baseline) is done once per seed automatically.
Runs already in the output CSV are skipped, so you can stop with Ctrl+C and
re-run the same command to carry on.
"""

import argparse
import csv
import glob
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed

DEFAULT_NS3 = os.path.expanduser("~/ns3-workspace/ns-allinone-3.41/ns-3.41")



def parse_seeds(items):
    seeds = []
    for item in items:
        for part in str(item).split(","):
            if "-" in part:
                a, b = part.split("-")
                seeds.extend(range(int(a), int(b) + 1))
            elif part:
                seeds.append(int(part))
    return seeds


def find_binary(ns3_dir):
    """Return (argv prefix, env) that runs UasnDbr without rebuilding."""
    hits = [
        p
        for p in glob.glob(os.path.join(ns3_dir, "build", "**", "ns3*-UasnDbr*"), recursive=True)
        if os.access(p, os.X_OK) and os.path.isfile(p)
    ]
    env = dict(os.environ)
    if hits:
        hits.sort(key=os.path.getmtime, reverse=True)
        lib = os.path.join(ns3_dir, "build", "lib")
        env["LD_LIBRARY_PATH"] = lib + os.pathsep + env.get("LD_LIBRARY_PATH", "")
        return [hits[0]], env
    ns3 = os.path.join(ns3_dir, "ns3")
    if not os.path.exists(ns3):
        sys.exit(f"Can't find ns-3 in {ns3_dir}. Pass --ns3-dir.")
    return None, env  # fall back to ./ns3 run


def run_one(ns3_dir, binary, env, args):
    """Run UasnDbr with a list of --key=value args, return the CSV row dict."""
    if binary:
        cmd = binary + args
    else:
        cmd = [os.path.join(ns3_dir, "ns3"), "run", "--no-build", "UasnDbr " + " ".join(args)]
    res = subprocess.run(cmd, cwd=ns3_dir, env=env, capture_output=True, text=True)
    for line in res.stdout.splitlines():
        if line.startswith("CSV,"):
            return line[4:].split(","), res
    raise RuntimeError(
        "UasnDbr gave no CSV line.\ncommand: "
        + " ".join(cmd)
        + "\n--- stdout ---\n"
        + res.stdout[-2000:]
        + "\n--- stderr ---\n"
        + res.stderr[-2000:]
    )


def csv_header(ns3_dir, binary, env):
    if binary:
        cmd = binary + ["--csvHeader"]
    else:
        cmd = [os.path.join(ns3_dir, "ns3"), "run", "--no-build", "UasnDbr --csvHeader"]
    out = subprocess.run(cmd, cwd=ns3_dir, env=env, capture_output=True, text=True).stdout
    for line in out.splitlines():
        if line.startswith("CSVHDR,"):
            return line[7:].split(",")
    sys.exit("Couldn't read the CSV header from UasnDbr. Is the scenario built? (./ns3 build)")


def common_args(a, box=None, rng=None):
    w, l, d = box if box else (a.width, a.length, a.depth)
    out = [
        f"--relays={a.relays}",
        f"--width={w}",
        f"--length={l}",
        f"--depth={d}",
        f"--range={rng if rng is not None else a.range}",
        f"--packets={a.packets}",
        f"--interval={a.interval}",
        f"--bands={a.bands}",
        "--verbose=0",
    ]
    if a.spoof_z is not None:
        out.append(f"--spoofZ={a.spoof_z}")
    return out + list(a.extra or [])


def norm(x):
    return f"{float(x):.6g}"


def build_jobs(a, seeds):
    jobs = []
    for s in seeds:
        jobs.append((s, "none", "random", 0.0, 1.0))  # baseline
    for attack in a.attacks:
        probs = a.drop_probs if attack == "grayhole" else [1.0]
        for p in probs:
            for placement in a.placements:
                for f in a.fractions:
                    if f <= 0:
                        continue
                    for s in seeds:
                        jobs.append((s, attack, placement, f, p))
    return jobs


def cmd_run(a):
    seeds = parse_seeds(a.seeds)
    binary, env = find_binary(a.ns3_dir)
    header = csv_header(a.ns3_dir, binary, env)
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)

    done = set()
    if os.path.exists(a.out):
        with open(a.out) as fh:
            for r in csv.DictReader(fh):
                done.add((r["run"], r["attack"], r["placement"], norm(r["fractionAsked"]), norm(r["dropProb"])))
        print(f"{a.out}: {len(done)} runs already there, skipping those")

    # Key each job on what we ask for (fractionAsked); the real fraction can be
    # lower when a band has too few relays.
    jobs = build_jobs(a, seeds)
    todo = []
    for (s, attack, placement, f, p) in jobs:
        nominal = round(f * a.relays) / a.relays
        if attack == "none":
            key = (str(s), "none", "none", norm(0), norm(1.0))
        else:
            key = (str(s), attack, placement, norm(nominal), norm(p if attack == "grayhole" else 1.0))
        if key in done:
            continue
        done.add(key)
        args = common_args(a) + [
            f"--RngRun={s}",
            f"--attack={attack}",
            f"--placement={placement}",
            f"--fraction={f}",
            f"--dropProb={p}",
        ]
        todo.append((key, args))

    print(f"{len(todo)} runs to do ({len(seeds)} seeds), {a.jobs} at a time -> {a.out}")
    new_file = not os.path.exists(a.out) or os.path.getsize(a.out) == 0
    with open(a.out, "a", newline="") as fh:
        w = csv.writer(fh)
        if new_file:
            w.writerow(header)
        n = 0
        with ThreadPoolExecutor(max_workers=a.jobs) as pool:
            futs = {pool.submit(run_one, a.ns3_dir, binary, env, args): (key, args) for key, args in todo}
            try:
                for fut in as_completed(futs):
                    key, args = futs[fut]
                    n += 1
                    try:
                        row, _ = fut.result()
                    except Exception as e:  # keep going, report at the end
                        print(f"[{n}/{len(todo)}] FAILED {' '.join(args)}\n{e}", file=sys.stderr)
                        continue
                    w.writerow(row)
                    fh.flush()
                    r = dict(zip(header, row))
                    print(
                        f"[{n}/{len(todo)}] seed={r['run']:>3} {r['attack']:<10} {r['placement']:<8} "
                        f"bad={r['bad']:>3} pdr={float(r['pdr']):.2f} delay={float(r['avgDelay']):.2f}s "
                        f"cost={float(r['txCost']):.1f}"
                        + ("" if r["reachable"] == "1" else "  (layout not connected)")
                    )
            except KeyboardInterrupt:
                print("\nStopped. Re-run the same command to continue.")
                pool.shutdown(wait=False, cancel_futures=True)
                raise
    print(f"Done. Results in {a.out}. Next: python3 scripts/analyze.py {a.out}")


def cmd_connectivity(a):
    seeds = parse_seeds(a.seeds)
    binary, env = find_binary(a.ns3_dir)
    header = csv_header(a.ns3_dir, binary, env)
    boxes = [tuple(float(v) for v in b.lower().split("x")) for b in a.boxes]
    combos = [(b, r) for b in boxes for r in a.ranges]
    a.packets = a.packets_sweep
    print(f"{len(combos)} settings x {len(seeds)} seeds, baseline only (no attackers)\n")
    rows = []
    with ThreadPoolExecutor(max_workers=a.jobs) as pool:
        futs = {}
        for b, r in combos:
            for s in seeds:
                args = common_args(a, box=b, rng=r) + [f"--RngRun={s}"]
                futs[pool.submit(run_one, a.ns3_dir, binary, env, args)] = (b, r)
        for fut in as_completed(futs):
            b, r = futs[fut]
            row, _ = fut.result()
            rows.append((b, r, dict(zip(header, row))))
    print(f"{'box (m)':<18}{'range':>7}{'connected':>12}{'mean PDR':>10}{'min sink nbrs':>15}")
    best = None
    for b, r in combos:
        rs = [x for bb, rr, x in rows if bb == b and rr == r]
        conn = sum(x["reachable"] == "1" for x in rs)
        pdr = sum(float(x["pdr"]) for x in rs) / len(rs)
        minsink = min(int(x["sinkNeighbours"]) for x in rs)
        label = "x".join(f"{v:g}" for v in b)
        print(f"{label:<18}{r:>7g}{conn:>7}/{len(rs):<4}{pdr:>10.2f}{minsink:>15}")
        if conn == len(rs) and best is None:
            best = (label, r)
    if best:
        print(f"\nFirst setting connected on every seed: box {best[0]}, range {best[1]:g} m")
    else:
        print("\nNo setting was connected on every seed: try a smaller box or a longer range.")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", nargs="?", default="run", choices=["run", "connectivity"])
    ap.add_argument("--ns3-dir", default=DEFAULT_NS3, help="ns-3.41 folder (default: %(default)s)")
    ap.add_argument("--out", default="results/results.csv", help="CSV to append to")
    ap.add_argument("--jobs", type=int, default=2, help="runs in parallel (one per CPU core)")
    # network
    ap.add_argument("--relays", type=int, default=100)
    ap.add_argument("--width", type=float, default=300)
    ap.add_argument("--length", type=float, default=300)
    ap.add_argument("--depth", type=float, default=500)
    ap.add_argument("--range", type=float, default=100)
    ap.add_argument("--packets", type=int, default=50)
    ap.add_argument("--interval", type=float, default=10)
    ap.add_argument("--bands", type=int, default=3)
    # experiment grid
    ap.add_argument("--attacks", nargs="+", default=["blackhole"],
                    choices=["blackhole", "grayhole", "depthspoof", "sinkhole"])
    ap.add_argument("--placements", nargs="+", default=["random", "shallow", "middle", "deep"])
    ap.add_argument("--fractions", nargs="+", type=float, default=[0.05, 0.1, 0.2, 0.3])
    ap.add_argument("--seeds", nargs="+", default=["1-10"], help="e.g. 1-20 or 1 2 3")
    ap.add_argument("--drop-probs", nargs="+", type=float, default=[0.5], help="grayhole only")
    ap.add_argument("--spoof-z", type=float, default=None,
                    help="height spoofers claim (default depth + range)")
    # connectivity sweep
    ap.add_argument("--boxes", nargs="+", default=["300x300x500", "250x250x500", "200x200x500"])
    ap.add_argument("--ranges", nargs="+", type=float, default=[100, 120])
    ap.add_argument("--packets-sweep", type=int, default=5, help="packets per sweep run")
    ap.add_argument("--extra", nargs="*", help="extra UasnDbr args, e.g. --extra --drain=120")
    a = ap.parse_args()
    a.ns3_dir = os.path.abspath(os.path.expanduser(a.ns3_dir))
    if a.mode == "connectivity":
        cmd_connectivity(a)
    else:
        cmd_run(a)


if __name__ == "__main__":
    main()
