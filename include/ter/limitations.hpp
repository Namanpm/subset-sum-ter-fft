#pragma once

// Authoritative TER runtime-policy registry.
//
// This file owns deliberate runtime limits, resource ceilings, applicability
// gates, benchmark safety caps, and checked resource arithmetic for TER.
// Algorithm implementations own their local accounting and validation, but
// must not redeclare these policies. Exceeding a limit is not necessarily a
// solver failure: an unavailable optional backend may return NotApplicable
// while its exact fallback remains available.
//
// Cancellation is a cooperative runtime event, not a numeric ceiling. Its
// checks deliberately stay inside generation and merge loops and retain
// Status::Cancelled. Root/audit/provenance failures likewise remain their local
// invariant classifications rather than being relabelled as resource limits.

#include "ter/types.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace ter::limits {

// Unit used by all byte-valued runtime defaults below.
inline constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;

namespace checked {

// Canonical overflow-safe unsigned resource arithmetic. Overflow is a
// representability/resource refusal at the calling API; this helper deliberately
// does not choose a terminal status.
[[nodiscard]] constexpr bool checked_add(const std::uint64_t left,
                                         const std::uint64_t right,
                                         std::uint64_t* const out) noexcept {
  if (out == nullptr || left > std::numeric_limits<std::uint64_t>::max() - right) {
    return false;
  }
  *out = left + right;
  return true;
}

[[nodiscard]] constexpr bool checked_add(const std::uint64_t left,
                                         const std::uint64_t right,
                                         std::uint64_t& out) noexcept {
  return checked_add(left, right, &out);
}

[[nodiscard]] constexpr bool checked_multiply(const std::uint64_t left,
                                              const std::uint64_t right,
                                              std::uint64_t* const out) noexcept {
  if (out == nullptr ||
      (left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left)) {
    return false;
  }
  *out = left * right;
  return true;
}

[[nodiscard]] constexpr bool checked_multiply(const std::uint64_t left,
                                              const std::uint64_t right,
                                              std::uint64_t& out) noexcept {
  return checked_multiply(left, right, &out);
}

// Adds one count * element_size allocation to a live byte total. This is the
// canonical building block for stage-local peak accounting.
[[nodiscard]] constexpr bool checked_add_bytes(std::uint64_t* const total,
                                               const std::uint64_t count,
                                               const std::uint64_t element_size) noexcept {
  if (total == nullptr) {
    return false;
  }
  std::uint64_t bytes = 0U;
  return checked_multiply(count, element_size, &bytes) && checked_add(*total, bytes, total);
}

[[nodiscard]] constexpr bool checked_accumulate(std::uint64_t& total,
                                                const std::uint64_t value) noexcept {
  return checked_add(total, value, total);
}

// Closed memory-budget check. A zero ceiling is interpreted by individual APIs
// where documented; callers that use zero as "no ceiling" must keep that local
// API convention before calling this checked finite-budget helper.
[[nodiscard]] constexpr bool fits_memory_budget(const std::uint64_t used,
                                                const std::uint64_t requested,
                                                const std::uint64_t limit) noexcept {
  std::uint64_t total = 0U;
  return checked_add(used, requested, &total) && total <= limit;
}

[[nodiscard]] constexpr bool remaining_memory(const std::uint64_t used,
                                              const std::uint64_t limit,
                                              std::uint64_t* const remaining) noexcept {
  if (remaining == nullptr || used > limit) {
    return false;
  }
  *remaining = limit - used;
  return true;
}

[[nodiscard]] constexpr bool fits_size_t(const std::uint64_t value) noexcept {
  return value <= static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max());
}

}  // namespace checked

// Shared exact representation gate, used by tiled and NTT policies.
[[nodiscard]] constexpr bool is_power_of_two(const std::uint64_t value) noexcept {
  return value != 0U && (value & (value - 1U)) == 0U;
}

[[nodiscard]] constexpr std::uint32_t power_of_two_exponent(
    const std::uint64_t value) noexcept {
  return is_power_of_two(value) ? static_cast<std::uint32_t>(std::countr_zero(value)) : 0U;
}

namespace arithmetic {

// Modular quotient arithmetic is stored in one unsigned machine word. These
// are representational limits, not TER shape restrictions. Configuration
// callers keep their existing InvalidConfiguration result when they fail.
inline constexpr std::uint32_t kMachineWordBits = 64U;
inline constexpr std::uint32_t kMaximumModulusBits = kMachineWordBits - 1U;
inline constexpr std::uint32_t kMaximumQuotientBits = 32U;

[[nodiscard]] constexpr bool modulus_bits_supported(const std::uint32_t n,
                                                     const std::uint32_t b1) noexcept {
  return n != 0U && b1 != 0U && b1 < n && b1 <= kMaximumModulusBits;
}

[[nodiscard]] constexpr bool quotient_arithmetic_supported(
    const std::uint32_t n,
    const std::uint32_t b2,
    const std::uint32_t b1) noexcept {
  return b2 != 0U && b2 < b1 && modulus_bits_supported(n, b1) &&
         b1 - b2 <= kMaximumQuotientBits;
}

}  // namespace arithmetic

namespace full_ter {

// Finite TER construction rule, not a claim about every asymptotic TER
// parameterization. Violations are InvalidShape/InvalidConfiguration at the
// particular caller.
inline constexpr std::uint32_t kFiniteDimensionDivisor = 36U;

// User-configurable Full TER defaults. Zero limits are invalid configuration.
// Exhausting attempts is AttemptLimitReached; breaching the memory budget is
// ResourceLimit. Neither outcome implies UNSAT.
inline constexpr std::uint32_t kDefaultAttemptLimit = 1U;
inline constexpr std::uint64_t kDefaultMemoryBytes = 512ULL * kMiB;

// Fixture-only leaf-list cap. This bounds the finite test harness, not the
// classical Full TER mathematics. A fixture expansion past it is ResourceLimit.
inline constexpr std::uint64_t kDefaultFixtureLevel3Limit = 64ULL;

[[nodiscard]] constexpr bool finite_dimension_supported(const std::uint32_t n) noexcept {
  return n != 0U && n % kFiniteDimensionDivisor == 0U;
}

[[nodiscard]] constexpr bool modulus_width_supported(const std::uint32_t n,
                                                      const std::uint32_t modulus_bits) noexcept {
  return modulus_bits != 0U && modulus_bits <= n;
}

[[nodiscard]] constexpr bool runtime_limit_supported(const std::uint64_t value) noexcept {
  return value != 0U;
}

[[nodiscard]] constexpr bool instance_word_layout_supported(
    const std::uint32_t n,
    const std::uint64_t words_per_value) noexcept {
  return n != 0U && words_per_value == (static_cast<std::uint64_t>(n) + 63U) / 64U;
}

// The complete L3 cardinality is derived locally from TER's exact shape. This
// helper documents its representability gate; failure remains ResourceLimit.
[[nodiscard]] constexpr bool complete_level3_cardinality_representable(
    const bool cardinality_overflow) noexcept {
  return !cardinality_overflow;
}

}  // namespace full_ter

namespace generator {

// Defaults for the small authentic snapshot generator. They are practical
// generator settings, all user-overridable through SnapshotConfig.
inline constexpr std::uint32_t kDefaultN = 36U;
inline constexpr std::uint8_t kDefaultB2 = 6U;
inline constexpr std::uint8_t kDefaultB1 = 10U;
inline constexpr std::uint64_t kDefaultSeed = 1U;
inline constexpr std::uint32_t kDefaultRequestedListSize = 1U;
inline constexpr std::uint32_t kDefaultHalfPoolSize = 64U;
inline constexpr std::uint32_t kDefaultMaxHalfPoolSize = 1'024U;
inline constexpr std::uint64_t kDefaultAttemptLimit = 1'000'000ULL;
inline constexpr std::uint64_t kDefaultMemoryBytes = 256ULL * kMiB;
// Exhaustive reference generation is a finite correctness tool. Beyond n=36
// it refuses with ResourceLimit instead of trying to materialize its domain.
inline constexpr std::uint32_t kExhaustiveReferenceMaximumN = 36U;

[[nodiscard]] constexpr bool nonzero_requested_list_size(const std::uint64_t size) noexcept {
  return size != 0U;
}

[[nodiscard]] constexpr bool half_pool_range_supported(const std::uint64_t initial,
                                                       const std::uint64_t maximum) noexcept {
  return initial != 0U && maximum >= initial;
}

[[nodiscard]] constexpr bool exhaustive_reference_dimension_supported(
    const std::uint32_t n) noexcept {
  return n <= kExhaustiveReferenceMaximumN;
}

}  // namespace generator

namespace paper_generator {

// Declared benchmark/practical defaults for paper-rounded generation. These
// are not universal TER mathematical ceilings and remain config-overridable.
inline constexpr std::uint32_t kDefaultN = 360U;
inline constexpr std::uint64_t kDefaultMemoryBytes = 512ULL * kMiB;
inline constexpr std::uint32_t kDefaultHalfPoolSize = 4'096U;
inline constexpr std::uint64_t kDefaultAttemptLimit = 5'000'000ULL;

}  // namespace paper_generator

namespace oracle {

// Correctness-oracle safety cap, not a TER mathematical limit. Exceeding it
// refuses cubic execution with the caller's existing ResourceLimit behavior;
// it never implies UNSAT.
inline constexpr std::uint64_t kCubicCandidateTripleCap = 1'000'000ULL;

[[nodiscard]] constexpr bool cubic_work_supported(const std::uint64_t triples) noexcept {
  return triples <= kCubicCandidateTripleCap;
}

}  // namespace oracle

namespace attempts {

// Shared attempt-budget predicate. Exhaustion remains a caller-specific
// terminal result (for example AttemptLimitReached versus GenerationIncomplete)
// and never implies UNSAT.
[[nodiscard]] constexpr bool within_budget(const std::uint64_t attempts_used,
                                          const std::uint64_t attempt_limit) noexcept {
  return attempts_used < attempt_limit;
}

}  // namespace attempts

namespace tiled {

// Tiled Q3REPORT represents bucket offsets and doubled-record chains with
// 32-bit indices. Violations are NotApplicable for Q, InvalidConfiguration for
// tau/q values, or ResourceLimit for capacity/memory at the existing caller.
inline constexpr std::uint32_t kDefaultPreferredTileBits = 15U;
inline constexpr std::uint32_t kMaximumQuotientBits = arithmetic::kMaximumQuotientBits;
inline constexpr std::uint64_t kMaximumQuotientUniverse =
    std::uint64_t{1} << kMaximumQuotientBits;

[[nodiscard]] constexpr bool quotient_universe_supported(const std::uint64_t q) noexcept {
  return is_power_of_two(q) && q <= kMaximumQuotientUniverse;
}

[[nodiscard]] constexpr bool tau_supported(const std::uint64_t tau,
                                           const std::uint64_t q) noexcept {
  return tau < q;
}

[[nodiscard]] constexpr bool tile_configuration_supported(
    const std::uint64_t q,
    const std::uint32_t preferred_tile_bits) noexcept {
  return is_power_of_two(q) && preferred_tile_bits != 0U;
}

[[nodiscard]] constexpr bool input_entry_count_supported(
    const std::uint64_t count) noexcept {
  return count <= static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max());
}

[[nodiscard]] constexpr bool doubled_lookup_count_supported(
    const std::uint64_t lookup_count,
    std::uint64_t* const doubled_count) noexcept {
  return checked::checked_multiply(lookup_count, 2U, doubled_count) &&
         *doubled_count < static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max());
}

[[nodiscard]] constexpr bool doubled_lookup_count_supported(
    const std::uint64_t lookup_count) noexcept {
  std::uint64_t doubled = 0U;
  return doubled_lookup_count_supported(lookup_count, &doubled);
}

[[nodiscard]] constexpr bool logical_pair_count_supported(const std::uint64_t first,
                                                          const std::uint64_t second,
                                                          std::uint64_t* const pair_count) noexcept {
  return checked::checked_multiply(first, second, pair_count);
}

[[nodiscard]] constexpr bool logical_pair_count_supported(const std::uint64_t first,
                                                          const std::uint64_t second) noexcept {
  std::uint64_t ignored = 0U;
  return logical_pair_count_supported(first, second, &ignored);
}

[[nodiscard]] constexpr std::uint32_t tile_bits(const std::uint64_t q,
                                                const std::uint32_t preferred_tile_bits) noexcept {
  if (!is_power_of_two(q)) {
    return 0U;
  }
  const std::uint32_t q_bits = power_of_two_exponent(q);
  return preferred_tile_bits < q_bits ? preferred_tile_bits : q_bits;
}

// Optional native Full TER L2->L1 tiling uses a machine-word arithmetic path.
// Unsupported widths deliberately select the existing generic exact merge;
// they are not a solver failure.
[[nodiscard]] constexpr bool native_full_ter_l2_l1_supported(
    const std::uint32_t r1,
    const std::uint32_t r2) noexcept {
  return r2 != 0U && r2 < r1 && r1 < arithmetic::kMachineWordBits &&
         r1 - r2 <= kMaximumQuotientBits;
}

}  // namespace tiled

namespace ntt {

// The original single-prime count-only Q3REPORT/crossover NTT is exactly
// supported through 2^23. Larger Q is NotApplicable, not a correctness result.
inline constexpr std::uint32_t kQ3ReportMaximumPower = 23U;
inline constexpr std::uint64_t kQ3ReportMaximumLength =
    std::uint64_t{1} << kQ3ReportMaximumPower;
inline constexpr bool kQ3ReportIsCountOnly = true;
// Arithmetic parameters of the existing single-prime count-only core. They
// are implementation compatibility policy, not a witness-recovery capability.
inline constexpr std::uint64_t kQ3ReportPrime = 998'244'353ULL;
inline constexpr std::uint64_t kQ3ReportPrimitiveRoot = 3ULL;

[[nodiscard]] constexpr bool q3report_length_supported(const std::uint64_t length) noexcept {
  return is_power_of_two(length) && length <= kQ3ReportMaximumLength;
}

// The paper-scaling comparator has a distinct dual-prime implementation. Its
// 2^27 second-prime support is deliberately separate from the older 2^23 core.
inline constexpr std::uint64_t kPaperPrime1 = 998'244'353ULL;
inline constexpr std::uint64_t kPaperPrimitiveRoot1 = 3ULL;
inline constexpr std::uint32_t kPaperMaximumPower1 = 23U;
inline constexpr std::uint64_t kPaperPrime2 = 2'013'265'921ULL;
inline constexpr std::uint64_t kPaperPrimitiveRoot2 = 31ULL;
inline constexpr std::uint32_t kPaperMaximumPower2 = 27U;

}  // namespace ntt

namespace q3report {

// Frozen count-only experiment defaults. These are explicit audit/output
// ceilings rather than solver mathematics; callers retain configuration
// overrides and existing status mappings.
inline constexpr std::uint64_t kDefaultMemoryBytes = 64ULL * kMiB;
inline constexpr std::uint64_t kDefaultMaximumStructuralChecks = 200'000ULL;
inline constexpr std::uint64_t kDefaultMaximumStoredParents = 20'000ULL;
inline constexpr std::uint64_t kDefaultMaximumOracleTriples =
    oracle::kCubicCandidateTripleCap;
// Q3REPORT derives tau through widened signed 64-bit arithmetic, so r1=63 is
// deliberately excluded. This is narrower than the generic native Full TER
// tiled fallback gate and remains InvalidConfiguration at Q3 callers.
inline constexpr std::uint32_t kMaximumSignedNativeModulusBits = 62U;

[[nodiscard]] constexpr bool signed_quotient_arithmetic_supported(
    const std::uint32_t r1,
    const std::uint32_t r2) noexcept {
  return r2 != 0U && r2 < r1 && r1 <= kMaximumSignedNativeModulusBits &&
         r1 - r2 < arithmetic::kMachineWordBits;
}

[[nodiscard]] constexpr bool frozen_snapshot_arithmetic_supported(
    const std::uint32_t n,
    const std::uint32_t r1,
    const std::uint32_t r2,
    const std::uint32_t instance_b1) noexcept {
  return signed_quotient_arithmetic_supported(r1, r2) && r1 < n && instance_b1 == r1;
}

// Full TER capture feeds the native tiled reporter, whose quotient index is
// 32-bit. Unsupported widths remain NotApplicable rather than solver failure.
[[nodiscard]] constexpr bool native_capture_quotient_supported(
    const std::uint32_t r1,
    const std::uint32_t r2) noexcept {
  return signed_quotient_arithmetic_supported(r1, r2) &&
         r1 - r2 <= arithmetic::kMaximumQuotientBits;
}

[[nodiscard]] constexpr bool nonzero_resource_ceilings_supported(
    const std::uint64_t memory_bytes,
    const std::uint64_t structural_checks,
    const std::uint64_t stored_parents,
    const std::uint64_t oracle_triples) noexcept {
  return memory_bytes != 0U && structural_checks != 0U && stored_parents != 0U &&
         oracle_triples != 0U;
}

}  // namespace q3report

namespace crossover {

// TER-scaling-emulation defaults, not Full TER solver limits.
inline constexpr std::uint32_t kDefaultN = 36U;
inline constexpr std::uint32_t kDefaultListSize = 64U;
inline constexpr std::uint8_t kDefaultR2 = 6U;
inline constexpr std::uint8_t kDefaultR1 = 12U;
inline constexpr std::uint64_t kDefaultSeed = 1U;
inline constexpr std::uint32_t kDefaultHalfPoolSize = 512U;
inline constexpr std::uint32_t kDefaultMaxHalfPoolSize = 8'192U;
inline constexpr std::uint64_t kDefaultAttemptLimit = 4'000'000ULL;
inline constexpr std::uint64_t kDefaultMemoryBytes = 256ULL * kMiB;

[[nodiscard]] constexpr bool single_prime_snapshot_arithmetic_supported(
    const std::uint32_t n,
    const std::uint32_t r2,
    const std::uint32_t r1) noexcept {
  return arithmetic::quotient_arithmetic_supported(n, r2, r1) &&
         r1 - r2 <= ntt::kQ3ReportMaximumPower;
}

}  // namespace crossover

namespace paper_crossover {

// Paper-scaling comparator defaults. These are declared experiment resource
// budgets, not TER-theorem bounds. Zero remains invalid as in the existing API.
inline constexpr std::uint64_t kDefaultRequestedListSize = 64U;
inline constexpr std::uint64_t kDefaultSeed = 0x1001ULL;
inline constexpr std::uint32_t kDefaultInitialHalfPool = 256U;
inline constexpr std::uint32_t kDefaultMaximumHalfPool = 65'536U;
inline constexpr std::uint64_t kDefaultMemoryBytes = 2ULL * 1024ULL * kMiB;
inline constexpr std::uint32_t kDefaultWarmupRepetitions = 1U;
inline constexpr std::uint32_t kDefaultTimedRepetitions = 5U;
// Paper-scaling shape search caps. These are comparator practicality bounds;
// exhaustion returns GenerationIncomplete and is not a TER theorem.
inline constexpr std::uint64_t kShapeDomainCapacityCap = std::uint64_t{1} << 31U;
inline constexpr std::uint32_t kShapeSearchMaximumSteps = 64U;
// Per-pool sampling budget: a deterministic practical cap used only by the
// paper comparator's generator; failure remains GenerationIncomplete.
inline constexpr std::uint64_t kPoolSamplingAttemptMultiplier = 64U;
inline constexpr std::uint64_t kPoolSamplingAttemptSlack = 65'536U;

}  // namespace paper_crossover

namespace benchmark {

// Full TER smoke benchmark ceiling. It is intentionally lower than the
// FullTerConfig default so constrained behavior (including n=72 refusal) is
// observable. There is no global wall-clock timeout policy.
inline constexpr std::uint64_t kFullTerMemoryBytes = 256ULL * kMiB;

// Fixed resource settings for the two legacy count-only benchmark fixtures.
// They are benchmark sampling limits, not solver defaults.
inline constexpr std::uint32_t kQ3ReportBenchmarkInitialHalfPool = 384U;
inline constexpr std::uint32_t kQ3ReportBenchmarkMaximumHalfPool = 1'024U;
inline constexpr std::uint64_t kQ3ReportBenchmarkAttemptLimit = 2'000'000ULL;
inline constexpr std::uint32_t kCrossoverBenchmarkInitialHalfPool = 256U;
inline constexpr std::uint32_t kCrossoverBenchmarkMaximumHalfPool = 3'072U;
inline constexpr std::uint64_t kCrossoverBenchmarkAttemptLimit = 8'000'000ULL;

// The paper crossover's declared sampling schedule. The non-executable final
// point is reported because its required transform exceeds the verified 2^27
// paper-comparator ceiling; it is not silently skipped.
struct PaperCrossoverSchedule {
  std::uint32_t seeds = 0U;
  std::uint32_t repetitions = 0U;
  bool executable = false;
};

[[nodiscard]] constexpr PaperCrossoverSchedule paper_crossover_schedule_for(
    const std::uint64_t list_size,
    const bool extended_16384) noexcept {
  if (list_size <= 4'096U) {
    return {5U, 5U, true};
  }
  if (list_size == 8'192U) {
    return {5U, 3U, true};
  }
  if (list_size == 16'384U) {
    return extended_16384 ? PaperCrossoverSchedule{5U, 3U, true}
                          : PaperCrossoverSchedule{3U, 2U, true};
  }
  return {5U, 1U, false};
}

[[nodiscard]] constexpr bool full_ter_warmup_acceptable(const Status status) noexcept {
  return status == Status::Found || status == Status::AttemptLimitReached;
}

}  // namespace benchmark

}  // namespace ter::limits
