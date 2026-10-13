"""Build manuscript tables and scientific figures from frozen verified inputs.

Run with the bundled Python runtime. No solver, build, test, or benchmark is
invoked. All output is confined to this script's paper/ directory.

    python paper/build_results.py --freeze-inputs
    python paper/build_results.py

--freeze-inputs copies the completed, immutable regime/positive raw CSVs and
the reviewed derivation JSON into paper/data. Whole-root checkpoints are taken
from the review's explicit cutoff, not from the still-growing live JSONL.
Subsequent normal runs use only those frozen paper/data files.
"""

from __future__ import annotations

import argparse
import collections
import csv
import datetime as dt
import hashlib
import json
import math
from pathlib import Path
import random
import shutil
import statistics

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D
from matplotlib.patches import FancyArrowPatch, FancyBboxPatch
from matplotlib.ticker import FuncFormatter


PAPER = Path(__file__).resolve().parent
REPO = PAPER.parent
DATA = PAPER / "data"
TABLES = PAPER / "tables"
FIGURES = PAPER / "figures"
BOOTSTRAP_SEED = 0xB007
BOOTSTRAP_DRAWS = 10_000
DPI = 360
ROOT_COLORS = {"0x1001": "#0072B2", "0x2003": "#D55E00", "0x3007": "#009E73"}
ROLE_COLORS = {"calibration": "#006C75", "holdout": "#AD5900"}
BACKEND_COLORS = {"k2": "#0072B2", "k2_wide": "#D55E00", "arwlf": "#009E73"}


def write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False, allow_nan=False) + "\n", encoding="utf-8")


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def freeze_inputs() -> None:
    DATA.mkdir(parents=True, exist_ok=True)
    reviewed = PAPER / "working" / "results_review.json"
    result = json.loads(reviewed.read_text(encoding="utf-8"))
    expected = {str(Path(k)): v for k, v in result["methods"]["hashes"].items()}
    source_map = {
        "results/arwlf/regime-20261001/cases.csv": "regime_cases.csv",
        "results/arwlf/regime-20261001/samples.csv": "regime_samples.csv",
        "results/arwlf/positive-20261003/cases.csv": "positive_cases.csv",
        "results/arwlf/positive-20261003/samples.csv": "positive_samples.csv",
    }
    inputs = []
    for source, name in source_map.items():
        path = REPO / source
        digest = sha256(path)
        if digest != expected[str(Path(source))]:
            raise ValueError(f"Reviewed raw input changed: {source}")
        dest = DATA / name
        shutil.copyfile(path, dest)
        inputs.append({"file": name, "origin": source, "sha256": digest})
    shutil.copyfile(reviewed, DATA / "results_review.json")
    inputs.append({"file": "results_review.json", "origin": "paper/working/results_review.json", "sha256": sha256(reviewed)})
    records = [c for name in ("arwlf-1", "dispatch-1", "k2-1") for c in result["whole_root"][name]["records"]]
    whole = DATA / "whole_root_cutoff.jsonl"
    whole.write_text("".join(json.dumps(c, sort_keys=True, separators=(",", ":")) + "\n" for c in records), encoding="utf-8")
    inputs.append({"file": whole.name, "origin": "reviewed whole-root records only", "sha256": sha256(whole)})
    write_json(DATA / "snapshot_manifest.json", result["snapshot_manifest"])
    inputs.append({"file": "snapshot_manifest.json", "origin": "reviewed snapshot identity metadata", "sha256": sha256(DATA / "snapshot_manifest.json")})
    write_json(DATA / "input_manifest.json", {
        "cutoff_utc": result["whole_root"]["k2-1"]["last_utc"],
        "inputs": inputs,
        "semantics": "Historical prepared-root cutoff input set. Current DOCX also requires separately frozen complete-root, n72 solve and scaling inputs; this mode does not refresh those additions.",
    })


def quantile(values: list[float], fraction: float) -> float:
    a = sorted(values)
    position = (len(a) - 1) * fraction
    lower = int(position)
    upper = min(lower + 1, len(a) - 1)
    return a[lower] + (a[upper] - a[lower]) * (position - lower)


def geomean(values: list[float]) -> float:
    return math.exp(statistics.mean(math.log(x) for x in values))


def bootstrap_roots(cases: list[dict], ratio: str, balanced: bool) -> dict:
    roots = sorted({c["root"] for c in cases})
    logs = {root: [math.log(c[ratio]) for c in cases if c["root"] == root] for root in roots}
    rng = random.Random(BOOTSTRAP_SEED)
    estimates = []
    for _ in range(BOOTSTRAP_DRAWS):
        draw = [rng.choice(roots) for _ in roots]
        sample = [statistics.mean(logs[root]) for root in draw] if balanced else [v for root in draw for v in logs[root]]
        estimates.append(math.exp(statistics.mean(sample)))
    point = math.exp(statistics.mean([statistics.mean(logs[root]) for root in roots])) if balanced else geomean([c[ratio] for c in cases])
    return {"estimate": point, "ci95": [quantile(estimates, .025), quantile(estimates, .975)], "roots": len(roots), "cases": len(cases)}


def load_whole_complete() -> dict:
    """Complete whole-root runs: K2 must cover all 64 target families exactly once."""
    runs = [json.loads(l) for l in (DATA / "whole_root_complete.jsonl").read_text(encoding="utf-8-sig").splitlines() if l.strip()]
    k2 = [x for x in runs if x["run_id"] == "k2-1"]
    if sorted(x["chunk"] for x in k2) != [f"u{u:02d}" for u in range(64)] or any(x["status"] != "COMPLETED" for x in runs):
        raise ValueError("K2 whole-root run is not complete")
    single = {x["run_id"]: x for x in runs if x["run_id"] in ("arwlf-1", "dispatch-1", "arwlf-2", "dispatch-2")}
    pairs = single["arwlf-1"]["logical_pairs"]
    if sum(x["logical_pairs"] for x in k2) != pairs:
        raise ValueError("K2 chunks do not cover the root's logical pairs")
    outputs = {json.dumps(sorted(x["witnesses"])) for x in single.values()}
    outputs.add(json.dumps(sorted(w for x in k2 for w in x["witnesses"])))
    if len(outputs) != 1:
        raise ValueError("Whole-root runs disagree on their outputs")
    chunk_ms = [x["ms"] for x in k2]
    return {"pairs": pairs, "k2_ms": sum(chunk_ms), "k2_peak": max(x["peak_working_set_mib"] for x in k2),
            "k2_chunk_min": min(chunk_ms), "k2_chunk_max": max(chunk_ms), "k2_chunk_median": statistics.median(chunk_ms),
            "k2_chunk_lower_half_median": statistics.median(sorted(chunk_ms)[:32]),
            "runs": single, "probes": [single["dispatch-1"]["dispatch_probe_ms"], single["dispatch-2"]["dispatch_probe_ms"]]}


def verify_frozen_data(result: dict) -> dict:
    manifest = json.loads((DATA / "input_manifest.json").read_text(encoding="utf-8"))
    for record in manifest["inputs"]:
        if sha256(DATA / record["file"]) != record["sha256"]:
            raise ValueError(f"Frozen paper input changed: {record['file']}")
    report = {}
    for stem, case_key in (("regime", "regime_cases"), ("positive", "positive")):
        with (DATA / f"{stem}_cases.csv").open(newline="", encoding="utf-8") as f:
            summaries = list(csv.DictReader(f))
        with (DATA / f"{stem}_samples.csv").open(newline="", encoding="utf-8") as f:
            raw = list(csv.DictReader(f))
        completed = {(s["attempt"], s["root"], s["case_id"], s["backend"]): s for s in summaries if s["status"] == "COMPLETED"}
        samples = collections.defaultdict(list)
        orphans = []
        for s in raw:
            key = tuple(s[x] for x in ("attempt", "root", "case_id", "backend"))
            if key not in completed:
                orphans.append(s)
            else:
                samples[key].append(float(s["ms"]))
        reviewed = {(c["attempt"], c["root"], c["case_id"]): c for c in result[case_key]}
        raw_medians = collections.defaultdict(dict)
        for key, summary in completed.items():
            values = samples[key]
            if len(values) != int(summary["timed_samples"]):
                raise ValueError(f"Sample count mismatch: {key}")
            median = statistics.median(values)
            retained = reviewed[key[:3]]["medians_ms"][key[3]]
            if not math.isclose(median, retained, rel_tol=1e-12, abs_tol=1e-9):
                raise ValueError(f"Reviewed median mismatch: {key}")
            raw_medians[key[:3]][key[3]] = median
            case = reviewed[key[:3]]
            for field in summary:
                # These fields are sampled again while each backend's summary
                # is written and can differ slightly within one logical case.
                if field in ("working_set_mib", "peak_working_set_mib"):
                    continue
                if field in case and str(case[field]) != summary[field]:
                    raise ValueError(f"Reviewed case metadata mismatch: {key}, {field}")
            if any(summary[field] != "1" for field in summary if field.startswith("gate_")):
                raise ValueError(f"Recorded exactness gate failure: {key}")
        for key, medians in raw_medians.items():
            case = reviewed[key]
            # Derive the ratios used in the following bootstrap directly from
            # the frozen raw timing medians, rather than from reviewed ratios.
            for ratio, numerator, denominator in (
                ("k2_over_arwlf", "k2", "arwlf"),
                ("k2wide_over_arwlf", "k2_wide", "arwlf"),
                ("k2_over_dispatch", "k2", "dispatch"),
            ):
                value = medians[numerator] / medians[denominator]
                if not math.isclose(value, case[ratio], rel_tol=1e-12):
                    raise ValueError(f"Reviewed ratio mismatch: {key}, {ratio}")
                case[ratio] = value
            case["medians_ms"] = medians
            case["raw_stats"] = {
                backend: {"median": value, "p25": quantile(samples[key + (backend,)], .25),
                          "p75": quantile(samples[key + (backend,)], .75),
                          "min": min(samples[key + (backend,)]), "max": max(samples[key + (backend,)]),
                          "n": len(samples[key + (backend,)])}
                for backend, value in medians.items()
            }
            rival = min(value for backend, value in medians.items() if backend not in ("k2", "arwlf", "dispatch"))
            exact = {backend: value for backend, value in medians.items() if backend != "dispatch"}
            case["arwlf_over_best_alternative"] = medians["arwlf"] / rival
            case["best"] = min(exact, key=exact.get)
            case["regret"] = medians["dispatch"] / min(exact.values())
        representative = {key[:3]: summary for key, summary in completed.items()}
        report[stem] = {
            "completed_cases": len(reviewed), "backend_summaries": len(completed),
            "accepted_timings": sum(map(len, samples.values())), "orphan_timings": len(orphans),
            "all_recorded_exactness_gates_pass": True,
            "case_output_counts": {field: dict(collections.Counter(summary[field] for summary in representative.values()))
                                   for field in ("full_key_triples", "residue_exact", "structural", "witnesses")},
        }
    report["aggregates"] = {}
    for role in ("calibration", "holdout"):
        cases = [c for c in result["regime_cases"] if c["n"] == "108" and c["role"] == role and c["source_kind"] == "AUTHENTIC_FULLTER"]
        report["aggregates"][role] = {}
        for ratio in ("k2_over_arwlf", "k2wide_over_arwlf", "k2_over_dispatch"):
            for balanced, kind in ((False, "pooled"), (True, "balanced")):
                stat = bootstrap_roots(cases, ratio, balanced)
                old = result["subsets"]["n108_" + role][kind][ratio]
                if not math.isclose(stat["estimate"], old["estimate"], rel_tol=1e-12):
                    raise ValueError("Aggregate point estimate mismatch")
                if any(not math.isclose(a, b, rel_tol=1e-12) for a, b in zip(stat["ci95"], old["ci95"])):
                    raise ValueError("Root-block interval mismatch")
                report["aggregates"][role][kind + ":" + ratio] = stat
        # Table/figure case views point at the verified raw-derived case objects.
        result["subsets"]["n108_final_" + role]["cases"] = [c for c in cases if c["protocol"] == "final"]
    result["n72_whole"] = [c for c in result["regime_cases"] if c["phase"] == "whole"]
    result["thin_y"] = [c for c in result["regime_cases"] if c["phase"] == "thin-y"]
    result["confirm"] = [c for c in result["regime_cases"] if c["phase"] == "confirm"]
    for aggregate in result["thin_qx"]:
        cases = [c for c in result["regime_cases"] if c["phase"] == "thin-qx" and c["root"] == aggregate["root"]
                 and float(c["target_lanes_per_packet"]) == aggregate["target_L"]]
        aggregate["gmean_arwlf_over_best_alt"] = geomean([c["arwlf_over_best_alternative"] for c in cases])
        aggregate["gmean_k2_over_arwlf"] = geomean([c["k2_over_arwlf"] for c in cases])
    write_json(DATA / "recompute_report.json", report)
    return report


def root_seed(c: dict) -> str:
    return c["root"].split("_s")[-1].split("_a")[0]


def make_tables(r: dict) -> dict:
    tables = {}
    def put(marker: str, caption: str, headers: list[str], rows: list[list[object]], notes: str) -> None:
        tables[marker] = {"caption": caption, "headers": headers, "rows": [[str(value) for value in row] for row in rows], "notes": notes}
    anchors = r["subsets"]["n108_final_calibration"]["cases"] + r["subsets"]["n108_final_holdout"]["cases"]
    # Separate hash-join run on the 0x1001 anchor cell; it must describe the
    # same cell as the final anchor and report exact output on every round.
    hj_anchor = next(c for c in anchors if root_seed(c) == "0x1001")
    hj_rows = r["hash_join"]
    hj = collections.defaultdict(list)
    for row in hj_rows:
        if row["phase"] != "sample" or row["source_kind"] != "AUTHENTIC_FULLTER" or row["n"] != "108" or \
           row["microproblems"] != "1" or row["logical_pairs"] != hj_anchor["logical_pairs"] or \
           row["host_valid"] != "1" or row["exact"] != "1" or \
           (row["backend"] == "k2" and (row["raw_triples"], row["full_q32"]) != (hj_anchor["k2_hits25"], hj_anchor["k2_hits32"])):
            raise ValueError("Hash-join record does not match the 0x1001 anchor cell or is not exact")
        hj[row["backend"]].append(float(row["ms"]))
    if sorted(hj) != ["arwlf", "hash_join", "k2", "k2_wide"] or any(len(v) != 3 for v in hj.values()):
        raise ValueError("Hash-join record must hold three timed rounds for each of four backends")
    hj = {b: statistics.median(v) for b, v in hj.items()}
    hj_pairs = int(hj_anchor["logical_pairs"])
    put("anchors", "Final-protocol medians for six authentic dimension-108 anchor cells (seconds).",
        ["Seed", "Role", "L", "K2", "K2-WIDE", "ARWLF", "Dispatch"],
        [[root_seed(c), c["role"], f"{float(c['lanes_per_packet']):.2f}", *[f"{c['medians_ms'][b]/1000:.4f}" for b in ("k2", "k2_wide", "arwlf", "dispatch")]] for c in anchors],
        "Three warmup rounds and seven timed rounds per cell; each value is a median. All rows cover one selected cell, not a whole root. QUEUE-DENSE is capacity-inapplicable. The 0x2003 K2 median has IQR 91.0567–145.7907 s; that row is retained. "
        f"A separate later run on the 0x1001 cell (one warmup and three timed rounds per backend, data/hash_join_n108_anchor.csv) gives medians K2 {hj['k2']/1000:.4f}, K2-WIDE {hj['k2_wide']/1000:.4f} and ARWLF {hj['arwlf']/1000:.4f} s, and {hj['hash_join']/1000:.4f} s ({hj['hash_join']*1e6/hj_pairs:.2f} ns per logical pair) for an untuned scalar exact hash join (linear-probing table keyed on the W-bit residual, then a high-bit check; table build included). In every round each backend matches an untimed K2-WIDE reference; the cell has no full-quotient matches, so this checks only for false matches.")
    rows = []
    for final in (False, True):
        for role in ("calibration", "holdout"):
            group = r["subsets"][("n108_final_" if final else "n108_") + role]
            stats = group["stats"] if final else group["pooled"]
            a = stats["k2_over_arwlf"]
            rows.append([("Final anchors, " if final else "Pooled cells, ") + role, f"{a['independent_roots']} / {a['cases']}", f"{a['estimate']:.4f}", f"{stats['k2wide_over_arwlf']['estimate']:.4f}", f"{stats['k2_over_dispatch']['estimate']:.4f}", f"[{a['ci95'][0]:.2f}, {a['ci95'][1]:.2f}]"])
    put("aggregates", "Geometric-mean speedups on authentic dimension-108 cells, with the final-protocol subset kept separate.",
        ["Subset", "Roots / cells", "K2 / ARWLF", "Wide / ARWLF", "K2 / dispatch", "95% CI: K2 / ARWLF"], rows,
        "Pooled estimates weight cells equally and combine declared discovery/final protocols. Intervals resample entire roots as blocks (10,000 percentile draws, seed 0xB007); only three independent roots occur per role. Equal-root calibration sensitivity gives 52.7449, 32.7335, and 52.0020 for the three ratios; holdout equal-root estimates coincide with pooled estimates. Final anchors have one cell per root.")
    put("sparse", "Complete authentic dimension-72 prepared-root medians (milliseconds), covering all 4,096 cells.",
        ["Root", "K2", "K2-WIDE", "QUEUE-DENSE", "ARWLF", "Dispatch", "Regret"],
        [["anchor (cal.)" if c["role"] == "calibration" else root_seed(c) + " (hold.)", *[f"{c['medians_ms'][b]:.3f}" for b in ("k2", "k2_wide", "queue_dense", "arwlf", "dispatch")], f"{c['regret']:.4f}"] for c in r["n72_whole"]],
        "Final protocol: three warmup and seven timed rounds. QUEUE-DENSE wins all four rows and the dispatcher runs it in every sample. Regret divides dispatcher median by the fastest legal reporter median, including K2. No end-to-end list-generation cost is included.")
    positive = sorted(r["positive"], key=lambda c: (int(c["plant_count"]), c["plant_kind"] != "positive"))
    put("positive", "Constructed target augmentation on authentic source families of calibration root 0x1001: medians in seconds.",
        ["Appended M", "Condition", "Residue-exact", "K2", "K2-WIDE", "ARWLF", "Dispatch"],
        [[f"{int(c['plant_count']):,}", c["plant_kind"], f"{int(c['residue_exact']):,}", *[f"{c['medians_ms'][b]/1000:.4f}" for b in ("k2", "k2_wide", "arwlf", "dispatch")]] for c in positive],
        "Every case uses one warmup and three timed rounds. Residue-exact counts also equal the attached finite-predicate counts in these records: M for positives and zero for decoys. That predicate has (negative, zero, positive) = (2,86,20), not the production binary-root distribution. Constructed coordinates and residues are assigned separately; original-weight consistency and native L1 membership are unestablished. Only one root has completed repeated evidence.")
    w = r["whole_complete"]
    k2_ms = w["k2_ms"]
    rows = [["K2", "64 checkpointed calls", f"{k2_ms/1000:,.1f}", f"{k2_ms/3.6e6:.2f}", f"{k2_ms*1e6/w['pairs']:.3f}", "1.00", f"{w['k2_peak']:.1f}"]]
    for name, label in (("arwlf-1", "ARWLF (before K2)"), ("dispatch-1", "Dispatch (before K2)"), ("arwlf-2", "ARWLF (after K2)"), ("dispatch-2", "Dispatch (after K2)")):
        run = w["runs"][name]
        rows.append([label, "1 call", f"{run['ms']/1000:,.1f}", f"{run['ms']/3.6e6:.3f}", f"{run['ms']*1e6/w['pairs']:.4f}", f"{k2_ms/run['ms']:.2f}", f"{run['peak_working_set_mib']:.1f}"])
    put("whole", "Complete dimension-108 prepared root 0x1001: every backend over all 4,096 cells (single measured passes).",
        ["Run", "Calls", "Backend time (s)", "Hours", "ns per logical pair", "K2 / run", "Peak (MiB)"], rows,
        f"Each run covers 4,096 cells and {w['pairs']:,} logical pairs and reports the same single residue-exact EntryId triple (the constructed target). K2 runs family-major as 64 calls, one per target family u, in the order of a single call; its chunks took {w['k2_chunk_min']/1000:,.0f} to {w['k2_chunk_max']/1000:,.0f} s (median {w['k2_chunk_median']/1000:,.0f} s). The dispatcher selected ARWLF in both passes (probes {w['probes'][0]:.0f} and {w['probes'][1]:.0f} ms). No warmup or repeated rounds; the K2 run spans daytime machine use, and no interference log was recorded.")
    s = r["scaling"]
    v72,v108=s['validation']
    vrows=[
        ['Captured mean entries per L1 list',f"{v72['l1_measured_mean']:,.0f}",f"{v108['l1_measured_mean']:,.0f}"],
        ['Model mean entries per leaf',f"{v72['leaf_occupancy']:.3g}",f"{v108['leaf_occupancy']:.3g}"],
        ['Conceptual packet L: model / census',f"{v72['L']:.2f} / 1.05",f"{v108['L']:.1f} / {s['measured']['census_median_L']['n108_s0x2003_a0']:.1f}"],
        ['Byte-pass probability: model / observed',f"{v72['byte_pass']:.3g} / zero survivors",f"{v108['byte_pass']:.3g} / 9.2e-6 to 1.1e-5"],
        ['K2 projected-pair survival: model / observed',f"{v72['k2_pair_survival']:.4f} / 0.0010",f"{v108['k2_pair_survival']:.4f} / {s['measured']['k2_whole_root_pair_survival']:.4f}"],
        ['Root time (s): model / observed','Dense-rate prediction inapplicable; QUEUE-DENSE ran',f"{v108['predicted_root_s']:,.0f} / 6,403 to 7,396"]]
    put("scaling_validation", "Occupancy model and cross-root checks at the measured dimensions.",
        ["Quantity", "Dimension 72", "Dimension 108"], vrows,
        f"Sizes use 22 captures of seed 0x7201 and seven captures of seed 0x8001 (attempts 0-6); n108 times use six completed roots (attempts 0-5). Census and survival checks use comparable campaign roots, not those same captures. Poisson means use |L1|/(64*8192); byte widths are 8,8,8,4 at n72 and 8,8,8,8 at n108. Only dense time uses the observed {s['arwlf_ns_per_pair_0x1001']:.4f} ns/pair coefficient. Zero n72 survivors does not validate the tiny predicted probability.")
    c = s["l1_calibration_measured_over_model"]
    ps=s['n144']
    geo=[
        ['Entries per L1 list',*[f"{p['l1']:.3g}" for p in ps]],
        ['Mean entries per 13-bit leaf',*[f"{p['leaf_occupancy']:,.0f}" for p in ps]],
        ['Conceptual packet L',*[f"{p['L']:.1f}" for p in ps]],
        ['Byte-pass probability',*[f"{p['byte_pass']:.3f}" for p in ps]],
        ['K2 projected-pair survival',*[f"{p['k2_pair_survival']:.3f}" for p in ps]],
        ['Logical root pairs',*[f"{p['root_pairs']:.3g}" for p in ps]],
        ['Three lists, 48-byte proxy (decimal TB)',*[f"{p['list_storage_proxy_bytes']/1e12:.2f}" for p in ps]],
        ['Three snapshots, 68 bytes/entry (decimal TB)',*[f"{p['snapshot_storage_estimate_bytes']/1e12:.2f}" for p in ps]]]
    headers=['Quantity','72-calibrated','108-calibrated','Uncalibrated']
    put("scaling_n144", "Hypothetical dimension-144 geometry retaining thirteen-bit leaves.",headers,geo,
        f"Equation (8) is scaled by the n72 measured/model ratio ({c['72']:.4f}), the n108 ratio ({c['108']:.4f}), or left unscaled. These are scenarios, not confidence bounds. Q=91 and n=144 exceed the frozen key implementation's limits; no n144 backend ran. Storage excludes prepared keys, directories, temporaries and output. The 48-byte record is an n108 proxy; the native snapshot formula gives 68 bytes/entry at n144.")
    cost=[
        ['Fixed geometry, one core (years)',*[f"{p['frozen_arwlf_core_years']:.3g}" for p in ps]],
        ['Fixed geometry, all hardware CPU cores (years)',*[f"{p['frozen_arwlf_frontier_years']:.2f}" for p in ps]],
        ['Pair sweep only, one core (years)',*[f"{p['retuned_core_years']:.1f}" for p in ps]],
        ['Pair sweep only, all hardware CPU cores (hours)',*[f"{p['retuned_frontier_hours']:.2f}" for p in ps]],
        ['Retuned power-of-two leaves per family',*[f"2^{p['retuned_leaves_per_family'].bit_length()-1}" for p in ps]]]
    put("scaling_cost", "Conditional cost scenarios for one dimension-144 root.",headers,cost,
        f"A hypothetical wider-key design retains the observed {s['arwlf_ns_per_pair_0x1001']:.4f} ns/pair coefficient. Fixed geometry adds a saturated posting scan with coefficient {s['resolver_ns_per_scan']:.4f} ns inferred from the decoy time contrast. Pair sweep only drops that scan term; the listed retuned geometries were not implemented and their setup, directories, residual recovery and communication are unmodeled. Hardware-core figures divide by 630,784 hypothetical equal-speed CPU cores; Frontier defaults to 551,936 allocatable cores. Intel-to-AMD rate transfer is unmeasured and GPUs are absent. These are conditional model costs, not achieved runtimes or universal lower bounds. List construction adds per attempt; attempt count multiplies the per-attempt sum.")
    n72 = r["full_solve_n72"]
    found = [a for a in n72 if a["status"] == "Found"]
    forced = r["full_solve_n72_arwlf"]
    if [a["status"] for a in forced] != [a["status"] for a in n72] or \
       [a["structural_triples"] for a in forced] != [a["structural_triples"] for a in n72] or \
       [a["parents"] for a in forced if a["status"] == "Found"] != [a["parents"] for a in found] or \
       any(a["backend"] != "arwlf" for a in forced):
        raise ValueError("Forced-ARWLF staged solve disagrees with the dispatcher solve")
    froot = [a["root_ms"] / 1000 for a in forced]
    qroot = [a["root_ms"] / 1000 for a in n72]
    n108, cap108 = r["full_solve_n108"], r["full_solve_n108_captures"]
    if [a["attempt"] for a in n108] != list(range(32)) or [c["attempt"] for c in cap108] != list(range(32)) or \
       any(a["status"] != "NoWitnessThisAttempt" or a["dispatch_ran"] != "ARWLF" or a["parents"] for a in n108):
        raise ValueError("n108 staged-solve records changed; Table 7 and Section 8.7 text describe 32 ARWLF attempts without a witness")
    # Design-B comparison: classic solver root (ter_full_bench medians) against
    # the staged backends on the same four n=72 attempts.
    db_summary = {(s["case"], s["reporter"].split("(")[0]): float(s["ms_median"]) for s in r["design_b_summary"]}
    db_classic, db_staged = {}, collections.defaultdict(list)
    for c in r["design_b_classic"]:
        meta = [l for l in c["lines"] if l.startswith("#")]
        rows_c = list(csv.DictReader(l for l in c["lines"] if not l.startswith("#")))
        seed, attempt = int(c["case"][1:7], 16), int(c["case"].split("_a")[1])
        if len(meta) != 1 or " warmup=3 repeat=7 " not in meta[0] or " family=FiniteSanity " not in meta[0] or len(rows_c) != 1 or \
           int(rows_c[0]["terminal_attempt_index"]) != attempt or int(rows_c[0]["terminal_attempt_seed"]) != seed + attempt:
            raise ValueError(f"Design-B classic record is not attempt {attempt} of seed {seed:#x} with 3+7 solves")
        db_classic[c["case"]] = rows_c[0]
    for s in r["design_b_staged"]:
        db_staged[(s["case"], s["backend"])].append(s)
    for (case, backend), runs in db_staged.items():
        classic = db_classic[case]
        is_found = classic["terminal_attempt_status"] == "Found"
        if [s["run"] for s in runs] != list(range(8)) or \
           any(s["record"]["attempt_seed"] != int(classic["terminal_attempt_seed"]) or s["record"]["n"] != 72 or
               s["record"]["status"] != ("Found" if is_found else "NoWitnessThisAttempt") or
               not s["record"]["pinned_cpu"] == s["record"]["cpu_start"] == s["record"]["cpu_end"] or
               (backend == "dispatch" and s["record"]["dispatch_ran"] != "QUEUE_DENSE") for s in runs) or \
           classic["l1_to_l0_early_stopped"] != ("1" if is_found else "0"):
            raise ValueError(f"Design-B staged record disagrees with the classic attempt: {case} {backend}")
        median = statistics.median(s["record"]["root_ms"] for s in runs[1:])
        if not math.isclose(median, db_summary[(case, backend)], rel_tol=1e-12):
            raise ValueError(f"Design-B summary median mismatch: {case} {backend}")
    if len(db_staged) != 16 or len(db_classic) != 4:
        raise ValueError("Design-B record must hold four attempts and four staged backends")
    db = {b: [] for b in ("classic", "k2", "k2_wide", "arwlf", "dispatch")}
    for case, classic in db_classic.items():
        if classic["terminal_attempt_status"] == "Found":
            db_found = (case, int(classic["l1_to_l0_ns"]) / 1e6)
            continue
        db["classic"].append(int(classic["l1_to_l0_ns"]) / 1e6)
        for b in ("k2", "k2_wide", "arwlf", "dispatch"):
            db[b].append(statistics.median(s["record"]["root_ms"] for s in db_staged[(case, b)][1:]))
    over_qd = [c / q for c, q in zip(db["classic"], db["dispatch"])]
    under_arwlf = [a / c for c, a in zip(db["classic"], db["arwlf"])]
    span = lambda v, f=".0f": f"{min(v):{f}}-{max(v):{f}}"
    root108 = sorted(a["root_ms"] / 3.6e6 for a in n108)
    cap_s = [c["elapsed_ns"] / 1e9 for c in cap108]
    put("integration", "Staged integration evidence (staged solver lists, frozen root backend, independent witness verification).",
        ["Check", "Scope", "Result"],
        [["Release test suite", "Historical development log: 59 CTest tests; current tree rebuilt 2026-10-13: 61 tests", "59 passed; 61 passed (results/arwlf/ctest-release-20261013)"],
         ["Differential vs. classic solver (design-B root)", "n=72, 4 seeds x 8 attempts + pinned attempt 21 of seed 0x7201; DISPATCH and forced ARWLF", "341 checks, 0 failures: same outcome and accepted-triple count on all 33 attempts; the classic witness is among the verified parents on the Found attempt"],
         ["AddressSanitizer (MSVC, Debug)", "Historical CTest log: 58 selected tests; n=108 planted excluded", "57 passed; differential timed out at 1 h. Recorded rerun of the differential test (2026-10-11): 341 checks, 0 failures, exit 0, no sanitizer diagnostic"],
         ["Fail-closed cases", "n=36 (Q=24), missing and corrupted snapshots", "NotApplicable / refused, never solved"],
         ["Staged solve, n=72 seed 0x7201, dispatcher", f"{len(n72)} attempts, budget 32; dispatcher selected QUEUE-DENSE", f"Found at attempt {found[0]['attempt']}: 1 verified parent (the planted solution); root {min(qroot):.2f}-{max(qroot):.2f} s per attempt"],
         ["Staged solve, n=72 seed 0x7201, ARWLF forced", f"{len(forced)} attempts, budget 32; ARWLF on every attempt", f"Found at attempt {found[0]['attempt']}: the identical verified parent; same outcome and accepted-triple count on every attempt; root {min(froot):.2f}-{max(froot):.2f} s per attempt"],
         ["Staged solve, n=108 seed 0x8001, dispatcher", f"{len(n108)} attempts, budget 32; dispatcher selected ARWLF on every attempt", f"AttemptLimitReached (not UNSAT): no attempt yields a witness; {sum(a['residue_exact'] > 0 for a in n108)} attempts have 1-{max(a['residue_exact'] for a in n108)} residue-exact triples, none binary of weight n/2; root {root108[0]:.2f}-{root108[-1]:.2f} h per attempt (median {(root108[15] + root108[16]) / 2:.2f} h), {sum(root108):.1f} h in total; lists {min(cap_s):.0f}-{max(cap_s):.0f} s per attempt"],
         ["Root time vs. the classic solver's own root (design B), n=72", f"{len(db['classic'])} attempts without a witness (0x7201 attempts 0 and 10, 0x7203 attempt 0) and Found attempt {db_found[0].split('_a')[1]} of 0x7201; identical level-one lists checked", f"No-witness attempts: design B {span(db['classic'])} ms, QUEUE-DENSE via dispatcher {span(db['dispatch'])} ms, K2 {span(db['k2'])} ms, ARWLF {span(db['arwlf'])} ms; design B is {span(over_qd, '.2f')}x slower than QUEUE-DENSE and ARWLF {span(under_arwlf, '.2f')}x slower than design B. On the Found attempt design B stops at the first witness ({db_found[1]:.0f} ms), so it is not compared"]],
        "Acceptance uses a binary parent of weight n/2, independently verified against the original balanced modular instance. The benchmark sink's FiniteSanity predicate is absent from this path. All staged solves use the same executables; the forced-ARWLF run was unpinned beside the running n108 solve, so neither run's root times are controlled measurements. Release test evidence comes from a saved development log; the differential ASan result is a recorded rerun (data/asan_differential_record.json). The n108 instance has a planted solution, so exhausting 32 attempts reflects TER's finite per-attempt success probability, not unsatisfiability. Design-B times are the classic solver's level-one-to-root stage time, medians of seven solves after three warmups (ter_full_bench reports medians only); staged times are medians of seven pinned runs after one warmup, cover sink reset and the backend call, and exclude snapshot load and key preparation (32-47 ms per run); design B's stage includes its own setup (about 5 ms). The two stage boundaries differ, so the design-B ratios are approximate. No matched full-solver speedup is reported.")
    roots = [(role, e) for role in ("calibration", "holdout") for e in r["subsets"]["n108_" + role]["roots"]]
    put("per_root", "Per-root geometric-mean speedups over all declared authentic cells of each dimension-108 root.",
        ["Root seed", "Role", "Cells", "K2 / ARWLF", "K2-WIDE / ARWLF", "Observed L"],
        [[e["root"].split("_")[1][1:], role, e["cases"], f"{e['gmean_k2_over_arwlf']:.2f}", f"{e['gmean_k2wide_over_arwlf']:.2f}", f"{e['L_range'][0]:.1f}-{e['L_range'][1]:.1f}"] for role, e in roots],
        "Each value is a geometric mean over that root's declared cells, mixing the declared discovery and final protocols. With three roots per role, the root-block bootstrap interval of Table 2 equals the range of the three values shown here.")
    prov = json.loads((DATA / "provenance_hashes.json").read_text(encoding="utf-8"))
    put("provenance", "Frozen build records of the executables behind each reported experiment.",
        ["Experiment", "Executable", "SHA-256 (first 16 hex)", "Source revision", "Tree"],
        [[p["experiment"], {"ter_arwlf_campaign": "campaign", "ter_arwlf_whole_root": "whole-root", "ter_root_snapshot_capture": "capture", "ter_arwlf_solve_attempt": "solve", "ter_hash_join_bench": "hash-join", "ter_full_bench": "classic bench"}[name], digest[:16], p["git_commit"][:8], "dirty" if p["git_dirty"] else "clean"] for p in prov for name, digest in p["executables"].items()],
        "Executables are ter_arwlf_campaign, ter_arwlf_whole_root, ter_root_snapshot_capture (capture), ter_arwlf_solve_attempt (solve), ter_hash_join_bench (hash-join) and ter_full_bench (classic bench). Full hashes, source-manifest hashes and the 0x1001 snapshot hash are in data/provenance_hashes.json. Every build was made from a working tree with uncommitted changes, so the revision alone does not identify the code. The three campaign builds recorded per-file source manifests, which reviewer/check_build_sources.py recovers from the archived tree and history. The staged-solve builds recorded executable hashes only, and the hash-join and design-B entries have no build-freeze record; their hashes are those of the executables that produced the runs.")
    groups = collections.Counter((c["n"], c["phase"], c["protocol"]) for c in r["regime_cases"])
    order = [("108", "anchor", "final"), ("108", "stratified-corners", "discovery"), ("72", "whole", "final"), ("72", "anchor", "final"), ("72", "stratified", "discovery"), ("108", "thin-qx", "discovery"), ("108", "confirm", "final"), ("108", "thin-y", "discovery")]
    rows = [[f"n={n}, {phase}", groups[(n, phase, protocol)], "3" if protocol == "final" else "1", "7" if protocol == "final" else "3", protocol] for n, phase, protocol in order]
    rows += [["Positive / decoy augmentation", len(r["positive"]), "1", "3", "discovery"], ["n=108 ARWLF / dispatch complete-root passes", "4", "0", "1", "single pass"], ["n=108 K2 complete root", "1 (64 calls)", "0", "1", "checkpointed pass"], ["n=72 staged solve, dispatcher", "22 attempts", "0", "1", "Found at attempt 21"], ["n=72 staged solve, ARWLF forced", "22 attempts", "0", "1", "Found at attempt 21; unpinned"], ["n=108 hash-join comparison, 0x1001 anchor cell", "1", "1", "3", "separate run"], ["n=72 design-B root comparison", "4 attempts", "3 classic / 1 staged", "7", "separate run"],["Unreachable source-thinning cases", "3", "—", "—", "no timing"]]
    put("protocols", "Measurement protocols and completed case counts, preserving workload and scope boundaries.",
        ["Dataset / phase", "Cases", "Warmups", "Timed rounds", "Protocol"], rows,
        "The first eight rows sum to 221 completed regime cases. Nine orphan timings from one interrupted attempt remain outside those complete-case groups. All 10 roots are censused at all 4,096 cells; census is not timing. Cases from one root share an independent input instance. Three unreachable rows are statuses, not omitted failures or timed points.")
    y_cases = [c for c in anchors if c["role"] == "calibration"] + r["thin_y"]
    y_cases.sort(key=lambda c: (root_seed(c), -float(c["retain_y"])))
    put("target_detail", "Target-density ablation medians (seconds), with full-density anchor protocol stated explicitly.",
        ["Seed", "Target retention", "Protocol", "K2", "K2-WIDE", "ARWLF"],
        [[root_seed(c), {1.: "1", .5: "1/2", .25: "1/4", .125: "1/8"}[float(c["retain_y"])], c["protocol"], *[f"{c['medians_ms'][b]/1000:.4f}" for b in ("k2", "k2_wide", "arwlf")]] for c in y_cases],
        "Source pair mass is fixed within each root. Full-density references use three warmups/seven timed rounds; thinned cases use one/three, seed 0xA11CE001. Target thinning changes occupancy/filter selectivity and posting recovery, while observed packet occupancy L is nearly unchanged. This is a controlled sensitivity study, not an isolated causal instruction-cost decomposition.")
    confirmation = sorted(r["confirm"], key=lambda c: (root_seed(c), float(c["target_lanes_per_packet"])))
    put("crossover_confirm", "Final-protocol confirmations of the source-thinning crossover bracket.",
        ["Seed", "Observed L", "K2-WIDE (ms)", "ARWLF (ms)", "ARWLF / rival", "Dispatch regret"],
        [[root_seed(c), f"{float(c['lanes_per_packet']):.4f}", f"{c['medians_ms']['k2_wide']:.4f}", f"{c['medians_ms']['arwlf']:.4f}", f"{c['arwlf_over_best_alternative']:.4f}", f"{c['regret']:.4f}"] for c in confirmation],
        "Three warmups/seven timed rounds, thinning seed 0xA11CE001. Rival = min(K2-WIDE, legal QUEUE-DENSE); QUEUE-DENSE is capacity-inapplicable here. Ratios below one favor ARWLF. The dispatcher runs K2-WIDE in all six cases; it misses the measured ARWLF gain near L=2. The two tested points bracket rather than identify an exact universal threshold.")
    return tables


def style() -> None:
    plt.rcParams.update({
        "font.family": "DejaVu Sans", "font.size": 9, "axes.labelsize": 9,
        "axes.titlesize": 10, "xtick.labelsize": 8, "ytick.labelsize": 8,
        "legend.fontsize": 8, "axes.linewidth": .65, "lines.linewidth": 1.45,
        "pdf.fonttype": 42, "ps.fonttype": 42, "mathtext.fontset": "dejavusans",
        "savefig.dpi": DPI, "axes.spines.top": False, "axes.spines.right": False,
    })


def save_figure(fig: plt.Figure, name: str) -> None:
    FIGURES.mkdir(parents=True, exist_ok=True)
    fig.savefig(FIGURES / f"{name}.png", dpi=DPI, facecolor="white", metadata={"Software": "paper/build_results.py"})
    fig.savefig(FIGURES / f"{name}.pdf", facecolor="white", metadata={"Creator": "paper/build_results.py", "CreationDate": dt.datetime(2026, 10, 4, 12, 0, tzinfo=dt.timezone.utc), "ModDate": dt.datetime(2026, 10, 4, 12, 0, tzinfo=dt.timezone.utc)})
    plt.close(fig)


def box(ax, x, y, width, height, label, color="#F5F7F8", edge="#52616A", size=8.7, dashed=False):
    patch = FancyBboxPatch((x, y), width, height, boxstyle="round,pad=0.010,rounding_size=0.012", linewidth=.8, facecolor=color, edgecolor=edge, linestyle="--" if dashed else "-")
    ax.add_patch(patch)
    ax.text(x + width / 2, y + height / 2, label, ha="center", va="center", fontsize=size, linespacing=1.3)


def arrow(ax, start, end, dashed=False, color="#52616A"):
    ax.add_patch(FancyArrowPatch(start, end, arrowstyle="-|>", mutation_scale=10, linewidth=.9, color=color, linestyle="--" if dashed else "-"))


def topology() -> None:
    fig, ax = plt.subplots(figsize=(6.5, 3.6))
    fig.subplots_adjust(left=.025, right=.975, top=.97, bottom=.03)
    ax.set(xlim=(0, 1), ylim=(0, 1)); ax.axis("off")
    ax.text(.5, .965, "Complete native construction and isolated reporting boundary", ha="center", fontsize=10, fontweight="bold")
    locations = [.025, .265, .505, .755]
    labels = ["18 logical L3 lists", "9 L2 lists", "3 L1 lists\nA, B, C", "Production L0\nbinary-root path"]
    for x, label in zip(locations, labels):
        box(ax, x, .71, .20, .15, label, color="#FFFFFF" if x == .755 else "#EAF2F4", size=8.7, dashed=x == .755)
    for i, word in enumerate(["binary", "ternary", "ternary"]):
        arrow(ax, (locations[i] + .21, .785), (locations[i+1] - .012, .785), dashed=i == 2)
        ax.text((locations[i]+.20+locations[i+1])/2, .886, word + " merge", ha="center", fontsize=7.8)
    ax.text(.855, .66, "Untimed production branch", ha="center", fontsize=7.8, color="#59646B")
    box(ax, .10, .055, .81, .54, "", color="#FBFCFC", edge="#A2ADB3", dashed=True)
    ax.text(.122, .565, "Prepared benchmark boundary", fontsize=8.5, fontweight="bold", color="#006C75")
    box(ax, .235, .39, .63, .14, "Captured authentic pre-root state\nOriginal residues, ternary coordinates, EntryIds", color="#EAF2F4", size=8.6)
    arrow(ax, (.605, .70), (.605, .54))
    arrow(ax, (.55, .385), (.55, .325))
    box(ax, .235, .185, .63, .125, "Selected-family exact arithmetic reporter\n" + r"$q_A+q_B\equiv d_C\ (\mathrm{mod}\ 2^Q)$", color="#FFFFFF", size=8.7)
    ax.text(.16, .245, "ARWLF /\nbaselines", ha="center", va="center", fontsize=7.5)
    arrow(ax, (.55, .18), (.55, .12))
    ax.text(.55, .09, "Residue-exact EntryId multiset; separate attached finite predicate", ha="center", fontsize=8.0)
    save_figure(fig, "topology")


def pipeline() -> None:
    fig, ax = plt.subplots(figsize=(6.5, 4.6))
    fig.subplots_adjust(left=.025, right=.975, top=.975, bottom=.025)
    ax.set(xlim=(0, 1), ylim=(0, 1)); ax.axis("off")
    ax.text(.5, .970, "Filtering is a necessary condition; exact recovery is the authority", ha="center", fontsize=10, fontweight="bold")
    steps = [
        (.805, r"Family cell $(s,t)$:  $u=(s+t)\,\mathrm{mod}\,64$" + "\n" + r"Family carry $\kappa=\lfloor(s+t)/64\rfloor$", "#EAF2F4"),
        (.665, r"Target leaf $k$; route $j=(k-i-\kappa)\,\mathrm{mod}\,8192$" + "\n" + r"Carry $c=\lfloor(i+j+\kappa)/8192\rfloor$; skip empty target leaves", "#EAF2F4"),
        (.525, "One A row × 16 B lanes\nFirst two byte-occupancy tests + valid-lane mask", "#EAF2F4"),
        (.385, "Drain masks; check remaining mid bytes\nResolve every Y posting in the leaf; compare the full Q-bit key", "#FFFFFF"),
        (.245, "Emit original identity triples with posting multiplicity\nCheck original stored residue sum ≡ T  (mod 2ⁿ)", "#FFFFFF"),
    ]
    for y, text, color in steps:
        box(ax, .07, y, .86, .105, text, color=color, size=8.7)
    for first, second in zip(steps, steps[1:]):
        arrow(ax, (.50, first[0]-.008), (.50, second[0]+.118))
    arrow(ax, (.31, .237), (.27, .186))
    arrow(ax, (.69, .237), (.74, .186))
    box(ax, .045, .065, .43, .11, "Residue-exact identity multiset\nCaptured before the finite predicate", color="#EAF2F4", size=8.3)
    box(ax, .53, .065, .43, .11, "Attached finite-predicate counter\nn=108: (negative,zero,positive)=(2,86,20)", color="#FFF4E7", edge="#AD5900", size=7.7)
    ax.text(.50, .010, "The attached finite predicate differs from the production binary-root contract.", ha="center", fontsize=7.6)
    save_figure(fig, "pipeline")


def dense(r: dict) -> None:
    cases = [c for c in r["regime_cases"] if c["n"] == "108" and c["source_kind"] == "AUTHENTIC_FULLTER"]
    roots = sorted({c["root"] for c in cases})
    fig, ax = plt.subplots(figsize=(6.5, 4.05))
    fig.subplots_adjust(left=.105, right=.98, top=.78, bottom=.19)
    positions, tick_labels = [], []
    x = 0
    for root in roots:
        group = sorted([c for c in cases if c["root"] == root], key=lambda c: (c["protocol"] != "final", int(c["cell"])))
        color = ROLE_COLORS[group[0]["role"]]
        xs = list(range(x, x + len(group)))
        for at, c in zip(xs, group):
            ax.scatter(at, c["k2_over_arwlf"], marker="D" if c["protocol"] == "final" else "o", s=35, facecolor=color, edgecolor="white", linewidth=.55, zorder=3)
        ax.hlines(geomean([c["k2_over_arwlf"] for c in group]), x-.38, x+len(group)-.62, colors=color, linestyles="--", linewidth=1.1)
        if x:
            ax.axvline(x-.5, color="#CDD3D7", linewidth=.5)
        positions.append(statistics.mean(xs)); tick_labels.append(root_seed(group[0])); x += len(group)
    ax.axvspan(-.5, 13.5, color=ROLE_COLORS["calibration"], alpha=.045)
    ax.axvspan(13.5, 28.5, color=ROLE_COLORS["holdout"], alpha=.045)
    ax.set_xlim(-.7, 28.7)
    vals = [c["k2_over_arwlf"] for c in cases]
    ax.set_ylim(math.floor(min(vals)/10)*10-3, math.ceil(max(vals)/10)*10+3)
    ax.set_xticks(positions, tick_labels)
    ax.set_xlabel("Captured root seed (each point is one selected cell)")
    ax.set_ylabel("K2 median / ARWLF median")
    ax.grid(axis="y", alpha=.22, linewidth=.6)
    ax.text(.22, 1.11, "Calibration: 14 cells / 3 roots", transform=ax.transAxes, ha="center", color=ROLE_COLORS["calibration"], fontsize=9, fontweight="bold")
    ax.text(.22, 1.025, "Case-pooled GM = 53.0875×", transform=ax.transAxes, ha="center", color=ROLE_COLORS["calibration"], fontsize=8.4)
    ax.text(.75, 1.11, "Holdout: 15 cells / 3 roots", transform=ax.transAxes, ha="center", color=ROLE_COLORS["holdout"], fontsize=9, fontweight="bold")
    ax.text(.75, 1.025, "Case-pooled GM = 52.8933×", transform=ax.transAxes, ha="center", color=ROLE_COLORS["holdout"], fontsize=8.4)
    handles = [Line2D([], [], marker="D", ls="", color="#47555E", label="Final protocol (7 rounds)", markersize=5), Line2D([], [], marker="o", ls="", color="#47555E", label="Discovery (3 rounds)", markersize=5), Line2D([], [], ls="--", color="#47555E", label="Root geometric mean")]
    fig.legend(handles=handles, loc="upper center", bbox_to_anchor=(.5, .99), ncol=3, frameon=False, handlelength=1.6, columnspacing=1.4)
    fig.text(.54, .032, "Prepared authentic cells; repeated cells share their captured root.", ha="center", fontsize=8, color="#59646B")
    save_figure(fig, "dense")


def crossover(r: dict) -> None:
    fig, ax = plt.subplots(figsize=(6.5, 4.2))
    fig.subplots_adjust(left=.105, right=.98, top=.87, bottom=.20)
    for seed, color in ROOT_COLORS.items():
        aggregate = sorted([c for c in r["thin_qx"] if seed in c["root"]], key=lambda c: c["target_L"])
        xs = []
        for c in aggregate:
            original = [x for x in r["regime_cases"] if x["root"] == c["root"] and x["phase"] == "thin-qx" and float(x["target_lanes_per_packet"]) == c["target_L"]]
            xs.append(geomean([float(x["lanes_per_packet"]) for x in original]))
        ax.plot(xs, [c["gmean_arwlf_over_best_alt"] for c in aggregate], color=color, marker="o", markersize=4, label=seed)
        final = [c for c in r["confirm"] if seed in c["root"]]
        ax.scatter([float(c["lanes_per_packet"]) for c in final], [c["arwlf_over_best_alternative"] for c in final], facecolor="white", edgecolor=color, marker="D", linewidth=1.4, s=48, zorder=5)
    ax.set_xscale("log", base=2); ax.set_yscale("log")
    ax.set_xlim(.90, 108); ax.set_ylim(.023, 210)
    ticks = [1, 2, 4, 8, 16, 32, 64, 96]
    ax.set_xticks(ticks, [str(x) for x in ticks])
    ax.set_yticks([.03, .1, .3, 1, 3, 10, 30, 100], ["0.03", "0.1", "0.3", "1", "3", "10", "30", "100"])
    ax.axhspan(.023, 1, color="#EAF2F4", alpha=.55)
    ax.axhline(1, color="#6C7478", ls="--", linewidth=.9)
    ax.text(75, 1.16, "equal time", ha="center", fontsize=7.6, color="#59646B")
    ax.text(73, .057, "ARWLF faster", ha="center", fontsize=8.2, color="#006C75")
    ax.set_xlabel("Observed logical lanes per 16 × 16 accounting packet, L")
    ax.set_ylabel("ARWLF median / fastest rival median")
    ax.grid(which="major", alpha=.20, linewidth=.55)
    handles, labels = ax.get_legend_handles_labels()
    handles.append(Line2D([], [], marker="D", ls="", markerfacecolor="white", markeredgecolor="#47555E", markersize=6))
    labels.append("Final confirmation")
    fig.legend(handles, labels, loc="upper center", bbox_to_anchor=(.5, .995), ncol=4, frameon=False, columnspacing=1.2, handlelength=1.6)
    fig.text(.53, .078, "Lines: discovery geometric means over 3 thinning seeds; diamonds: 7-round final cases.", ha="center", fontsize=7.9)
    fig.text(.53, .035, "Rival = min(K2-WIDE, legal QUEUE-DENSE); source thinning of n=108 roots.", ha="center", fontsize=7.9, color="#59646B")
    save_figure(fig, "crossover")


def target(r: dict) -> None:
    fig, axes = plt.subplots(1, 3, figsize=(6.5, 4.0), sharey=True)
    fig.subplots_adjust(left=.09, right=.985, top=.80, bottom=.21, wspace=.12)
    markers = {"k2": "o", "k2_wide": "s", "arwlf": "^"}
    for ax, seed in zip(axes, ROOT_COLORS):
        full = next(c for c in r["subsets"]["n108_final_calibration"]["cases"] if root_seed(c) == seed)
        controls = sorted([c for c in r["thin_y"] if root_seed(c) == seed], key=lambda c: float(c["retain_y"]))
        cases = controls + [full]
        for backend, color in BACKEND_COLORS.items():
            y = [c["medians_ms"][backend]/1000 for c in cases]
            ax.plot([0, 1, 2], y[:3], color=color, marker=markers[backend], markersize=4)
            ax.plot([2, 3], y[2:], color=color, ls="--")
            ax.scatter(3, y[3], marker=markers[backend], facecolor="white", edgecolor=color, linewidth=1.1, s=35, zorder=4)
        ax.set_title(seed, pad=9)
        ax.set_yscale("log"); ax.set_xlim(-.2, 3.25); ax.set_ylim(.45, 180)
        ax.set_xticks([0, 1, 2, 3], ["1/8", "1/4", "1/2", "1"])
        ax.set_xlabel("Target retention")
        ax.grid(axis="y", which="major", alpha=.22, linewidth=.55)
    axes[0].set_ylabel("Median backend time (seconds)")
    axes[0].set_yticks([.5, 1, 2, 5, 10, 20, 50, 100])
    axes[0].yaxis.set_major_formatter(FuncFormatter(lambda value, _: f"{value:g}"))
    handles = [Line2D([], [], color=color, marker=markers[b], label={"k2": "K2", "k2_wide": "K2-WIDE", "arwlf": "ARWLF"}[b], markersize=4) for b, color in BACKEND_COLORS.items()]
    fig.legend(handles=handles, loc="upper center", bbox_to_anchor=(.5, .995), ncol=3, frameon=False, columnspacing=2.5)
    fig.text(.54, .87, "Fixed source pairs; three calibration n=108 roots", ha="center", fontsize=9)
    fig.text(.54, .08, "Filled points: discovery (3 rounds). Open full-density points: final anchors (7 rounds).", ha="center", fontsize=7.9)
    fig.text(.54, .035, "Dashed segments connect differing protocols; lines guide the eye and are not fitted costs.", ha="center", fontsize=7.9, color="#59646B")
    save_figure(fig, "target")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--freeze-inputs", action="store_true")
    parser.add_argument("--tables-only", action="store_true", help="Update numerical tables without writing figure PDFs or images")
    args = parser.parse_args()
    if args.freeze_inputs:
        freeze_inputs()
    path = DATA / "results_review.json"
    if not path.exists():
        raise SystemExit("Frozen data are absent; run once with --freeze-inputs.")
    result = json.loads(path.read_text(encoding="utf-8"))
    verification = verify_frozen_data(result)
    result["whole_complete"] = load_whole_complete()
    result["full_solve_n72"] = [json.loads(l) for l in (DATA / "full_solve_n72_attempts.jsonl").read_text(encoding="utf-8-sig").splitlines() if l.strip()]
    result["full_solve_n72_arwlf"] = [json.loads(l) for l in (DATA / "full_solve_n72_arwlf_attempts.jsonl").read_text(encoding="utf-8-sig").splitlines() if l.strip()]
    for key in ("full_solve_n108_attempts", "full_solve_n108_captures"):
        result[key.replace("_attempts", "")] = [json.loads(l) for l in (DATA / f"{key}.jsonl").read_text(encoding="utf-8-sig").splitlines() if l.strip()]
    result["design_b_classic"] = [json.loads(l) for l in (DATA / "design_b_n72_classic.jsonl").read_text(encoding="utf-8").splitlines() if l.strip()]
    result["design_b_staged"] = [json.loads(l) for l in (DATA / "design_b_n72_staged.jsonl").read_text(encoding="utf-8").splitlines() if l.strip()]
    with (DATA / "design_b_n72_summary.csv").open(newline="", encoding="utf-8-sig") as f:
        result["design_b_summary"] = list(csv.DictReader(f))
    with (DATA / "hash_join_n108_anchor.csv").open(newline="", encoding="utf-8") as f:
        result["hash_join"] = list(csv.DictReader(f))
    result["scaling"] = json.loads((DATA / "scaling_model.json").read_text(encoding="utf-8"))
    tables = make_tables(result)
    if set(tables) != {"anchors", "aggregates", "sparse", "positive", "whole", "protocols", "target_detail", "crossover_confirm", "scaling_validation", "scaling_n144", "scaling_cost", "integration", "per_root", "provenance"}:
        raise ValueError("Manuscript table marker set changed")
    for table in tables.values():
        if any(len(row) != len(table["headers"]) for row in table["rows"]):
            raise ValueError("Table width mismatch")
    write_json(TABLES / "tables.json", tables)
    if args.tables_only:
        print(json.dumps({"tables":list(tables),"mode":"tables-only","raw_verification":{k:v for k,v in verification.items() if k!='aggregates'}},indent=2))
        return
    style()
    topology(); pipeline(); dense(result); crossover(result); target(result)
    captions = {
        "topology": "Complete native TER list construction and the isolated prepared reporting boundary used in Sections 7–8.6. The production binary-root branch is untimed in those prepared-reporter experiments; Section 8.7 evaluates the staged solver integration separately. Captured native L1 lists provide arithmetic keys, stored residues, coordinate data and EntryIds. The prepared reporter outputs alone do not certify a solver solution.",
        "pipeline": "ARWLF's routing, necessary byte predicates and exact survivor resolution. The arithmetic reporter retains every full quotient equality and original posting multiplicity. Residue-exact identities are collected before the attached finite test-parent predicate of Section 2.3, which differs from the production binary-root rule.",
        "dense": "All 29 declared authentic dimension-108 cell speedups, grouped by captured root and calibration role. Diamonds use the final protocol (seven timed rounds) and circles discovery (three); dashed segments show each root's geometric mean. Aggregate labels are case-pooled geometric means. Repeated cells share their root and are not independent instances.",
        "crossover": "Controlled source-thinning crossover for three calibration dimension-108 roots. Discovery lines use geometric means over three thinning seeds; plotted L is their geometric-mean observed census occupancy. Open diamonds are separate final-protocol confirmations using seed 0xA11CE001. Ratios below one favor ARWLF. Lines connect measured points, not a fitted or universal threshold. Target L=96 is unreachable for root 0x1001.",
        "target": "Absolute median times under target-only thinning with fixed source-pair mass on three calibration roots. Filled points use discovery (one warmup, three timed rounds); open full-density references use final anchors (three warmups, seven timed rounds). Dashed connections mark the protocol change. Target thinning is a controlled sensitivity experiment, not a smaller-dimension authentic benchmark.",
    }
    descriptions = {
        "topology": "Flow diagram: 18 logical L3 lists merge to 9 L2 lists and then 3 L1 lists. A dashed, untimed production L0 branch is separate. A downward capture branch supplies an isolated prepared arithmetic reporter, which produces a residue-exact identity multiset and separate finite-predicate count.",
        "pipeline": "Five sequential boxes show family carry calculation, 13-bit leaf routing, one-row-by-16-lane byte tests, exact full-key resolution, and stored-residue checking. Output branches separately to the residue-exact multiset and the finite-predicate counter; n=108 finite counts are negative 2, zero 86, positive 20.",
        "dense": "Scatterplot of all 29 K2/ARWLF median ratios across six root-seed groups. Calibration points are teal; holdout points orange. Diamonds indicate seven-round final anchors and circles three-round discovery cases. Per-root geometric means are dashed. Most ratios lie near 47 to 54; the 0x2003 final anchor is about 78, with holdout discovery points near 62 to 63 and one near 47.",
        "crossover": "Log-log plot of ARWLF/rival time ratio against observed packet occupancy L for three source-thinned roots. Discovery averages over three thinning seeds are colored connected circles; final confirmations are open diamonds. Ratios exceed one at L=1 and fall below one near L=2, approaching about 0.03 at the densest tested controls.",
        "target": "Three side-by-side panels show absolute median times for K2, K2-WIDE and ARWLF as target retention increases from one-eighth to one. K2 and K2-WIDE become much slower with denser targets, while ARWLF changes more modestly. Open full-density points use seven-round final anchors; filled thinned points use three-round discovery cases, with dashed segments separating the protocols.",
    }
    write_json(FIGURES / "figures.json", {name: {"caption": caption, "description": descriptions[name], "png": f"figures/{name}.png", "pdf": f"figures/{name}.pdf", "width_inches": 6.5, "dpi": DPI} for name, caption in captions.items()})
    print(json.dumps({"tables": list(tables), "figures": list(captions), "raw_verification": {k: v for k, v in verification.items() if k != "aggregates"}}, indent=2))


if __name__ == "__main__":
    main()
