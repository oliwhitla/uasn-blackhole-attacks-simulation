#!/usr/bin/env python3
"""
Average the UasnDbr CSV results and plot delivery ratio, delay and
transmission cost against the fraction of bad nodes, one line per placement.

  python3 scripts/analyze.py results/blackhole.csv
  python3 scripts/analyze.py results/*.csv --out results/figures

Writes <out>/summary.csv (mean, std, 95% CI and n for every group) and one
PNG per attack. Needs matplotlib for the graphs:  sudo apt install python3-matplotlib

Seeds whose layout is not connected even with no attackers (reachable=0)
are left out by default, since their 0% delivery has nothing to do with the
attack. Use --keep-unreachable to include them.

Delay and cost only exist for runs that delivered something, so they are
averaged over those runs; the n_delivered column says how many that was.
"""

import argparse
import csv
import math
import os
import sys
from collections import defaultdict

PLACEMENT_ORDER = ["random", "shallow", "middle", "deep"]
METRICS = [
    ("pdr", "Delivery ratio", False),
    ("avgDelay", "Average delay (s)", True),
    ("txCost", "Transmissions per delivered packet", True),
]


def read_rows(paths):
    rows = []
    for p in paths:
        with open(p) as fh:
            rows.extend(csv.DictReader(fh))
    if not rows:
        sys.exit("No rows found.")
    return rows


def mean_ci(values):
    n = len(values)
    if n == 0:
        return float("nan"), float("nan"), float("nan"), 0
    m = sum(values) / n
    if n == 1:
        return m, 0.0, 0.0, 1
    sd = math.sqrt(sum((v - m) ** 2 for v in values) / (n - 1))
    # t critical value for 95% (close enough for n >= 2 without scipy)
    t = {2: 12.71, 3: 4.30, 4: 3.18, 5: 2.78, 6: 2.57, 7: 2.45, 8: 2.36, 9: 2.31, 10: 2.26}
    tc = t.get(n, 2.0 if n < 30 else 1.96)
    return m, sd, tc * sd / math.sqrt(n), n


def label(attack, drop_prob):
    if attack == "grayhole":
        return f"grayhole (drop {float(drop_prob):g})"
    return attack


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv", nargs="+")
    ap.add_argument("--out", default=None, help="output folder (default: next to the first CSV)")
    ap.add_argument("--keep-unreachable", action="store_true")
    a = ap.parse_args()
    out = a.out or os.path.join(os.path.dirname(os.path.abspath(a.csv[0])), "figures")
    os.makedirs(out, exist_ok=True)

    rows = read_rows(a.csv)
    # Drop seeds that are disconnected with no attackers.
    bad_seeds = {r["run"] for r in rows if r["reachable"] == "0"}
    if bad_seeds and not a.keep_unreachable:
        print(f"Leaving out {len(bad_seeds)} disconnected seed(s): {', '.join(sorted(bad_seeds, key=int))}")
        rows = [r for r in rows if r["run"] not in bad_seeds]

    baseline = [r for r in rows if r["attack"] == "none"]
    attacked = [r for r in rows if r["attack"] != "none"]

    # group: (attack label, placement, fraction asked)
    groups = defaultdict(list)
    for r in attacked:
        groups[(label(r["attack"], r["dropProb"]), r["placement"], float(r["fractionAsked"]))].append(r)

    def stats(rs):
        res = {}
        for key, _, needs_delivery in METRICS:
            vals = [float(r[key]) for r in rs if not needs_delivery or int(r["delivered"]) > 0]
            res[key] = mean_ci(vals)
        res["n"] = len(rs)
        res["n_delivered"] = sum(int(r["delivered"]) > 0 for r in rs)
        res["bad"] = sum(int(r["bad"]) for r in rs) / max(len(rs), 1)
        res["fraction"] = sum(float(r["fraction"]) for r in rs) / max(len(rs), 1)
        return res

    base = stats(baseline) if baseline else None
    summary_path = os.path.join(out, "summary.csv")
    with open(summary_path, "w", newline="") as fh:
        w = csv.writer(fh)
        hdr = ["attack", "placement", "fractionAsked", "fractionActual", "badNodesMean", "n", "n_delivered"]
        for key, _, _ in METRICS:
            hdr += [f"{key}_mean", f"{key}_std", f"{key}_ci95"]
        w.writerow(hdr)

        def write(att, pl, fr, s):
            row = [att, pl, f"{fr:g}", f"{s['fraction']:.4g}", f"{s['bad']:.3g}", s["n"], s["n_delivered"]]
            for key, _, _ in METRICS:
                m, sd, ci, _ = s[key]
                row += [f"{m:.4g}", f"{sd:.4g}", f"{ci:.4g}"]
            w.writerow(row)

        if base:
            write("none", "none", 0, base)
        for (att, pl, fr) in sorted(groups, key=lambda k: (k[0], PLACEMENT_ORDER.index(k[1]) if k[1] in PLACEMENT_ORDER else 9, k[2])):
            write(att, pl, fr, stats(groups[(att, pl, fr)]))
    print(f"Summary table: {summary_path}")

    # quick text table for the terminal
    if base:
        print(f"\nbaseline (no attackers, n={base['n']}): pdr={base['pdr'][0]:.3f} "
              f"delay={base['avgDelay'][0]:.2f}s cost={base['txCost'][0]:.1f}")
    attacks = sorted({k[0] for k in groups})
    for att in attacks:
        print(f"\n{att}: delivery ratio (mean over seeds)")
        fracs = sorted({k[2] for k in groups if k[0] == att})
        pls = [p for p in PLACEMENT_ORDER if any(k[0] == att and k[1] == p for k in groups)]
        pls += sorted({k[1] for k in groups if k[0] == att} - set(pls))
        print("  fraction " + "".join(f"{p:>10}" for p in pls))
        for fr in fracs:
            cells = []
            for p in pls:
                g = groups.get((att, p, fr))
                cells.append(f"{stats(g)['pdr'][0]:>10.3f}" if g else f"{'-':>10}")
            print(f"  {fr:>8g} " + "".join(cells))

    try:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("\nmatplotlib is not installed, so no graphs. Install it with:\n"
              "  sudo apt install -y python3-matplotlib")
        return

    colours = {"random": "#555555", "shallow": "#1f77b4", "middle": "#2ca02c", "deep": "#d62728"}
    markers = {"random": "o", "shallow": "^", "middle": "s", "deep": "v"}
    for att in attacks:
        fig, axes = plt.subplots(1, 3, figsize=(15, 4.2))
        pls = [p for p in PLACEMENT_ORDER if any(k[0] == att and k[1] == p for k in groups)]
        pls += sorted({k[1] for k in groups if k[0] == att} - set(pls))
        for ax, (key, title, _) in zip(axes, METRICS):
            for p in pls:
                fr = sorted(k[2] for k in groups if k[0] == att and k[1] == p)
                xs, ys, es = [], [], []
                if base:  # every line starts at the shared baseline
                    xs.append(0.0)
                    ys.append(base[key][0])
                    es.append(base[key][2])
                for f in fr:
                    m, _, ci, n = stats(groups[(att, p, f)])[key]
                    if n:
                        xs.append(f)
                        ys.append(m)
                        es.append(ci)
                ax.errorbar([x * 100 for x in xs], ys, yerr=es, label=p, capsize=3,
                            color=colours.get(p), marker=markers.get(p, "o"), linewidth=1.6)
            ax.set_xlabel("Bad nodes (% of relays)")
            ax.set_title(title)
            ax.grid(True, alpha=0.3)
            if key == "pdr":
                ax.set_ylim(-0.02, 1.05)
        axes[0].legend(title="Placement")
        fig.suptitle(f"DBR under {att} attack (mean over seeds, 95% CI)")
        fig.tight_layout()
        name = att.replace(" ", "_").replace("(", "").replace(")", "")
        path = os.path.join(out, f"{name}.png")
        fig.savefig(path, dpi=150)
        plt.close(fig)
        print(f"Graph: {path}")


if __name__ == "__main__":
    main()
