"""Closed-form TER root-occupancy model checked at n=72 and n=108.

    python scaling_model.py --freeze-inputs   # read repository artifacts -> data/scaling_inputs.json
    python scaling_model.py                   # data/scaling_inputs.json -> data/scaling_model.{csv,json}

Normal operation reads only data/scaling_inputs.json.  Nothing here runs a
solver or a backend. n=144 is an abstract geometry/cost extrapolation: the
frozen native preparation is limited to n<=128 and full quotient Q<=69.

Model (FiniteSanity: every epsilon is zero, so level-3 halves carry n/36 ones,
level-2 vectors n/18 ones, level-1 vectors n/6 ones):
  |L3| = C(n/2, n/36)                                     exact
  E|L2| = |L3|^2 / 2^r2                                   expected
  E|L1| = C(n, n/6) 2^-r1 * Rep2 * 2^(-2 r2) * f2^3       independence approximation,
          f2 = C(n/2, n/36)^2 / C(n, n/18)                calibrated below by measurement
Leaf occupancy per family/leaf is |L1| / (64 * 8192).  Leaf sizes are modelled
as Poisson; L = E[a]E[b] / (E[ceil(a/16)] E[ceil(b/16)]) over nonempty leaves;
The byte pass rate averages independent chunk occupancy over target leaf size;
n=72 uses chunks (8,8,8,4), and n=108 uses (8,8,8,8). K2's 25-bit pair
survival is 1 - exp(-|C_u| / 2^19).
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import re
import statistics
import subprocess
from math import comb, ceil, exp, lgamma, log
from pathlib import Path

PAPER = Path(__file__).resolve().parent
REPO = PAPER.parent
DATA = PAPER / "data"
INPUTS = DATA / "scaling_inputs.json"
LEAVES = 8192
FAMILIES = 64
FRONTIER_CPU_CORES = 9856 * 64  # OLCF Frontier User Guide: 9,856 nodes, one 64-core EPYC each


# --------------------------------------------------------------------------- inputs
def freeze_inputs() -> None:
    """Collect every measured value the model is checked against, with its source."""
    preflight = {}
    exe = REPO / "build-full" / "Release" / "ter_full_bench.exe"
    for n in (72, 108, 144):
        out = subprocess.run([str(exe), f"--n={n}", "--preflight"], capture_output=True, text=True, check=True).stdout
        r1, r2 = (int(x) for x in re.search(r"r1=(\d+) r2=(\d+)", out).groups())
        rep2 = int(re.search(r"representation2=(\d+)", out).group(1))
        preflight[str(n)] = {"r1": r1, "r2": r2, "representation2": rep2}

    def capture_sizes(log: Path) -> dict:
        text = log.read_text(encoding="utf-8", errors="replace")
        l2 = [int(x) for x in re.search(r"level2 sizes: ([\d ]+)", text).group(1).split()]
        l1 = [int(x) for x in re.search(r"level1 sizes: A=(\d+) B=(\d+) C=(\d+)", text).groups()]
        return {"source": str(log.relative_to(REPO)), "l2": l2, "l1": l1}

    n72_logs = sorted((REPO / "results/arwlf/full-solve-n72-s0x7201").glob("attempt_*.capture.log"))
    n108_logs = sorted((REPO / "results/arwlf/full-solve-n108-s0x8001").glob("attempt_*.capture.log"))

    regime = list(csv.DictReader(open(REPO / "results/arwlf/regime-20261001/cases.csv", encoding="utf-8")))
    anchors = [r for r in regime if r["backend"] == "arwlf" and r["phase"] == "anchor" and r["n"] == "108"]
    census = {}
    for path in sorted((REPO / "results/arwlf/regime-20261001").glob("*_census.csv")):
        rows = list(csv.DictReader(open(path, encoding="utf-8")))
        census[path.name.replace("_census.csv", "")] = statistics.median(float(r["lanes_per_packet"]) for r in rows)

    whole = [json.loads(l) for l in open(REPO / "results/arwlf/whole-root-20261003/runs.jsonl", encoding="utf-8") if l.strip()]
    k2 = [w for w in whole if w["run_id"] == "k2-1"]
    arwlf_runs = {w["run_id"]: w["ms"] for w in whole if w["run_id"] in ("arwlf-1", "arwlf-2")}
    attempts = [json.loads(l) for l in open(REPO / "results/arwlf/full-solve-n108-s0x8001/attempts.jsonl", encoding="utf-8-sig") if l.strip()]

    positive = {r["case_id"]: r for r in csv.DictReader(open(REPO / "results/arwlf/positive-20261003/cases.csv", encoding="utf-8")) if r["backend"] == "arwlf"}
    dense, base = positive["plant:decoy:M1000000:s0xb1a57001"], positive["plant:decoy:M100:s0xb1a57001"]

    payload = {
        "preflight": preflight,
        "n72_captures": [capture_sizes(p) for p in n72_logs],
        "n108_captures": [capture_sizes(p) for p in n108_logs],
        "census_median_L": census,
        "n108_anchor_filter_survivors_per_pair": {r["root"]: int(r["arwlf_filter_survivors"]) / int(r["logical_pairs"]) for r in anchors},
        "k2_whole_root_pair_survival": sum(c["pair_hits"] for c in k2) / sum(c["logical_pairs"] for c in k2),
        "whole_root_0x1001": {"logical_pairs": whole[0]["logical_pairs"], "arwlf_ms": arwlf_runs},
        "n108_s0x8001_root_ms": {str(a["attempt"]): a["root_ms"] for a in attempts},
        "resolver_scan": {
            "source": "results/arwlf/positive-20261003/cases.csv, ARWLF decoy M=1e6 minus decoy M=100",
            "extra_ms": float(dense["median_ms"]) - float(base["median_ms"]),
            "survivors": int(dense["arwlf_filter_survivors"]),
            "target_leaf_mean": float(dense["y_leaf_mean"]),
        },
        "frontier": {"nodes": 9856, "cpu_cores_per_node": 64, "ddr4_gb_per_node": 512,
                     "source": "OLCF Frontier User Guide, https://docs.olcf.ornl.gov/systems/frontier_user_guide.html (accessed 2026-10-08)"},
    }
    INPUTS.write_text(json.dumps(payload, indent=2), encoding="utf-8")


# --------------------------------------------------------------------------- model
def pois(lam: float, k: int) -> float:
    return exp(-lam + k * log(lam) - lgamma(k + 1))


def poisson_terms(lam: float):
    kmax = int(lam + 12 * math.sqrt(lam) + 40)
    return ((k, pois(lam, k)) for k in range(0, kmax))


def lanes_per_packet(a: float, b: float) -> float:
    def moments(lam: float) -> tuple[float, float]:
        mean = sum(p * k for k, p in poisson_terms(lam))
        packets = sum(p * ceil(k / 16) for k, p in poisson_terms(lam) if k > 0)
        return mean, packets
    ea, ca = moments(a)
    eb, cb = moments(b)
    return ea * eb / (ca * cb)


def byte_pass(y: float, chunk_bits: tuple[int,...]=(8,8,8,8)) -> float:
    terms=list(poisson_terms(y))
    mass=math.fsum(p for _,p in terms)
    value = math.fsum(
        p * math.prod(-math.expm1(k * math.log1p(-1 / (2**bits))) for bits in chunk_bits)
        for k, p in terms
    ) / mass
    return min(1.0,max(0.0,value))


def expected_sizes(n: int, pre: dict) -> tuple[int, float, float]:
    l3 = comb(n // 2, n // 36)
    l2 = l3 ** 2 / 2 ** pre["r2"]
    f2 = l3 ** 2 / comb(n, n // 18)
    l1 = comb(n, n // 6) * 2.0 ** -pre["r1"] * pre["representation2"] * 2.0 ** (-2 * pre["r2"]) * f2 ** 3
    return l3, l2, l1


def root_geometry(l1: float, q_bits: int=69) -> dict:
    occ = l1 / (FAMILIES * LEAVES)
    mid_bits=min(32,min(q_bits,64)-6-13)
    chunks=tuple(min(8,mid_bits-start) for start in range(0,mid_bits,8))
    return {"leaf_occupancy": occ, "L": lanes_per_packet(occ, occ), "byte_filter_chunk_bits":chunks,"byte_pass": byte_pass(occ,chunks),
            "k2_pair_survival": 1.0 - exp(-(l1 / FAMILIES) / 2 ** 19), "root_pairs": l1 * l1}


def build() -> dict:
    inp = json.loads(INPUTS.read_text(encoding="utf-8"))
    pre = {int(k): v for k, v in inp["preflight"].items()}
    w = inp["whole_root_0x1001"]
    ns_per_pair = min(w["arwlf_ms"].values()) * 1e6 / w["logical_pairs"]
    rs = inp["resolver_scan"]
    ns_per_scan = rs["extra_ms"] * 1e6 / (rs["survivors"] * rs["target_leaf_mean"])

    m72 = [c["l1"] for c in inp["n72_captures"]]
    m108 = [c["l1"] for c in inp["n108_captures"]]
    mean72 = statistics.mean(x for row in m72 for x in row)
    mean108 = statistics.mean(x for row in m108 for x in row)

    rows = []
    for n in (72, 108, 144):
        l3, l2, l1 = expected_sizes(n, pre[n])
        rows.append({"n": n, "r1": pre[n]["r1"], "r2": pre[n]["r2"], "Q": n - pre[n]["r1"],
                     "L3": l3, "E_L2": l2, "E_L1_independence": l1})
    calib = {72: mean72 / rows[0]["E_L1_independence"], 108: mean108 / rows[1]["E_L1_independence"]}

    out = {"per_dimension": rows, "l1_calibration_measured_over_model": calib,
           "native_applicability":{"maximum_n":128,"maximum_full_quotient_bits":69,"n144_applicable":False},
           "cost_assumptions":{"processor_rate":"Intel dense prepared-root rate reused on every hypothetical CPU core; AMD rate unmeasured","resolver_rate":"positive-control time contrast, not an isolated instruction measurement","fixed_geometry":"hypothetical wider-key implementation retaining thirteen-bit leaves","retuned":"pair-sweep floor only; setup, communication, memory limits and residual recovery unmodelled"},
           "arwlf_ns_per_pair_0x1001": ns_per_pair, "resolver_ns_per_scan": ns_per_scan,
           "validation": [], "n144": []}

    for n,label,l1 in ((72,f"n=72, seed 0x7201 ({len(m72)} captures)",mean72),(108,f"n=108, seed 0x8001 (captures 0-{len(m108)-1}; completed root timings 0-5)",mean108)):
        g = root_geometry(l1,n-pre[n]['r1'])
        out["validation"].append({"case": label, "l1_measured_mean": l1, **g,
                                  "predicted_root_s": g["root_pairs"] * ns_per_pair * 1e-9 if n == 108 else None})

    for label, factor in (("low (n=72 calibration)", min(calib.values())),
                          ("mid (n=108 calibration)", calib[108]),
                          ("uncalibrated model", 1.0)):
        l1 = rows[2]["E_L1_independence"] * factor
        g = root_geometry(l1,91)
        retuned_leaf_count=2**math.ceil(math.log2(l1/(FAMILIES*13)))
        per_pair_saturated_ns = ns_per_pair + g["byte_pass"] * g["leaf_occupancy"] * ns_per_scan
        frozen_core_s = g["root_pairs"] * per_pair_saturated_ns * 1e-9
        ideal_core_s = g["root_pairs"] * ns_per_pair * 1e-9
        out["n144"].append({"scenario": label, "l1": l1, **g,
                            "retuned_leaves_per_family":retuned_leaf_count,
                            "retuned_target_leaf_mean":l1/(FAMILIES*retuned_leaf_count),
                            "list_storage_proxy_bytes": 3 * l1 * 48,
                            "snapshot_storage_estimate_bytes": 3 * l1 * 68,
                            "frozen_arwlf_core_years": frozen_core_s / 3.15576e7,
                            "frozen_arwlf_frontier_years": frozen_core_s / FRONTIER_CPU_CORES / 3.15576e7,
                            "retuned_core_years": ideal_core_s / 3.15576e7,
                            "retuned_frontier_hours": ideal_core_s / FRONTIER_CPU_CORES / 3600})
    out["measured"] = {k: inp[k] for k in ("census_median_L", "n108_anchor_filter_survivors_per_pair",
                                           "k2_whole_root_pair_survival", "n108_s0x8001_root_ms")}
    out["frontier"] = inp["frontier"]
    out["frontier"].update({"hardware_cpu_cores":FRONTIER_CPU_CORES,"default_allocatable_cpu_cores":9856*56,"source_verified":"2026-10-09"})
    return out


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--freeze-inputs", action="store_true")
    if parser.parse_args().freeze_inputs:
        freeze_inputs()
    result = build()
    (DATA / "scaling_model.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    with open(DATA / "scaling_model.csv", "w", newline="", encoding="utf-8") as f:
        fields = ["section", "case", "l1", "leaf_occupancy", "L", "byte_pass", "k2_pair_survival", "root_pairs",
                  "predicted_root_s", "frozen_arwlf_frontier_years", "retuned_frontier_hours"]
        wr = csv.DictWriter(f, fieldnames=fields)
        wr.writeheader()
        for v in result["validation"]:
            wr.writerow({"section": "validation", "case": v["case"], "l1": v["l1_measured_mean"],
                         **{k: v[k] for k in fields[3:9] if k in v}})
        for p in result["n144"]:
            wr.writerow({"section": "n144_prediction", "case": p["scenario"], "l1": p["l1"],
                         **{k: p[k] for k in fields[3:] if k in p}})
    print(json.dumps({k: result[k] for k in ("l1_calibration_measured_over_model", "arwlf_ns_per_pair_0x1001",
                                             "resolver_ns_per_scan", "validation", "n144")}, indent=1))


if __name__ == "__main__":
    main()
