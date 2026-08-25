#!/usr/bin/env python3
"""Strict paired analysis for the TER root-partition benchmark schema v1."""

from __future__ import annotations

import argparse
import json
import math
import random
import sys
from pathlib import Path
from typing import Any, Iterable


SCHEMA_VERSION = 1
BOOTSTRAP_RESAMPLES = 10_000
DEFAULT_BOOTSTRAP_SEED = 0x5EED_C0DE
REQUIRED_FIELDS = (
    "schema_version",
    "backend",
    "N",
    "bits",
    "shift",
    "threads_requested",
    "threads_used",
    "distribution",
    "seed",
    "repetition",
    "elapsed_ns",
    "histogram_ns",
    "prefix_ns",
    "scatter_ns",
    "local_classification_ns",
    "block_permute_ns",
    "cleanup_ns",
    "scratch_peak_bytes",
    "record_scratch_bytes",
    "block_buffer_bytes",
    "records_classified",
    "records_written",
    "blocks_flushed",
    "blocks_permuted",
    "input_fingerprint",
    "output_bucket_fingerprint",
    "status",
)
PAIR_FIELDS = (
    "N",
    "bits",
    "shift",
    "threads_requested",
    "distribution",
    "seed",
    "repetition",
    "input_fingerprint",
)
CAPTURE_FIELDS = (
    "dataset",
    "capture_generation_ns",
    "root_n",
    "root_seed",
    "root_mode",
    "root_target_mode",
    "capture_solver_status",
    "capture_status",
    "capture_A_size",
    "capture_B_size",
    "capture_C_size",
    "capture_r1",
    "capture_Q",
    "capture_list",
)
CAPTURE_PAIR_FIELDS = tuple(
    field for field in CAPTURE_FIELDS if field != "capture_generation_ns"
)
CAPTURE_SOLVER_STATUSES = {
    "Completed",
    "Found",
    "NoWitnessThisAttempt",
    "AttemptLimitReached",
    "InvalidConfiguration",
    "InvalidShape",
    "GenerationIncomplete",
    "ResourceLimit",
    "Cancelled",
    "InvariantViolation",
    "NotApplicable",
    "UnsupportedWidth",
}


class AnalysisError(ValueError):
    """Raised when input cannot support a valid paired comparison."""


def _is_integer(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def validate_row(row: Any, source: str) -> dict[str, Any]:
    if not isinstance(row, dict):
        raise AnalysisError(f"{source}: JSON value must be an object")
    missing = [field for field in REQUIRED_FIELDS if field not in row]
    if missing:
        raise AnalysisError(f"{source}: missing required fields: {', '.join(missing)}")
    if row["schema_version"] != SCHEMA_VERSION:
        raise AnalysisError(f"{source}: unsupported schema_version {row['schema_version']!r}")
    if row["backend"] not in ("current-scatter", "block-inplace"):
        raise AnalysisError(f"{source}: invalid backend {row['backend']!r}")
    if row["status"] != "Completed":
        raise AnalysisError(f"{source}: non-comparable status {row['status']!r}")
    integer_fields = (
        "N",
        "bits",
        "shift",
        "threads_requested",
        "threads_used",
        "seed",
        "repetition",
        "elapsed_ns",
        "histogram_ns",
        "prefix_ns",
        "scatter_ns",
        "local_classification_ns",
        "block_permute_ns",
        "cleanup_ns",
        "scratch_peak_bytes",
        "record_scratch_bytes",
        "block_buffer_bytes",
        "records_classified",
        "records_written",
        "blocks_flushed",
        "blocks_permuted",
    )
    for field in integer_fields:
        if not _is_integer(row[field]) or row[field] < 0:
            raise AnalysisError(f"{source}: {field} must be a nonnegative integer")
    if row["elapsed_ns"] <= 0:
        raise AnalysisError(f"{source}: elapsed_ns must be positive")
    if row["N"] <= 0 or row["threads_requested"] <= 0 or row["threads_used"] <= 0:
        raise AnalysisError(f"{source}: N and thread counts must be positive")
    if row["bits"] not in (8, 12, 16):
        raise AnalysisError(f"{source}: bits must be 8, 12, or 16")
    if row["backend"] == "block-inplace" and row["bits"] != 8:
        raise AnalysisError(f"{source}: block-inplace is comparable only at bits=8")
    if row["scratch_peak_bytes"] <= 0:
        raise AnalysisError(f"{source}: scratch_peak_bytes must be positive")
    for field in ("distribution", "input_fingerprint", "output_bucket_fingerprint"):
        if not isinstance(row[field], str) or not row[field]:
            raise AnalysisError(f"{source}: {field} must be a nonempty string")
    present_capture_fields = [field for field in CAPTURE_FIELDS if field in row]
    if present_capture_fields:
        missing_capture_fields = [field for field in CAPTURE_FIELDS if field not in row]
        if missing_capture_fields:
            raise AnalysisError(
                f"{source}: incomplete captured-root fields: {', '.join(missing_capture_fields)}"
            )
        if row["dataset"] != "captured-root":
            raise AnalysisError(f"{source}: capture metadata requires dataset captured-root")
        for field in (
            "capture_generation_ns",
            "root_n",
            "root_seed",
            "capture_A_size",
            "capture_B_size",
            "capture_C_size",
            "capture_r1",
            "capture_Q",
        ):
            if not _is_integer(row[field]) or row[field] < 0:
                raise AnalysisError(f"{source}: {field} must be a nonnegative integer")
        if row["capture_generation_ns"] <= 0 or row["root_n"] <= 0:
            raise AnalysisError(f"{source}: capture time and root_n must be positive")
        if row["root_mode"] not in ("fixture", "full"):
            raise AnalysisError(f"{source}: invalid root_mode {row['root_mode']!r}")
        if row["root_target_mode"] not in ("random", "guided"):
            raise AnalysisError(f"{source}: unsupported root_target_mode")
        if row["capture_solver_status"] not in CAPTURE_SOLVER_STATUSES:
            raise AnalysisError(f"{source}: invalid capture_solver_status")
        if row["capture_status"] != "Completed":
            raise AnalysisError(f"{source}: capture itself did not complete")
        if row["capture_list"] not in ("A", "B", "C"):
            raise AnalysisError(f"{source}: invalid capture_list {row['capture_list']!r}")
        selected_size = row[f"capture_{row['capture_list']}_size"]
        if row["N"] != selected_size or selected_size <= 0:
            raise AnalysisError(f"{source}: N does not match the selected capture list")
        if row["seed"] != row["root_seed"]:
            raise AnalysisError(f"{source}: seed/root_seed mismatch")
        if row["capture_r1"] > row["root_n"] or row["capture_Q"] != (
            row["root_n"] - row["capture_r1"]
        ):
            raise AnalysisError(f"{source}: inconsistent captured quotient geometry")
        if row["shift"] + row["bits"] > min(32, row["capture_Q"]):
            raise AnalysisError(f"{source}: partition digit exceeds captured low quotient")
        if row["distribution"] != f"captured-root-{row['capture_list']}":
            raise AnalysisError(f"{source}: captured distribution/list mismatch")
    return row


def read_rows(paths: Iterable[Path]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for path in paths:
        try:
            with path.open("r", encoding="utf-8") as handle:
                for line_number, line in enumerate(handle, 1):
                    if not line.strip():
                        raise AnalysisError(f"{path}:{line_number}: blank JSONL line")
                    try:
                        parsed = json.loads(line)
                    except json.JSONDecodeError as error:
                        raise AnalysisError(f"{path}:{line_number}: invalid JSON: {error}") from error
                    rows.append(validate_row(parsed, f"{path}:{line_number}"))
        except OSError as error:
            raise AnalysisError(f"{path}: could not read input: {error}") from error
    if not rows:
        raise AnalysisError("no benchmark rows were supplied")
    return rows


def pair_rows(rows: Iterable[dict[str, Any]]) -> list[tuple[dict[str, Any], dict[str, Any]]]:
    indexed: dict[tuple[Any, ...], dict[str, dict[str, Any]]] = {}
    for row_number, raw_row in enumerate(rows, 1):
        row = validate_row(raw_row, f"row {row_number}")
        identity = tuple(row[field] for field in PAIR_FIELDS) + tuple(
            row.get(field) for field in CAPTURE_PAIR_FIELDS
        )
        by_backend = indexed.setdefault(identity, {})
        backend = row["backend"]
        if backend in by_backend:
            raise AnalysisError(f"duplicate dataset/backend key: {identity!r} / {backend}")
        by_backend[backend] = row

    pairs: list[tuple[dict[str, Any], dict[str, Any]]] = []
    for identity in sorted(indexed, key=repr):
        by_backend = indexed[identity]
        if set(by_backend) != {"current-scatter", "block-inplace"}:
            raise AnalysisError(f"unpaired dataset: {identity!r}")
        current = by_backend["current-scatter"]
        candidate = by_backend["block-inplace"]
        if current["status"] != candidate["status"]:
            raise AnalysisError(f"mismatched statuses for dataset: {identity!r}")
        if current["input_fingerprint"] != candidate["input_fingerprint"]:
            raise AnalysisError(f"mismatched input fingerprints for dataset: {identity!r}")
        if current["output_bucket_fingerprint"] != candidate["output_bucket_fingerprint"]:
            raise AnalysisError(f"mismatched output fingerprints for dataset: {identity!r}")
        if current["threads_used"] != candidate["threads_used"]:
            raise AnalysisError(f"mismatched effective thread counts for dataset: {identity!r}")
        pairs.append((current, candidate))
    return pairs


def _percentile(sorted_values: list[float], probability: float) -> float:
    position = probability * (len(sorted_values) - 1)
    lower = int(math.floor(position))
    upper = int(math.ceil(position))
    if lower == upper:
        return sorted_values[lower]
    fraction = position - lower
    return sorted_values[lower] * (1.0 - fraction) + sorted_values[upper] * fraction


def analyse_pairs(
    pairs: list[tuple[dict[str, Any], dict[str, Any]]], bootstrap_seed: int
) -> dict[str, Any]:
    if not pairs:
        raise AnalysisError("no paired datasets were supplied")
    log_speedups = [
        math.log(current["elapsed_ns"] / candidate["elapsed_ns"])
        for current, candidate in pairs
    ]
    geometric_mean = math.exp(math.fsum(log_speedups) / len(log_speedups))
    speedups = sorted(
        current["elapsed_ns"] / candidate["elapsed_ns"]
        for current, candidate in pairs
    )
    candidate_time_ratios = sorted(
        candidate["elapsed_ns"] / current["elapsed_ns"]
        for current, candidate in pairs
    )
    candidate_scratch_ratios = sorted(
        candidate["scratch_peak_bytes"] / current["scratch_peak_bytes"]
        for current, candidate in pairs
    )
    generator = random.Random(bootstrap_seed)
    sample_size = len(log_speedups)
    bootstrap_means = []
    for _ in range(BOOTSTRAP_RESAMPLES):
        sample_sum = math.fsum(
            log_speedups[generator.randrange(sample_size)] for _ in range(sample_size)
        )
        bootstrap_means.append(sample_sum / sample_size)
    bootstrap_means.sort()
    return {
        "schema_version": SCHEMA_VERSION,
        "count": len(log_speedups),
        "raw_pairing_count": len(pairs),
        "geometric_mean_speedup": geometric_mean,
        "median_current_over_candidate_elapsed_ratio": _percentile(speedups, 0.5),
        "p95_candidate_over_current_elapsed_ratio": _percentile(
            candidate_time_ratios, 0.95
        ),
        "median_candidate_over_current_scratch_peak_bytes_ratio": _percentile(
            candidate_scratch_ratios, 0.5
        ),
        "worst_candidate_over_current_scratch_peak_bytes_ratio": max(
            candidate_scratch_ratios
        ),
        "bootstrap_95_ci": [
            math.exp(_percentile(bootstrap_means, 0.025)),
            math.exp(_percentile(bootstrap_means, 0.975)),
        ],
        "bootstrap_resamples": BOOTSTRAP_RESAMPLES,
        "bootstrap_seed": bootstrap_seed,
    }


def analyse_grouped_by_threads(
    pairs: list[tuple[dict[str, Any], dict[str, Any]]], bootstrap_seed: int
) -> dict[str, Any]:
    result = analyse_pairs(pairs, bootstrap_seed)
    thread_counts = sorted({current["threads_requested"] for current, _ in pairs})
    result["by_threads_requested"] = {
        str(thread_count): analyse_pairs(
            [
                pair
                for pair in pairs
                if pair[0]["threads_requested"] == thread_count
            ],
            bootstrap_seed,
        )
        for thread_count in thread_counts
    }
    return result


def _fixture_row(backend: str, repetition: int, elapsed_ns: int) -> dict[str, Any]:
    row = {field: 0 for field in REQUIRED_FIELDS}
    row.update(
        {
            "schema_version": SCHEMA_VERSION,
            "backend": backend,
            "N": 100,
            "bits": 8,
            "shift": 0,
            "threads_requested": 2,
            "threads_used": 2,
            "distribution": "uniform",
            "seed": 7,
            "repetition": repetition,
            "elapsed_ns": elapsed_ns,
            "scratch_peak_bytes": 1_000,
            "records_classified": 100,
            "records_written": 100,
            "input_fingerprint": f"input-{repetition}",
            "output_bucket_fingerprint": f"output-{repetition}",
            "status": "Completed",
        }
    )
    return row


def self_test() -> None:
    rows = [
        _fixture_row("block-inplace", 1, 100),
        _fixture_row("current-scatter", 0, 200),
        _fixture_row("current-scatter", 1, 400),
        _fixture_row("block-inplace", 0, 100),
    ]
    rows[0]["scratch_peak_bytes"] = 500
    rows[3]["scratch_peak_bytes"] = 250
    pairs = pair_rows(rows)
    result = analyse_pairs(pairs, 1234)
    expected = math.sqrt(2.0 * 4.0)
    if len(pairs) != 2 or not math.isclose(result["geometric_mean_speedup"], expected):
        raise AssertionError("deterministic pairing/geometric-mean fixture failed")
    expected_metrics = {
        "median_current_over_candidate_elapsed_ratio": 3.0,
        "p95_candidate_over_current_elapsed_ratio": 0.4875,
        "median_candidate_over_current_scratch_peak_bytes_ratio": 0.375,
        "worst_candidate_over_current_scratch_peak_bytes_ratio": 0.5,
    }
    for name, expected_value in expected_metrics.items():
        if not math.isclose(result[name], expected_value):
            raise AssertionError(f"{name} fixture failed: {result[name]!r}")
    grouped = analyse_grouped_by_threads(pairs, 1234)
    if set(grouped["by_threads_requested"]) != {"2"} or (
        grouped["by_threads_requested"]["2"]["count"] != 2
    ):
        raise AssertionError("thread-grouped analysis fixture failed")
    try:
        pair_rows(rows[:-1])
    except AnalysisError as error:
        if "unpaired dataset" not in str(error):
            raise
    else:
        raise AssertionError("unpaired fixture was accepted")
    captured = _fixture_row("current-scatter", 0, 100)
    captured.update(
        {
            "N": 19_109,
            "distribution": "captured-root-A",
            "seed": 19_797_034_556_212_562,
            "records_classified": 19_109,
            "records_written": 19_109,
            "dataset": "captured-root",
            "capture_generation_ns": 654_063_500,
            "root_n": 72,
            "root_seed": 19_797_034_556_212_562,
            "root_mode": "full",
            "root_target_mode": "random",
            "capture_solver_status": "AttemptLimitReached",
            "capture_status": "Completed",
            "capture_A_size": 19_109,
            "capture_B_size": 32_496,
            "capture_C_size": 33_505,
            "capture_r1": 25,
            "capture_Q": 47,
            "capture_list": "A",
        }
    )
    validate_row(captured, "captured self-test")
    captured_candidate = dict(captured)
    captured_candidate["backend"] = "block-inplace"
    captured_candidate["elapsed_ns"] = 50
    captured_candidate["capture_generation_ns"] += 1234
    if len(pair_rows([captured, captured_candidate])) != 1:
        raise AssertionError("captured rows with untimed generation variance did not pair")
    invalid_solver_status = dict(captured)
    invalid_solver_status["capture_solver_status"] = "garbage"
    try:
        validate_row(invalid_solver_status, "invalid captured status self-test")
    except AnalysisError as error:
        if "invalid capture_solver_status" not in str(error):
            raise
    else:
        raise AssertionError("invalid captured solver status was accepted")
    invalid_candidate_width = _fixture_row("block-inplace", 0, 50)
    invalid_candidate_width["bits"] = 12
    try:
        validate_row(invalid_candidate_width, "invalid candidate width self-test")
    except AnalysisError as error:
        if "block-inplace is comparable only at bits=8" not in str(error):
            raise
    else:
        raise AssertionError("non-radix8 BlockInPlace row was accepted")
    zero_scratch = _fixture_row("current-scatter", 0, 100)
    zero_scratch["scratch_peak_bytes"] = 0
    try:
        validate_row(zero_scratch, "zero scratch self-test")
    except AnalysisError as error:
        if "scratch_peak_bytes must be positive" not in str(error):
            raise
    else:
        raise AssertionError("zero scratch row was accepted")
    print("analyse_root_partition self-test passed")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", nargs="*", type=Path, help="benchmark JSONL files")
    parser.add_argument("--bootstrap-seed", type=int, default=DEFAULT_BOOTSTRAP_SEED)
    parser.add_argument("--group-by-threads", action="store_true")
    parser.add_argument("--output", type=Path, help="write the exact analysis JSON")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        if args.inputs:
            parser.error("--self-test does not accept input files")
        self_test()
        return 0
    if not args.inputs:
        parser.error("at least one JSONL input is required")
    pairs = pair_rows(read_rows(args.inputs))
    result = (
        analyse_grouped_by_threads(pairs, args.bootstrap_seed)
        if args.group_by_threads
        else analyse_pairs(pairs, args.bootstrap_seed)
    )
    serialized = json.dumps(result, sort_keys=True, separators=(",", ":"))
    if args.output is None:
        print(serialized)
    else:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(serialized + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AnalysisError as error:
        print(f"analyse_root_partition: {error}", file=sys.stderr)
        raise SystemExit(2) from error
