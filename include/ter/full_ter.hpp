#pragma once

#include "ter/limitations.hpp"
#include "ter/model.hpp"
#include "ter/reference_vector.hpp"
#include "ter/shape.hpp"
#include "ter/types.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace ter {

struct ExactRepresentationCount {
  std::string decimal;
  std::uint32_t floor_log2 = 0;
};

struct PaperAsymptoticReference {
  double representation1_exponent = 1.1473;
  double representation2_exponent = 0.3396;
  double level3_exponent = 0.1922;
  double level2_exponent = 0.2147;
  double level1_exponent = 0.2221;
  double level0_exponent = 0.0;
  std::array<double, 4> merge_work_exponents{0.1922, 0.2146, 0.24025, 0.23995};
  double time_exponent = 0.2400;
  double space_exponent = 0.2221;
};

struct FullTerParameters {
  ShapeFamily family = ShapeFamily::FiniteSanity;
  std::uint32_t n = 0;

  // Integer finite counterparts of epsilon*n from Tables 3 and 4.
  std::uint32_t epsilon1_level1 = 0;
  std::uint32_t epsilon0_level1 = 0;
  std::uint32_t epsilon1_level2 = 0;
  std::uint32_t epsilon0_level2 = 0;
  std::uint32_t epsilon_negative1_level2 = 0;

  DigitCounts level1{};
  DigitCounts level2{};
  DigitCounts level3_active_half{};
  ExactRepresentationCount representation1{};
  ExactRepresentationCount representation2{};
  std::uint32_t r1 = 0;
  std::uint32_t r2 = 0;
  PaperAsymptoticReference asymptotic{};
};

struct FullTerParameterResult {
  Status status = Status::InvalidConfiguration;
  FullTerParameters value{};
  std::string message;
};

// Narrow M2 replay seam. When supplied through FullTerConfig, this receives
// the genuine three L1 lists immediately before the root merge. Residues stay
// in little-endian 32-bit words, so n>64 information is never truncated.
struct FullTerRootCapture {
  Status status = Status::InvalidConfiguration;
  FullTerParameters parameters{};
  std::uint64_t seed = 0;
  TerExecutionMode execution_mode = TerExecutionMode::FixtureTer;
  std::array<std::vector<ReferenceTernaryVector>, 3> children;
  std::array<std::vector<std::vector<std::uint32_t>>, 3> residues_words_le;
  std::array<std::vector<std::uint32_t>, 3> child_targets_words_le;
  std::vector<std::uint32_t> root_target_words_le;
  std::string message;
};

// Root-output ownership is policy, not TER arithmetic. CountOnly is exposed
// only through the narrow root-report APIs; solve_full_ter() rejects it
// because a SAT result must retain an independently verifiable witness.
enum class RootOutputPolicy : std::uint8_t {
  FirstWitness,
  CountOnly,
  FullCanonicalOutput,
};

struct FullTerConfig {
  ShapeFamily family = ShapeFamily::FiniteSanity;
  TerExecutionMode execution_mode = TerExecutionMode::FixtureTer;
  TerTargetMode target_mode = TerTargetMode::Random;
  std::uint64_t seed = 1;
  std::uint32_t attempt_limit = limits::full_ter::kDefaultAttemptLimit;
  std::uint64_t memory_limit_bytes = limits::full_ter::kDefaultMemoryBytes;
  std::uint64_t fixture_level3_limit = limits::full_ter::kDefaultFixtureLevel3Limit;
  // M2 fixed root prefilter policy. The effective width is clamped to the
  // root quotient width and the tiled reporter's 32-bit universe.
  std::uint32_t root_prefilter_bits = limits::full_ter::kDefaultRootPrefilterBits;
  std::uint32_t root_preferred_tile_bits =
      limits::full_ter::kDefaultRootPreferredTileBits;
  // Audited solve_full_ter() calls force FullCanonicalOutput so that the
  // existing oracle/provenance comparison remains complete.
  RootOutputPolicy root_output_policy = RootOutputPolicy::FirstWitness;
  FullTerRootCapture* root_capture = nullptr;
  bool audit = true;
  const std::atomic_bool* cancellation = nullptr;
};

struct FullTerStageTelemetry {
  std::uint64_t pair_candidates = 0;
  // Materialized pair records. Every tiled merge path keeps this at zero.
  std::uint64_t pair_records = 0;
  std::uint32_t quotient_bits = 0;
  std::uint32_t prefilter_bits = 0;
  std::uint64_t low_prefilter_candidates = 0;
  std::uint64_t full_quotient_rejects = 0;
  bool full_quotient_uses_bits_above_63 = false;
  std::uint64_t modular_candidates = 0;
  std::uint64_t structural_rejects = 0;
  std::uint64_t distribution_rejects = 0;
  std::uint64_t survivors_before_dedup = 0;
  std::uint64_t survivors_after_dedup = 0;
  std::uint64_t elapsed_ns = 0;
  std::uint64_t allocated_bytes = 0;
  std::uint64_t output_fingerprint = 0;
  bool tiled_used = false;
  std::uint32_t tile_bits = 0;
  std::uint64_t tiled_pair_probes = 0;
  std::uint64_t tiled_nonempty_windows = 0;
  std::uint64_t tiled_auxiliary_bytes = 0;
  std::uint64_t tiled_setup_ns = 0;
  std::uint64_t tiled_scan_ns = 0;
  // Root-stage fields. output_peak_bytes and working_peak_bytes use the
  // declared accounting model, including bounded vector growth overlap.
  RootOutputPolicy effective_root_output_policy = RootOutputPolicy::FullCanonicalOutput;
  std::uint64_t valid_root_count = 0;
  std::uint64_t first_witness_output_ceiling_bytes = 0;
  std::uint64_t output_peak_bytes = 0;
  std::uint64_t output_live_bytes = 0;
  std::uint64_t working_peak_bytes = 0;
  bool early_stopped = false;
};

struct FullTerAttemptTelemetry {
  std::uint64_t seed = 0;
  std::uint32_t attempt = 0;
  std::array<std::uint64_t, 9> level2_sizes{};
  std::array<std::uint64_t, 3> level1_sizes{};
  std::uint64_t level0_size = 0;
  std::uint64_t level2_allocated_bytes = 0;
  std::uint64_t level1_allocated_bytes = 0;
  std::uint64_t level0_allocated_bytes = 0;
  FullTerStageTelemetry level3_to_level2{};
  FullTerStageTelemetry level2_to_level1{};
  FullTerStageTelemetry level1_to_level0{};
  std::uint64_t verification_ns = 0;
  std::uint64_t total_ns = 0;
  Status status = Status::NoWitnessThisAttempt;
};

struct FullTerTelemetry {
  std::uint64_t seed = 0;
  std::uint32_t n = 0;
  std::uint32_t modulus_bits = 0;
  TerExecutionMode execution_mode = TerExecutionMode::FixtureTer;
  ShapeFamily family = ShapeFamily::FiniteSanity;
  FullTerParameters parameters{};
  std::uint64_t logical_level3_lists = 18;
  std::uint64_t physical_level3_lists = 2;
  std::array<std::uint64_t, 2> physical_level3_sizes{};
  std::uint64_t level3_generation_ns = 0;
  std::uint64_t level3_allocated_bytes = 0;
  std::uint64_t arithmetic_allocated_bytes = 0;
  std::uint64_t hot_entry_bytes = 0;
  std::uint64_t packed_vector_bytes = 0;
  std::uint64_t peak_working_bytes = 0;
  std::uint64_t total_solver_ns = 0;
  std::uint64_t fingerprint = 0;
  bool witness_found = false;
  Status terminal_status = Status::InvalidConfiguration;
  std::vector<FullTerAttemptTelemetry> attempts;
};

struct FullTerResult {
  Status status = Status::InvalidConfiguration;
  ReferenceTernaryVector witness;
  FullTerTelemetry telemetry{};
  std::string message;
};

// Narrow diagnostic-only result for replaying a captured L1 root. It is not a
// SAT solver result and deliberately never carries a subset-sum witness.
struct FullTerRootReportResult {
  Status status = Status::InvalidConfiguration;
  RootOutputPolicy effective_root_output_policy = RootOutputPolicy::CountOnly;
  // Exact pre-dedup accepted-triple multiplicity: every counted root passed
  // the full quotient, ternary-distribution, and full-modulus residue checks.
  std::uint64_t valid_root_count = 0;
  std::uint64_t retained_root_count = 0;
  FullTerStageTelemetry telemetry{};
  std::string message;
};

// Convenience projection for the count-only benchmark/report path.
struct FullTerRootCountResult {
  Status status = Status::InvalidConfiguration;
  // Same pre-dedup accepted-triple multiplicity as FullTerRootReportResult.
  std::uint64_t valid_root_count = 0;
  FullTerStageTelemetry telemetry{};
  std::string message;
};

// A narrowly scoped extraction seam for the count-only Q3REPORT experiment.
// It captures one genuine complete Full TER L2(A,B,C)->L1 group.  It is not a
// general solver hook and does not expose Full TER's internal packed entries.
struct FullTerL2CaptureResult {
  Status status = Status::InvalidConfiguration;
  InputSnapshot snapshot{};
  FullTerParameters parameters{};
  std::uint32_t group = 0;
  std::string message;
};

struct Level3Generation {
  std::vector<ReferenceTernaryVector> first_half;
  std::vector<ReferenceTernaryVector> second_half;
  std::uint64_t complete_cardinality = 0;
  bool complete_cardinality_overflow = false;
  bool complete = false;
};

struct Level3GenerationResult {
  Status status = Status::InvalidConfiguration;
  Level3Generation value{};
  std::string message;
};

[[nodiscard]] FullTerParameterResult derive_full_ter_parameters(
    ShapeFamily family,
    std::uint32_t n,
    std::uint32_t modulus_bits);

[[nodiscard]] Level3GenerationResult generate_level3_lists(
    const FullTerParameters& parameters,
    TerExecutionMode mode,
    std::uint64_t fixture_limit,
    std::uint64_t memory_limit_bytes);

[[nodiscard]] OperationResult verify_full_ter_witness(
    const Instance& instance,
    const ReferenceTernaryVector& witness);

[[nodiscard]] FullTerResult solve_full_ter(const Instance& instance,
                                           const FullTerConfig& config);

[[nodiscard]] FullTerRootReportResult report_full_ter_root(
    const FullTerRootCapture& capture,
    const FullTerConfig& config,
    RootOutputPolicy output_policy);

[[nodiscard]] FullTerRootCountResult count_full_ter_root(
    const FullTerRootCapture& capture,
    const FullTerConfig& config);

[[nodiscard]] FullTerL2CaptureResult capture_full_ter_l2_merge_snapshot(
    const Instance& instance,
    const FullTerConfig& config,
    std::uint32_t group);

}  // namespace ter
