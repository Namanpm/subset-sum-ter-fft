#pragma once

#include "ter/limitations.hpp"
#include "ter/model.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ter {

enum class Q3SnapshotFamily : std::uint8_t {
  AuthenticFullTer,
  DistributionMatched,
  FixedQMicrobenchmark,
};

enum class Q3ReportStatus : std::uint8_t {
  Completed,
  NotApplicable,
  InvalidSnapshot,
  InvalidConfiguration,
  ResourceLimit,
  ArithmeticSafetyFailure,
  InternalInvariantFailure,
};

struct FrozenL2MergeSnapshot {
  InputSnapshot input{};
  Q3SnapshotFamily family = Q3SnapshotFamily::DistributionMatched;
  std::uint64_t seed = 0;
  std::uint64_t fingerprint = 0;
};

struct FrozenL2MergeSnapshotResult {
  Q3ReportStatus status = Q3ReportStatus::InvalidConfiguration;
  FrozenL2MergeSnapshot value{};
  std::string message;
};

struct Q3ReportConfig {
  std::uint64_t memory_limit_bytes = limits::q3report::kDefaultMemoryBytes;
  // These are explicit exact-work/output ceilings, not heuristic cutoffs.
  std::uint64_t max_structural_checks = limits::q3report::kDefaultMaximumStructuralChecks;
  std::uint64_t max_stored_parents = limits::q3report::kDefaultMaximumStoredParents;
  std::uint64_t max_oracle_triples = limits::q3report::kDefaultMaximumOracleTriples;
  bool full_residue_audit = true;
  bool run_scalar_reference = true;
  bool run_ntt = true;
};

struct Q3Histogram {
  std::uint64_t population = 0;
  std::uint64_t zero = 0;
  std::uint64_t one = 0;
  std::uint64_t at_most_two = 0;
  std::uint64_t at_most_four = 0;
  std::uint64_t p50 = 0;
  std::uint64_t p90 = 0;
  std::uint64_t p99 = 0;
  std::uint64_t maximum = 0;
};

struct Q3TimingTelemetry {
  std::uint64_t snapshot_audit_ns = 0;
  std::uint64_t direct_arithmetic_ns = 0;
  std::uint64_t direct_structural_ns = 0;
  std::uint64_t direct_dedup_ns = 0;
  std::uint64_t scalar_convolution_ns = 0;
  std::uint64_t ntt_frequency_build_ns = 0;
  std::uint64_t ntt_transform_ns = 0;
  std::uint64_t ntt_reconstruction_ns = 0;
  std::uint64_t selected_target_lookup_ns = 0;
  std::uint64_t full_histogram_ns = 0;
};

struct Q3MemoryTelemetry {
  std::uint64_t direct_peak_bytes = 0;
  std::uint64_t ntt_persistent_bytes = 0;
  std::uint64_t ntt_scratch_bytes = 0;
  std::uint64_t crt_bytes = 0;
};

struct Q3ReportTelemetry {
  std::uint64_t a_size = 0;
  std::uint64_t b_size = 0;
  std::uint64_t c_size = 0;
  std::uint8_t r2 = 0;
  std::uint8_t r1 = 0;
  std::uint64_t q = 0;
  std::uint64_t tau = 0;
  std::uint64_t snapshot_fingerprint = 0;
  std::uint64_t pair_universe = 0;
  long double lambda = 0.0L;
  long double predicted_support_fraction = 0.0L;
  std::uint64_t occupied_pair_residues = 0;
  Q3Histogram all_residues{};
  std::uint64_t unique_selected_targets = 0;
  Q3Histogram unique_selected{};
  Q3Histogram c_weighted_selected{};
  std::uint64_t raw_arithmetic_triples = 0;
  std::uint64_t structurally_valid_triples = 0;
  std::uint64_t structural_rejects = 0;
  std::uint64_t distribution_rejects = 0;
  std::uint64_t parent_arithmetic_rejects = 0;
  std::uint64_t unique_parents = 0;
  bool independent_oracle_ran = false;
  bool exactness_passed = false;
  bool cheap_kill_passed = false;
  std::vector<std::uint64_t> scalar_pair_counts;
  std::vector<std::uint64_t> ntt_pair_counts;
  Q3TimingTelemetry timing{};
  Q3MemoryTelemetry memory{};
};

struct Q3ReportResult {
  Q3ReportStatus status = Q3ReportStatus::InvalidConfiguration;
  Q3ReportTelemetry telemetry{};
  std::string message;
};

[[nodiscard]] FrozenL2MergeSnapshotResult freeze_l2_merge_snapshot(
    InputSnapshot input,
    Q3SnapshotFamily family,
    std::uint64_t seed);

[[nodiscard]] Q3ReportResult run_q3report_count_only_experiment(
    const FrozenL2MergeSnapshot& snapshot,
    const Q3ReportConfig& config);

}  // namespace ter
