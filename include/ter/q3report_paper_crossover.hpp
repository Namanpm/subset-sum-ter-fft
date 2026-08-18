#pragma once

// TER-Q3REPORT paper-scaling dense-NTT crossover experiment (V2).
//
// This header declares one isolated, separately named screening experiment.  It
// does not alter Full TER solver semantics, it does not implement witness
// recovery, and it does not replace the historical q3report_audit experiment.
//
// The experiment answers exactly one question: does a reasonably optimized exact
// dense NTT beat direct arithmetic on Full-TER-L2-shaped data when L and Q
// follow the classical TER paper scaling relationship?

#include "ter/full_ter.hpp"
#include "ter/model.hpp"
#include "ter/types.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ter {

enum class PaperCrossoverStatus : std::uint8_t {
  Completed,
  NotApplicable,
  InvalidConfiguration,
  InvalidSnapshot,
  GenerationIncomplete,
  ResourceLimit,
  ArithmeticSafetyFailure,
  InternalInvariantFailure,
};

// FULLTER_L2_PAPER_SCALING_EMULATION is a controlled sample of actual Full TER
// L2-shaped vectors constructed from valid Full TER L3 half shapes, with
// arithmetic widths following the paper asymptotic L2/Q relationship.  It is
// NOT a complete Full TER execution and must never be labelled AUTHENTIC_FULLTER.
enum class PaperCrossoverFamily : std::uint8_t {
  FullTerL2PaperScalingEmulation,
  AuthenticFullTer,
};

[[nodiscard]] const char* paper_crossover_family_name(PaperCrossoverFamily family) noexcept;
[[nodiscard]] const char* paper_crossover_status_name(PaperCrossoverStatus status) noexcept;

// ---------------------------------------------------------------------------
// Section 2/3: paper scaling derivation.
// ---------------------------------------------------------------------------

struct PaperScalingPoint {
  std::uint64_t list_size = 0;
  // Metadata only.  effective_n is NOT a real finite Full TER dimension and is
  // never a licence to claim "authentic n = effective_n".
  double effective_n = 0.0;
  std::uint32_t q_bits = 0;
  std::uint32_t r2 = 0;
  std::uint32_t r1 = 0;
  std::uint64_t q = 0;
};

[[nodiscard]] PaperScalingPoint derive_paper_scaling_point(std::uint64_t list_size) noexcept;
[[nodiscard]] std::span<const std::uint64_t> declared_paper_scaling_ladder() noexcept;
[[nodiscard]] std::span<const std::uint64_t> declared_paper_scaling_seeds() noexcept;

// ---------------------------------------------------------------------------
// Section 11: verified NTT moduli.  No CRT, no dynamic prime search.
// ---------------------------------------------------------------------------

inline constexpr std::uint64_t kPaperNttPrime1 = 998'244'353ULL;
inline constexpr std::uint64_t kPaperNttRoot1 = 3ULL;
inline constexpr std::uint32_t kPaperNttMaxPower1 = 23U;
inline constexpr std::uint64_t kPaperNttPrime2 = 2'013'265'921ULL;
inline constexpr std::uint64_t kPaperNttRoot2 = 31ULL;
inline constexpr std::uint32_t kPaperNttMaxPower2 = 27U;

struct NttPrimeSelection {
  PaperCrossoverStatus status = PaperCrossoverStatus::NotApplicable;
  std::uint64_t prime = 0;
  std::uint64_t primitive_root = 0;
  std::uint32_t max_power = 0;
};

[[nodiscard]] NttPrimeSelection select_paper_crossover_prime(
    std::uint64_t q,
    std::uint64_t pair_universe) noexcept;

[[nodiscard]] std::uint64_t modular_power(std::uint64_t base,
                                          std::uint64_t exponent,
                                          std::uint64_t modulus) noexcept;

// Deterministic 32-bit primality decision (Miller-Rabin over a complete base set).
[[nodiscard]] bool is_prime_u32(std::uint64_t value) noexcept;

[[nodiscard]] bool is_primitive_root_u32(
    std::uint64_t root,
    std::uint64_t prime,
    std::span<const std::uint64_t> distinct_prime_factors) noexcept;

// ---------------------------------------------------------------------------
// Section 12/13: one bounded NTT kernel with a precomputed immutable plan.
// ---------------------------------------------------------------------------

struct NttPlan {
  std::uint64_t q = 0;
  std::uint64_t prime = 0;
  std::uint64_t primitive_root = 0;
  std::uint32_t log_q = 0;
  std::uint32_t inverse_length = 0;
  // Bit-reversal permutation data held as a compact two-level table: the
  // high_bits-wide reversal of every high_bits-wide index.  Both halves of a
  // log_q-bit index are reversed through this one immutable table.
  std::uint32_t reversal_bits = 0;
  std::vector<std::uint32_t> reversal_table;
  // Flattened forward stage twiddles: stage length `len` occupies
  // [len/2 - 1, len - 1) and holds w^0 .. w^(len/2 - 1).  Total q-1 entries.
  std::vector<std::uint32_t> forward_twiddles;

  [[nodiscard]] std::uint64_t plan_bytes() const noexcept;
  [[nodiscard]] std::uint32_t bit_reverse(std::uint32_t index) const noexcept;
  [[nodiscard]] std::uint32_t forward_twiddle(std::uint64_t length,
                                              std::uint64_t offset) const noexcept;
  [[nodiscard]] std::uint32_t inverse_twiddle(std::uint64_t length,
                                              std::uint64_t offset) const noexcept;
};

struct NttPlanResult {
  PaperCrossoverStatus status = PaperCrossoverStatus::InvalidConfiguration;
  NttPlan value{};
  std::string message;
};

[[nodiscard]] NttPlanResult build_ntt_plan(std::uint64_t q,
                                           std::uint64_t prime,
                                           std::uint64_t primitive_root);

struct PairCountResult {
  PaperCrossoverStatus status = PaperCrossoverStatus::InvalidConfiguration;
  std::vector<std::uint32_t> counts;
  std::string message;
};

// Exact cyclic pair counts over Z_Q from two quotient-residue lists.
[[nodiscard]] PairCountResult ntt_cyclic_pair_counts(
    const NttPlan& plan,
    const std::vector<std::uint32_t>& left,
    const std::vector<std::uint32_t>& right);

[[nodiscard]] PairCountResult direct_full_pair_counts(
    const std::vector<std::uint32_t>& left,
    const std::vector<std::uint32_t>& right,
    std::uint64_t q);

// ---------------------------------------------------------------------------
// Sections 4-9: the paper-scaling Full TER L2 snapshot.
// ---------------------------------------------------------------------------

struct PaperCrossoverSnapshotConfig {
  std::uint64_t requested_list_size = 64U;
  std::uint64_t seed = 0x1001ULL;
  std::uint32_t initial_half_pool = 256U;
  // Declared before any timing result was seen and never silently raised.
  std::uint32_t max_half_pool = 65'536U;
  std::uint64_t memory_limit_bytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
  // Deterministic test-only overrides.  Zero means "derive from the paper
  // ladder".  The benchmark never sets these.
  std::uint32_t override_shape_n = 0U;
  std::uint32_t override_r2 = 0U;
  std::uint32_t override_r1 = 0U;
};

struct PaperShapeSelection {
  PaperCrossoverStatus status = PaperCrossoverStatus::InvalidConfiguration;
  std::uint32_t shape_n = 0;
  FullTerParameters parameters{};
  // Saturating exact half-domain cardinality.  When `half_domain_saturated` is
  // true the true value is only known to be at least `half_domain`.
  std::uint64_t half_domain = 0;
  bool half_domain_saturated = false;
  std::uint32_t search_steps = 0;
  std::string message;
};

[[nodiscard]] PaperShapeSelection select_paper_shape(const PaperScalingPoint& point);

struct PaperSnapshotTelemetry {
  std::uint32_t shape_n = 0;
  std::uint64_t half_domain = 0;
  bool half_domain_saturated = false;
  std::uint32_t shape_search_steps = 0;
  std::array<std::uint32_t, 3> half_pool_size{};
  std::array<std::uint64_t, 3> compatible_pairs{};
  std::array<std::uint32_t, 3> pool_doublings{};
};

struct PaperCrossoverSnapshot {
  PaperCrossoverFamily family = PaperCrossoverFamily::FullTerL2PaperScalingEmulation;
  std::uint64_t seed = 0;
  std::uint64_t fingerprint = 0;
  std::uint64_t requested_list_size = 0;
  PaperScalingPoint point{};
  std::uint32_t shape_n = 0;
  FullTerParameters parameters{};
  InputSnapshot input{};
  PaperSnapshotTelemetry generation{};
};

struct PaperCrossoverSnapshotResult {
  PaperCrossoverStatus status = PaperCrossoverStatus::InvalidConfiguration;
  PaperCrossoverSnapshot value{};
  std::string message;
};

[[nodiscard]] PaperCrossoverSnapshotResult build_paper_scaling_snapshot(
    const PaperCrossoverSnapshotConfig& config);

// Adopts a genuine Full TER L2->L1 capture as an AUTHENTIC_FULLTER anchor.
[[nodiscard]] PaperCrossoverSnapshotResult adopt_authentic_full_ter_snapshot(
    InputSnapshot input,
    FullTerParameters parameters,
    std::uint64_t seed);

[[nodiscard]] std::uint64_t paper_crossover_fingerprint(
    const PaperCrossoverSnapshot& snapshot) noexcept;

struct PaperAuditResult {
  PaperCrossoverStatus status = PaperCrossoverStatus::InvalidSnapshot;
  std::string message;
};

// Recomputes every structural and arithmetic claim from raw digits.  Stored
// metadata is never the correctness authority.
[[nodiscard]] PaperAuditResult audit_paper_crossover_snapshot(
    const PaperCrossoverSnapshot& snapshot);

// ---------------------------------------------------------------------------
// Sections 16-20: the three-phase measurement.
// ---------------------------------------------------------------------------

struct PaperCrossoverConfig {
  std::uint64_t memory_limit_bytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
  std::uint32_t warmup_repetitions = 1U;
  std::uint32_t timed_repetitions = 5U;
  bool run_correctness_phase = true;
};

struct PaperTimingSummary {
  std::uint32_t samples = 0;
  std::uint64_t median_ns = 0;
  std::uint64_t minimum_ns = 0;
  std::uint64_t maximum_ns = 0;
};

struct PaperHistogram {
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

struct PaperMethodOutcome {
  PaperCrossoverStatus status = PaperCrossoverStatus::InvalidConfiguration;
  PaperTimingSummary total{};
  std::uint64_t peak_bytes = 0;
  std::string message;
};

struct PaperComponentTimings {
  PaperTimingSummary direct_full_setup{};
  PaperTimingSummary direct_full_pair_count{};
  PaperTimingSummary direct_full_lookup{};
  PaperTimingSummary direct_production_index{};
  PaperTimingSummary direct_production_scan{};
  PaperTimingSummary ntt_plan_build{};
  PaperTimingSummary ntt_frequency_build{};
  PaperTimingSummary ntt_bit_reversal{};
  PaperTimingSummary ntt_forward{};
  PaperTimingSummary ntt_pointwise{};
  PaperTimingSummary ntt_inverse{};
  PaperTimingSummary ntt_reconstruction{};
  PaperTimingSummary ntt_target_lookup{};
  // bit reversal + forward + pointwise + inverse + reconstruction.
  PaperTimingSummary ntt_transform{};
};

struct PaperCrossoverTelemetry {
  PaperCrossoverFamily family = PaperCrossoverFamily::FullTerL2PaperScalingEmulation;
  std::uint64_t seed = 0;
  std::uint64_t fingerprint = 0;
  std::uint64_t requested_list_size = 0;
  std::uint64_t a_size = 0;
  std::uint64_t b_size = 0;
  std::uint64_t c_size = 0;
  double effective_n = 0.0;
  std::uint32_t shape_n = 0;
  DigitCounts level2{};
  DigitCounts level3_active_half{};
  std::uint32_t r2 = 0;
  std::uint32_t r1 = 0;
  std::uint32_t q_bits = 0;
  std::uint64_t q = 0;
  std::uint64_t tau = 0;
  std::uint64_t pair_universe = 0;
  long double lambda = 0.0L;
  NttPrimeSelection prime{};

  // Selectivity (Section 24).
  std::uint64_t occupied_pair_residues = 0;
  long double support_fraction = 0.0L;
  long double poisson_support_estimate = 0.0L;  // heuristic only, not a theorem
  PaperHistogram selected_weighted{};
  std::uint64_t raw_arithmetic_triples = 0;

  // Correctness (Section 15/16).
  bool exactness_passed = false;
  bool pair_conservation_passed = false;
  bool triple_conservation_passed = false;
  bool cold_warm_identical = false;

  PaperMethodOutcome direct_full{};
  PaperMethodOutcome direct_production{};
  PaperMethodOutcome ntt_warm{};
  PaperMethodOutcome ntt_cold{};
  std::uint64_t ntt_three_merge_amortized_ns = 0;
  std::uint64_t ntt_plan_bytes = 0;
  std::uint64_t ntt_run_bytes = 0;
  std::uint64_t ntt_peak_bytes = 0;
  PaperComponentTimings components{};
};

struct PaperCrossoverResult {
  PaperCrossoverStatus status = PaperCrossoverStatus::InvalidConfiguration;
  PaperCrossoverTelemetry telemetry{};
  std::string message;
};

[[nodiscard]] PaperCrossoverResult run_paper_crossover_experiment(
    const PaperCrossoverSnapshot& snapshot,
    const PaperCrossoverConfig& config);

}  // namespace ter
