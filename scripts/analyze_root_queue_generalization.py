#!/usr/bin/env python3
"""Analysis of the TER^2 root-queue generalization campaign.

Reads the benchmark's cells.csv (one row per workload cell) and reports, for
the QUEUE_DENSE / K2 comparison:

  * per-cell speedup S_i = median(K2) / median(QUEUE_DENSE), backend-total
    (index build and authority charged) and steady (queue index build removed);
  * aggregates that never average milliseconds or arithmetic-average ratios:
    geometric mean, median, quartiles, min, max, wins / cells;
  * the same per tier (authentic / controlled), per family and per scale;
  * Pearson and Spearman correlation of ln S against workload telemetry
    (rho_q, bits per queued word, mean popcount, P0, candidate bits, scale);
  * an OLS crossover fit ln S = a + b ln(rho_q) and the rho_q where S = 1;
  * the pre-registered interpretation (EXCELLENT / STILL_PUBLISHABLE / BAD).

Only rows with valid == 1, power_ok == 1 and a positive speedup enter any
aggregate.  Everything else is listed, never silently dropped.

Standard library only.  Usage:
    python scripts/analyze_root_queue_generalization.py <campaign_dir> [<campaign_dir> ...]
"""

from __future__ import annotations

import csv
import json
import math
import statistics
import sys
from pathlib import Path


def load(directories: list[Path]) -> list[dict]:
    rows = []
    for directory in directories:
        with open(directory / "cells.csv", newline="") as handle:
            for row in csv.DictReader(handle):
                row["_campaign"] = str(directory)
                rows.append(row)
    return rows


def number(row: dict, key: str) -> float:
    try:
        return float(row[key])
    except (KeyError, ValueError):
        return 0.0


def usable(row: dict) -> bool:
    return (
        row.get("valid") == "1"
        and row.get("power_ok") == "1"
        and row.get("status") == "Completed"
        and number(row, "speedup_backend") > 0.0
    )


def quantile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    if not ordered:
        return float("nan")
    position = fraction * (len(ordered) - 1)
    lower = int(position)
    upper = min(lower + 1, len(ordered) - 1)
    weight = position - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def aggregate(speedups: list[float]) -> dict:
    if not speedups:
        return {"cells": 0}
    return {
        "cells": len(speedups),
        "geometric_mean": math.exp(sum(math.log(s) for s in speedups) / len(speedups)),
        "median": statistics.median(speedups),
        "p25": quantile(speedups, 0.25),
        "p75": quantile(speedups, 0.75),
        "min": min(speedups),
        "max": max(speedups),
        "wins": sum(1 for s in speedups if s > 1.0),
    }


def pearson(xs: list[float], ys: list[float]) -> float:
    if len(xs) < 3:
        return float("nan")
    mean_x = statistics.fmean(xs)
    mean_y = statistics.fmean(ys)
    sxx = sum((x - mean_x) ** 2 for x in xs)
    syy = sum((y - mean_y) ** 2 for y in ys)
    if sxx == 0.0 or syy == 0.0:
        return float("nan")
    return sum((x - mean_x) * (y - mean_y) for x, y in zip(xs, ys)) / math.sqrt(sxx * syy)


def ranks(values: list[float]) -> list[float]:
    order = sorted(range(len(values)), key=lambda index: values[index])
    result = [0.0] * len(values)
    position = 0
    while position < len(order):
        end = position
        while end + 1 < len(order) and values[order[end + 1]] == values[order[position]]:
            end += 1
        rank = (position + end) / 2.0 + 1.0
        for index in range(position, end + 1):
            result[order[index]] = rank
        position = end + 1
    return result


def spearman(xs: list[float], ys: list[float]) -> float:
    return pearson(ranks(xs), ranks(ys))


def ols(xs: list[float], ys: list[float]) -> tuple[float, float, float]:
    """Returns (intercept, slope, r_squared)."""
    mean_x = statistics.fmean(xs)
    mean_y = statistics.fmean(ys)
    sxx = sum((x - mean_x) ** 2 for x in xs)
    if sxx == 0.0:
        return (mean_y, 0.0, 0.0)
    slope = sum((x - mean_x) * (y - mean_y) for x, y in zip(xs, ys)) / sxx
    intercept = mean_y - slope * mean_x
    total = sum((y - mean_y) ** 2 for y in ys)
    residual = sum((y - (intercept + slope * x)) ** 2 for x, y in zip(xs, ys))
    return (intercept, slope, 1.0 - residual / total if total > 0.0 else 0.0)


def fmt(value: float, digits: int = 3) -> str:
    if value is None or (isinstance(value, float) and math.isnan(value)):
        return "n/a"
    return f"{value:.{digits}f}"


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    directories = [Path(argument) for argument in sys.argv[1:]]
    rows = load(directories)
    used = [row for row in rows if usable(row)]
    excluded = [row for row in rows if not usable(row)]
    lines: list[str] = []
    out: dict = {"campaigns": [str(d) for d in directories]}

    def emit(text: str = "") -> None:
        lines.append(text)

    emit("# Root-queue generalization: QUEUE_DENSE vs frozen K2")
    emit()
    emit(f"cells read: {len(rows)}; usable (valid, AC power, timed): {len(used)}; "
         f"listed but excluded: {len(excluded)}")
    emit()

    # Per-cell table.
    emit("## Per-cell results (backend-total medians, ms)")
    emit()
    emit("| cell | family | n | scale | P0 | rho_q | bits/word | K2 | QUEUE | S | S steady | lane-skip |")
    emit("|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for row in used:
        emit(
            f"| {row['cell_id']} | {row['family']} | {row['n']} | {row['scale']} | "
            f"{int(number(row, 'p0')):,} | {fmt(number(row, 'rho_q'), 4)} | "
            f"{fmt(number(row, 'bits_per_queued_word'), 3)} | {fmt(number(row, 'k2_median_ms'), 2)} | "
            f"{fmt(number(row, 'queue_median_ms'), 2)} | {fmt(number(row, 'speedup_backend'), 3)} | "
            f"{fmt(number(row, 'speedup_steady'), 3)} | "
            f"{fmt(number(row, 'lane_skip_median_ms'), 2) if number(row, 'lane_skip_median_ms') > 0 else '-'} |"
        )
    emit()
    if excluded:
        emit("## Cells not entering aggregates")
        emit()
        for row in excluded:
            emit(f"- {row['cell_id']}: status={row['status']} valid={row['valid']} "
                 f"power_ok={row.get('power_ok')} {row.get('detail', '')}")
        emit()

    # Aggregates.
    groups = {
        "ALL": used,
        "AUTHENTIC_FULLTER": [r for r in used if r["family"] == "AUTHENTIC_FULLTER"],
        "AUTHENTIC_FULLTER (declared seeds only, anchor excluded)": [
            r for r in used if r["family"] == "AUTHENTIC_FULLTER" and r["anchor"] == "0"
        ],
        "CONTROLLED (both synthetic families)": [
            r for r in used if r["family"] != "AUTHENTIC_FULLTER"
        ],
        "SYNTHETIC_UNIFORM": [r for r in used if r["family"] == "SYNTHETIC_UNIFORM"],
        "RESIDUAL_PROFILE_EMULATION": [
            r for r in used if r["family"] == "RESIDUAL_PROFILE_EMULATION"
        ],
    }
    emit("## Aggregate speedup S = median(K2) / median(QUEUE_DENSE)")
    emit()
    emit("| group | cells | geo-mean | median | p25 | p75 | min | max | wins | steady geo-mean |")
    emit("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    out["aggregates"] = {}
    for name, members in groups.items():
        stats = aggregate([number(r, "speedup_backend") for r in members])
        steady = aggregate([number(r, "speedup_steady") for r in members
                            if number(r, "speedup_steady") > 0.0])
        out["aggregates"][name] = {"backend_total": stats, "steady": steady}
        if stats["cells"] == 0:
            emit(f"| {name} | 0 | - | - | - | - | - | - | - | - |")
            continue
        emit(
            f"| {name} | {stats['cells']} | {fmt(stats['geometric_mean'])} | "
            f"{fmt(stats['median'])} | {fmt(stats['p25'])} | {fmt(stats['p75'])} | "
            f"{fmt(stats['min'])} | {fmt(stats['max'])} | {stats['wins']}/{stats['cells']} | "
            f"{fmt(steady.get('geometric_mean', float('nan')))} |"
        )
    emit()

    # Per scale, per family.
    controlled = groups["CONTROLLED (both synthetic families)"]
    scales = sorted({number(r, "scale") for r in controlled})
    if scales:
        emit("## Controlled density sweep (geometric mean over seeds)")
        emit()
        emit("| family | scale | cells | mean rho_q | mean bits/word | geo-mean S | min S | max S | "
             "K2 median ms (geo) | QUEUE median ms (geo) |")
        emit("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
        out["sweep"] = []
        for family in ("SYNTHETIC_UNIFORM", "RESIDUAL_PROFILE_EMULATION"):
            for scale in scales:
                members = [r for r in controlled
                           if r["family"] == family and number(r, "scale") == scale]
                if not members:
                    continue
                stats = aggregate([number(r, "speedup_backend") for r in members])
                k2 = math.exp(statistics.fmean(math.log(number(r, "k2_median_ms")) for r in members))
                queue = math.exp(statistics.fmean(math.log(number(r, "queue_median_ms"))
                                                  for r in members))
                rho = statistics.fmean(number(r, "rho_q") for r in members)
                bits = statistics.fmean(number(r, "bits_per_queued_word") for r in members)
                out["sweep"].append({"family": family, "scale": scale, "rho_q": rho,
                                     "bits_per_word": bits, **stats,
                                     "k2_ms_geo": k2, "queue_ms_geo": queue})
                emit(f"| {family} | {scale:.2f} | {stats['cells']} | {fmt(rho, 4)} | {fmt(bits, 3)} | "
                     f"{fmt(stats['geometric_mean'])} | {fmt(stats['min'])} | {fmt(stats['max'])} | "
                     f"{fmt(k2, 2)} | {fmt(queue, 2)} |")
        emit()

    # Correlations and crossover.
    if len(used) >= 3:
        log_s = [math.log(number(r, "speedup_backend")) for r in used]
        predictors = {
            "ln rho_q": [math.log(max(number(r, "rho_q"), 1e-12)) for r in used],
            "bits per queued word": [number(r, "bits_per_queued_word") for r in used],
            "mean popcount": [number(r, "popcount_mean") for r in used],
            "ln P0": [math.log(max(number(r, "p0"), 1.0)) for r in used],
            "ln candidate bits": [math.log(max(number(r, "candidate_bits"), 1.0)) for r in used],
            "ln (candidate bits / P0)": [
                math.log(max(number(r, "candidate_bits"), 1.0) / max(number(r, "p0"), 1.0))
                for r in used
            ],
        }
        emit("## Does workload telemetry predict the speedup?")
        emit()
        emit("| predictor of ln S | Pearson r | Spearman rho |")
        emit("|---|---:|---:|")
        out["correlations"] = {}
        for name, values in predictors.items():
            r = pearson(values, log_s)
            rho = spearman(values, log_s)
            out["correlations"][name] = {"pearson": r, "spearman": rho}
            emit(f"| {name} | {fmt(r)} | {fmt(rho)} |")
        emit()
        intercept, slope, r2 = ols(predictors["ln rho_q"], log_s)
        crossover = math.exp(-intercept / slope) if slope != 0.0 else float("nan")
        out["crossover_fit"] = {"model": "ln S = a + b ln rho_q", "a": intercept, "b": slope,
                                "r_squared": r2, "rho_q_at_S_equal_1": crossover}
        emit(f"OLS fit over all usable cells: ln S = {fmt(intercept, 4)} + {fmt(slope, 4)} ln rho_q, "
             f"R^2 = {fmt(r2, 3)}; predicted break-even rho_q (S = 1) = {fmt(crossover, 4)}.")
        emit()

    # Pre-registered interpretation (spec section 19).
    authentic = groups["AUTHENTIC_FULLTER"]
    auth_stats = aggregate([number(r, "speedup_backend") for r in authentic])
    all_stats = aggregate([number(r, "speedup_backend") for r in used])
    density_r = out.get("correlations", {}).get("ln rho_q", {}).get("spearman", float("nan"))
    verdict = "INSUFFICIENT_DATA"
    note = ""
    if all_stats["cells"] > 0:
        authentic_win_share = (auth_stats["wins"] / auth_stats["cells"]) if auth_stats["cells"] else 0.0
        if (all_stats["geometric_mean"] >= 1.4 and authentic_win_share >= 0.8
                and all_stats["wins"] / all_stats["cells"] >= 0.6):
            verdict = "EXCELLENT"
            note = "queue wins across most authentic and controlled cells with S_G >= 1.4"
        elif not math.isnan(density_r) and abs(density_r) >= 0.7:
            verdict = "STILL_PUBLISHABLE_CROSSOVER"
            note = ("speedup varies but is explained by workload density "
                    f"(|Spearman(ln S, ln rho_q)| = {abs(density_r):.2f})")
        else:
            verdict = "BAD_SPECIALIZATION"
            note = "speedup is not explained by the measured workload geometry"
    out["verdict"] = verdict
    out["verdict_note"] = note
    emit(f"## Pre-registered interpretation: {verdict}")
    emit()
    emit(note)
    emit()
    emit("The automated rule above tests a MONOTONIC association (Spearman rank correlation")
    emit("and a linear fit in ln rho_q).  It is reported unchanged.  The section below was")
    emit("added after the data were seen and is labelled post-hoc accordingly.")
    emit()

    # ------------------------------------------------------------------
    # POST-HOC (not pre-registered): non-monotonic dependence on density.
    # ------------------------------------------------------------------
    post: dict = {}
    if controlled:
        values = [math.log(number(r, "speedup_backend")) for r in controlled]
        grand = statistics.fmean(values)
        total = sum((v - grand) ** 2 for v in values)
        within = 0.0
        for family in ("SYNTHETIC_UNIFORM", "RESIDUAL_PROFILE_EMULATION"):
            for scale in scales:
                members = [math.log(number(r, "speedup_backend")) for r in controlled
                           if r["family"] == family and number(r, "scale") == scale]
                if members:
                    centre = statistics.fmean(members)
                    within += sum((v - centre) ** 2 for v in members)
        post["controlled_ln_s_variance_explained_by_family_x_scale"] = (
            1.0 - within / total if total > 0.0 else float("nan"))
    if len(used) >= 4:
        xs = [math.log(max(number(r, "rho_q"), 1e-12)) for r in used]
        ys = [math.log(number(r, "speedup_backend")) for r in used]
        try:
            import numpy  # optional; the pre-registered part never needs it

            coefficients = numpy.polyfit(xs, ys, 2)
            predicted = numpy.polyval(coefficients, xs)
            mean_y = statistics.fmean(ys)
            ss_total = sum((y - mean_y) ** 2 for y in ys)
            ss_residual = sum((y - p) ** 2 for y, p in zip(ys, predicted))
            post["quadratic_fit_ln_s_on_ln_rho"] = {
                "c2": float(coefficients[0]), "c1": float(coefficients[1]),
                "c0": float(coefficients[2]),
                "r_squared": 1.0 - ss_residual / ss_total if ss_total > 0.0 else float("nan"),
                "peak_rho_q": math.exp(-coefficients[1] / (2.0 * coefficients[0]))
                if coefficients[0] != 0.0 else float("nan"),
            }
        except ImportError:
            post["quadratic_fit_ln_s_on_ln_rho"] = "numpy unavailable"
    crossings = []
    for family in ("SYNTHETIC_UNIFORM", "RESIDUAL_PROFILE_EMULATION"):
        points = sorted((e["scale"], e["geometric_mean"], e["rho_q"])
                        for e in out.get("sweep", []) if e["family"] == family)
        for (s0, g0, r0), (s1, g1, r1) in zip(points, points[1:]):
            l0, l1 = math.log(g0), math.log(g1)
            if (l0 < 0.0) != (l1 < 0.0):
                w = l0 / (l0 - l1)
                crossings.append({
                    "family": family,
                    "between_scales": [s0, s1],
                    "scale_at_S_equal_1": math.exp(math.log(s0) + w * (math.log(s1) - math.log(s0))),
                    "rho_q_at_S_equal_1": math.exp(math.log(r0) + w * (math.log(r1) - math.log(r0))),
                    "direction": "queue starts winning" if l1 > l0 else "K2 starts winning",
                })
    post["break_even_points"] = crossings
    out["post_hoc"] = post
    emit("## POST-HOC (not pre-registered): non-monotonic dependence on density")
    emit()
    share = post.get("controlled_ln_s_variance_explained_by_family_x_scale")
    if share is not None:
        emit(f"Share of controlled-cell variance in ln S explained by (family, scale): {fmt(share, 4)}")
    quad = post.get("quadratic_fit_ln_s_on_ln_rho")
    if isinstance(quad, dict):
        emit(f"Quadratic fit over all usable cells: ln S = {fmt(quad['c2'], 4)} (ln rho)^2 + "
             f"{fmt(quad['c1'], 4)} ln rho + {fmt(quad['c0'], 4)}, R^2 = {fmt(quad['r_squared'], 3)}, "
             f"peak at rho_q = {fmt(quad['peak_rho_q'], 4)}")
    emit()
    if crossings:
        emit("| family | between scales | scale at S = 1 | rho_q at S = 1 | direction |")
        emit("|---|---|---:|---:|---|")
        for c in crossings:
            emit(f"| {c['family']} | {c['between_scales'][0]:.2f}-{c['between_scales'][1]:.2f} | "
                 f"{fmt(c['scale_at_S_equal_1'], 3)} | {fmt(c['rho_q_at_S_equal_1'], 4)} | "
                 f"{c['direction']} |")
        emit()

    target = directories[0]
    (target / "analysis.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    (target / "analysis.json").write_text(json.dumps(out, indent=2) + "\n", encoding="utf-8")
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
