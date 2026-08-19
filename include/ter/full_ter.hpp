#pragma once

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

struct FullTerConfig {
  ShapeFamily family = ShapeFamily::FiniteSanity;
  TerExecutionMode execution_mode = TerExecutionMode::FixtureTer;
  TerTargetMode target_mode = TerTargetMode::Random;
  std::uint64_t seed = 1;
  std::uint32_t attempt_limit = 1;
  std::uint64_t memory_limit_bytes = 512ULL * 1024ULL * 1024ULL;
  std::uint64_t fixture_level3_limit = 64;
  bool audit = true;
  const std::atomic_bool* cancellation = nullptr;
};

struct FullTerStageTelemetry {
  std::uint64_t pair_candidates = 0;
  // Materialized pair records.  The tiled L2-to-L1 path keeps this at zero.
  std::uint64_t pair_records = 0;
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

[[nodiscard]] FullTerL2CaptureResult capture_full_ter_l2_merge_snapshot(
    const Instance& instance,
    const FullTerConfig& config,
    std::uint32_t group);

}  // namespace ter
