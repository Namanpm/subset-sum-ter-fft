"""Summarize an ARWLF whole-root run directory (ARWLF_WHOLE_ROOT.md).

    python -X utf8 ter2_root/scripts/summarize_arwlf_whole_root.py results/arwlf/whole-root-20261003

Reads runs.jsonl, writes summary.md next to it and prints it.  Exit 1 on an
exactness failure (a failed chunk, or complete runs that disagree on the final
outputs or miss the planted one), 0 otherwise.  Incomplete runs are listed as
such and never enter a ratio.
"""

import csv
import json
import statistics
import sys
from collections import defaultdict
from datetime import datetime, timedelta, timezone
from pathlib import Path

CHUNKS = {"k2": [f"u{u:02d}" for u in range(64)], "k2_wide": [f"u{u:02d}" for u in range(64)],
          "arwlf": ["all"], "dispatch": ["all"]}
ORDER = ["k2", "k2_wide", "arwlf", "dispatch"]
# Interference audit (bench_guard.csv, written by ter2_root/scripts/bench_guard.ps1).  A chunk is flagged
# when a heavy process (compiler, build or test tool, another ter_* program) was seen during it, or when
# the mean CPU of all other processes over it exceeded FOREIGN_CORES.  Flags are reported, never used to
# drop or replace a measurement.
FOREIGN_CORES = 1.0


def utc(text):
    return datetime.strptime(text, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc)


def load_guard(path):
    if not path.exists():
        return []
    return [r for r in csv.DictReader(path.open(encoding="utf-8-sig"))]


def audit(chunk, guard):
    """Guard rows covering one chunk: (coverage 0..1, mean foreign cores, max foreign cores, heavy names)."""
    end = utc(chunk["utc"])
    start = end - timedelta(milliseconds=chunk["ms"])
    inside = []
    for g in guard:
        t = utc(g["utc"])
        interval = float(g["interval_s"] or 0)
        # A guard row describes (t - interval, t]; keep rows whose interval overlaps the chunk.
        if t > start and t - timedelta(seconds=interval) < end + timedelta(seconds=1):
            inside.append(g)
    if not inside:
        return 0.0, None, None, set()
    window = max((end - start).total_seconds(), 1.0)
    covered = min(1.0, sum(float(g["interval_s"] or 0) for g in inside) / window)
    foreign = [float(g["foreign_cpu_cores"] or 0) for g in inside]
    heavy = {name for g in inside for name in g["heavy"].split() if name}
    return covered, sum(foreign) / len(foreign), max(foreign), heavy


def main(out_dir: Path) -> int:
    rows = [json.loads(line) for line in (out_dir / "runs.jsonl").read_text(encoding="utf-8").splitlines() if line]
    runs = defaultdict(list)
    for row in rows:
        runs[row["run_id"]].append(row)

    lines = []
    emit = lines.append
    failed = False
    first = rows[0]
    emit("# ARWLF whole-root run\n")
    emit(f"Root {first['root']} (n={first['n']}, Q={first['Q']}, seed {first['root_seed']}), all 4096 microproblems, "
         f"one planted final output {first['planted'][0]} on the anchor microproblem {first['anchor_cell']} "
         f"(plant seed {first['plant_seed']}). Build {first['build']} {first['compiler']}, commit "
         f"{first['git_commit'][:12]} (dirty at configure: {first['git_dirty_at_configure']}), pin {first['pin']} "
         f"(cpu {first['pinned_cpu']}).\n")

    complete = {}
    emit("## Runs\n")
    emit("| run | backend | chunks | microproblems | whole-root ms | ns per pair | final outputs | planted found | "
         "chunk ms min / median / max | off-core chunks | AC throughout | peak WS MiB | DISPATCH ran |")
    emit("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for run_id in sorted(runs, key=lambda r: (ORDER.index(runs[r][0]["backend"]), r)):
        group = runs[run_id]
        backend = group[0]["backend"]
        bad = [r for r in group if r["status"] == "EXACTNESS_FAILURE"]
        limited = [r for r in group if r["status"] == "RESOURCE_LIMIT"]
        done = {r["chunk"]: r for r in group if r["status"] == "COMPLETED"}
        if bad:
            failed = True
            emit(f"| {run_id} | {backend} | EXACTNESS FAILURE in {', '.join(r['chunk'] for r in bad)} |"
                 + " |" * 10)
            continue
        if limited:
            emit(f"| {run_id} | {backend} | RESOURCE_LIMIT ({limited[0].get('working_set_mib')} MiB after setup) |"
                 + " |" * 10)
            continue
        missing = [c for c in CHUNKS[backend] if c not in done]
        chunks = [done[c] for c in CHUNKS[backend] if c in done]
        cells = sum(r["cells"] for r in chunks)
        ms = sum(r["ms"] for r in chunks)
        pairs = sum(r["logical_pairs"] for r in chunks)
        witnesses = sorted({tuple(w) for r in chunks for w in r["witnesses"]})
        planted = tuple(first["planted"][0])
        chunk_ms = [r["ms"] for r in chunks]
        off_core = sum(1 for r in chunks if not r["on_pinned_core"])
        on_ac = all(r["ac_start"] == 1 and r["ac_end"] == 1 for r in chunks)
        peak = max(r["peak_working_set_mib"] for r in chunks) if chunks else 0.0
        ran = ", ".join(sorted({r["dispatch_ran"] for r in chunks if r["dispatch_ran"]}))
        state = "" if not missing else f" (INCOMPLETE: {len(missing)} chunks to run)"
        emit(f"| {run_id}{state} | {backend} | {len(chunks)} | {cells:,} | {ms:,.1f} | "
             f"{ms * 1e6 / pairs if pairs else 0:.3f} | {len(witnesses)} | {'yes' if planted in witnesses else 'NO'} | "
             f"{min(chunk_ms):,.1f} / {statistics.median(chunk_ms):,.1f} / {max(chunk_ms):,.1f} | {off_core} | "
             f"{'yes' if on_ac else 'NO'} | {peak:,.1f} | {ran} |")
        if not missing and cells == 4096:
            complete[run_id] = {"backend": backend, "ms": ms, "witnesses": witnesses,
                                "structural": sum(r["structural"] for r in chunks)}
    emit("")

    emit("## Exactness\n")
    planted = tuple(first["planted"][0])
    if complete:
        reference = next(iter(complete.values()))
        agree = all(c["witnesses"] == reference["witnesses"] and c["structural"] == reference["structural"]
                    for c in complete.values())
        has_planted = all(planted in c["witnesses"] for c in complete.values())
        failed = failed or not agree or not has_planted
        authentic = [w for w in reference["witnesses"] if w != planted]
        emit(f"- Complete runs: {len(complete)}; identical final outputs and structural counts: "
             f"**{'yes' if agree else 'NO'}**; every run reports the planted output: "
             f"**{'yes' if has_planted else 'NO'}**.")
        emit(f"- Final outputs of the whole root: {len(reference['witnesses'])} = the planted one + "
             f"{len(authentic)} authentic {authentic if authentic else ''}.\n")
    else:
        emit("- No complete run yet.\n")

    emit("## Whole-root time and speedup vs frozen K2\n")
    k2 = [c for c in complete.values() if c["backend"] == "k2"]
    if k2:
        base = k2[0]["ms"]
        emit("| run | backend | whole-root time | speedup vs K2 |")
        emit("|---|---|---|---|")
        for run_id, c in sorted(complete.items(), key=lambda kv: (ORDER.index(kv[1]["backend"]), kv[0])):
            emit(f"| {run_id} | {c['backend']} | {c['ms'] / 1000.0:,.1f} s ({c['ms'] / 3.6e6:.2f} h) | "
                 f"{base / c['ms']:.2f}x |")
        emit("")
    else:
        emit("- Frozen K2 has not completed the whole root yet; no ratio is reported.\n")

    emit("## Setup (untimed, once per process, ms)\n")
    emit("| run | load snapshot | plant | family keys | index + tables | census |")
    emit("|---|---|---|---|---|---|")
    for run_id in sorted(runs):
        r = runs[run_id][-1]
        emit(f"| {run_id} | {r['setup_load_ms']:,.1f} | {r['setup_plant_ms']:,.1f} | {r['setup_keys_ms']:,.1f} | "
             f"{r['setup_index_ms']:,.1f} | {r['setup_census_ms']:,.1f} |")
    emit("\nThe timed region of every run is the regime campaign's (sink reset + the frozen backend call), so "
         "ARWLF's leaf-cache construction is inside its time. Family keys are used by K2_WIDE, ARWLF and DISPATCH, "
         "not by K2; the index by all four. A resumed run reports the setup of its last process.\n")

    emit("## Interference audit (bench_guard.csv)\n")
    guard = load_guard(out_dir.parent / "bench_guard.csv")
    if not guard:
        emit("- No guard log: interference was not recorded for any run.\n")
    else:
        emit(f"Flag: a heavy process seen, or mean foreign CPU > {FOREIGN_CORES} core over the chunk. "
             "Flags are reported, never used to drop a measurement.\n")
        emit("| run | chunks | guarded (time-weighted) | mean foreign cores (max chunk) | max foreign cores (one interval) | "
             "flagged chunks | heavy seen |")
        emit("|---|---|---|---|---|---|---|")
        for run_id in sorted(runs, key=lambda r: (ORDER.index(runs[r][0]["backend"]), r)):
            chunks = [r for r in runs[run_id] if r["status"] == "COMPLETED"]
            if not chunks:
                continue
            results = [(c, audit(c, guard)) for c in chunks]
            total_ms = sum(c["ms"] for c in chunks)
            guarded = sum(c["ms"] * a[0] for c, a in results) / total_ms if total_ms else 0.0
            means = [a[1] for _, a in results if a[1] is not None]
            maxes = [a[2] for _, a in results if a[2] is not None]
            flagged = [c["chunk"] for c, a in results if a[3] or (a[1] is not None and a[1] > FOREIGN_CORES)]
            heavy = sorted({h for _, a in results for h in a[3]})
            emit(f"| {run_id} | {len(chunks)} | {guarded:.0%} | {max(means) if means else float('nan'):.2f} | "
                 f"{max(maxes) if maxes else float('nan'):.2f} | {', '.join(flagged) if flagged else 'none'} | "
                 f"{' '.join(heavy) if heavy else 'none'} |")
        emit("")

    text = "\n".join(lines)
    (out_dir / "summary.md").write_text(text + "\n", encoding="utf-8")
    print(text)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(Path(sys.argv[1])))
