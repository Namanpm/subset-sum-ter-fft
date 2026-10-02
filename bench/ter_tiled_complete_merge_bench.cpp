// Complete exact merge comparison: existing pair-materializing public baseline
// versus tiled public merge.
//
// SCOPE AND LABELLING.  This is NOT the Q3REPORT paper-crossover headline
// comparison.  `run_pair_baseline` builds a Q-wide uint64 offset array *and*
// materializes an |X||Y| pair-record array; `direct_production_method` in the
// paper crossover builds only a Q-wide uint32 index over the lookup list and
// materializes no pairs.  The two baselines are algorithmically different, so a
// ratio measured here must never be reported as, substituted for, or averaged
// with the DIRECT_PRODUCTION / DIRECT_PRODUCTION_TILED headline ratio.
//
// What this benchmark does add is the one thing the arithmetic-only comparator
// cannot: both timed methods here run the *complete* exact merge - arithmetic
// location, structural validation, parent construction and deduplication -
// through the repository's own validators and finalizer, on one shared
// snapshot, under one output policy.  The merge tests already prove the two
// paths agree exactly; this program reconfirms that agreement outside every
// timed region and then reports what the complete merge costs.
//
// Fairness policy (Section 36).  Both methods are warmed before any timing.
// The per-repetition method order alternates, so neither method is permanently
// second behind the other's cache footprint.  Output comparison, fingerprinting
// and CSV formatting all happen outside the timed regions.

#include "ter/generator.hpp"
#include "ter/limitations.hpp"
#include "ter/merge.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

// Declared before any timing was observed.
constexpr std::array<std::uint32_t, 5> kListSizes{256U, 512U, 1'024U, 2'048U, 4'096U};
constexpr std::array<std::uint64_t, 3> kSeeds{0x1001ULL, 0x2003ULL, 0x3007ULL};
constexpr std::uint32_t kDefaultTimedRepetitions = 3U;

// The quotient universe must track the list size the way TER's paper scaling
// does, Q ~ L^2 / lambda with lambda held near constant.  Holding b1 fixed
// while L grows drives lambda into the thousands and, once S reaches Q, leaves
// the tiled path with a single bucket and a single window - a configuration in
// which tiling is inert by construction and the comparison measures nothing.
constexpr std::uint64_t kTargetLambda = 2U;

[[nodiscard]] std::uint32_t log2_exact(std::uint64_t value) noexcept {
  std::uint32_t bits = 0U;
  while (value > 1U) {
    value >>= 1U;
    ++bits;
  }
  return bits;
}

// Q = L^2 / lambda, expressed as the b1 - b2 width the generator consumes.
[[nodiscard]] std::uint8_t b1_for_list_size(const std::uint32_t list_size) noexcept {
  const std::uint32_t q_bits = 2U * log2_exact(list_size) - log2_exact(kTargetLambda);
  return static_cast<std::uint8_t>(ter::limits::benchmark::kBaselineB2 + q_bits);
}

// MergeArithmetic carries the modulus Q, not its exponent; the universe is a
// power of two by construction, so the exponent is recovered exactly.
// The repository exposes no generic ter::Status printer, so the benchmark keeps
// its own exhaustive mapping.  Distinct outcomes stay distinct in the CSV:
// ResourceLimit is never printed as a completed row or as an empty result.
[[nodiscard]] const char* status_name(const ter::Status status) noexcept {
  switch (status) {
    case ter::Status::Completed:            return "Completed";
    case ter::Status::Found:                return "Found";
    case ter::Status::NoWitnessThisAttempt: return "NoWitnessThisAttempt";
    case ter::Status::AttemptLimitReached:  return "AttemptLimitReached";
    case ter::Status::InvalidConfiguration: return "InvalidConfiguration";
    case ter::Status::InvalidShape:         return "InvalidShape";
    case ter::Status::GenerationIncomplete: return "GenerationIncomplete";
    case ter::Status::ResourceLimit:        return "ResourceLimit";
    case ter::Status::Cancelled:            return "Cancelled";
    case ter::Status::InvariantViolation:   return "InvariantViolation";
    case ter::Status::NotApplicable:        return "NotApplicable";
    case ter::Status::UnsupportedWidth:     return "UnsupportedWidth";
  }
  return "UNKNOWN";
}

[[nodiscard]] std::uint32_t quotient_bits(std::uint64_t q) noexcept {
  std::uint32_t bits = 0U;
  while (q > 1U) {
    q >>= 1U;
    ++bits;
  }
  return bits;
}

[[nodiscard]] std::uint64_t elapsed_ns(const Clock::time_point start) noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
}

[[nodiscard]] std::uint64_t median(std::vector<std::uint64_t> samples) {
  std::sort(samples.begin(), samples.end());
  return samples[samples.size() / 2U];
}

[[nodiscard]] std::uint64_t minimum(const std::vector<std::uint64_t>& samples) {
  return *std::min_element(samples.begin(), samples.end());
}

[[nodiscard]] std::uint64_t maximum(const std::vector<std::uint64_t>& samples) {
  return *std::max_element(samples.begin(), samples.end());
}

// Covers the parent set, its multiplicities and the validated provenance, so an
// equality of fingerprints is an equality of the whole merge result, not only
// of a triple count.
[[nodiscard]] std::uint64_t output_fingerprint(const ter::MergeOutput& output) {
  std::uint64_t hash = 1469598103934665603ULL;
  const auto mix = [&hash](const std::uint64_t value) {
    hash ^= value;
    hash *= 1099511628211ULL;
  };
  for (const auto& parent : output.parents) {
    for (const std::int8_t digit : parent.digits) {
      mix(static_cast<std::uint64_t>(static_cast<std::int64_t>(digit) + 1));
    }
    mix(parent.residue_m1);
    mix(parent.multiplicity);
  }
  for (const auto& record : output.valid_provenance) {
    mix(record.a);
    mix(record.b);
    mix(record.c);
  }
  mix(output.telemetry.raw_valid_triples);
  mix(output.telemetry.unique_parents);
  mix(output.telemetry.candidate_triples);
  return hash;
}

[[nodiscard]] bool parse_u64(const std::string& text, std::uint64_t* const out) noexcept {
  if (text.empty()) {
    return false;
  }
  for (const char character : text) {
    if (character < '0' || character > '9') {
      return false;
    }
  }
  errno = 0;
  char* end = nullptr;
  const unsigned long long value = std::strtoull(text.c_str(), &end, 10);
  if (errno != 0 || end != text.c_str() + text.size()) {
    return false;
  }
  *out = static_cast<std::uint64_t>(value);
  return true;
}

struct MethodSamples {
  std::vector<std::uint64_t> total;
  std::vector<std::uint64_t> candidate_loop;
  std::uint64_t working_set_bytes = 0;
  std::uint64_t pair_index_bytes = 0;
  std::uint64_t pair_records = 0;
};

[[nodiscard]] int run_point(const std::uint32_t list_size,
                            const std::uint64_t seed,
                            const std::uint32_t repetitions) {
  ter::SnapshotConfig config;
  config.family = ter::ShapeFamily::PaperRounded;
  config.n = ter::limits::paper_generator::kDefaultN;
  config.b2 = ter::limits::benchmark::kBaselineB2;
  config.b1 = b1_for_list_size(list_size);
  config.seed = seed;
  config.requested_list_size = list_size;
  config.mode = ter::GeneratorMode::MitmPerformance;
  config.plant_valid_triple = false;
  config.half_pool_size = std::max<std::uint32_t>(
      ter::limits::benchmark::kBaselineMinimumHalfPool,
      list_size * ter::limits::benchmark::kBaselineHalfPoolMultiplier);
  // paper_generator::kDefaultHalfPoolSize is 4096, which refuses the 2L initial
  // pool once L reaches 4096.  The ceiling is a generator search bound, not a
  // merge property, so it is raised here rather than truncating the list ladder.
  config.max_half_pool_size = 65'536U;
  config.attempt_limit = ter::limits::paper_generator::kDefaultAttemptLimit;
  config.memory_limit_bytes = ter::limits::paper_generator::kDefaultMemoryBytes;

  const auto generated = ter::build_snapshot(config);
  if (generated.status != ter::Status::Completed) {
    std::cerr << "L=" << list_size << " seed=" << seed
              << " generation failed: " << generated.message << '\n';
    return 1;
  }
  const auto& snapshot = generated.snapshot;

  // Warm both methods before any timing, and establish the exact agreement the
  // merge tests already assert.  Both checks are outside every timed region.
  const auto baseline_warm = ter::run_pair_baseline(snapshot, ter::OutputPolicy::Correctness);
  const auto tiled_warm = ter::run_pair_tiled(snapshot, ter::OutputPolicy::Correctness);
  // A declared-ceiling refusal is a real property of the method under test, not
  // a harness fault, so the point is recorded as a row carrying both statuses
  // instead of aborting the campaign or silently vanishing.  ResourceLimit is
  // never rewritten as a completed row or as a zero result.
  if (baseline_warm.status != ter::Status::Completed ||
      tiled_warm.status != ter::Status::Completed) {
    std::cerr << "L=" << list_size << " seed=" << seed << " not comparable: baseline="
              << status_name(baseline_warm.status) << " (" << baseline_warm.message
              << ") tiled=" << status_name(tiled_warm.status) << " (" << tiled_warm.message
              << ")\n";
    std::cout << "PAIR_MATERIALIZING_BASELINE_VS_TILED_PUBLIC_MERGE,"
              << snapshot.spec.shape.n << ',' << list_size << ',' << seed << ','
              << snapshot.lists[0].entries.size() << ',' << snapshot.lists[1].entries.size()
              << ',' << snapshot.lists[2].entries.size() << ','
              << snapshot.spec.arithmetic.q << ','
              << quotient_bits(snapshot.spec.arithmetic.q)
              << ",0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0," << repetitions << ",0,"
              << status_name(baseline_warm.status) << ','
              << status_name(tiled_warm.status) << '\n';
    std::cout.flush();
    return 0;
  }
  const std::uint64_t baseline_fingerprint = output_fingerprint(baseline_warm);
  const std::uint64_t tiled_fingerprint = output_fingerprint(tiled_warm);
  const bool outputs_identical =
      baseline_fingerprint == tiled_fingerprint &&
      baseline_warm.parents == tiled_warm.parents &&
      baseline_warm.valid_provenance == tiled_warm.valid_provenance &&
      baseline_warm.telemetry.raw_valid_triples == tiled_warm.telemetry.raw_valid_triples &&
      baseline_warm.telemetry.unique_parents == tiled_warm.telemetry.unique_parents;
  if (!outputs_identical) {
    std::cerr << "L=" << list_size << " seed=" << seed
              << " FATAL: complete merge outputs disagree\n";
    return 1;
  }

  MethodSamples baseline;
  MethodSamples tiled;
  baseline.total.reserve(repetitions);
  tiled.total.reserve(repetitions);

  for (std::uint32_t repetition = 0U; repetition < repetitions; ++repetition) {
    // Alternate which method runs first so neither is permanently measured
    // behind the other's residual cache state.
    const bool tiled_first = (repetition % 2U) == 1U;
    for (std::uint32_t slot = 0U; slot < 2U; ++slot) {
      const bool run_tiled = (slot == 0U) == tiled_first;
      const auto start = Clock::now();
      const auto output = run_tiled ? ter::run_pair_tiled(snapshot, ter::OutputPolicy::Benchmark)
                                    : ter::run_pair_baseline(snapshot, ter::OutputPolicy::Benchmark);
      const std::uint64_t total = elapsed_ns(start);
      if (output.status != ter::Status::Completed) {
        std::cerr << "L=" << list_size << " seed=" << seed << " timed run failed: "
                  << output.message << '\n';
        return 1;
      }
      MethodSamples& into = run_tiled ? tiled : baseline;
      into.total.push_back(total);
      into.candidate_loop.push_back(output.telemetry.candidate_loop_ns);
      into.working_set_bytes = output.telemetry.working_set_budget_bytes;
      into.pair_index_bytes = output.telemetry.pair_index_bytes;
      into.pair_records = output.telemetry.pair_records;
    }
  }

  std::cout << "PAIR_MATERIALIZING_BASELINE_VS_TILED_PUBLIC_MERGE," << snapshot.spec.shape.n << ','
            << list_size << ',' << seed << ',' << snapshot.lists[0].entries.size() << ','
            << snapshot.lists[1].entries.size() << ',' << snapshot.lists[2].entries.size() << ','
            << snapshot.spec.arithmetic.q << ',' << quotient_bits(snapshot.spec.arithmetic.q) << ','
            << baseline_warm.telemetry.pair_combinations << ','
            << baseline_warm.telemetry.candidate_triples << ','
            << baseline_warm.telemetry.raw_valid_triples << ','
            << baseline_warm.telemetry.unique_parents << ','
            << median(baseline.total) << ',' << minimum(baseline.total) << ','
            << maximum(baseline.total) << ',' << median(baseline.candidate_loop) << ','
            << median(tiled.total) << ',' << minimum(tiled.total) << ','
            << maximum(tiled.total) << ',' << median(tiled.candidate_loop) << ','
            // Comparable with each other: each method's own arithmetic-index
            // accounting for the same snapshot.
            << baseline.pair_index_bytes << ',' << tiled.pair_index_bytes << ','
            // NOT comparable with each other, and never divided.  The baseline
            // value is a measured peak; src/merge.cpp gives the tiled path
            // fixed_bytes plus a candidate ceiling that absorbs the entire
            // remaining budget.  Emitted for provenance only.
            << baseline.working_set_bytes << ',' << tiled.working_set_bytes << ','
            << baseline.pair_records << ',' << tiled.pair_records << ',' << repetitions
            << ",1,Completed,Completed\n";
  std::cout.flush();
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::uint32_t repetitions = kDefaultTimedRepetitions;
  bool emit_header = true;
  for (int index = 1; index < argc; ++index) {
    const std::string argument{argv[index]};
    if (argument == "--no-header") {
      emit_header = false;
    } else if (argument.rfind("--repetitions=", 0U) == 0U) {
      std::uint64_t requested = 0U;
      if (!parse_u64(argument.substr(14U), &requested) || requested == 0U ||
          requested > 0xFFFFFFFFULL) {
        std::cerr << "fatal: --repetitions requires a positive 32-bit value\n";
        return 2;
      }
      repetitions = static_cast<std::uint32_t>(requested);
    } else {
      std::cerr << "usage: ter_tiled_complete_merge_bench [--repetitions=N] [--no-header]\n";
      return 2;
    }
  }

  if (emit_header) {
    std::cout << "family,shape_n,list_size,seed,L_A,L_B,L_C,Q,q_bits,pair_combinations,"
                 "candidate_triples,raw_valid_triples,unique_parents,"
                 "baseline_total_median_ns,baseline_total_min_ns,baseline_total_max_ns,"
                 "baseline_candidate_loop_median_ns,"
                 "tiled_total_median_ns,tiled_total_min_ns,tiled_total_max_ns,"
                 "tiled_candidate_loop_median_ns,"
                 "baseline_arithmetic_index_bytes,tiled_arithmetic_index_bytes,"
                 "baseline_measured_peak_bytes,tiled_budget_ceiling_bytes,"
                 "baseline_pair_records,tiled_pair_records,timed_repetitions,"
                 "outputs_identical,baseline_status,tiled_status\n";
  }
  for (const std::uint32_t list_size : kListSizes) {
    for (const std::uint64_t seed : kSeeds) {
      if (run_point(list_size, seed, repetitions) != 0) {
        return 1;
      }
    }
  }
  return 0;
}
