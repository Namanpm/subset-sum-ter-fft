#pragma once

// TER-faithful NTT crossover benchmark.
//
// Scope: answer one question only -- does an exact, count-only cyclic NTT
// ever beat the equivalent direct O(|A|*|B|) cyclic-convolution count over
// larger, genuinely TER-shaped L2 lists than the frozen Q3REPORT experiment
// in q3report_audit.hpp/.cpp tested. This module does not touch, relabel, or
// reinterpret that frozen experiment's results.
//
// The snapshot family produced here is TER_SCALING_EMULATION: real ternary
// {-1,0,+1}^n vectors with the shape's exact L2 digit distribution, real
// instance coefficients, and quotient residues derived by the shared
// quotient-arithmetic module (ter/arithmetic.hpp). It is never a complete
// FullTer capture and must never be reported as AUTHENTIC_FULLTER.

#include "ter/model.hpp"
#include "ter/limitations.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ter {

enum class CrossoverStatus : std::uint8_t {
  Completed,
  NotApplicable,
  ResourceLimit,
  GenerationIncomplete,
  ArithmeticSafetyFailure,
  InvalidConfiguration,
  InternalInvariantFailure,
};

struct CrossoverSnapshot {
  InputSnapshot input{};
  std::uint64_t seed = 0;
  std::uint64_t fingerprint = 0;
};

struct CrossoverSnapshotConfig {
  std::uint32_t n = limits::crossover::kDefaultN;
  std::uint32_t list_size = limits::crossover::kDefaultListSize;
  std::uint8_t r2 = limits::crossover::kDefaultR2;
  std::uint8_t r1 = limits::crossover::kDefaultR1;  // Q = 2^(r1-r2)
  std::uint64_t seed = limits::crossover::kDefaultSeed;
  std::uint32_t half_pool_size = limits::crossover::kDefaultHalfPoolSize;
  std::uint32_t max_half_pool_size = limits::crossover::kDefaultMaxHalfPoolSize;
  std::uint64_t attempt_limit = limits::crossover::kDefaultAttemptLimit;
  std::uint64_t memory_limit_bytes = limits::crossover::kDefaultMemoryBytes;
};

struct CrossoverSnapshotResult {
  CrossoverStatus status = CrossoverStatus::InvalidConfiguration;
  CrossoverSnapshot value{};
  std::string message;
};

// Builds a genuine, unplanted TER_SCALING_EMULATION L2 merge snapshot: real
// ternary vectors with the exact required child digit distribution, real
// instance coefficients, and residues satisfying the real child target
// modulo 2^r2. No witness pair is planted or required to exist.
[[nodiscard]] CrossoverSnapshotResult build_scaling_snapshot(
    const CrossoverSnapshotConfig& config);

// Independent audit: recomputes every stored residue and quotient from the
// raw ternary vectors and instance coefficients; never trusts stored
// metadata. Must pass before any benchmark path runs.
[[nodiscard]] CrossoverStatus audit_scaling_snapshot(const CrossoverSnapshot& snapshot,
                                                      std::string* message);

// A reusable, Q-specific NTT plan: bit-reversal permutation and per-stage
// twiddle roots. Depends only on the transform length, not on snapshot data,
// so it may be built once and reused ("amortized") across every snapshot
// and seed that shares the same Q.
struct NttPlan {
  std::uint64_t length = 0;
  std::vector<std::uint32_t> bit_reversal;
  std::vector<std::uint64_t> forward_root;
  std::vector<std::uint64_t> inverse_root;
  std::uint64_t inverse_length = 0;
};

[[nodiscard]] CrossoverStatus build_ntt_plan(std::uint64_t length,
                                             NttPlan* plan,
                                             std::string* message);

struct CrossoverHistogram {
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

struct CrossoverTiming {
  std::uint64_t direct_setup_ns = 0;
  std::uint64_t direct_count_ns = 0;
  std::uint64_t direct_lookup_ns = 0;
  std::uint64_t ntt_plan_build_ns = 0;
  std::uint64_t ntt_frequency_ns = 0;
  std::uint64_t ntt_transform_ns = 0;
  std::uint64_t ntt_reconstruction_ns = 0;
  std::uint64_t ntt_lookup_ns = 0;
};

struct CrossoverMemory {
  std::uint64_t direct_bytes = 0;
  std::uint64_t ntt_plan_bytes = 0;
  std::uint64_t ntt_per_run_bytes = 0;
};

struct CrossoverReport {
  std::uint64_t a_size = 0;
  std::uint64_t b_size = 0;
  std::uint64_t c_size = 0;
  std::uint8_t r2 = 0;
  std::uint8_t r1 = 0;
  std::uint64_t q = 0;
  long double lambda = 0.0L;
  long double predicted_support_fraction = 0.0L;
  std::uint64_t occupied_pair_residues = 0;
  CrossoverHistogram all_residues{};
  std::uint64_t unique_selected_targets = 0;
  CrossoverHistogram unique_selected{};
  CrossoverHistogram c_weighted_selected{};
  CrossoverTiming timing{};
  CrossoverMemory memory{};
  bool exactness_passed = false;
  bool direct_matches_ntt = false;
};

struct CrossoverRunResult {
  CrossoverStatus status = CrossoverStatus::InvalidConfiguration;
  CrossoverReport report{};
  std::string message;
};

// Runs one fair count-only comparison on a frozen snapshot.
//
// DIRECT path: naive O(|A|*|B|) cyclic pair-count build over the raw
// quotient residues, then the same required-target lookup the NTT path
// performs.
//
// NTT path: frequency-array build, forward/forward/inverse transform,
// exact-safety-checked reconstruction, then required-target lookup.
//
// plan == nullptr selects the cold NTT path: a plan is built internally and
// its cost is charged to timing.ntt_plan_build_ns.
// plan != nullptr selects the amortized NTT path: the supplied plan must
// already match the snapshot's Q (verified); timing.ntt_plan_build_ns is 0.
[[nodiscard]] CrossoverRunResult run_crossover_point(const CrossoverSnapshot& snapshot,
                                                      const NttPlan* plan,
                                                      std::uint64_t memory_limit_bytes);

}  // namespace ter
