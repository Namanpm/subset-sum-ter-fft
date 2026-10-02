"""Summarize an ARWLF regime campaign (see ARWLF_REGIME_CAMPAIGN.md).

Reads <dir>/cases.csv, <dir>/*_census.csv and <dir>/sentinel.csv; writes
<dir>/summary.md plus plot-ready speedup_vs_L.csv and regret_vs_L.csv, and
prints the preregistered crossover confirmation cases.  Standard library only.

    python scripts/summarize_arwlf_regime_campaign.py results/arwlf/regime-...
"""

import csv
import glob
import math
import os
import random
import statistics
import sys
from collections import defaultdict

EXACT_BACKENDS = ("k2", "k2_wide", "queue_dense", "arwlf")
BOOTSTRAP = 10000
BOOTSTRAP_SEED = 0xB007


def read(path):
    with open(path, newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def num(text):
    try:
        return float(text)
    except (TypeError, ValueError):
        return math.nan


def geomean(values):
    values = [v for v in values if v > 0 and math.isfinite(v)]
    return math.exp(sum(math.log(v) for v in values) / len(values)) if values else math.nan


def quantile(sorted_values, fraction):
    if not sorted_values:
        return math.nan
    position = fraction * (len(sorted_values) - 1)
    lower = int(position)
    upper = min(lower + 1, len(sorted_values) - 1)
    weight = position - lower
    return sorted_values[lower] * (1 - weight) + sorted_values[upper] * weight


def bootstrap_ci(groups, rng):
    """Percentile 95 % CI of the geometric mean; `groups` are resampled units
    (each a list of per-case speedups)."""
    if len(groups) < 2:
        return math.nan, math.nan
    estimates = []
    for _ in range(BOOTSTRAP):
        pooled = []
        for _ in range(len(groups)):
            pooled.extend(groups[rng.randrange(len(groups))])
        estimates.append(geomean(pooled))
    estimates.sort()
    return quantile(estimates, 0.025), quantile(estimates, 0.975)


def fmt(value, digits=2):
    if value is None or (isinstance(value, float) and not math.isfinite(value)):
        return "—"
    return f"{value:,.{digits}f}"


class Case:
    def __init__(self, rows):
        first = rows[0]
        self.root = first["root"]
        self.role = first["role"]
        self.phase = first["phase"]
        self.case_id = first["case_id"]
        self.kind = first["source_kind"]
        self.protocol = first["protocol"]
        self.status = first["status"]
        self.n = int(first["n"])
        self.lanes = num(first["lanes_per_packet"])
        self.target = num(first["target_lanes_per_packet"])
        self.retain = num(first["retain_q"])
        self.retain_y = num(first["retain_y"])
        self.seed = first["thinning_seed"]
        self.pairs = num(first["logical_pairs"])
        self.cell = first["cell"]
        self.queue_legal = first["queue_legal"] == "1"
        self.median = {r["backend"]: num(r["median_ms"]) for r in rows if r["backend"]}
        self.p25 = {r["backend"]: num(r["p25_ms"]) for r in rows if r["backend"]}
        self.p75 = {r["backend"]: num(r["p75_ms"]) for r in rows if r["backend"]}
        self.ran = first["dispatch_ran_mode"]
        self.predicted = {
            "QUEUE_DENSE": num(first["dispatch_pred_queue_ms"]),
            "ARWLF": num(first["dispatch_pred_arwlf_ms"]),
            "K2_WIDE": num(first["dispatch_pred_k2wide_ms"]),
        }
        self.k2_hits32 = first["k2_hits32"]
        self.witnesses = first["witnesses"]
        self.exactness = first["exactness"]
        self.off_core = int(first["off_core_samples"] or 0)

    @property
    def timed(self):
        return self.status == "COMPLETED"

    def speedup(self, backend):
        return self.median["k2"] / self.median[backend] if backend in self.median else math.nan

    def best(self):
        legal = {b: self.median[b] for b in EXACT_BACKENDS if b in self.median}
        name = min(legal, key=legal.get)
        return name, legal[name]

    def regret(self):
        return self.median["dispatch"] / self.best()[1]

    def winner_correct(self):
        return self.ran.lower() == self.best()[0]

    def rival(self):
        """min(K2_WIDE, QUEUE_DENSE if legal): the crossover reference."""
        return min(self.median[b] for b in ("k2_wide", "queue_dense") if b in self.median)


def load(directory):
    by_case = defaultdict(list)
    for row in read(os.path.join(directory, "cases.csv")):
        by_case[(row["root"], row["case_id"])].append(row)
    return [Case(rows) for rows in by_case.values()]


def main(directory):
    rng = random.Random(BOOTSTRAP_SEED)
    cases = load(directory)
    timed = [c for c in cases if c.timed]
    out = []
    emit = out.append

    statuses = defaultdict(int)
    for c in cases:
        statuses[(c.kind, c.status)] += 1
    emit("# ARWLF regime campaign summary\n")
    emit(f"Cases: {len(cases)} ({', '.join(f'{k[0]} {k[1]}: {v}' for k, v in sorted(statuses.items()))}).")
    emit(f"Exactness: {sum(c.exactness == 'EXACT' for c in timed)}/{len(timed)} timed cases EXACT; "
         f"off-core timed samples: {sum(c.off_core for c in cases)}.\n")

    # --- census -------------------------------------------------------------------
    emit("## Census (all 4096 microproblems per root, no backend runs)\n")
    emit("| root | role | n | Q | microproblems with work | log2 pairs p5 / median / p95 | L min / p5 / median / p95 / max | anchor cell | anchor L (rank) | QUEUE legal |")
    emit("|---|---|---|---|---|---|---|---|---|---|")
    census_rows = {}
    for path in sorted(glob.glob(os.path.join(directory, "*_census.csv"))):
        rows = read(path)
        root = rows[0]["root"]
        census_rows[root] = rows
        work = [r for r in rows if int(r["packets"]) > 0]
        lanes = sorted(num(r["lanes_per_packet"]) for r in work)
        pairs = sorted(num(r["log2_pairs"]) for r in work)
        anchor = next(r for r in rows if r["anchor"] == "1")
        anchor_l = num(anchor["lanes_per_packet"])
        rank = sum(v <= anchor_l for v in lanes) / len(lanes)
        emit(f"| {root} | {rows[0]['role']} | {rows[0]['n']} | {rows[0]['Q']} | {len(work)} | "
             f"{fmt(quantile(pairs, .05))} / {fmt(quantile(pairs, .5))} / {fmt(quantile(pairs, .95))} | "
             f"{fmt(lanes[0])} / {fmt(quantile(lanes, .05))} / {fmt(quantile(lanes, .5))} / {fmt(quantile(lanes, .95))} / {fmt(lanes[-1])} | "
             f"{anchor['cell']} | {fmt(anchor_l)} ({rank:.0%}) | {'yes' if anchor['queue_legal_root'] == '1' else 'no (capacity)'} |")
    emit("")

    # --- authentic cases --------------------------------------------------------------
    def backend_table(selection, title):
        emit(f"## {title}\n")
        emit("| root | case | protocol | L | pairs | K2 ms | K2_WIDE | QUEUE_DENSE | ARWLF | DISPATCH (ran) | ARWLF vs K2 | DISPATCH vs K2 | regret |")
        emit("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
        for c in sorted(selection, key=lambda c: (c.n, c.root, c.case_id)):
            m = c.median
            emit(f"| {c.root} | {c.case_id} | {c.protocol} | {fmt(c.lanes)} | {c.pairs:.3g} | {fmt(m['k2'], 1)} | "
                 f"{fmt(m['k2_wide'], 1)} | {fmt(m.get('queue_dense'), 1) if 'queue_dense' in m else 'N/A'} | "
                 f"{fmt(m['arwlf'], 1)} | {fmt(m['dispatch'], 1)} ({c.ran}) | {fmt(c.speedup('arwlf'))}× | "
                 f"{fmt(c.speedup('dispatch'))}× | {fmt(c.regret(), 3)} |")
        emit("")

    authentic = [c for c in timed if c.kind == "AUTHENTIC_FULLTER"]
    backend_table([c for c in authentic if c.phase in ("anchor", "whole", "confirm") and c.protocol == "final"],
                  "Final-protocol authentic cases (3 warmups + 7 matched)")
    backend_table([c for c in authentic if c.phase.startswith("stratified")],
                  "Stratified authentic microproblems (discovery: 1 + 3)")

    emit("## Generalization: geometric-mean speedup over authentic n=108 microproblems\n")
    emit("Per-case speedups are median(K2) / median(backend). CIs: percentile bootstrap, "
         f"{BOOTSTRAP} resamples, seed {hex(BOOTSTRAP_SEED)}.\n")
    emit("| set | cases | roots | backend | geomean speedup vs K2 | 95 % CI (cases) | 95 % CI (roots) | min | max |")
    emit("|---|---|---|---|---|---|---|---|---|")
    for role in ("calibration", "holdout"):
        group = [c for c in authentic if c.n == 108 and c.role == role and c.cell != "4096"]
        if not group:
            continue
        roots = sorted({c.root for c in group})
        for backend in ("arwlf", "dispatch", "k2_wide"):
            values = [c.speedup(backend) for c in group]
            by_root = [[c.speedup(backend) for c in group if c.root == r] for r in roots]
            lo, hi = bootstrap_ci([[v] for v in values], rng)
            rlo, rhi = bootstrap_ci(by_root, rng)
            emit(f"| {role} | {len(group)} | {len(roots)} | {backend.upper()} | {fmt(geomean(values))}× | "
                 f"{fmt(lo)}–{fmt(hi)} | {fmt(rlo)}–{fmt(rhi)} | {fmt(min(values))}× | {fmt(max(values))}× |")
    emit("")

    # --- crossover --------------------------------------------------------------------
    thinned = [c for c in timed if c.kind == "AUTHENTIC_SOURCE_THINNED"]
    unreachable = [c for c in cases if c.kind == "AUTHENTIC_SOURCE_THINNED" and c.status == "UNREACHABLE"]
    confirm = defaultdict(list)
    if thinned:
        emit("## Source-thinning sweep (AUTHENTIC_SOURCE_THINNED; discovery unless marked final)\n")
        emit("Ratio = median(ARWLF) / min(median(K2_WIDE), median(QUEUE_DENSE) if legal); geometric mean over the "
             "thinning seeds. < 1: ARWLF wins.\n")
        emit("| root | target L | observed L (seeds) | p used | pairs | ARWLF/rival | ARWLF vs K2 | K2_WIDE vs K2 | DISPATCH vs K2 | regret | dispatch ran |")
        emit("|---|---|---|---|---|---|---|---|---|---|---|")
        for root in sorted({c.root for c in thinned}):
            rows = [c for c in thinned if c.root == root and c.protocol == "discovery"]
            ratios = []
            for target in sorted({c.target for c in rows}):
                group = [c for c in rows if c.target == target]
                ratio = geomean([c.median["arwlf"] / c.rival() for c in group])
                ratios.append((target, ratio))
                emit(f"| {root} | {fmt(target, 0)} | {', '.join(fmt(c.lanes) for c in group)} | "
                     f"{fmt(statistics.mean(c.retain for c in group), 4)} | {statistics.mean(c.pairs for c in group):.3g} | "
                     f"{fmt(ratio, 3)} | {fmt(geomean([c.speedup('arwlf') for c in group]))}× | "
                     f"{fmt(geomean([c.speedup('k2_wide') for c in group]))}× | "
                     f"{fmt(geomean([c.speedup('dispatch') for c in group]))}× | "
                     f"{fmt(geomean([c.regret() for c in group]), 3)} | {'/'.join(sorted({c.ran for c in group}))} |")
            # Preregistered confirm rule.
            bracket = None
            for (t0, r0), (t1, r1) in zip(ratios, ratios[1:]):
                if (r0 - 1.0) * (r1 - 1.0) <= 0.0:
                    bracket = (t0, t1)
                    break
            confirm[root] = list(bracket) if bracket else [ratios[0][0]] if ratios else []
            emit(f"| {root} | crossover | {'between L≈' + fmt(bracket[0], 0) + ' and ' + fmt(bracket[1], 0) if bracket else 'none in the reachable range'} | | | | | | | | |")
        emit("")
        if unreachable:
            emit(f"Unreachable targets (above the authentic L): "
                 f"{', '.join(sorted({c.root + ' L=' + fmt(c.target, 0) for c in unreachable}))}.\n")
        final_thin = [c for c in thinned if c.protocol == "final"]
        if final_thin:
            backend_table(final_thin, "Confirmed crossover points (final protocol)")

    target_thin = [c for c in timed if c.kind == "AUTHENTIC_TARGET_THINNED"]
    if target_thin:
        emit("## Target-thinning ablation (AUTHENTIC_TARGET_THINNED, Q and X untouched)\n")
        emit("| root | p_Y | L | K2 ms | K2_WIDE | ARWLF | DISPATCH | ARWLF vs K2 | ARWLF vs K2_WIDE | k32 survivors |")
        emit("|---|---|---|---|---|---|---|---|---|---|")
        for c in sorted(target_thin, key=lambda c: (c.root, -c.retain_y)):
            m = c.median
            emit(f"| {c.root} | {fmt(c.retain_y, 3)} | {fmt(c.lanes)} | {fmt(m['k2'], 1)} | {fmt(m['k2_wide'], 1)} | "
                 f"{fmt(m['arwlf'], 1)} | {fmt(m['dispatch'], 1)} | {fmt(c.speedup('arwlf'))}× | "
                 f"{fmt(m['k2_wide'] / m['arwlf'])}× | {c.k2_hits32} |")
        emit("")

    # --- dispatcher ---------------------------------------------------------------------
    emit("## Dispatcher (frozen v3) regret = median(DISPATCH) / fastest legal exact backend\n")
    emit("| set | cases | median | p95 | max | ran the fastest |")
    emit("|---|---|---|---|---|---|")
    sets = [("all timed", timed)] + [(k, [c for c in timed if c.kind == k]) for k in
                                     ("AUTHENTIC_FULLTER", "AUTHENTIC_SOURCE_THINNED", "AUTHENTIC_TARGET_THINNED")]
    for label, group in sets:
        if not group:
            continue
        regrets = sorted(c.regret() for c in group)
        emit(f"| {label} | {len(group)} | {fmt(quantile(regrets, .5), 3)} | {fmt(quantile(regrets, .95), 3)} | "
             f"{fmt(regrets[-1], 3)} | {sum(c.winner_correct() for c in group)}/{len(group)} |")
    emit("")

    # --- sentinel ----------------------------------------------------------------------
    sentinel_path = os.path.join(directory, "sentinel.csv")
    if os.path.exists(sentinel_path):
        values = sorted(num(r["sentinel_ms"]) for r in read(sentinel_path))
        emit(f"Machine sentinel (fixed CPU workload, {len(values)} runs): min {fmt(values[0], 2)} ms, median "
             f"{fmt(quantile(values, .5), 2)} ms, max {fmt(values[-1], 2)} ms (spread {fmt(100 * (values[-1] / values[0] - 1), 1)} %).\n")

    with open(os.path.join(directory, "summary.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")

    with open(os.path.join(directory, "speedup_vs_L.csv"), "w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(["root", "role", "source_kind", "case_id", "protocol", "lanes_per_packet", "logical_pairs",
                         "backend", "median_ms", "speedup_vs_k2"])
        for c in timed:
            for backend, median in c.median.items():
                writer.writerow([c.root, c.role, c.kind, c.case_id, c.protocol, c.lanes, int(c.pairs), backend,
                                 median, c.speedup(backend)])
    with open(os.path.join(directory, "regret_vs_L.csv"), "w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(["root", "role", "source_kind", "case_id", "lanes_per_packet", "regret", "dispatch_ran",
                         "fastest", "predicted_ms_queue", "predicted_ms_arwlf", "predicted_ms_k2wide"])
        for c in timed:
            writer.writerow([c.root, c.role, c.kind, c.case_id, c.lanes, c.regret(), c.ran, c.best()[0],
                             c.predicted["QUEUE_DENSE"], c.predicted["ARWLF"], c.predicted["K2_WIDE"]])

    for root, targets in sorted(confirm.items()):
        specs = ",".join(f"qx={t:g}@0xA11CE001" for t in targets)
        print(f"confirm {root}: {specs}")
    print("\n".join(out))


if __name__ == "__main__":
    main(sys.argv[1])
