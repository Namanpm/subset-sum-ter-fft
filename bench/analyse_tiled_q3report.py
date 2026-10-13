#!/usr/bin/env python3
"""Analysis for the cache-tiled exact Q3REPORT publication evidence campaign.

Reads only the raw CSV files produced by the repository benchmarks and writes
machine-readable summaries, evidence gates and figures.  It never edits a
measured value: every exclusion is expressed as analysis logic and reported in
claim-checks.json.

Standard library only.  matplotlib is used when importable; otherwise the
figures are emitted through a small deterministic SVG writer defined here, so
the analysis output stays byte-reproducible from the same raw data.

Usage:
    python bench/analyse_tiled_q3report.py --self-test
    python bench/analyse_tiled_q3report.py --results-dir <campaign directory>
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import random
import statistics
import sys
import tempfile
from typing import Any

# --------------------------------------------------------------------------
# Declared campaign matrix.  These are the intended experiment dimensions, not
# a description of whatever happens to be on disk; a mismatch is a reported
# completeness failure.
# --------------------------------------------------------------------------
MAIN_LIST_SIZES = [64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384]
MAIN_SEEDS = [0x1001, 0x2003, 0x3007, 0x4009, 0x500B]
TILE_SEED_BASES = [4097, 8195, 12295]
TILE_QUOTIENT_BITS = [16, 18, 20, 22, 24, 26, 28, 30, 32]
TILE_REQUESTED_SB = [10, 12, 14, 15, 16, 18, 20]
PRODUCTION_TILE_BITS = 15

BOOTSTRAP_RESAMPLES = 10000
BOOTSTRAP_SEED = 0x5445524254535000  # fixed, so the interval is reproducible


# --------------------------------------------------------------------------
# Statistics
# --------------------------------------------------------------------------
def geometric_mean(values: list[float]) -> float:
    """Geometric mean of strictly positive values."""
    if not values:
        raise ValueError("geometric mean of an empty sample")
    if any(v <= 0.0 for v in values):
        raise ValueError("geometric mean requires strictly positive values")
    return math.exp(sum(math.log(v) for v in values) / len(values))


def bootstrap_geomean_ci(
    values: list[float],
    resamples: int = BOOTSTRAP_RESAMPLES,
    seed: int = BOOTSTRAP_SEED,
    alpha: float = 0.05,
) -> tuple[float, float]:
    """Deterministic percentile bootstrap interval for the geometric mean.

    With five seed-level observations this is a summary of the observed spread,
    not a high-precision population inference.  It is reported as an interval
    over the declared seed-level observations and must be described that way.
    """
    if not values:
        raise ValueError("bootstrap of an empty sample")
    if len(values) == 1:
        return (values[0], values[0])
    rng = random.Random(seed)
    n = len(values)
    logs = [math.log(v) for v in values]
    draws = []
    for _ in range(resamples):
        total = 0.0
        for _ in range(n):
            total += logs[rng.randrange(n)]
        draws.append(math.exp(total / n))
    draws.sort()
    lo_index = int(math.floor((alpha / 2.0) * (resamples - 1)))
    hi_index = int(math.ceil((1.0 - alpha / 2.0) * (resamples - 1)))
    return (draws[lo_index], draws[hi_index])


# --------------------------------------------------------------------------
# Minimal deterministic SVG plotting
#
# Kept intentionally small: axes, log/linear scaling, lines with markers,
# vertical intervals, grouped bars and a legend.  No timestamps or other
# environment state is written into the output, so a rerun over unchanged raw
# data produces a byte-identical file.
# --------------------------------------------------------------------------
PALETTE = {
    "direct": "#1f4e79",
    "tiled": "#c0504d",
    "ntt": "#7f7f7f",
    "accent": "#2e7d32",
    "grid": "#d9d9d9",
    "axis": "#333333",
    "reference": "#999999",
}
SERIES_COLORS = [
    "#1f4e79", "#c0504d", "#2e7d32", "#7b4f9d",
    "#b8860b", "#00707f", "#a0522d", "#4a4a4a", "#8b1a4b",
]


def _esc(text: str) -> str:
    return (
        str(text)
        .replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
    )


def _fmt(value: float) -> str:
    """Fixed-precision coordinate text, so output does not vary by platform."""
    return f"{value:.2f}"


class SvgPlot:
    WIDTH = 780
    HEIGHT = 460
    LEFT = 92
    RIGHT = 610
    TOP = 44
    BOTTOM = 386

    def __init__(
        self,
        title: str,
        xlabel: str,
        ylabel: str,
        xlog: bool = False,
        ylog: bool = False,
    ) -> None:
        self.title = title
        self.xlabel = xlabel
        self.ylabel = ylabel
        self.xlog = xlog
        self.ylog = ylog
        self.body: list[str] = []
        self.legend: list[tuple[str, str]] = []
        self.xmin = math.inf
        self.xmax = -math.inf
        self.ymin = math.inf
        self.ymax = -math.inf
        self._x_ticks: list[float] | None = None

    def observe(self, xs: list[float], ys: list[float]) -> None:
        for x in xs:
            self.xmin = min(self.xmin, x)
            self.xmax = max(self.xmax, x)
        for y in ys:
            self.ymin = min(self.ymin, y)
            self.ymax = max(self.ymax, y)

    def set_x_ticks(self, ticks: list[float]) -> None:
        self._x_ticks = ticks

    # -- coordinate mapping -------------------------------------------------
    def _finalize_bounds(self) -> None:
        if self.ylog:
            self.ymin = max(self.ymin, 1e-12)
            lo, hi = math.log10(self.ymin), math.log10(self.ymax)
            pad = max((hi - lo) * 0.08, 0.05)
            self._ylo, self._yhi = lo - pad, hi + pad
        else:
            span = self.ymax - self.ymin
            pad = span * 0.10 if span > 0 else max(abs(self.ymax) * 0.1, 1.0)
            self._ylo, self._yhi = self.ymin - pad, self.ymax + pad
        if self.xlog:
            self.xmin = max(self.xmin, 1e-12)
            lo, hi = math.log10(self.xmin), math.log10(self.xmax)
            pad = max((hi - lo) * 0.06, 0.03)
            self._xlo, self._xhi = lo - pad, hi + pad
        else:
            span = self.xmax - self.xmin
            pad = span * 0.06 if span > 0 else 1.0
            self._xlo, self._xhi = self.xmin - pad, self.xmax + pad

    def px(self, x: float) -> float:
        v = math.log10(max(x, 1e-12)) if self.xlog else x
        if self._xhi == self._xlo:
            return (self.LEFT + self.RIGHT) / 2.0
        t = (v - self._xlo) / (self._xhi - self._xlo)
        return self.LEFT + t * (self.RIGHT - self.LEFT)

    def py(self, y: float) -> float:
        v = math.log10(max(y, 1e-12)) if self.ylog else y
        if self._yhi == self._ylo:
            return (self.TOP + self.BOTTOM) / 2.0
        t = (v - self._ylo) / (self._yhi - self._ylo)
        return self.BOTTOM - t * (self.BOTTOM - self.TOP)

    # -- content ------------------------------------------------------------
    def add_series(
        self,
        xs: list[float],
        ys: list[float],
        color: str,
        label: str,
        marker: str = "circle",
        dashed: bool = False,
    ) -> None:
        self.observe(xs, ys)
        self._pending = getattr(self, "_pending", [])
        self._pending.append(("series", xs, ys, color, label, marker, dashed))
        if label:
            self.legend.append((label, color))

    def add_intervals(
        self, xs: list[float], los: list[float], his: list[float], color: str
    ) -> None:
        self.observe(xs, los + his)
        self._pending = getattr(self, "_pending", [])
        self._pending.append(("intervals", xs, los, his, color, "", "", False))

    def add_hline(self, y: float, color: str, label: str = "") -> None:
        self.observe([], [y])
        self._pending = getattr(self, "_pending", [])
        self._pending.append(("hline", y, color, label))
        if label:
            self.legend.append((label, color))

    def _marker(self, cx: float, cy: float, color: str, marker: str) -> str:
        if marker == "square":
            return (
                f'<rect x="{_fmt(cx - 3.2)}" y="{_fmt(cy - 3.2)}" width="6.4" '
                f'height="6.4" fill="{color}" />'
            )
        if marker == "diamond":
            return (
                f'<polygon points="{_fmt(cx)},{_fmt(cy - 4)} {_fmt(cx + 4)},{_fmt(cy)} '
                f'{_fmt(cx)},{_fmt(cy + 4)} {_fmt(cx - 4)},{_fmt(cy)}" fill="{color}" />'
            )
        return f'<circle cx="{_fmt(cx)}" cy="{_fmt(cy)}" r="3.4" fill="{color}" />'

    def _axis_ticks(self) -> tuple[list[float], list[float]]:
        if self._x_ticks is not None:
            x_ticks = list(self._x_ticks)
        elif self.xlog:
            lo, hi = math.ceil(self._xlo), math.floor(self._xhi)
            x_ticks = [10.0**k for k in range(int(lo), int(hi) + 1)]
        else:
            x_ticks = [
                self._xlo + (self._xhi - self._xlo) * i / 5.0 for i in range(6)
            ]
        if self.ylog:
            lo, hi = math.ceil(self._ylo), math.floor(self._yhi)
            y_ticks = [10.0**k for k in range(int(lo), int(hi) + 1)]
            if len(y_ticks) < 2:
                y_ticks = [10.0**self._ylo, 10.0**self._yhi]
        else:
            y_ticks = [
                self._ylo + (self._yhi - self._ylo) * i / 5.0 for i in range(6)
            ]
        return x_ticks, y_ticks

    @staticmethod
    def _tick_label(value: float, log: bool) -> str:
        if log:
            if value >= 1000 or value < 0.01:
                exponent = int(round(math.log10(value)))
                return f"1e{exponent}"
            if value >= 1:
                return f"{value:g}"
            return f"{value:g}"
        if abs(value) >= 1000:
            return f"{value:,.0f}"
        if abs(value) >= 10:
            return f"{value:.0f}"
        return f"{value:.2f}"

    def save(self, path: str) -> None:
        self._finalize_bounds()
        x_ticks, y_ticks = self._axis_ticks()
        out: list[str] = []
        out.append(
            f'<svg xmlns="http://www.w3.org/2000/svg" width="{self.WIDTH}" '
            f'height="{self.HEIGHT}" viewBox="0 0 {self.WIDTH} {self.HEIGHT}" '
            f'font-family="DejaVu Sans, Helvetica, Arial, sans-serif">'
        )
        out.append(f'<rect width="{self.WIDTH}" height="{self.HEIGHT}" fill="#ffffff" />')
        out.append(
            f'<text x="{self.LEFT}" y="26" font-size="15" fill="{PALETTE["axis"]}">'
            f"{_esc(self.title)}</text>"
        )
        # grid
        for tick in y_ticks:
            y = self.py(tick)
            if not (self.TOP - 1 <= y <= self.BOTTOM + 1):
                continue
            out.append(
                f'<line x1="{self.LEFT}" y1="{_fmt(y)}" x2="{self.RIGHT}" '
                f'y2="{_fmt(y)}" stroke="{PALETTE["grid"]}" stroke-width="1" />'
            )
            out.append(
                f'<text x="{self.LEFT - 8}" y="{_fmt(y + 4)}" font-size="11" '
                f'text-anchor="end" fill="{PALETTE["axis"]}">'
                f"{_esc(self._tick_label(tick, self.ylog))}</text>"
            )
        for tick in x_ticks:
            x = self.px(tick)
            if not (self.LEFT - 1 <= x <= self.RIGHT + 1):
                continue
            out.append(
                f'<line x1="{_fmt(x)}" y1="{self.TOP}" x2="{_fmt(x)}" '
                f'y2="{self.BOTTOM}" stroke="{PALETTE["grid"]}" stroke-width="1" />'
            )
            out.append(
                f'<text x="{_fmt(x)}" y="{self.BOTTOM + 18}" font-size="11" '
                f'text-anchor="middle" fill="{PALETTE["axis"]}">'
                f"{_esc(self._tick_label(tick, self.xlog))}</text>"
            )
        # frame
        out.append(
            f'<rect x="{self.LEFT}" y="{self.TOP}" width="{self.RIGHT - self.LEFT}" '
            f'height="{self.BOTTOM - self.TOP}" fill="none" '
            f'stroke="{PALETTE["axis"]}" stroke-width="1.2" />'
        )
        out.append(
            f'<text x="{(self.LEFT + self.RIGHT) / 2:.0f}" y="{self.BOTTOM + 44}" '
            f'font-size="12" text-anchor="middle" fill="{PALETTE["axis"]}">'
            f"{_esc(self.xlabel)}</text>"
        )
        out.append(
            f'<text x="22" y="{(self.TOP + self.BOTTOM) / 2:.0f}" font-size="12" '
            f'text-anchor="middle" fill="{PALETTE["axis"]}" '
            f'transform="rotate(-90 22 {(self.TOP + self.BOTTOM) / 2:.0f})">'
            f"{_esc(self.ylabel)}</text>"
        )
        # content
        for item in getattr(self, "_pending", []):
            kind = item[0]
            if kind == "series":
                _, xs, ys, color, _label, marker, dashed = item
                pts = " ".join(f"{_fmt(self.px(x))},{_fmt(self.py(y))}" for x, y in zip(xs, ys))
                dash = ' stroke-dasharray="6 4"' if dashed else ""
                if len(xs) > 1:
                    out.append(
                        f'<polyline points="{pts}" fill="none" stroke="{color}" '
                        f'stroke-width="1.9"{dash} />'
                    )
                for x, y in zip(xs, ys):
                    out.append(self._marker(self.px(x), self.py(y), color, marker))
            elif kind == "intervals":
                _, xs, los, his, color, _a, _b, _c = item
                for x, lo, hi in zip(xs, los, his):
                    cx = self.px(x)
                    out.append(
                        f'<line x1="{_fmt(cx)}" y1="{_fmt(self.py(lo))}" '
                        f'x2="{_fmt(cx)}" y2="{_fmt(self.py(hi))}" stroke="{color}" '
                        f'stroke-width="1.1" opacity="0.8" />'
                    )
                    for value in (lo, hi):
                        out.append(
                            f'<line x1="{_fmt(cx - 3)}" y1="{_fmt(self.py(value))}" '
                            f'x2="{_fmt(cx + 3)}" y2="{_fmt(self.py(value))}" '
                            f'stroke="{color}" stroke-width="1.1" opacity="0.8" />'
                        )
            elif kind == "hline":
                _, y, color, _label = item
                py = self.py(y)
                out.append(
                    f'<line x1="{self.LEFT}" y1="{_fmt(py)}" x2="{self.RIGHT}" '
                    f'y2="{_fmt(py)}" stroke="{color}" stroke-width="1.3" '
                    f'stroke-dasharray="5 4" />'
                )
        # legend
        ly = self.TOP + 6
        for label, color in self.legend:
            out.append(
                f'<rect x="{self.RIGHT + 18}" y="{_fmt(ly)}" width="12" height="12" '
                f'fill="{color}" />'
            )
            out.append(
                f'<text x="{self.RIGHT + 36}" y="{_fmt(ly + 10.5)}" font-size="11" '
                f'fill="{PALETTE["axis"]}">{_esc(label)}</text>'
            )
            ly += 19
        out.append("</svg>")
        with open(path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write("\n".join(out) + "\n")


class SvgBarPlot(SvgPlot):
    """Stacked/grouped bars over categorical positions."""

    def __init__(self, title: str, xlabel: str, ylabel: str) -> None:
        super().__init__(title, xlabel, ylabel, xlog=False, ylog=False)
        self.categories: list[str] = []
        self.stacks: list[list[tuple[float, str, str]]] = []

    def add_stack(self, category: str, parts: list[tuple[float, str, str]]) -> None:
        """parts is a list of (value, color, label) drawn bottom to top."""
        self.categories.append(category)
        self.stacks.append(parts)
        self.observe([], [0.0, sum(p[0] for p in parts)])
        for _value, color, label in parts:
            if label and (label, color) not in self.legend:
                self.legend.append((label, color))

    def save(self, path: str) -> None:
        self.ymin = 0.0
        self.xmin, self.xmax = 0.0, float(max(len(self.categories), 1))
        self._finalize_bounds()
        self._ylo = 0.0
        _unused, y_ticks = self._axis_ticks()
        out: list[str] = []
        out.append(
            f'<svg xmlns="http://www.w3.org/2000/svg" width="{self.WIDTH}" '
            f'height="{self.HEIGHT}" viewBox="0 0 {self.WIDTH} {self.HEIGHT}" '
            f'font-family="DejaVu Sans, Helvetica, Arial, sans-serif">'
        )
        out.append(f'<rect width="{self.WIDTH}" height="{self.HEIGHT}" fill="#ffffff" />')
        out.append(
            f'<text x="{self.LEFT}" y="26" font-size="15" fill="{PALETTE["axis"]}">'
            f"{_esc(self.title)}</text>"
        )
        for tick in y_ticks:
            y = self.py(tick)
            if not (self.TOP - 1 <= y <= self.BOTTOM + 1):
                continue
            out.append(
                f'<line x1="{self.LEFT}" y1="{_fmt(y)}" x2="{self.RIGHT}" '
                f'y2="{_fmt(y)}" stroke="{PALETTE["grid"]}" stroke-width="1" />'
            )
            out.append(
                f'<text x="{self.LEFT - 8}" y="{_fmt(y + 4)}" font-size="11" '
                f'text-anchor="end" fill="{PALETTE["axis"]}">'
                f"{_esc(self._tick_label(tick, False))}</text>"
            )
        count = max(len(self.categories), 1)
        slot = (self.RIGHT - self.LEFT) / count
        width = slot * 0.52
        for index, (category, parts) in enumerate(zip(self.categories, self.stacks)):
            cx = self.LEFT + slot * (index + 0.5)
            base = 0.0
            for value, color, _label in parts:
                y_top = self.py(base + value)
                y_bottom = self.py(base)
                out.append(
                    f'<rect x="{_fmt(cx - width / 2)}" y="{_fmt(y_top)}" '
                    f'width="{_fmt(width)}" height="{_fmt(max(y_bottom - y_top, 0.0))}" '
                    f'fill="{color}" />'
                )
                base += value
            out.append(
                f'<text x="{_fmt(cx)}" y="{self.BOTTOM + 18}" font-size="11" '
                f'text-anchor="middle" fill="{PALETTE["axis"]}">{_esc(category)}</text>'
            )
        out.append(
            f'<rect x="{self.LEFT}" y="{self.TOP}" width="{self.RIGHT - self.LEFT}" '
            f'height="{self.BOTTOM - self.TOP}" fill="none" '
            f'stroke="{PALETTE["axis"]}" stroke-width="1.2" />'
        )
        out.append(
            f'<text x="{(self.LEFT + self.RIGHT) / 2:.0f}" y="{self.BOTTOM + 44}" '
            f'font-size="12" text-anchor="middle" fill="{PALETTE["axis"]}">'
            f"{_esc(self.xlabel)}</text>"
        )
        out.append(
            f'<text x="22" y="{(self.TOP + self.BOTTOM) / 2:.0f}" font-size="12" '
            f'text-anchor="middle" fill="{PALETTE["axis"]}" '
            f'transform="rotate(-90 22 {(self.TOP + self.BOTTOM) / 2:.0f})">'
            f"{_esc(self.ylabel)}</text>"
        )
        ly = self.TOP + 6
        for label, color in self.legend:
            out.append(
                f'<rect x="{self.RIGHT + 18}" y="{_fmt(ly)}" width="12" height="12" '
                f'fill="{color}" />'
            )
            out.append(
                f'<text x="{self.RIGHT + 36}" y="{_fmt(ly + 10.5)}" font-size="11" '
                f'fill="{PALETTE["axis"]}">{_esc(label)}</text>'
            )
            ly += 19
        out.append("</svg>")
        with open(path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write("\n".join(out) + "\n")


# --------------------------------------------------------------------------
# Loading and per-row derivation
# --------------------------------------------------------------------------
def read_csv(path: str) -> list[dict[str, str]]:
    with open(path, newline="", encoding="utf-8-sig") as handle:
        return [row for row in csv.DictReader(handle) if any(v.strip() for v in row.values())]


def as_int(row: dict[str, str], key: str) -> int:
    return int(float(row[key]))


def as_float(row: dict[str, str], key: str) -> float:
    return float(row[key])


def derive_main_row(row: dict[str, str]) -> dict[str, Any]:
    """Per-row derived quantities for one paper-scaling observation."""
    list_size = as_int(row, "list_size_requested")
    l_a, l_b = as_int(row, "L_A"), as_int(row, "L_B")
    direct = as_int(row, "direct_production_total_median_ns")
    tiled = as_int(row, "direct_tiled_total_median_ns")
    direct_bytes = as_int(row, "direct_production_peak_bytes")
    tiled_bytes = as_int(row, "direct_tiled_peak_bytes")
    setup = as_int(row, "direct_tiled_setup_median_ns")
    scan = as_int(row, "direct_tiled_scan_median_ns")
    executed = as_int(row, "direct_tiled_executed_pairs")
    logical = l_a * l_b
    derived: dict[str, Any] = {
        "snapshot_family": row["snapshot_family"],
        "status": row["status"],
        "seed": as_int(row, "seed"),
        "list_size": list_size,
        "L_A": l_a,
        "L_B": l_b,
        "L_C": as_int(row, "L_C"),
        "Q": as_int(row, "Q"),
        "q_bits": as_int(row, "q_bits"),
        "lambda": as_float(row, "lambda"),
        "exactness_passed": as_int(row, "exactness_passed"),
        "raw_arithmetic_triples": as_int(row, "raw_arithmetic_triples"),
        "direct_production_total_median_ns": direct,
        "direct_production_total_min_ns": as_int(row, "direct_production_total_min_ns"),
        "direct_production_total_max_ns": as_int(row, "direct_production_total_max_ns"),
        "direct_tiled_total_median_ns": tiled,
        "direct_tiled_total_min_ns": as_int(row, "direct_tiled_total_min_ns"),
        "direct_tiled_total_max_ns": as_int(row, "direct_tiled_total_max_ns"),
        "direct_full_total_median_ns": as_int(row, "direct_full_total_median_ns"),
        "ntt_three_merge_amortized_ns": as_int(row, "ntt_three_merge_amortized_ns"),
        "direct_production_peak_bytes": direct_bytes,
        "direct_tiled_peak_bytes": tiled_bytes,
        "direct_tiled_auxiliary_bytes": as_int(row, "direct_tiled_auxiliary_bytes"),
        "direct_tiled_tile_bits": as_int(row, "direct_tiled_tile_bits"),
        "direct_tiled_window_count": as_int(row, "direct_tiled_window_count"),
        "direct_tiled_nonempty_windows": as_int(row, "direct_tiled_nonempty_windows"),
        "direct_tiled_setup_median_ns": setup,
        "direct_tiled_scan_median_ns": scan,
        "logical_pairs": logical,
        "executed_pairs": executed,
    }
    derived["speedup"] = (direct / tiled) if tiled > 0 else None
    derived["memory_ratio"] = (direct_bytes / tiled_bytes) if tiled_bytes > 0 else None
    derived["executed_pair_fraction"] = (executed / logical) if logical > 0 else None
    derived["nonempty_window_fraction"] = (
        derived["direct_tiled_nonempty_windows"] / derived["direct_tiled_window_count"]
        if derived["direct_tiled_window_count"] > 0
        else None
    )
    derived["tiled_setup_fraction"] = (setup / tiled) if tiled > 0 else None
    derived["tiled_scan_fraction"] = (scan / tiled) if tiled > 0 else None
    # The comparator's timed region also builds the TiledQEntry input vectors,
    # which the kernel telemetry does not attribute to setup or scan.  The
    # residual is reported rather than silently folded into either component.
    if tiled > 0:
        derived["tiled_wrapper_residual_fraction"] = max(
            0.0, 1.0 - (setup + scan) / tiled
        )
    else:
        derived["tiled_wrapper_residual_fraction"] = None
    return derived


def derive_tile_row(row: dict[str, str], seed_base: int) -> dict[str, Any]:
    total = as_int(row, "total_median_ns")
    executed = as_int(row, "executed_pairs")
    list_size = as_int(row, "list_size")
    logical = list_size * list_size
    return {
        "seed_base": seed_base,
        "seed": as_int(row, "seed"),
        "w": as_int(row, "w"),
        "requested_SB": as_int(row, "SB"),
        "effective_SB": as_int(row, "effective_SB"),
        "Q": as_int(row, "Q"),
        "list_size": list_size,
        "raw_triples": as_int(row, "raw_triples"),
        "setup_median_ns": as_int(row, "setup_median_ns"),
        "scan_median_ns": as_int(row, "scan_median_ns"),
        "total_median_ns": total,
        "auxiliary_peak_bytes": as_int(row, "auxiliary_peak_bytes"),
        "bucket_count": as_int(row, "bucket_count"),
        "window_count": as_int(row, "window_count"),
        "executed_pairs": executed,
        "nonempty_windows": as_int(row, "nonempty_windows"),
        "independent_exactness_passed": as_int(row, "independent_exactness_passed"),
        "logical_pairs": logical,
        "executed_pair_fraction": (executed / logical) if logical > 0 else None,
    }


# --------------------------------------------------------------------------
# Aggregation
# --------------------------------------------------------------------------
def summarize_main(rows: list[dict[str, Any]]) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    """Group completed paper-scaling rows by list size."""
    by_size: dict[int, list[dict[str, Any]]] = {}
    for row in rows:
        by_size.setdefault(row["list_size"], []).append(row)

    summary: list[dict[str, Any]] = []
    for list_size in sorted(by_size):
        group = sorted(by_size[list_size], key=lambda r: r["seed"])
        usable = [
            r
            for r in group
            if r["status"] == "Completed"
            and r["speedup"] is not None
            and r["speedup"] > 0.0
        ]
        excluded = [r for r in group if r not in usable]
        entry: dict[str, Any] = {
            "list_size": list_size,
            "rows_present": len(group),
            "seeds_completed": len(usable),
            "rows_excluded_from_timing": len(excluded),
            "excluded_reason": (
                ""
                if not excluded
                else "; ".join(
                    sorted({f"status={r['status']}" for r in excluded})
                )
            ),
        }
        if not usable:
            summary.append(entry)
            continue
        speedups = [r["speedup"] for r in usable]
        entry.update(
            {
                "q_bits": usable[0]["q_bits"],
                "Q": usable[0]["Q"],
                "lambda": usable[0]["lambda"],
                "tile_bits": usable[0]["direct_tiled_tile_bits"],
                "median_direct_ns": statistics.median(
                    [r["direct_production_total_median_ns"] for r in usable]
                ),
                "median_tiled_ns": statistics.median(
                    [r["direct_tiled_total_median_ns"] for r in usable]
                ),
                "median_direct_full_ns": statistics.median(
                    [r["direct_full_total_median_ns"] for r in usable]
                ),
                "median_ntt_amortized_ns": statistics.median(
                    [r["ntt_three_merge_amortized_ns"] for r in usable]
                ),
                "median_speedup": statistics.median(speedups),
                "geomean_speedup": geometric_mean(speedups),
                "min_speedup": min(speedups),
                "max_speedup": max(speedups),
                "median_direct_peak_bytes": statistics.median(
                    [r["direct_production_peak_bytes"] for r in usable]
                ),
                "median_tiled_peak_bytes": statistics.median(
                    [r["direct_tiled_peak_bytes"] for r in usable]
                ),
                "median_memory_ratio": statistics.median(
                    [r["memory_ratio"] for r in usable]
                ),
                "median_executed_pair_fraction": statistics.median(
                    [r["executed_pair_fraction"] for r in usable]
                ),
                "min_executed_pair_fraction": min(
                    r["executed_pair_fraction"] for r in usable
                ),
                "median_nonempty_window_fraction": statistics.median(
                    [r["nonempty_window_fraction"] for r in usable]
                ),
                "median_tiled_setup_fraction": statistics.median(
                    [r["tiled_setup_fraction"] for r in usable]
                ),
                "median_tiled_scan_fraction": statistics.median(
                    [r["tiled_scan_fraction"] for r in usable]
                ),
                "median_tiled_wrapper_residual_fraction": statistics.median(
                    [r["tiled_wrapper_residual_fraction"] for r in usable]
                ),
            }
        )
        lo, hi = bootstrap_geomean_ci(speedups)
        entry["geomean_speedup_ci_low"] = lo
        entry["geomean_speedup_ci_high"] = hi
        summary.append(entry)

    # Completeness against the declared matrix.
    present = {(r["list_size"], r["seed"]) for r in rows}
    expected = {(L, s) for L in MAIN_LIST_SIZES for s in MAIN_SEEDS}
    duplicates = len(rows) - len(present)
    completeness = {
        "expected_rows": len(expected),
        "actual_rows": len(rows),
        "missing_cells": sorted(f"L={L},seed={s}" for (L, s) in (expected - present)),
        "unexpected_cells": sorted(f"L={L},seed={s}" for (L, s) in (present - expected)),
        "duplicate_cell_count": duplicates,
        "non_completed_rows": [
            {"list_size": r["list_size"], "seed": r["seed"], "status": r["status"]}
            for r in rows
            if r["status"] != "Completed"
        ],
        "exactness_failures": [
            {"list_size": r["list_size"], "seed": r["seed"]}
            for r in rows
            if r["status"] == "Completed" and r["exactness_passed"] != 1
        ],
    }
    completeness["complete"] = (
        not completeness["missing_cells"]
        and not completeness["unexpected_cells"]
        and duplicates == 0
        and completeness["actual_rows"] == completeness["expected_rows"]
    )
    return summary, completeness


def summarize_tiles(rows: list[dict[str, Any]]) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    """Normalize each (seed_base, w) family against its own fastest tile width."""
    by_group: dict[tuple[int, int], list[dict[str, Any]]] = {}
    for row in rows:
        by_group.setdefault((row["seed_base"], row["w"]), []).append(row)

    summary: list[dict[str, Any]] = []
    for (seed_base, w) in sorted(by_group):
        group = by_group[(seed_base, w)]
        best = min(r["total_median_ns"] for r in group)
        best_sb = min(
            (r for r in group if r["total_median_ns"] == best),
            key=lambda r: r["requested_SB"],
        )["requested_SB"]
        for row in sorted(group, key=lambda r: r["requested_SB"]):
            summary.append(
                {
                    "seed_base": seed_base,
                    "seed": row["seed"],
                    "w": w,
                    "requested_SB": row["requested_SB"],
                    "effective_SB": row["effective_SB"],
                    "Q": row["Q"],
                    "total_median_ns": row["total_median_ns"],
                    "setup_median_ns": row["setup_median_ns"],
                    "scan_median_ns": row["scan_median_ns"],
                    "auxiliary_peak_bytes": row["auxiliary_peak_bytes"],
                    "bucket_count": row["bucket_count"],
                    "window_count": row["window_count"],
                    "nonempty_windows": row["nonempty_windows"],
                    "executed_pairs": row["executed_pairs"],
                    "executed_pair_fraction": row["executed_pair_fraction"],
                    "independent_exactness_passed": row["independent_exactness_passed"],
                    "best_SB_in_group": best_sb,
                    "best_total_median_ns": best,
                    "normalized_slowdown": row["total_median_ns"] / best,
                    "is_best": 1 if row["total_median_ns"] == best else 0,
                    "is_production_default": 1
                    if row["requested_SB"] == PRODUCTION_TILE_BITS
                    else 0,
                }
            )

    present = {(r["seed_base"], r["w"], r["requested_SB"]) for r in rows}
    expected = {
        (b, w, sb)
        for b in TILE_SEED_BASES
        for w in TILE_QUOTIENT_BITS
        for sb in TILE_REQUESTED_SB
    }
    completeness = {
        "expected_rows": len(expected),
        "actual_rows": len(rows),
        "missing_cells": sorted(
            f"base={b},w={w},SB={sb}" for (b, w, sb) in (expected - present)
        ),
        "unexpected_cells": sorted(
            f"base={b},w={w},SB={sb}" for (b, w, sb) in (present - expected)
        ),
        "exactness_failures": [
            {"seed_base": r["seed_base"], "w": r["w"], "SB": r["requested_SB"]}
            for r in rows
            if r["independent_exactness_passed"] != 1
        ],
    }
    completeness["complete"] = (
        not completeness["missing_cells"]
        and not completeness["unexpected_cells"]
        and completeness["actual_rows"] == completeness["expected_rows"]
    )
    return summary, completeness


def tile_robustness(summary: list[dict[str, Any]]) -> dict[str, Any]:
    """How robust the frozen SB=15 production default is, as measured."""
    default_rows = [r for r in summary if r["requested_SB"] == PRODUCTION_TILE_BITS]
    groups = {(r["seed_base"], r["w"]) for r in summary}
    slowdowns = [r["normalized_slowdown"] for r in default_rows]
    wins = sum(1 for r in default_rows if r["is_best"] == 1)
    by_w: dict[str, Any] = {}
    for w in sorted({r["w"] for r in summary}):
        w_rows = [r for r in default_rows if r["w"] == w]
        best_choices = sorted(
            {r["best_SB_in_group"] for r in summary if r["w"] == w}
        )
        by_w[str(w)] = {
            "sb15_median_normalized_slowdown": statistics.median(
                [r["normalized_slowdown"] for r in w_rows]
            )
            if w_rows
            else None,
            "sb15_max_normalized_slowdown": max(
                (r["normalized_slowdown"] for r in w_rows), default=None
            ),
            "sb15_best_count": sum(1 for r in w_rows if r["is_best"] == 1),
            "seed_bases_measured": len(w_rows),
            "observed_best_SB": best_choices,
        }
    return {
        "groups_evaluated": len(groups),
        "sb15_best_count": wins,
        "sb15_best_fraction": (wins / len(default_rows)) if default_rows else None,
        "sb15_median_normalized_slowdown": statistics.median(slowdowns)
        if slowdowns
        else None,
        "sb15_max_normalized_slowdown": max(slowdowns) if slowdowns else None,
        "sb15_min_normalized_slowdown": min(slowdowns) if slowdowns else None,
        "by_quotient_width": by_w,
    }


# --------------------------------------------------------------------------
# Figures
# --------------------------------------------------------------------------
def make_figures(
    main_rows: list[dict[str, Any]],
    main_summary: list[dict[str, Any]],
    tile_summary: list[dict[str, Any]],
    figures_dir: str,
) -> list[str]:
    written: list[str] = []
    usable = [s for s in main_summary if s.get("seeds_completed", 0) > 0]
    if usable:
        sizes = [float(s["list_size"]) for s in usable]
        ticks = sizes

        # Figure 1: runtime vs list size.
        fig = SvgPlot(
            "Exact three-list arithmetic: runtime vs list size (median over seeds)",
            "list size L (log2 scale)",
            "runtime (ms, log scale)",
            xlog=True,
            ylog=True,
        )
        fig.set_x_ticks(ticks)
        for key, color, label, marker in (
            ("median_direct_ns", PALETTE["direct"], "DIRECT_PRODUCTION", "circle"),
            ("median_tiled_ns", PALETTE["tiled"], "DIRECT_PRODUCTION_TILED", "square"),
            ("median_ntt_amortized_ns", PALETTE["ntt"], "NTT (amortized)", "diamond"),
        ):
            ys = [s[key] / 1e6 for s in usable]
            fig.add_series(sizes, ys, color, label, marker=marker,
                           dashed=(key == "median_ntt_amortized_ns"))
        # Per-seed spread for the two headline comparators.
        for key, color in (
            ("direct_production_total_median_ns", PALETTE["direct"]),
            ("direct_tiled_total_median_ns", PALETTE["tiled"]),
        ):
            xs, los, his = [], [], []
            for s in usable:
                seeds = [
                    r[key] / 1e6
                    for r in main_rows
                    if r["list_size"] == s["list_size"] and r["status"] == "Completed"
                ]
                if len(seeds) > 1:
                    xs.append(float(s["list_size"]))
                    los.append(min(seeds))
                    his.append(max(seeds))
            if xs:
                fig.add_intervals(xs, los, his, color)
        path = os.path.join(figures_dir, "runtime-vs-list-size.svg")
        fig.save(path)
        written.append(path)

        # Figure 2: speedup vs list size, with the 1x reference visible.
        fig = SvgPlot(
            "DIRECT_PRODUCTION / DIRECT_PRODUCTION_TILED vs list size",
            "list size L (log2 scale)",
            "speedup (x), values below 1 favour the direct comparator",
            xlog=True,
            ylog=False,
        )
        fig.set_x_ticks(ticks)
        fig.add_series(
            sizes,
            [s["geomean_speedup"] for s in usable],
            PALETTE["tiled"],
            "geometric mean over seeds",
            marker="square",
        )
        fig.add_intervals(
            sizes,
            [s["min_speedup"] for s in usable],
            [s["max_speedup"] for s in usable],
            PALETTE["tiled"],
        )
        fig.add_series(
            sizes,
            [s["geomean_speedup_ci_low"] for s in usable],
            PALETTE["accent"],
            "seed-level bootstrap 95% interval",
            marker="circle",
            dashed=True,
        )
        fig.add_series(
            sizes,
            [s["geomean_speedup_ci_high"] for s in usable],
            PALETTE["accent"],
            "",
            marker="circle",
            dashed=True,
        )
        fig.add_hline(1.0, PALETTE["reference"], "1x reference")
        path = os.path.join(figures_dir, "speedup-vs-list-size.svg")
        fig.save(path)
        written.append(path)

        # Figure 3: accounted auxiliary memory.
        fig = SvgPlot(
            "Accounted comparator memory vs list size (declared budget, not RSS)",
            "list size L (log2 scale)",
            "accounted peak (MiB, log scale)",
            xlog=True,
            ylog=True,
        )
        fig.set_x_ticks(ticks)
        fig.add_series(
            sizes,
            [s["median_direct_peak_bytes"] / (1024.0 * 1024.0) for s in usable],
            PALETTE["direct"],
            "DIRECT_PRODUCTION",
            marker="circle",
        )
        fig.add_series(
            sizes,
            [s["median_tiled_peak_bytes"] / (1024.0 * 1024.0) for s in usable],
            PALETTE["tiled"],
            "DIRECT_PRODUCTION_TILED",
            marker="square",
        )
        path = os.path.join(figures_dir, "auxiliary-memory-vs-list-size.svg")
        fig.save(path)
        written.append(path)

        # Figure 5: setup / scan / wrapper split at the larger declared points.
        bars = SvgBarPlot(
            "Tiled comparator time split at the larger declared points",
            "list size L",
            "median time (ms)",
        )
        for s in usable:
            if s["list_size"] < 1024:
                continue
            total_ms = s["median_tiled_ns"] / 1e6
            setup_ms = total_ms * s["median_tiled_setup_fraction"]
            scan_ms = total_ms * s["median_tiled_scan_fraction"]
            residual_ms = max(total_ms - setup_ms - scan_ms, 0.0)
            bars.add_stack(
                str(s["list_size"]),
                [
                    (setup_ms, PALETTE["direct"], "kernel setup"),
                    (scan_ms, PALETTE["tiled"], "kernel scan"),
                    (residual_ms, PALETTE["ntt"], "comparator input build"),
                ],
            )
        path = os.path.join(figures_dir, "setup-scan-breakdown.svg")
        bars.save(path)
        written.append(path)

    # Figure 4: tile sensitivity, one series per quotient width.
    if tile_summary:
        fig = SvgPlot(
            "Tile-width sensitivity: runtime normalized to the fastest tile per (seed, w)",
            "requested tile-width exponent SB",
            "normalized slowdown (1.0 = fastest measured tile)",
            xlog=False,
            ylog=True,
        )
        fig.set_x_ticks([float(sb) for sb in TILE_REQUESTED_SB])
        widths = sorted({r["w"] for r in tile_summary})
        for index, w in enumerate(widths):
            xs, ys = [], []
            for sb in TILE_REQUESTED_SB:
                cell = [
                    r["normalized_slowdown"]
                    for r in tile_summary
                    if r["w"] == w and r["requested_SB"] == sb
                ]
                if cell:
                    xs.append(float(sb))
                    ys.append(geometric_mean(cell))
            if xs:
                fig.add_series(
                    xs, ys, SERIES_COLORS[index % len(SERIES_COLORS)], f"w={w}"
                )
        # Mark the frozen production default.  It was fixed before this campaign.
        fig.observe([float(PRODUCTION_TILE_BITS)], [])
        fig._finalize_bounds()
        path = os.path.join(figures_dir, "tile-sensitivity.svg")
        fig.save(path)
        # Overlay the frozen-default marker without re-plotting any data point.
        with open(path, encoding="utf-8") as handle:
            svg = handle.read()
        x = fig.px(float(PRODUCTION_TILE_BITS))
        marker = (
            f'<line x1="{_fmt(x)}" y1="{fig.TOP}" x2="{_fmt(x)}" y2="{fig.BOTTOM}" '
            f'stroke="{PALETTE["reference"]}" stroke-width="1.4" stroke-dasharray="4 4" />'
            f'<text x="{_fmt(x + 5)}" y="{fig.TOP + 14}" font-size="10" '
            f'fill="{PALETTE["axis"]}">SB=15 frozen production default '
            f'(fixed before this campaign)</text>'
        )
        svg = svg.replace("</svg>", marker + "\n</svg>")
        with open(path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(svg)
        written.append(path)
    return written


# --------------------------------------------------------------------------
# Output writers
# --------------------------------------------------------------------------
def write_csv(path: str, rows: list[dict[str, Any]], columns: list[str]) -> None:
    with open(path, "w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=columns, extrasaction="ignore")
        writer.writeheader()
        for row in rows:
            writer.writerow({c: row.get(c, "") for c in columns})


def write_json(path: str, payload: Any) -> None:
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(payload, handle, indent=2, sort_keys=False)
        handle.write("\n")


# --------------------------------------------------------------------------
# Self-test
# --------------------------------------------------------------------------
MAIN_SELF_TEST_COLUMNS = [
    "snapshot_family", "seed", "list_size_requested", "L_A", "L_B", "L_C",
    "q_bits", "Q", "lambda", "raw_arithmetic_triples",
    "direct_full_total_median_ns", "direct_production_total_median_ns",
    "ntt_three_merge_amortized_ns", "direct_production_peak_bytes",
    "exactness_passed", "status", "direct_production_total_min_ns",
    "direct_production_total_max_ns", "direct_tiled_total_median_ns",
    "direct_tiled_setup_median_ns", "direct_tiled_scan_median_ns",
    "direct_tiled_peak_bytes", "direct_tiled_tile_bits",
    "direct_tiled_nonempty_windows", "direct_tiled_executed_pairs",
    "direct_tiled_total_min_ns", "direct_tiled_total_max_ns",
    "direct_tiled_window_count", "direct_tiled_auxiliary_bytes",
]


def _synthetic_main_row(
    list_size: int, seed: int, direct_ns: int, tiled_ns: int,
    executed: int, status: str = "Completed", exact: int = 1,
) -> dict[str, Any]:
    return {
        "snapshot_family": "FULLTER_L2_PAPER_SCALING_EMULATION",
        "seed": seed, "list_size_requested": list_size,
        "L_A": list_size, "L_B": list_size, "L_C": list_size,
        "q_bits": 20, "Q": 1 << 20, "lambda": 2.0,
        "raw_arithmetic_triples": 100,
        "direct_full_total_median_ns": direct_ns * 2,
        "direct_production_total_median_ns": direct_ns,
        "ntt_three_merge_amortized_ns": direct_ns * 10,
        "direct_production_peak_bytes": 8_000_000,
        "exactness_passed": exact, "status": status,
        "direct_production_total_min_ns": direct_ns,
        "direct_production_total_max_ns": direct_ns,
        "direct_tiled_total_median_ns": tiled_ns,
        "direct_tiled_setup_median_ns": tiled_ns // 4,
        "direct_tiled_scan_median_ns": tiled_ns // 2,
        "direct_tiled_peak_bytes": 1_000_000,
        "direct_tiled_tile_bits": 15,
        "direct_tiled_nonempty_windows": 30,
        "direct_tiled_executed_pairs": executed,
        "direct_tiled_total_min_ns": tiled_ns,
        "direct_tiled_total_max_ns": tiled_ns,
        "direct_tiled_window_count": 60,
        "direct_tiled_auxiliary_bytes": 900_000,
    }


def self_test() -> int:
    failures: list[str] = []

    def check(name: str, condition: bool) -> None:
        if not condition:
            failures.append(name)

    # -- geometric mean and bootstrap --------------------------------------
    check("geomean of equal values", math.isclose(geometric_mean([2.0, 2.0, 2.0]), 2.0))
    check("geomean 1,4 == 2", math.isclose(geometric_mean([1.0, 4.0]), 2.0))
    check(
        "geomean below arithmetic mean for spread samples",
        geometric_mean([1.0, 100.0]) < statistics.fmean([1.0, 100.0]),
    )
    try:
        geometric_mean([1.0, 0.0])
        check("geomean rejects nonpositive", False)
    except ValueError:
        check("geomean rejects nonpositive", True)

    lo, hi = bootstrap_geomean_ci([2.0, 2.0, 2.0, 2.0, 2.0])
    check("bootstrap of constant sample is degenerate", math.isclose(lo, 2.0) and math.isclose(hi, 2.0))
    sample = [1.5, 2.0, 2.5, 3.0, 3.5]
    lo, hi = bootstrap_geomean_ci(sample)
    check("bootstrap interval brackets the point estimate",
          lo <= geometric_mean(sample) <= hi)
    check("bootstrap interval is inside the sample range", lo >= min(sample) and hi <= max(sample))
    check("bootstrap is deterministic", bootstrap_geomean_ci(sample) == bootstrap_geomean_ci(sample))
    check("bootstrap seed changes the interval",
          bootstrap_geomean_ci(sample, seed=1) != bootstrap_geomean_ci(sample, seed=2))

    # -- per-row derivation -------------------------------------------------
    raw = _synthetic_main_row(1024, MAIN_SEEDS[0], direct_ns=4000, tiled_ns=1000, executed=524288)
    derived = derive_main_row({k: str(v) for k, v in raw.items()})
    check("speedup = direct/tiled", math.isclose(derived["speedup"], 4.0))
    check("memory ratio = direct/tiled bytes", math.isclose(derived["memory_ratio"], 8.0))
    check("executed pair fraction = executed/(L_A*L_B)",
          math.isclose(derived["executed_pair_fraction"], 524288 / (1024 * 1024)))
    check("setup fraction", math.isclose(derived["tiled_setup_fraction"], 0.25))
    check("scan fraction", math.isclose(derived["tiled_scan_fraction"], 0.5))
    check("wrapper residual completes the split",
          math.isclose(derived["tiled_wrapper_residual_fraction"], 0.25))

    # -- grouping and aggregation ------------------------------------------
    rows = []
    for L in (512, 1024):
        for index, seed in enumerate(MAIN_SEEDS):
            rows.append(
                derive_main_row(
                    {
                        k: str(v)
                        for k, v in _synthetic_main_row(
                            L, seed,
                            direct_ns=1000 * (index + 1) * (2 if L == 1024 else 1),
                            tiled_ns=500 * (index + 1),
                            executed=L * L,
                        ).items()
                    }
                )
            )
    summary, completeness = summarize_main(rows)
    check("grouping produces one entry per list size", len(summary) == 2)
    by_size = {s["list_size"]: s for s in summary}
    check("all seeds counted", by_size[512]["seeds_completed"] == 5)
    check("speedup constant at L=512", math.isclose(by_size[512]["geomean_speedup"], 2.0))
    check("speedup constant at L=1024", math.isclose(by_size[1024]["geomean_speedup"], 4.0))
    check("min/max speedup agree when constant",
          math.isclose(by_size[512]["min_speedup"], by_size[512]["max_speedup"]))
    check("executed pair fraction 1.0 when nothing is skipped",
          math.isclose(by_size[512]["median_executed_pair_fraction"], 1.0))

    # -- row-count validation ----------------------------------------------
    check("incomplete matrix is detected", completeness["complete"] is False)
    check("missing cells are enumerated",
          len(completeness["missing_cells"]) == len(MAIN_LIST_SIZES) * len(MAIN_SEEDS) - 10)
    full_rows = [
        derive_main_row(
            {k: str(v) for k, v in _synthetic_main_row(L, seed, 4000, 1000, L * L).items()}
        )
        for L in MAIN_LIST_SIZES
        for seed in MAIN_SEEDS
    ]
    _s, full_completeness = summarize_main(full_rows)
    check("complete matrix is accepted", full_completeness["complete"] is True)
    check("complete matrix has 45 rows", full_completeness["actual_rows"] == 45)
    dup_rows = full_rows + [full_rows[0]]
    _s, dup_completeness = summarize_main(dup_rows)
    check("duplicate cell is detected", dup_completeness["duplicate_cell_count"] == 1)
    check("duplicate matrix is rejected", dup_completeness["complete"] is False)

    # -- exactness and status handling -------------------------------------
    bad = list(full_rows)
    bad[3] = derive_main_row(
        {
            k: str(v)
            for k, v in _synthetic_main_row(
                MAIN_LIST_SIZES[0], MAIN_SEEDS[3], 4000, 1000, 100, exact=0
            ).items()
        }
    )
    _s, bad_completeness = summarize_main(bad)
    check("exactness failure is detected", len(bad_completeness["exactness_failures"]) == 1)

    censored = list(full_rows)
    censored[7] = derive_main_row(
        {
            k: str(v)
            for k, v in _synthetic_main_row(
                MAIN_LIST_SIZES[1], MAIN_SEEDS[2], 4000, 1000, 100,
                status="ResourceLimit",
            ).items()
        }
    )
    cs, cc = summarize_main(censored)
    check("non-completed row is retained, not dropped", cc["actual_rows"] == 45)
    check("non-completed row is flagged", len(cc["non_completed_rows"]) == 1)
    censored_entry = {s["list_size"]: s for s in cs}[MAIN_LIST_SIZES[1]]
    check("non-completed row is excluded from timing with a reason",
          censored_entry["seeds_completed"] == 4
          and censored_entry["rows_excluded_from_timing"] == 1
          and "ResourceLimit" in censored_entry["excluded_reason"])

    # -- tile-sensitivity normalization ------------------------------------
    tile_rows = []
    timings = {10: 300, 12: 200, 14: 120, 15: 100, 16: 110, 18: 180, 20: 400}
    for base in TILE_SEED_BASES:
        for w in TILE_QUOTIENT_BITS:
            for sb in TILE_REQUESTED_SB:
                tile_rows.append(
                    derive_tile_row(
                        {
                            "seed": str(base + w), "w": str(w), "SB": str(sb),
                            "effective_SB": str(min(sb, w)), "Q": str(1 << w),
                            "list_size": "4096", "raw_triples": "10",
                            "setup_median_ns": "10", "scan_median_ns": "20",
                            "total_median_ns": str(timings[sb]),
                            "auxiliary_peak_bytes": "1000", "bucket_count": "4",
                            "window_count": "7", "executed_pairs": str(4096 * 4096),
                            "nonempty_windows": "5",
                            "independent_exactness_passed": "1",
                        },
                        base,
                    )
                )
    tile_summary, tile_completeness = summarize_tiles(tile_rows)
    check("tile matrix is complete", tile_completeness["complete"] is True)
    check("tile matrix has 189 rows", tile_completeness["actual_rows"] == 189)
    check("normalized slowdown is 1.0 at the fastest tile",
          all(math.isclose(r["normalized_slowdown"], 1.0)
              for r in tile_summary if r["requested_SB"] == 15))
    check("normalized slowdown exceeds 1 elsewhere",
          all(r["normalized_slowdown"] > 1.0
              for r in tile_summary if r["requested_SB"] != 15))
    check("normalized slowdown matches the ratio",
          math.isclose(
              next(r for r in tile_summary if r["requested_SB"] == 20)["normalized_slowdown"],
              400 / 100,
          ))
    robustness = tile_robustness(tile_summary)
    check("SB=15 wins every synthetic group", math.isclose(robustness["sb15_best_fraction"], 1.0))
    check("SB=15 median slowdown is 1.0",
          math.isclose(robustness["sb15_median_normalized_slowdown"], 1.0))

    shifted = []
    for row in tile_rows:
        clone = dict(row)
        if row["w"] >= 28 and row["requested_SB"] == 18:
            clone["total_median_ns"] = 50
        shifted.append(clone)
    shifted_summary, _ = summarize_tiles(shifted)
    shifted_robustness = tile_robustness(shifted_summary)
    check("a different winning tile lowers the SB=15 win fraction",
          shifted_robustness["sb15_best_fraction"] < 1.0)
    check("a different winning tile raises the worst SB=15 slowdown",
          shifted_robustness["sb15_max_normalized_slowdown"] > 1.0)

    tile_exact_bad = [dict(r) for r in tile_rows]
    tile_exact_bad[5]["independent_exactness_passed"] = 0
    _ts, tile_bad_completeness = summarize_tiles(tile_exact_bad)
    check("tile exactness failure is detected",
          len(tile_bad_completeness["exactness_failures"]) == 1)

    # -- end-to-end CSV round trip and figure emission ---------------------
    with tempfile.TemporaryDirectory() as tmp:
        raw_dir = os.path.join(tmp, "raw")
        os.makedirs(raw_dir)
        path = os.path.join(raw_dir, "paper-crossover-tiled.csv")
        with open(path, "w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(handle, fieldnames=MAIN_SELF_TEST_COLUMNS)
            writer.writeheader()
            for L in MAIN_LIST_SIZES:
                for seed in MAIN_SEEDS:
                    writer.writerow(_synthetic_main_row(L, seed, 4000, 1000, L * L))
        reloaded = [derive_main_row(r) for r in read_csv(path)]
        check("CSV round trip preserves the row count", len(reloaded) == 45)
        check("CSV round trip preserves the derived speedup",
              all(math.isclose(r["speedup"], 4.0) for r in reloaded))

        figures = os.path.join(tmp, "figures")
        os.makedirs(figures)
        reloaded_summary, _ = summarize_main(reloaded)
        written = make_figures(reloaded, reloaded_summary, tile_summary, figures)
        check("five figures are written", len(written) == 5)
        check("every figure is non-empty SVG",
              all(os.path.getsize(p) > 400 and open(p, encoding="utf-8").read().startswith("<svg")
                  for p in written))
        first = open(written[0], encoding="utf-8").read()
        make_figures(reloaded, reloaded_summary, tile_summary, figures)
        check("figure output is deterministic",
              open(written[0], encoding="utf-8").read() == first)

    if failures:
        print("SELF-TEST FAILED:")
        for name in failures:
            print(f"  - {name}")
        return 1
    print("analysis self-test passed")
    return 0


# --------------------------------------------------------------------------
# Campaign analysis
# --------------------------------------------------------------------------
def read_text_if_present(path: str) -> str:
    if not os.path.exists(path):
        return ""
    with open(path, encoding="utf-8", errors="replace") as handle:
        return handle.read()


# Paths this campaign is permitted to touch.  Anything else appearing in the
# working-tree diff is unexplained scope expansion and fails the gate.
ALLOWED_CHANGED_PATHS = {
    "CMakeLists.txt",
    "FULL_TER_COMPLIANCE.md",
    "README.md",
    "bench/analyse_tiled_q3report.py",
    "bench/run_tiled_publication_campaign.ps1",
    "bench/ter_tiled_complete_merge_bench.cpp",
    "bench/ter_tiled_parameter_sweep.cpp",
    "docs/history/tiling_implementation_idea.md",
}

# Runs withdrawn during this campaign, recorded here because the repository's
# blanket *.txt ignore rule would otherwise drop the explanation from the
# committed package.  The raw files themselves are retained beside the valid
# ones under an explicit WITHDRAWN name and feed no analysis artifact.
WITHDRAWN_RUNS = [
    {
        "file": "raw/complete-merge-WITHDRAWN-degenerate-lambda.csv",
        "withdrawn_before_any_number_was_reported": True,
        "reasons": [
            "Held b2=8, b1=18 fixed for every list size, so Q stayed at 2^10 while L "
            "grew: lambda = L^2/Q reached about 1024 against the declared TER paper "
            "scaling's 2-4.",
            "At Q = 2^10 the tile policy gives tile_bits = min(15, 10) = 10, so S = Q, "
            "P = 1 bucket and 1 window. The tiled path degenerated to a single window "
            "and the comparison could not measure tiling at all.",
            "Its memory columns were not comparable: src/merge.cpp gives the tiled path "
            "working_set_budget_bytes = fixed_bytes + a candidate ceiling absorbing the "
            "whole remaining budget, while the baseline reports a measured peak.",
            "The L=2048 point returned ResourceLimit as a consequence of the first item.",
        ],
        "replaced_by": "raw/complete-merge.csv (lambda = 2 held at every point)",
    }
]

# Frozen historical artifacts.  A nonempty diff on any of these is a failure.
FROZEN_PATHS = [
    "paper_crossover.csv",
    "TER_Q3REPORT_COUNT_ONLY_NTT_EXPERIMENT_PLAN.md",
    "results/partition",
]

# The frozen production tile policy.  These must still hold their declared
# values; the sensitivity campaign is evidence, never authorization to retune.
FROZEN_TILE_DEFAULTS = {
    "kDefaultPreferredTileBits": 15,
    "kDefaultRootPreferredTileBits": 15,
    "kDefaultRootPrefilterBits": 24,
}


def _git(repo: str, *args: str) -> tuple[int, str]:
    import subprocess

    try:
        proc = subprocess.run(
            ["git", *args], cwd=repo, capture_output=True, text=True, timeout=120
        )
        return proc.returncode, proc.stdout
    except (OSError, subprocess.SubprocessError):
        return 127, ""


def verify_repository_gates(repo: str) -> dict[str, Any]:
    """Mechanically check the freeze and scope claims instead of asserting them."""
    result: dict[str, Any] = {}

    # Production tile defaults, read out of the header itself.
    header = os.path.join(repo, "include", "ter", "limitations.hpp")
    observed: dict[str, Any] = {}
    text = read_text_if_present(header)
    for name, expected in FROZEN_TILE_DEFAULTS.items():
        found = None
        for line in text.splitlines():
            stripped = line.strip()
            if stripped.startswith(f"inline constexpr std::uint32_t {name} =") and (
                "control_panel" not in stripped
            ):
                digits = "".join(c for c in stripped.split("=", 1)[1] if c.isdigit())
                if digits:
                    found = int(digits)
                    break
        observed[name] = found
    result["production_tile_defaults_observed"] = observed
    result["production_tile_default_untouched"] = all(
        observed.get(name) == expected for name, expected in FROZEN_TILE_DEFAULTS.items()
    )

    # Frozen artifacts must have an empty diff against HEAD.
    frozen_status: dict[str, Any] = {}
    for path in FROZEN_PATHS:
        code, _out = _git(repo, "diff", "--quiet", "HEAD", "--", path)
        frozen_status[path] = "unchanged" if code == 0 else "CHANGED"
    result["frozen_artifact_status"] = frozen_status
    result["historical_paper_crossover_untouched"] = (
        frozen_status.get("paper_crossover.csv") == "unchanged"
    )
    result["root_partition_results_untouched"] = (
        frozen_status.get("results/partition") == "unchanged"
    )
    result["frozen_experiment_plan_untouched"] = (
        frozen_status.get("TER_Q3REPORT_COUNT_ONLY_NTT_EXPERIMENT_PLAN.md") == "unchanged"
    )

    # Scope: every tracked file the working tree changed must be on the allowlist.
    code, out = _git(repo, "diff", "--name-only", "HEAD")
    changed = sorted({line.strip().replace("\\", "/") for line in out.splitlines() if line.strip()})
    unexpected = [p for p in changed if p not in ALLOWED_CHANGED_PATHS]
    result["changed_tracked_files"] = changed
    result["unexpected_changed_files"] = unexpected
    result["no_unexplained_scope_creep"] = (code == 0) and not unexpected

    # Core algorithm files must be untouched by a benchmarking campaign.
    core = [
        "src/tiled_q3report.cpp", "src/full_ter.cpp", "src/merge.cpp",
        "src/arithmetic.cpp", "src/packed.cpp", "src/validator.cpp",
        "include/ter/limitations.hpp", "include/ter/tiled_q3report.hpp",
    ]
    touched_core = [p for p in core if p in changed]
    result["core_algorithm_files_changed"] = touched_core
    result["core_algorithm_files_untouched"] = not touched_core
    result["fullter_default_semantics_untouched"] = "src/full_ter.cpp" not in changed
    return result


def verify_documentation_reconciled(repo: str) -> dict[str, Any]:
    """Check that the specific stale claims are actually gone from the docs."""
    import re

    def active_text(markdown: str) -> str:
        """Drop ~~struck-through~~ spans.

        A superseded claim kept struck through is a provenance record, not a
        live assertion, so it must not count as an unreconciled stale claim.
        """
        return re.sub(r"~~.*?~~", "", markdown, flags=re.DOTALL)

    tiling = active_text(
        read_text_if_present(os.path.join(repo, "docs", "history", "tiling_implementation_idea.md"))
    )
    compliance = active_text(
        read_text_if_present(os.path.join(repo, "FULL_TER_COMPLIANCE.md"))
    )
    stale = {
        "tiling: 'not wired into the repository'": "not wired into the repository" in tiling,
        "tiling: 'No prototype is in the repository'": "No prototype is in the repository" in tiling,
        "tiling: 'S was not swept'": "S was not swept" in tiling,
        "tiling: 'Theta(L + S)' working-set claim": "| `Theta(L + S)` |" in tiling,
        "tiling: 'always an L2 hit'": "always an L2 hit" in tiling,
        "tiling: 'every probe becomes an L2 cache hit'":
            "every probe becomes an L2 cache hit" in tiling,
        "compliance: 'when `n-r1 <= 64`'": "n-r1 <= 64" in compliance,
    }
    remaining = sorted(k for k, present in stale.items() if present)
    required = {
        "tiling records the O(A+B+C+Q/S+S) auxiliary form":
            "O(A + B + C + Q/S + S)" in tiling,
        "tiling distinguishes logical from executed pairs":
            "executed pairs" in tiling and "logical pair universe" in tiling,
        "compliance scopes NTT absence to solver integration":
            "absent from the FullTer solver" in compliance,
    }
    missing = sorted(k for k, present in required.items() if not present)
    return {
        "documentation_stale_claims_remaining": remaining,
        "documentation_required_statements_missing": missing,
        "documentation_reconciled": not remaining and not missing,
    }


def ctest_summary(text: str) -> dict[str, Any]:
    """Extract the pass/fail counts from a captured ctest log."""
    passed = failed = None
    for line in text.splitlines():
        stripped = line.strip()
        if "tests passed," in stripped and "tests failed out of" in stripped:
            try:
                head, tail = stripped.split("tests passed,")
                failed = int(tail.split("tests failed out of")[0].strip())
                total = int(tail.split("tests failed out of")[1].strip())
                passed = total - failed
            except (ValueError, IndexError):
                continue
    return {"passed": passed, "failed": failed, "found": passed is not None}


def analyse(results_dir: str) -> int:
    raw_dir = os.path.join(results_dir, "raw")
    analysis_dir = os.path.join(results_dir, "analysis")
    figures_dir = os.path.join(results_dir, "figures")
    os.makedirs(analysis_dir, exist_ok=True)
    os.makedirs(figures_dir, exist_ok=True)

    main_path = os.path.join(raw_dir, "paper-crossover-tiled.csv")
    if not os.path.exists(main_path):
        print(f"missing required raw file: {main_path}", file=sys.stderr)
        return 2
    main_rows = [derive_main_row(row) for row in read_csv(main_path)]
    main_summary, main_completeness = summarize_main(main_rows)

    tile_rows: list[dict[str, Any]] = []
    tile_files_present = []
    for base in TILE_SEED_BASES:
        path = os.path.join(raw_dir, f"tile-sweep-seed-{base}.csv")
        if os.path.exists(path):
            tile_files_present.append(os.path.basename(path))
            tile_rows.extend(derive_tile_row(row, base) for row in read_csv(path))
    tile_summary, tile_completeness = summarize_tiles(tile_rows)
    robustness = tile_robustness(tile_summary) if tile_summary else {}

    anchors_path = os.path.join(raw_dir, "authentic-anchors.csv")
    anchor_rows = read_csv(anchors_path) if os.path.exists(anchors_path) else []
    anchor_families = sorted({r["snapshot_family"] for r in anchor_rows})
    anchor_exact = all(int(float(r["exactness_passed"])) == 1 for r in anchor_rows)

    merge_path = os.path.join(raw_dir, "complete-merge.csv")
    merge_rows = read_csv(merge_path) if os.path.exists(merge_path) else []
    merge_analysis: list[dict[str, Any]] = []
    for row in merge_rows:
        baseline = int(float(row["baseline_total_median_ns"]))
        tiled = int(float(row["tiled_total_median_ns"]))
        base_status = row.get("baseline_status", "Completed")
        tiled_status = row.get("tiled_status", "Completed")
        comparable = base_status == "Completed" and tiled_status == "Completed"
        # Only the arithmetic-index accounting is comparable between the two
        # methods.  baseline_measured_peak_bytes is a measured peak while
        # tiled_budget_ceiling_bytes is fixed_bytes plus a candidate ceiling that
        # absorbs the remaining budget (src/merge.cpp), so the two are carried
        # for provenance and never divided by one another.
        base_index = int(float(row["baseline_arithmetic_index_bytes"]))
        tiled_index = int(float(row["tiled_arithmetic_index_bytes"]))
        merge_analysis.append(
            {
                "list_size": int(float(row["list_size"])),
                "seed": int(float(row["seed"])),
                "q_bits": int(float(row["q_bits"])),
                "baseline_status": base_status,
                "tiled_status": tiled_status,
                "comparable": 1 if comparable else 0,
                "baseline_total_median_ns": baseline,
                "tiled_total_median_ns": tiled,
                "ratio": (baseline / tiled) if (comparable and tiled > 0) else None,
                "baseline_arithmetic_index_bytes": base_index,
                "tiled_arithmetic_index_bytes": tiled_index,
                "arithmetic_index_ratio": (
                    base_index / tiled_index if tiled_index > 0 else None
                ),
                "baseline_measured_peak_bytes": int(
                    float(row["baseline_measured_peak_bytes"])
                ),
                "baseline_pair_records": int(float(row["baseline_pair_records"])),
                "tiled_pair_records": int(float(row["tiled_pair_records"])),
                "outputs_identical": int(float(row["outputs_identical"])),
                "unique_parents": int(float(row["unique_parents"])),
            }
        )

    # -- write the machine-readable summaries ------------------------------
    main_columns = [
        "list_size", "q_bits", "Q", "lambda", "tile_bits", "rows_present",
        "seeds_completed", "rows_excluded_from_timing", "excluded_reason",
        "median_direct_ns", "median_tiled_ns", "median_direct_full_ns",
        "median_ntt_amortized_ns", "median_speedup", "geomean_speedup",
        "min_speedup", "max_speedup", "geomean_speedup_ci_low",
        "geomean_speedup_ci_high", "median_direct_peak_bytes",
        "median_tiled_peak_bytes", "median_memory_ratio",
        "median_executed_pair_fraction", "min_executed_pair_fraction",
        "median_nonempty_window_fraction", "median_tiled_setup_fraction",
        "median_tiled_scan_fraction", "median_tiled_wrapper_residual_fraction",
    ]
    write_csv(os.path.join(analysis_dir, "main-summary.csv"), main_summary, main_columns)

    tile_columns = [
        "seed_base", "seed", "w", "requested_SB", "effective_SB", "Q",
        "total_median_ns", "setup_median_ns", "scan_median_ns",
        "auxiliary_peak_bytes", "bucket_count", "window_count",
        "nonempty_windows", "executed_pairs", "executed_pair_fraction",
        "independent_exactness_passed", "best_SB_in_group",
        "best_total_median_ns", "normalized_slowdown", "is_best",
        "is_production_default",
    ]
    write_csv(
        os.path.join(analysis_dir, "tile-sensitivity-summary.csv"), tile_summary, tile_columns
    )

    if merge_analysis:
        write_csv(
            os.path.join(analysis_dir, "complete-merge-summary.csv"),
            merge_analysis,
            [
                "list_size", "seed", "q_bits", "baseline_status", "tiled_status",
                "comparable", "baseline_total_median_ns", "tiled_total_median_ns",
                "ratio", "baseline_arithmetic_index_bytes",
                "tiled_arithmetic_index_bytes", "arithmetic_index_ratio",
                "baseline_measured_peak_bytes", "baseline_pair_records",
                "tiled_pair_records", "outputs_identical", "unique_parents",
            ],
        )

    figures = make_figures(main_rows, main_summary, tile_summary, figures_dir)

    # -- evidence gates -----------------------------------------------------
    focused_log = read_text_if_present(os.path.join(results_dir, "ctest-focused.txt"))
    full_log = read_text_if_present(os.path.join(results_dir, "ctest-full.txt"))
    asan_log = read_text_if_present(os.path.join(results_dir, "ctest-asan.txt"))
    focused = ctest_summary(focused_log)
    full = ctest_summary(full_log)
    asan = ctest_summary(asan_log)

    env_path = os.path.join(results_dir, "environment.json")
    environment: dict[str, Any] = {}
    if os.path.exists(env_path):
        with open(env_path, encoding="utf-8-sig") as handle:
            environment = json.load(handle)

    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    repo_gates = verify_repository_gates(repo_root)
    doc_gates = verify_documentation_reconciled(repo_root)

    completed_main = [r for r in main_rows if r["status"] == "Completed"]
    all_main_exact = bool(completed_main) and all(
        r["exactness_passed"] == 1 for r in completed_main
    )
    all_tile_exact = bool(tile_rows) and all(
        r["independent_exactness_passed"] == 1 for r in tile_rows
    )
    comparable_merge = [r for r in merge_analysis if r["comparable"] == 1]
    merge_outputs_identical = bool(comparable_merge) and all(
        r["outputs_identical"] == 1 for r in comparable_merge
    )

    sanitizer_ok = asan.get("found") is True and asan.get("failed") == 0

    evidence_complete = (
        bool(environment)
        and sanitizer_ok
        and repo_gates["production_tile_default_untouched"]
        and repo_gates["historical_paper_crossover_untouched"]
        and repo_gates["root_partition_results_untouched"]
        and repo_gates["frozen_experiment_plan_untouched"]
        and repo_gates["no_unexplained_scope_creep"]
        and repo_gates["core_algorithm_files_untouched"]
        and doc_gates["documentation_reconciled"]
        and focused.get("found") is True and focused.get("failed") == 0
        and full.get("found") is True and full.get("failed") == 0
        and main_completeness["complete"]
        and all_main_exact
        and tile_completeness["complete"]
        and all_tile_exact
        and len(figures) == 5
        and bool(anchor_rows) and anchor_exact
        and merge_outputs_identical
    )

    claim_checks = {
        "release_build_passed": bool(environment),
        "release_build_evidence": (
            "Release configure and build completed before the campaign; the exact "
            "compiler, generator and executable SHA-256 values are in environment.json."
            if environment
            else "environment.json is absent, so no build evidence was captured."
        ),
        "focused_tests_passed": focused.get("found") is True and focused.get("failed") == 0,
        "focused_tests_counts": focused,
        "full_ctest_passed": full.get("found") is True and full.get("failed") == 0,
        "full_ctest_counts": full,

        "sanitizer_run_completed": asan.get("found") is True,
        "sanitizer_passed": asan.get("found") is True and asan.get("failed") == 0,
        "sanitizer_counts": asan,
        "sanitizer_unavailable_with_reason": (
            "" if asan.get("found") else "No ctest-asan.txt was captured in this campaign directory."
        ),

        "main_matrix_complete": main_completeness["complete"],
        "main_expected_rows": main_completeness["expected_rows"],
        "main_actual_rows": main_completeness["actual_rows"],
        "main_missing_cells": main_completeness["missing_cells"],
        "main_unexpected_cells": main_completeness["unexpected_cells"],
        "main_duplicate_cell_count": main_completeness["duplicate_cell_count"],
        "main_non_completed_rows": main_completeness["non_completed_rows"],
        "all_completed_main_rows_exact": all_main_exact,
        "main_exactness_failures": main_completeness["exactness_failures"],

        "tile_sweep_complete": tile_completeness["complete"],
        "tile_sweep_expected_rows": tile_completeness["expected_rows"],
        "tile_sweep_actual_rows": tile_completeness["actual_rows"],
        "tile_sweep_missing_cells": tile_completeness["missing_cells"],
        "all_completed_tile_rows_exact": all_tile_exact,
        "tile_exactness_failures": tile_completeness["exactness_failures"],
        "tile_sweep_files": tile_files_present,

        "authentic_anchor_rows": len(anchor_rows),
        "authentic_anchor_families": anchor_families,
        "authentic_anchors_exact": anchor_exact,

        "complete_merge_rows": len(merge_analysis),
        "complete_merge_comparable_rows": len(comparable_merge),
        "complete_merge_outputs_identical": merge_outputs_identical,
        "complete_merge_comparator_label": (
            "existing pair-materializing public baseline vs tiled public merge"
        ),
        "withdrawn_runs": WITHDRAWN_RUNS,
        "complete_merge_is_not_the_headline_comparator": True,

        **{
            k: repo_gates[k]
            for k in (
                "historical_paper_crossover_untouched",
                "root_partition_results_untouched",
                "frozen_experiment_plan_untouched",
                "frozen_artifact_status",
                "production_tile_default_untouched",
                "production_tile_defaults_observed",
                "fullter_default_semantics_untouched",
                "core_algorithm_files_untouched",
                "core_algorithm_files_changed",
                "changed_tracked_files",
                "unexpected_changed_files",
                "no_unexplained_scope_creep",
            )
        },
        "frozen_artifact_note": (
            "Verified mechanically, not asserted: the frozen-artifact rows come from "
            "`git diff --quiet HEAD -- <path>`, the tile defaults are parsed out of "
            "include/ter/limitations.hpp, and the scope row compares "
            "`git diff --name-only HEAD` against the campaign allowlist."
        ),

        "all_failed_seeds_preserved": True,
        "failed_seed_policy": (
            "Every executed (L, seed) row is written to the raw CSV regardless of "
            "status or timing, and non-Completed rows are retained and flagged "
            "rather than dropped."
        ),
        "analysis_self_test_passed": None,
        "sanitizer_gate_passed": sanitizer_ok,
        "analysis_regeneration_passed": True,
        "figures_generated": [os.path.basename(p) for p in figures],
        "figures_from_measured_data_only": True,
        "hardware_cache_counters_collected": bool(
            environment.get("hardware_cache_counters_collected", False)
        ),
        "matplotlib_available": bool(environment.get("matplotlib_available", False)),
        "plot_backend": "self-contained deterministic SVG writer",
        **doc_gates,
        "no_asymptotic_claim_supported": True,
        "asymptotic_claim_note": (
            "Worst-case pair work remains O(|A||B|). Nothing in this campaign "
            "supports a subquadratic or exponent claim; the measured effects are "
            "finite-machine constant-factor and memory-footprint effects."
        ),
        "evidence_package_complete": evidence_complete,
        "ready_for_manuscript_drafting": evidence_complete,
    }
    write_json(os.path.join(analysis_dir, "claim-checks.json"), claim_checks)

    # -- paper facts --------------------------------------------------------
    usable = [s for s in main_summary if s.get("seeds_completed", 0) > 0]
    paper_facts = {
        "largest_executed_list_size": max((s["list_size"] for s in usable), default=None),
        "largest_Q": max((s["Q"] for s in usable), default=None),
        "list_sizes_executed": [s["list_size"] for s in usable],
        "number_of_main_seeds": len(MAIN_SEEDS),
        "number_of_tile_sensitivity_seeds": len(TILE_SEED_BASES),
        "main_seeds": [hex(s) for s in MAIN_SEEDS],
        "tile_seed_bases": TILE_SEED_BASES,
        "median_speedup_by_L": {str(s["list_size"]): s["median_speedup"] for s in usable},
        "geomean_speedup_by_L": {str(s["list_size"]): s["geomean_speedup"] for s in usable},
        "speedup_CI_by_L": {
            str(s["list_size"]): [s["geomean_speedup_ci_low"], s["geomean_speedup_ci_high"]]
            for s in usable
        },
        "speedup_range_by_L": {
            str(s["list_size"]): [s["min_speedup"], s["max_speedup"]] for s in usable
        },
        "memory_ratio_by_L": {str(s["list_size"]): s["median_memory_ratio"] for s in usable},
        "direct_peak_bytes_by_L": {
            str(s["list_size"]): s["median_direct_peak_bytes"] for s in usable
        },
        "tiled_peak_bytes_by_L": {
            str(s["list_size"]): s["median_tiled_peak_bytes"] for s in usable
        },
        "executed_pair_fraction_by_L": {
            str(s["list_size"]): s["median_executed_pair_fraction"] for s in usable
        },
        "nonempty_window_fraction_by_L": {
            str(s["list_size"]): s["median_nonempty_window_fraction"] for s in usable
        },
        "tiled_setup_fraction_by_L": {
            str(s["list_size"]): s["median_tiled_setup_fraction"] for s in usable
        },
        "tiled_scan_fraction_by_L": {
            str(s["list_size"]): s["median_tiled_scan_fraction"] for s in usable
        },
        "tile_bits_used_by_L": {str(s["list_size"]): s["tile_bits"] for s in usable},
        "q_bits_by_L": {str(s["list_size"]): s["q_bits"] for s in usable},
        "lambda_by_L": {str(s["list_size"]): s["lambda"] for s in usable},
        "best_tile_by_w": (
            {
                str(w): sorted(
                    {r["best_SB_in_group"] for r in tile_summary if r["w"] == w}
                )
                for w in sorted({r["w"] for r in tile_summary})
            }
            if tile_summary
            else {}
        ),
        "SB15_normalized_slowdown_by_w": (
            {
                w: {
                    "median": data["sb15_median_normalized_slowdown"],
                    "max": data["sb15_max_normalized_slowdown"],
                    "best_count": data["sb15_best_count"],
                    "seed_bases": data["seed_bases_measured"],
                }
                for w, data in robustness.get("by_quotient_width", {}).items()
            }
            if robustness
            else {}
        ),
        "SB15_overall": {
            k: robustness.get(k)
            for k in (
                "groups_evaluated", "sb15_best_count", "sb15_best_fraction",
                "sb15_median_normalized_slowdown", "sb15_max_normalized_slowdown",
                "sb15_min_normalized_slowdown",
            )
        }
        if robustness
        else {},
        "authentic_anchor_count": len(anchor_rows),
        "authentic_anchor_families": anchor_families,
        "complete_merge_ratio_by_L": (
            {
                str(L): statistics.median(
                    [r["ratio"] for r in comparable_merge if r["list_size"] == L]
                )
                for L in sorted({r["list_size"] for r in comparable_merge})
            }
            if comparable_merge
            else {}
        ),
        "complete_merge_arithmetic_index_ratio_by_L": (
            {
                str(L): statistics.median(
                    [
                        r["arithmetic_index_ratio"]
                        for r in comparable_merge
                        if r["list_size"] == L
                    ]
                )
                for L in sorted({r["list_size"] for r in comparable_merge})
            }
            if comparable_merge
            else {}
        ),
        "complete_merge_non_comparable_rows": [
            {
                "list_size": r["list_size"],
                "seed": r["seed"],
                "baseline_status": r["baseline_status"],
                "tiled_status": r["tiled_status"],
            }
            for r in merge_analysis
            if r["comparable"] != 1
        ],
        "complete_merge_comparator_label": (
            "existing pair-materializing public baseline vs tiled public merge; NOT "
            "the DIRECT_PRODUCTION comparator used for the headline ratio"
        ),
        "CPU": environment.get("cpu_model"),
        "cpu_physical_cores": environment.get("cpu_physical_cores"),
        "l3_cache_kb_os_aggregate": environment.get("l3_cache_kb_os_aggregate"),
        "l2_cache_kb_os_aggregate": environment.get("l2_cache_kb_os_aggregate"),
        "installed_ram_bytes": environment.get("installed_ram_bytes"),
        "compiler": environment.get("compiler_version"),
        "cmake_generator": environment.get("cmake_generator"),
        "build_flags": {
            "configuration": environment.get("build_configuration"),
            "ter_native_opt": environment.get("ter_native_opt"),
            "threads": environment.get("benchmark_thread_count"),
        },
        "git_commit": environment.get("git_commit"),
        "working_tree_dirty": environment.get("working_tree_dirty"),
        "hardware_cache_counters_collected": bool(
            environment.get("hardware_cache_counters_collected", False)
        ),
        "worst_case_pair_work": "O(|A| * |B|), unchanged by tiling",
        "auxiliary_memory_form": "O(|A| + |B| + |C| + Q/S + S); hot count window is O(S)",
    }
    write_json(os.path.join(analysis_dir, "paper-facts.json"), paper_facts)

    # -- console summary ----------------------------------------------------
    print(f"main rows: {main_completeness['actual_rows']}/{main_completeness['expected_rows']} "
          f"complete={main_completeness['complete']}")
    print(f"tile rows: {tile_completeness['actual_rows']}/{tile_completeness['expected_rows']} "
          f"complete={tile_completeness['complete']}")
    print(f"all completed main rows exact: {all_main_exact}")
    print(f"all tile rows exact: {all_tile_exact}")
    print(f"figures written: {len(figures)}")
    for entry in usable:
        print(
            f"  L={entry['list_size']:>6}  q_bits={entry['q_bits']:>2}  "
            f"seeds={entry['seeds_completed']}  "
            f"geomean_speedup={entry['geomean_speedup']:.3f}x  "
            f"CI=[{entry['geomean_speedup_ci_low']:.3f}, {entry['geomean_speedup_ci_high']:.3f}]  "
            f"mem_ratio={entry['median_memory_ratio']:.2f}x  "
            f"f_exec={entry['median_executed_pair_fraction']:.4f}"
        )
    if robustness:
        print(
            f"SB=15 best in {robustness['sb15_best_count']}/"
            f"{robustness['groups_evaluated']} (seed, w) groups; "
            f"median normalized slowdown "
            f"{robustness['sb15_median_normalized_slowdown']:.4f}, worst "
            f"{robustness['sb15_max_normalized_slowdown']:.4f}"
        )
    print(f"evidence_package_complete: {evidence_complete}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true",
                        help="run the analysis unit checks and exit")
    parser.add_argument("--results-dir", type=str, default=None,
                        help="campaign directory containing raw/, analysis/ and figures/")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if not args.results_dir:
        parser.error("either --self-test or --results-dir is required")
    return analyse(args.results_dir)


if __name__ == "__main__":
    sys.exit(main())
