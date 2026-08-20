#pragma once

#include "ter/generator.hpp"
#include "ter/merge.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ter {

// This API is deliberately separate from SnapshotConfig.  Its output is a
// valid merge snapshot, but it is synthetic/adversarial evidence and must not
// be reported as a paper-faithful or FullTer workload.
enum class StressMode : std::uint8_t {
  BucketSkew,
  GroupSizeSkew,
  SolutionMultiplicity,
  QuotientBoundary,
  ArithmeticStructure,
  Composite,
};

enum class BucketProfile : std::uint8_t {
  Uniform,
  HotBuckets,
};

enum class StressSolutionMode : std::uint8_t {
  NoPlantedSolution,
  SinglePlantedSolution,
  MultiplePlantedSolutions,
  DenseSolutionStress,
};

struct BucketOccupancy {
  std::uint32_t bucket_count = 0;
  std::uint32_t nonempty_bucket_count = 0;
  std::uint64_t entry_count = 0;
  std::uint64_t maximum_occupancy = 0;
  std::uint64_t collision_count = 0;
  std::uint32_t hot_mass_percent = 0;
  double mean_occupancy = 0.0;
  double occupancy_variance = 0.0;
  double normalized_entropy = 0.0;
};

struct TerStressConfig {
  // The normal generator remains the sole source of TER-valid child vectors.
  SnapshotConfig source{};
  StressMode mode = StressMode::Composite;
  BucketProfile bucket_profile = BucketProfile::Uniform;
  StressSolutionMode solution_mode = StressSolutionMode::SinglePlantedSolution;

  // Each requested output list is selected from an independently valid child
  // pool.  The selection is quotient-aware; it never fabricates residues.
  std::array<std::uint32_t, 3> requested_list_sizes{12U, 12U, 12U};
  std::uint32_t candidate_pool_size = 48U;
  std::uint8_t prefilter_bits = 2U;
  std::uint32_t hot_bucket_count = 1U;
  std::uint32_t target_hot_mass_percent = 75U;
  std::uint32_t hot_mass_tolerance_percent = 10U;

  // Used only by StressMode::QuotientBoundary.  A live fixture accepts the
  // current exact native widths (1..32); describe_quotient_boundary() records
  // an unsupported wider boundary without truncating it.
  std::uint8_t quotient_boundary_bits = 0U;

  // Multiple/Dense modes certify witnesses by the independent cubic oracle.
  // They never claim an exact global solution count when the oracle cannot run.
  std::uint32_t required_distinct_witnesses = 1U;
};

struct StressGenerationTelemetry {
  std::array<std::uint32_t, 3> requested_list_sizes{};
  std::array<std::uint32_t, 3> actual_list_sizes{};
  std::uint8_t quotient_bits = 0;
  std::uint8_t prefilter_bits = 0;
  BucketOccupancy buckets{};
  std::uint32_t deliberately_planted_witnesses = 0;
  std::uint32_t guaranteed_planted_witnesses = 0;
  std::uint32_t certified_witnesses = 0;
  bool exact_solution_count_known = false;
  std::uint64_t exact_solution_count = 0;
  std::uint64_t source_attempts = 0;
  std::uint64_t fixture_bytes = 0;
};

struct StressSnapshotResult {
  Status status = Status::InvalidConfiguration;
  InputSnapshot snapshot{};
  StressGenerationTelemetry telemetry{};
  std::vector<Provenance> certified_witnesses{};
  std::string message;
};

struct QuotientBoundaryReport {
  std::uint8_t quotient_bits = 0;
  bool native_merge_applicable = false;
  bool requires_widened_quotient_representation = false;
};

// Returns an explicit policy report.  q_bits=33 is intentionally a rejection
// boundary for this 32-bit native merge representation, not a fake execution.
[[nodiscard]] QuotientBoundaryReport describe_quotient_boundary(
    std::uint8_t quotient_bits) noexcept;

[[nodiscard]] StressSnapshotResult build_stress_snapshot(const TerStressConfig& config);

}  // namespace ter
