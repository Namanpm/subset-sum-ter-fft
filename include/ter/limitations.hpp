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

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace ter::limits {

// =============================================================================
// CENTRAL CONTROL PANEL -- EDIT POLICY VALUES HERE ONLY.
// =============================================================================
// Every configurable TER policy value has exactly one numeric definition in
// this block. The named constants below are compatibility aliases consumed by
// the solver, generators, experiments, and benchmarks. Changing a value here
// therefore changes every existing caller after recompilation.
//
// Values labelled "compatibility" or "representation" are not performance
// knobs: change them only with a matching algorithm/proof update. Config
// fields supplied at runtime still deliberately override their default values.
namespace control_panel {

inline constexpr std::uint64_t kBytesPerMiB = 1024ULL * 1024ULL;

namespace arithmetic {
inline constexpr std::uint32_t kMachineWordBits = 64U;  // representation
inline constexpr std::uint32_t kMaximumQuotientBits = 32U;  // representation
}  // namespace arithmetic

namespace full_ter {
inline constexpr std::uint32_t kFiniteDimensionDivisor = 36U;  // finite shape
inline constexpr std::uint32_t kDefaultAttemptLimit = 1U;
inline constexpr std::uint64_t kDefaultMemoryBytes = 512ULL * kBytesPerMiB;
inline constexpr std::uint64_t kDefaultFixtureLevel3Limit = 64ULL;
// M2 root prefilter defaults. They are deterministic implementation defaults,
// not measured optima; M3 is the only milestone allowed to tune them.
inline constexpr std::uint32_t kDefaultRootPrefilterBits = 24U;
inline constexpr std::uint32_t kDefaultRootPreferredTileBits = 15U;
}  // namespace full_ter

namespace root_partition {
inline constexpr std::uint32_t kDefaultThreads = 1U;
inline constexpr std::uint32_t kCurrentScatterMaximumBits = 16U;
inline constexpr std::uint32_t kBlockInPlaceBits = 8U;
inline constexpr std::uint32_t kDefaultBlockBytes = 2U * 1024U;
inline constexpr std::uint32_t kMinimumBlockBytes = 64U;
inline constexpr std::uint32_t kMaximumPhysicalBits = 16U;
inline constexpr std::uint32_t kMaximumProjectionBits = 32U;
}  // namespace root_partition

namespace generator {
inline constexpr std::uint32_t kDefaultN = 36U;
inline constexpr std::uint8_t kDefaultB2 = 6U;
inline constexpr std::uint8_t kDefaultB1 = 10U;
inline constexpr std::uint64_t kDefaultSeed = 1U;
inline constexpr std::uint32_t kDefaultRequestedListSize = 1U;
inline constexpr std::uint32_t kDefaultHalfPoolSize = 64U;
inline constexpr std::uint32_t kDefaultMaxHalfPoolSize = 1'024U;
inline constexpr std::uint64_t kDefaultAttemptLimit = 1'000'000ULL;
inline constexpr std::uint64_t kDefaultMemoryBytes = 256ULL * kBytesPerMiB;
inline constexpr std::uint32_t kExhaustiveReferenceMaximumN = 36U;
}  // namespace generator

namespace paper_generator {
inline constexpr std::uint32_t kDefaultN = 360U;
inline constexpr std::uint64_t kDefaultMemoryBytes = 512ULL * kBytesPerMiB;
inline constexpr std::uint32_t kDefaultHalfPoolSize = 4'096U;
inline constexpr std::uint64_t kDefaultAttemptLimit = 5'000'000ULL;
}  // namespace paper_generator

namespace oracle {
inline constexpr std::uint64_t kCubicCandidateTripleCap = 1'000'000ULL;
}  // namespace oracle

namespace tiled {
inline constexpr std::uint32_t kDefaultPreferredTileBits = 15U;
}  // namespace tiled

namespace ntt {
inline constexpr std::uint32_t kQ3ReportMaximumPower = 23U;  // compatibility
inline constexpr std::uint64_t kQ3ReportPrime = 998'244'353ULL;  // compatibility
inline constexpr std::uint64_t kQ3ReportPrimitiveRoot = 3ULL;  // compatibility
inline constexpr std::uint64_t kPaperPrime1 = 998'244'353ULL;  // compatibility
inline constexpr std::uint64_t kPaperPrimitiveRoot1 = 3ULL;  // compatibility
inline constexpr std::uint32_t kPaperMaximumPower1 = 23U;  // compatibility
inline constexpr std::uint64_t kPaperPrime2 = 2'013'265'921ULL;  // compatibility
inline constexpr std::uint64_t kPaperPrimitiveRoot2 = 31ULL;  // compatibility
inline constexpr std::uint32_t kPaperMaximumPower2 = 27U;  // compatibility
}  // namespace ntt

namespace q3report {
inline constexpr std::uint64_t kDefaultMemoryBytes = 64ULL * kBytesPerMiB;
inline constexpr std::uint64_t kDefaultMaximumStructuralChecks = 200'000ULL;
inline constexpr std::uint64_t kDefaultMaximumStoredParents = 20'000ULL;
inline constexpr std::uint32_t kMaximumSignedNativeModulusBits = 62U;  // signed representation
}  // namespace q3report

namespace crossover {
inline constexpr std::uint32_t kDefaultN = 36U;
inline constexpr std::uint32_t kDefaultListSize = 64U;
inline constexpr std::uint8_t kDefaultR2 = 6U;
inline constexpr std::uint8_t kDefaultR1 = 12U;
inline constexpr std::uint64_t kDefaultSeed = 1U;
inline constexpr std::uint32_t kDefaultHalfPoolSize = 512U;
inline constexpr std::uint32_t kDefaultMaxHalfPoolSize = 8'192U;
inline constexpr std::uint64_t kDefaultAttemptLimit = 4'000'000ULL;
inline constexpr std::uint64_t kDefaultMemoryBytes = 256ULL * kBytesPerMiB;
}  // namespace crossover

namespace paper_crossover {
inline constexpr std::uint64_t kMinimumRequestedListSize = 2U;
inline constexpr std::uint32_t kMinimumHalfPoolSize = 2U;
inline constexpr std::uint64_t kDefaultRequestedListSize = 64U;
inline constexpr std::uint64_t kDefaultSeed = 0x1001ULL;
inline constexpr std::uint32_t kDefaultInitialHalfPool = 256U;
inline constexpr std::uint32_t kDefaultMaximumHalfPool = 65'536U;
inline constexpr std::uint64_t kDefaultMemoryBytes = 2ULL * 1024ULL * kBytesPerMiB;
inline constexpr std::uint32_t kDefaultWarmupRepetitions = 1U;
inline constexpr std::uint32_t kDefaultTimedRepetitions = 5U;
inline constexpr std::uint32_t kShapeDomainCapacityBits = 31U;
inline constexpr std::uint32_t kShapeSearchMaximumSteps = 64U;
inline constexpr std::uint64_t kPoolSamplingAttemptMultiplier = 64U;
inline constexpr std::uint64_t kPoolSamplingAttemptSlack = 65'536U;
}  // namespace paper_crossover

namespace benchmark {
inline constexpr std::uint64_t kFullTerMemoryBytes = 4ULL * 1024ULL * kBytesPerMiB;
// Fixed finite-sanity smoke-fixture policy.  These values describe the
// benchmark input, not the FullTer dimension domain.
inline constexpr std::uint32_t kFullTerFixtureN = 36U;
inline constexpr std::uint8_t kFullTerFixtureModulusBits = 10U;
inline constexpr std::uint64_t kFullTerFixtureSeed = 0x46554c4c544552ULL;
inline constexpr std::uint32_t kFullTerTimedRepetitions = 5U;

// Fixed Q3REPORT benchmark fixtures.  They remain experiment-only and must
// not be read as FullTer solver defaults.
inline constexpr std::uint64_t kQ3ReportFixtureSeed = 0x51335245504f5254ULL;
inline constexpr std::uint64_t kQ3ReportFixtureInstanceSeed = 0x5143415054555245ULL;
inline constexpr std::uint32_t kQ3ReportFixtureN = 36U;
inline constexpr std::uint8_t kQ3ReportFixtureModulusBits = 10U;
inline constexpr std::uint8_t kQ3ReportGeneratedB2 = 4U;
inline constexpr std::uint8_t kQ3ReportGeneratedB1 = 10U;
inline constexpr std::uint32_t kQ3ReportGeneratedListSize = 64U;
inline constexpr std::uint8_t kQ3ReportFixedProjectionR1 = 9U;
inline constexpr std::uint32_t kQ3ReportTimedRepetitions = 7U;
inline constexpr std::uint32_t kQ3ReportInitialHalfPool = 384U;
inline constexpr std::uint32_t kQ3ReportMaximumHalfPool = 1'024U;
inline constexpr std::uint64_t kQ3ReportAttemptLimit = 2'000'000ULL;

// Legacy single-prime crossover schedule.  These are declared benchmark
// inputs and sampling rules; exceeding their transform gate is reported by
// the existing NotApplicable path.
inline constexpr std::uint8_t kCrossoverR2 = 6U;
inline constexpr std::uint32_t kCrossoverShapeN = 36U;
inline constexpr std::array<std::uint32_t, 8> kCrossoverListSizeLadder{
    64U, 128U, 256U, 512U, 1'024U, 2'048U, 4'096U, 8'192U};
inline constexpr std::array<long double, 4> kCrossoverLambdaTargets{
    0.25L, 1.0L, 4.0L, 16.0L};
inline constexpr std::array<std::uint64_t, 7> kCrossoverSeeds{
    0x1001ULL, 0x2003ULL, 0x3007ULL, 0x4009ULL, 0x500BULL, 0x600DULL,
    0x700FULL};
inline constexpr std::uint32_t kCrossoverTimedRepetitions = 7U;
inline constexpr std::uint32_t kCrossoverMaximumQuotientExponent = 40U;
inline constexpr std::uint32_t kCrossoverInitialHalfPool = 256U;
inline constexpr std::uint32_t kCrossoverMaximumHalfPool = 3'072U;
inline constexpr std::uint64_t kCrossoverAttemptLimit = 8'000'000ULL;

// Paper-rounded packed-baseline benchmark profile.  This profile is a
// workload definition, not a claim that these are universal TER parameters.
inline constexpr std::uint8_t kBaselineB2 = 8U;
inline constexpr std::uint8_t kBaselineB1 = 18U;
inline constexpr std::uint32_t kBaselineMinimumHalfPool = 256U;
inline constexpr std::uint32_t kBaselineHalfPoolMultiplier = 2U;
inline constexpr std::array<std::uint32_t, 3> kBaselineFullListSizes{
    128U, 256U, 512U};
inline constexpr std::array<std::uint32_t, 2> kBaselineQuickListSizes{64U, 128U};
inline constexpr std::uint32_t kBaselineFullSeedCount = 10U;
inline constexpr std::uint32_t kBaselineQuickSeedCount = 3U;
inline constexpr std::uint32_t kBaselineFullTimedRepetitions = 3U;
inline constexpr std::uint32_t kBaselineQuickTimedRepetitions = 2U;
inline constexpr std::uint64_t kBaselineSeedBase = 0x5445520000000000ULL;
inline constexpr std::uint64_t kBaselineSeedSizeMultiplier = 100U;

// Paper comparator anchors and declared input ladder.  The values are fixed
// experiment provenance, while the schedule below controls repetitions.
inline constexpr std::uint64_t kPaperAnchorSeed = 0x51335245504f5254ULL;
inline constexpr std::uint64_t kPaperAnchorInstanceSeed = 0x5143415054555245ULL;
inline constexpr std::array<std::uint64_t, 10> kPaperListSizeLadder{
    64U, 128U, 256U, 512U, 1'024U, 2'048U, 4'096U, 8'192U, 16'384U, 32'768U};
inline constexpr std::array<std::uint64_t, 5> kPaperSeeds{
    0x1001U, 0x2003U, 0x3007U, 0x4009U, 0x500BU};
inline constexpr std::uint32_t kPaperSingleSeedWarmupRepetitions = 1U;
inline constexpr std::uint64_t kPaperSmallListMaximum = 4'096U;
inline constexpr std::uint64_t kPaperMediumListSize = 8'192U;
inline constexpr std::uint64_t kPaperExtendedListSize = 16'384U;
inline constexpr std::uint32_t kPaperSmallSeeds = 5U;
inline constexpr std::uint32_t kPaperSmallRepetitions = 5U;
inline constexpr std::uint32_t kPaperMediumSeeds = 5U;
inline constexpr std::uint32_t kPaperMediumRepetitions = 3U;
inline constexpr std::uint32_t kPaperExtendedSeeds = 5U;
inline constexpr std::uint32_t kPaperExtendedRepetitions = 3U;
inline constexpr std::uint32_t kPaperDefaultExtendedSeeds = 3U;
inline constexpr std::uint32_t kPaperDefaultExtendedRepetitions = 2U;
inline constexpr std::uint32_t kPaperUnsupportedSeeds = 5U;
inline constexpr std::uint32_t kPaperUnsupportedRepetitions = 1U;
}  // namespace benchmark

}  // namespace control_panel

// Unit used by all byte-valued runtime defaults below.
inline constexpr std::uint64_t kMiB = control_panel::kBytesPerMiB;

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
inline constexpr std::uint32_t kMachineWordBits = control_panel::arithmetic::kMachineWordBits;
inline constexpr std::uint32_t kMaximumModulusBits = kMachineWordBits - 1U;
inline constexpr std::uint32_t kMaximumQuotientBits = control_panel::arithmetic::kMaximumQuotientBits;

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
inline constexpr std::uint32_t kFiniteDimensionDivisor =
    control_panel::full_ter::kFiniteDimensionDivisor;

// User-configurable Full TER defaults. Zero limits are invalid configuration.
// Exhausting attempts is AttemptLimitReached; breaching the memory budget is
// ResourceLimit. Neither outcome implies UNSAT.
inline constexpr std::uint32_t kDefaultAttemptLimit = control_panel::full_ter::kDefaultAttemptLimit;
inline constexpr std::uint64_t kDefaultMemoryBytes = control_panel::full_ter::kDefaultMemoryBytes;

// Fixture-only leaf-list cap. This bounds the finite test harness, not the
// classical Full TER mathematics. A fixture expansion past it is ResourceLimit.
inline constexpr std::uint64_t kDefaultFixtureLevel3Limit =
    control_panel::full_ter::kDefaultFixtureLevel3Limit;
inline constexpr std::uint32_t kDefaultRootPrefilterBits =
    control_panel::full_ter::kDefaultRootPrefilterBits;
inline constexpr std::uint32_t kDefaultRootPreferredTileBits =
    control_panel::full_ter::kDefaultRootPreferredTileBits;

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

// M2 keeps the full root quotient in uint64_t and projects only its low w
// bits into the existing 32-bit tiled reporter. A wider root quotient retains
// the legacy exact merge; it is not a solver failure.
[[nodiscard]] constexpr bool native_root_quotient_supported(const std::uint32_t n,
                                                            const std::uint32_t r1) noexcept {
  return r1 != 0U && r1 < n && n - r1 <= arithmetic::kMachineWordBits;
}

// Repair item P12/P14. The bounded tiled root streams the A x B space and never
// materializes a pair index, so it is admissible for any well-formed shape --
// including a root quotient wider than one machine word. When
// native_root_quotient_supported() is false the stored quotient holds only the
// low 64 bits and acts as an exact no-false-negative prefilter; the
// arbitrary-width residue words then decide. This predicate exists so a wide
// quotient selects the bounded path instead of the pair-materializing fallback.
[[nodiscard]] constexpr bool bounded_root_supported(const std::uint32_t n,
                                                    const std::uint32_t r1) noexcept {
  return r1 != 0U && r1 < n;
}

[[nodiscard]] constexpr bool root_prefilter_configuration_supported(
    const std::uint32_t configured_prefilter_bits,
    const std::uint32_t configured_tile_bits,
    const std::uint32_t quotient_bits) noexcept {
  if (configured_prefilter_bits == 0U || configured_tile_bits == 0U || quotient_bits == 0U) {
    return false;
  }
  const std::uint32_t effective_prefilter =
      configured_prefilter_bits < quotient_bits ? configured_prefilter_bits : quotient_bits;
  const std::uint32_t reporter_cap = arithmetic::kMaximumQuotientBits;
  const std::uint32_t supported_prefilter =
      effective_prefilter < reporter_cap ? effective_prefilter : reporter_cap;
  return supported_prefilter != 0U && configured_tile_bits <= supported_prefilter;
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
inline constexpr std::uint32_t kDefaultN = control_panel::generator::kDefaultN;
inline constexpr std::uint8_t kDefaultB2 = control_panel::generator::kDefaultB2;
inline constexpr std::uint8_t kDefaultB1 = control_panel::generator::kDefaultB1;
inline constexpr std::uint64_t kDefaultSeed = control_panel::generator::kDefaultSeed;
inline constexpr std::uint32_t kDefaultRequestedListSize =
    control_panel::generator::kDefaultRequestedListSize;
inline constexpr std::uint32_t kDefaultHalfPoolSize = control_panel::generator::kDefaultHalfPoolSize;
inline constexpr std::uint32_t kDefaultMaxHalfPoolSize =
    control_panel::generator::kDefaultMaxHalfPoolSize;
inline constexpr std::uint64_t kDefaultAttemptLimit = control_panel::generator::kDefaultAttemptLimit;
inline constexpr std::uint64_t kDefaultMemoryBytes = control_panel::generator::kDefaultMemoryBytes;
// Exhaustive reference generation is a finite correctness tool. Beyond n=36
// it refuses with ResourceLimit instead of trying to materialize its domain.
inline constexpr std::uint32_t kExhaustiveReferenceMaximumN =
    control_panel::generator::kExhaustiveReferenceMaximumN;

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
inline constexpr std::uint32_t kDefaultN = control_panel::paper_generator::kDefaultN;
inline constexpr std::uint64_t kDefaultMemoryBytes =
    control_panel::paper_generator::kDefaultMemoryBytes;
inline constexpr std::uint32_t kDefaultHalfPoolSize =
    control_panel::paper_generator::kDefaultHalfPoolSize;
inline constexpr std::uint64_t kDefaultAttemptLimit =
    control_panel::paper_generator::kDefaultAttemptLimit;

}  // namespace paper_generator

namespace oracle {

// Correctness-oracle safety cap, not a TER mathematical limit. Exceeding it
// refuses cubic execution with the caller's existing ResourceLimit behavior;
// it never implies UNSAT.
inline constexpr std::uint64_t kCubicCandidateTripleCap =
    control_panel::oracle::kCubicCandidateTripleCap;

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
inline constexpr std::uint32_t kDefaultPreferredTileBits =
    control_panel::tiled::kDefaultPreferredTileBits;
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

// Repair item P13. The L2->L1 quotient index is 32 bits wide, which used to cap
// r1-r2 at 32 and divert wider shapes (finite-sanity n=144 has r1-r2=37) into
// the pair-materializing generic merge. The bounded path now projects onto
// b1' = min(r1, r2+32), which is exactly the low 32 bits of the true quotient:
// identical to the old behavior whenever r1-r2 <= 32, and an exact
// no-false-negative prefilter above it, with the arbitrary-width residue
// congruence deciding the survivors. The remaining requirement is that the
// projected modulus stay inside the native signed-arithmetic window.
[[nodiscard]] constexpr bool bounded_full_ter_l2_l1_supported(
    const std::uint32_t n,
    const std::uint32_t r1,
    const std::uint32_t r2) noexcept {
  if (r2 == 0U || r2 >= r1 || r1 >= n) {
    return false;
  }
  const std::uint32_t projected_b1 =
      r1 < r2 + kMaximumQuotientBits ? r1 : r2 + kMaximumQuotientBits;
  return projected_b1 < arithmetic::kMachineWordBits && projected_b1 < n;
}

}  // namespace tiled

namespace ntt {

// The original single-prime count-only Q3REPORT/crossover NTT is exactly
// supported through 2^23. Larger Q is NotApplicable, not a correctness result.
inline constexpr std::uint32_t kQ3ReportMaximumPower =
    control_panel::ntt::kQ3ReportMaximumPower;
inline constexpr std::uint64_t kQ3ReportMaximumLength =
    std::uint64_t{1} << kQ3ReportMaximumPower;
inline constexpr bool kQ3ReportIsCountOnly = true;
// Arithmetic parameters of the existing single-prime count-only core. They
// are implementation compatibility policy, not a witness-recovery capability.
inline constexpr std::uint64_t kQ3ReportPrime = control_panel::ntt::kQ3ReportPrime;
inline constexpr std::uint64_t kQ3ReportPrimitiveRoot =
    control_panel::ntt::kQ3ReportPrimitiveRoot;

[[nodiscard]] constexpr bool q3report_length_supported(const std::uint64_t length) noexcept {
  return is_power_of_two(length) && length <= kQ3ReportMaximumLength;
}

// The paper-scaling comparator has a distinct dual-prime implementation. Its
// 2^27 second-prime support is deliberately separate from the older 2^23 core.
inline constexpr std::uint64_t kPaperPrime1 = control_panel::ntt::kPaperPrime1;
inline constexpr std::uint64_t kPaperPrimitiveRoot1 = control_panel::ntt::kPaperPrimitiveRoot1;
inline constexpr std::uint32_t kPaperMaximumPower1 = control_panel::ntt::kPaperMaximumPower1;
inline constexpr std::uint64_t kPaperPrime2 = control_panel::ntt::kPaperPrime2;
inline constexpr std::uint64_t kPaperPrimitiveRoot2 = control_panel::ntt::kPaperPrimitiveRoot2;
inline constexpr std::uint32_t kPaperMaximumPower2 = control_panel::ntt::kPaperMaximumPower2;

}  // namespace ntt

namespace q3report {

// Frozen count-only experiment defaults. These are explicit audit/output
// ceilings rather than solver mathematics; callers retain configuration
// overrides and existing status mappings.
inline constexpr std::uint64_t kDefaultMemoryBytes = control_panel::q3report::kDefaultMemoryBytes;
inline constexpr std::uint64_t kDefaultMaximumStructuralChecks =
    control_panel::q3report::kDefaultMaximumStructuralChecks;
inline constexpr std::uint64_t kDefaultMaximumStoredParents =
    control_panel::q3report::kDefaultMaximumStoredParents;
inline constexpr std::uint64_t kDefaultMaximumOracleTriples =
    oracle::kCubicCandidateTripleCap;
// Q3REPORT derives tau through widened signed 64-bit arithmetic, so r1=63 is
// deliberately excluded. This is narrower than the generic native Full TER
// tiled fallback gate and remains InvalidConfiguration at Q3 callers.
inline constexpr std::uint32_t kMaximumSignedNativeModulusBits =
    control_panel::q3report::kMaximumSignedNativeModulusBits;

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
inline constexpr std::uint32_t kDefaultN = control_panel::crossover::kDefaultN;
inline constexpr std::uint32_t kDefaultListSize = control_panel::crossover::kDefaultListSize;
inline constexpr std::uint8_t kDefaultR2 = control_panel::crossover::kDefaultR2;
inline constexpr std::uint8_t kDefaultR1 = control_panel::crossover::kDefaultR1;
inline constexpr std::uint64_t kDefaultSeed = control_panel::crossover::kDefaultSeed;
inline constexpr std::uint32_t kDefaultHalfPoolSize =
    control_panel::crossover::kDefaultHalfPoolSize;
inline constexpr std::uint32_t kDefaultMaxHalfPoolSize =
    control_panel::crossover::kDefaultMaxHalfPoolSize;
inline constexpr std::uint64_t kDefaultAttemptLimit =
    control_panel::crossover::kDefaultAttemptLimit;
inline constexpr std::uint64_t kDefaultMemoryBytes = control_panel::crossover::kDefaultMemoryBytes;

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
inline constexpr std::uint64_t kMinimumRequestedListSize =
    control_panel::paper_crossover::kMinimumRequestedListSize;
inline constexpr std::uint32_t kMinimumHalfPoolSize =
    control_panel::paper_crossover::kMinimumHalfPoolSize;
inline constexpr std::uint64_t kDefaultRequestedListSize =
    control_panel::paper_crossover::kDefaultRequestedListSize;
inline constexpr std::uint64_t kDefaultSeed = control_panel::paper_crossover::kDefaultSeed;
inline constexpr std::uint32_t kDefaultInitialHalfPool =
    control_panel::paper_crossover::kDefaultInitialHalfPool;
inline constexpr std::uint32_t kDefaultMaximumHalfPool =
    control_panel::paper_crossover::kDefaultMaximumHalfPool;
inline constexpr std::uint64_t kDefaultMemoryBytes =
    control_panel::paper_crossover::kDefaultMemoryBytes;
inline constexpr std::uint32_t kDefaultWarmupRepetitions =
    control_panel::paper_crossover::kDefaultWarmupRepetitions;
inline constexpr std::uint32_t kDefaultTimedRepetitions =
    control_panel::paper_crossover::kDefaultTimedRepetitions;
// Paper-scaling shape search caps. These are comparator practicality bounds;
// exhaustion returns GenerationIncomplete and is not a TER theorem.
inline constexpr std::uint64_t kShapeDomainCapacityCap =
    std::uint64_t{1} << control_panel::paper_crossover::kShapeDomainCapacityBits;
inline constexpr std::uint32_t kShapeSearchMaximumSteps =
    control_panel::paper_crossover::kShapeSearchMaximumSteps;
// Per-pool sampling budget: a deterministic practical cap used only by the
// paper comparator's generator; failure remains GenerationIncomplete.
inline constexpr std::uint64_t kPoolSamplingAttemptMultiplier =
    control_panel::paper_crossover::kPoolSamplingAttemptMultiplier;
inline constexpr std::uint64_t kPoolSamplingAttemptSlack =
    control_panel::paper_crossover::kPoolSamplingAttemptSlack;

[[nodiscard]] constexpr bool snapshot_configuration_supported(
    const std::uint64_t requested_list_size,
    const std::uint32_t initial_half_pool,
    const std::uint32_t maximum_half_pool,
    const std::uint64_t memory_limit_bytes) noexcept {
  return requested_list_size >= kMinimumRequestedListSize &&
         initial_half_pool >= kMinimumHalfPoolSize && maximum_half_pool >= initial_half_pool &&
         memory_limit_bytes != 0U;
}

[[nodiscard]] constexpr bool run_configuration_supported(
    const std::uint64_t memory_limit_bytes,
    const std::uint32_t timed_repetitions) noexcept {
  return memory_limit_bytes != 0U && timed_repetitions != 0U;
}

}  // namespace paper_crossover

namespace benchmark {

// Full TER smoke benchmark ceiling. It is intentionally lower than the
// FullTerConfig default so constrained behavior (including n=72 refusal) is
// observable. There is no global wall-clock timeout policy.
inline constexpr std::uint64_t kFullTerMemoryBytes =
    control_panel::benchmark::kFullTerMemoryBytes;
inline constexpr std::uint32_t kFullTerFixtureN = control_panel::benchmark::kFullTerFixtureN;
inline constexpr std::uint8_t kFullTerFixtureModulusBits =
    control_panel::benchmark::kFullTerFixtureModulusBits;
inline constexpr std::uint64_t kFullTerFixtureSeed =
    control_panel::benchmark::kFullTerFixtureSeed;
inline constexpr std::uint32_t kFullTerTimedRepetitions =
    control_panel::benchmark::kFullTerTimedRepetitions;

// Fixed resource settings for the two legacy count-only benchmark fixtures.
// They are benchmark sampling limits, not solver defaults.
inline constexpr std::uint64_t kQ3ReportFixtureSeed =
    control_panel::benchmark::kQ3ReportFixtureSeed;
inline constexpr std::uint64_t kQ3ReportFixtureInstanceSeed =
    control_panel::benchmark::kQ3ReportFixtureInstanceSeed;
inline constexpr std::uint32_t kQ3ReportFixtureN =
    control_panel::benchmark::kQ3ReportFixtureN;
inline constexpr std::uint8_t kQ3ReportFixtureModulusBits =
    control_panel::benchmark::kQ3ReportFixtureModulusBits;
inline constexpr std::uint8_t kQ3ReportGeneratedB2 =
    control_panel::benchmark::kQ3ReportGeneratedB2;
inline constexpr std::uint8_t kQ3ReportGeneratedB1 =
    control_panel::benchmark::kQ3ReportGeneratedB1;
inline constexpr std::uint32_t kQ3ReportGeneratedListSize =
    control_panel::benchmark::kQ3ReportGeneratedListSize;
inline constexpr std::uint8_t kQ3ReportFixedProjectionR1 =
    control_panel::benchmark::kQ3ReportFixedProjectionR1;
inline constexpr std::uint32_t kQ3ReportTimedRepetitions =
    control_panel::benchmark::kQ3ReportTimedRepetitions;
inline constexpr std::uint32_t kQ3ReportBenchmarkInitialHalfPool =
    control_panel::benchmark::kQ3ReportInitialHalfPool;
inline constexpr std::uint32_t kQ3ReportBenchmarkMaximumHalfPool =
    control_panel::benchmark::kQ3ReportMaximumHalfPool;
inline constexpr std::uint64_t kQ3ReportBenchmarkAttemptLimit =
    control_panel::benchmark::kQ3ReportAttemptLimit;
inline constexpr std::uint32_t kCrossoverBenchmarkInitialHalfPool =
    control_panel::benchmark::kCrossoverInitialHalfPool;
inline constexpr std::uint32_t kCrossoverBenchmarkMaximumHalfPool =
    control_panel::benchmark::kCrossoverMaximumHalfPool;
inline constexpr std::uint64_t kCrossoverBenchmarkAttemptLimit =
    control_panel::benchmark::kCrossoverAttemptLimit;
inline constexpr std::uint8_t kCrossoverR2 = control_panel::benchmark::kCrossoverR2;
inline constexpr std::uint32_t kCrossoverShapeN =
    control_panel::benchmark::kCrossoverShapeN;
inline constexpr const auto& kCrossoverListSizeLadder =
    control_panel::benchmark::kCrossoverListSizeLadder;
inline constexpr const auto& kCrossoverLambdaTargets =
    control_panel::benchmark::kCrossoverLambdaTargets;
inline constexpr const auto& kCrossoverSeeds = control_panel::benchmark::kCrossoverSeeds;
inline constexpr std::uint32_t kCrossoverTimedRepetitions =
    control_panel::benchmark::kCrossoverTimedRepetitions;
inline constexpr std::uint32_t kCrossoverMaximumQuotientExponent =
    control_panel::benchmark::kCrossoverMaximumQuotientExponent;

inline constexpr std::uint8_t kBaselineB2 = control_panel::benchmark::kBaselineB2;
inline constexpr std::uint8_t kBaselineB1 = control_panel::benchmark::kBaselineB1;
inline constexpr std::uint32_t kBaselineMinimumHalfPool =
    control_panel::benchmark::kBaselineMinimumHalfPool;
inline constexpr std::uint32_t kBaselineHalfPoolMultiplier =
    control_panel::benchmark::kBaselineHalfPoolMultiplier;
inline constexpr const auto& kBaselineFullListSizes =
    control_panel::benchmark::kBaselineFullListSizes;
inline constexpr const auto& kBaselineQuickListSizes =
    control_panel::benchmark::kBaselineQuickListSizes;
inline constexpr std::uint32_t kBaselineFullSeedCount =
    control_panel::benchmark::kBaselineFullSeedCount;
inline constexpr std::uint32_t kBaselineQuickSeedCount =
    control_panel::benchmark::kBaselineQuickSeedCount;
inline constexpr std::uint32_t kBaselineFullTimedRepetitions =
    control_panel::benchmark::kBaselineFullTimedRepetitions;
inline constexpr std::uint32_t kBaselineQuickTimedRepetitions =
    control_panel::benchmark::kBaselineQuickTimedRepetitions;
inline constexpr std::uint64_t kBaselineSeedBase =
    control_panel::benchmark::kBaselineSeedBase;
inline constexpr std::uint64_t kBaselineSeedSizeMultiplier =
    control_panel::benchmark::kBaselineSeedSizeMultiplier;

inline constexpr std::uint64_t kPaperAnchorSeed = control_panel::benchmark::kPaperAnchorSeed;
inline constexpr std::uint64_t kPaperAnchorInstanceSeed =
    control_panel::benchmark::kPaperAnchorInstanceSeed;
inline constexpr const auto& kPaperListSizeLadder =
    control_panel::benchmark::kPaperListSizeLadder;
inline constexpr const auto& kPaperSeeds = control_panel::benchmark::kPaperSeeds;
inline constexpr std::uint32_t kPaperSingleSeedWarmupRepetitions =
    control_panel::benchmark::kPaperSingleSeedWarmupRepetitions;

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
  if (list_size <= control_panel::benchmark::kPaperSmallListMaximum) {
    return {control_panel::benchmark::kPaperSmallSeeds,
            control_panel::benchmark::kPaperSmallRepetitions, true};
  }
  if (list_size == control_panel::benchmark::kPaperMediumListSize) {
    return {control_panel::benchmark::kPaperMediumSeeds,
            control_panel::benchmark::kPaperMediumRepetitions, true};
  }
  if (list_size == control_panel::benchmark::kPaperExtendedListSize) {
    return extended_16384
               ? PaperCrossoverSchedule{control_panel::benchmark::kPaperExtendedSeeds,
                                        control_panel::benchmark::kPaperExtendedRepetitions, true}
               : PaperCrossoverSchedule{control_panel::benchmark::kPaperDefaultExtendedSeeds,
                                        control_panel::benchmark::kPaperDefaultExtendedRepetitions, true};
  }
  return {control_panel::benchmark::kPaperUnsupportedSeeds,
          control_panel::benchmark::kPaperUnsupportedRepetitions, false};
}

[[nodiscard]] constexpr bool full_ter_warmup_acceptable(const Status status) noexcept {
  return status == Status::Found || status == Status::AttemptLimitReached;
}

}  // namespace benchmark

}  // namespace ter::limits
