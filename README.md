# Blackhole attacks in DBR for UASNs

CSC 466 project (Olivia Whitelaw, Esme Smith). Aqua-Sim NG on ns-3.41, with
depth-based routing (DBR), malicious relays and depth-band placement.

## Layout

| Path | What it is |
|---|---|
| `aqua-sim-ng/` | Aqua-Sim NG with our DBR fixes and attack modes |
| `aqua-sim-ng/examples/uasn-dbr.cc` | The scenario (`UasnDbr`) |
| `aqua-sim-ng/model/aqua-sim-routing-dbr.*` | DBR + `SetAttack()` |
| `scripts/run_experiments.py` | Runs the attack x placement x fraction x seed grid, writes CSV |
| `scripts/analyze.py` | Averages the CSV, writes `summary.csv` and graphs |

## Setup (once)

```bash
cd ~/ns3-workspace
git clone https://github.com/oliwhitla/uasn-blackhole-attacks-simulation.git
ln -s ~/ns3-workspace/uasn-blackhole-attacks-simulation/aqua-sim-ng \
      ~/ns3-workspace/ns-allinone-3.41/ns-3.41/src/aqua-sim-ng
cd ~/ns3-workspace/ns-allinone-3.41/ns-3.41
./ns3 configure --build-profile=debug --enable-examples --enable-tests --disable-python --disable-werror
./ns3 build -j2
sudo apt install -y python3-matplotlib
```

After any change to the C++ code, run `./ns3 build -j2` again.

## One run

```bash
cd ~/ns3-workspace/ns-allinone-3.41/ns-3.41
./ns3 run "UasnDbr --attack=blackhole --placement=shallow --fraction=0.1 --RngRun=1"
```

| Option | Default | Meaning |
|---|---|---|
| `--relays` | 100 | relay nodes (max 252) |
| `--width --length --depth` | 300 300 500 | box size (m) |
| `--range` | 100 | acoustic range (m) |
| `--packets --interval` | 50, 10 s | traffic from the source |
| `--attack` | none | `none`, `blackhole`, `grayhole`, `depthspoof`, `sinkhole` |
| `--placement` | random | `random`, `shallow`, `middle`, `deep`, or a band number |
| `--fraction` | 0 | fraction of relays that are bad |
| `--badNodes` | | exact number of bad nodes (overrides `--fraction`) |
| `--bands` | 3 | number of depth bands |
| `--dropProb` | 0.5 | grayhole drop probability |
| `--spoofZ` | depth + range | height a spoofer/sinkhole claims (z is height above the seafloor) |
| `--RngRun` | 1 | seed: same seed = same node layout in every run |

## Attack model

Bad nodes are picked only from relays, never the source or sink.

- **blackhole**: drops every packet it should forward and sends nothing (Das et al., 2024).
- **grayhole**: drops each packet with probability `dropProb`, decided once per packet, and forwards the rest normally (Das et al., 2024).
- **depthspoof**: immediately re-broadcasts the packet claiming a fake shallow depth, so honest neighbours cancel their own copies (Zuba et al., 2013). The copy is genuine, so the sink still accepts it if it hears it directly.
- **sinkhole**: the same fake-shallow broadcast to win forwarding, but the copy is addressed to no one, so it is never delivered (Sun et al., 2026).

For one seed, the node layout is identical across all attacks, placements and
fractions, and bad-node sets nest (the 10% set contains the 5% set). Comparisons
across settings therefore are not confounded by different layouts.

## Experiments

```bash
cd ~/ns3-workspace/uasn-blackhole-attacks-simulation

# 1. pick a box size / range that is connected on every seed
python3 scripts/run_experiments.py connectivity --boxes 300x300x500 250x250x500 --ranges 100 120 --seeds 1-20

# 2. main experiment (add the box/range you picked, e.g. --width 250 --length 250)
python3 scripts/run_experiments.py --attacks blackhole --placements random shallow middle deep \
    --fractions 0.05 0.1 0.2 0.3 --seeds 1-20 --out results/blackhole.csv

# 3. extensions
python3 scripts/run_experiments.py --attacks grayhole --drop-probs 0.25 0.5 0.75 --seeds 1-20 --out results/grayhole.csv
python3 scripts/run_experiments.py --attacks depthspoof sinkhole --seeds 1-20 --out results/spoof.csv

# 4. averages and graphs
python3 scripts/analyze.py results/blackhole.csv
```

The run script skips runs already in the output CSV, so you can stop it with
Ctrl+C and re-run the same command to continue. `--jobs 2` (default) runs two
simulations at once; use `--jobs 1` if the VM runs short of memory.

`analyze.py` leaves out seeds that are disconnected even without attackers
(`reachable=0`) and writes `results/figures/summary.csv` plus one PNG per attack
with delivery ratio, delay and transmission cost against the percentage of bad
nodes, one line per placement, with 95% confidence intervals.

## Reproducing the original DBR bug (`--legacyNewPacketTest`)

Aqua-Sim NG's DBR decided "this is a new packet from my own application" by a
forward count of zero. The source never increments that count, so its
neighbours also saw zero, stacked a second, empty DBR header on top and read
packet ID 0 for every packet. DBR forwards each ID once, so packets 2-50 were
discarded as duplicates. We test the header direction instead (the channel
marks every received copy UP).

`--legacyNewPacketTest=1` switches every node back to the original test, so
the before/after comparison can be shown with one command each:

```bash
cd ~/ns3-workspace/ns-allinone-3.41/ns-3.41
./ns3 run "UasnDbr --RngRun=1"                          # fixed:    delivered=50 (of 50)
./ns3 run "UasnDbr --RngRun=1 --legacyNewPacketTest=1"  # original: delivered=1  (of 50)
```

Expected results:

| Seed | Fixed (`=0`, default) | Original (`=1`) |
|---|---|---|
| 1 | 50/50 | 1/50 |
| 2 | 50/50 | 1/50 |
| 3 | 50/50 | 1/50 |

The run prints a `NOTE:` line when the option is on, and the last CSV column
(`legacyNewPacketTest`) records it. It only reverts this one fix; the other DBR
fixes stay in place. Leave it off for all real experiments.

## CSV columns

`run, relays, range, width, length, depth, packets, attack, placement,
fractionAsked, fraction, bad, dropProb, sent, delivered, pdr, avgDelay, tx,
txCost, duplicates, attackerDrops, spoofTx, srcNeighbours, sinkNeighbours,
reachable, badInRangeOfSink, legacyNewPacketTest`

- `fraction` is the real fraction of bad nodes. It is lower than `fractionAsked` when a band has too few relays.
- `reachable` is 1 if DBR could reach the sink with every node honest.
- `badInRangeOfSink` is the number of bad nodes that are one hop from the sink.
- `legacyNewPacketTest` is 1 if the run used the original buggy DBR test (see above).
