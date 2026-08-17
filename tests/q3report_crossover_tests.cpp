#include "ter/arithmetic.hpp"
#include "ter/q3report_crossover.hpp"
#include "ter/reference_vector.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

void require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

ter::CrossoverSnapshotConfig tiny_config(std::uint64_t seed) {
  ter::CrossoverSnapshotConfig config;
  config.n = 36U;
  config.list_size = 8U;
  config.r2 = 4U;
  config.r1 = 8U;  // Q = 16
  config.seed = seed;
  config.half_pool_size = 128U;
  config.max_half_pool_size = 2048U;
  config.attempt_limit = 2'000'000U;
  config.memory_limit_bytes = 64U * 1024U * 1024U;
  return config;
}

ter::CrossoverSnapshotConfig small_config(std::uint64_t seed) {
  ter::CrossoverSnapshotConfig config;
  config.n = 36U;
  config.list_size = 64U;
  config.r2 = 6U;
  config.r1 = 12U;  // Q = 64
  config.seed = seed;
  config.half_pool_size = 512U;
  config.max_half_pool_size = 4096U;
  config.attempt_limit = 4'000'000U;
  config.memory_limit_bytes = 64U * 1024U * 1024U;
  return config;
}

// Independent brute-force oracle: recomputes pair counts directly from the
// raw entries' quotient residues via O(|A|*|B|) enumeration, without calling
// any function from q3report_crossover.cpp. Used to differentially verify
// the module's own direct/NTT agreement rather than trust it blindly.
std::vector<std::uint64_t> brute_force_pair_counts(const ter::InputSnapshot& input,
                                                    std::uint64_t q) {
  std::vector<std::uint64_t> counts(static_cast<std::size_t>(q), 0U);
  for (const auto& a : input.lists[0].entries) {
    for (const auto& b : input.lists[1].entries) {
      const auto t = (static_cast<std::uint64_t>(a.q) + b.q) & (q - 1U);
      ++counts[static_cast<std::size_t>(t)];
    }
  }
  return counts;
}

void deterministic_generation_and_fingerprint_replay() {
  const auto first = ter::build_scaling_snapshot(tiny_config(0x1234ULL));
  const auto second = ter::build_scaling_snapshot(tiny_config(0x1234ULL));
  require(first.status == ter::CrossoverStatus::Completed, "tiny snapshot must build");
  require(second.status == ter::CrossoverStatus::Completed, "repeat tiny snapshot must build");
  require(first.value.fingerprint == second.value.fingerprint,
          "identical seed and config must reproduce an identical fingerprint");
  require(first.value.input.lists[0].entries.size() ==
              second.value.input.lists[0].entries.size(),
          "identical seed and config must reproduce identical list sizes");
}

void distinct_seeds_produce_distinct_snapshots() {
  const auto first = ter::build_scaling_snapshot(tiny_config(0x1001ULL));
  const auto second = ter::build_scaling_snapshot(tiny_config(0x2002ULL));
  require(first.status == ter::CrossoverStatus::Completed, "seed 1 snapshot must build");
  require(second.status == ter::CrossoverStatus::Completed, "seed 2 snapshot must build");
  require(first.value.fingerprint != second.value.fingerprint,
          "distinct seeds must produce distinct fingerprints");
}

void audit_detects_corrupted_digits_and_corrupted_quotient() {
  auto snapshot = ter::build_scaling_snapshot(tiny_config(0x3003ULL));
  require(snapshot.status == ter::CrossoverStatus::Completed, "snapshot must build");
  std::string message;
  require(ter::audit_scaling_snapshot(snapshot.value, &message) ==
              ter::CrossoverStatus::Completed,
          "unmodified snapshot must pass the independent audit");

  auto corrupted_digits = snapshot.value;
  corrupted_digits.input.lists[0].entries[0].digits[0] += 1;
  message.clear();
  require(ter::audit_scaling_snapshot(corrupted_digits, &message) !=
              ter::CrossoverStatus::Completed,
          "a corrupted digit must fail the digit-distribution or residue check");

  auto corrupted_quotient = snapshot.value;
  corrupted_quotient.input.lists[0].entries[0].q += 1U;
  message.clear();
  require(ter::audit_scaling_snapshot(corrupted_quotient, &message) !=
              ter::CrossoverStatus::Completed,
          "a corrupted stored quotient must fail the recomputed-quotient check");

  auto duplicated_id = snapshot.value;
  duplicated_id.input.lists[0].entries[1].id = duplicated_id.input.lists[0].entries[0].id;
  message.clear();
  require(ter::audit_scaling_snapshot(duplicated_id, &message) !=
              ter::CrossoverStatus::Completed,
          "a duplicated EntryId within one list must fail the audit");
}

void cold_ntt_matches_independent_brute_force_oracle() {
  const auto snapshot = ter::build_scaling_snapshot(small_config(0x4004ULL));
  require(snapshot.status == ter::CrossoverStatus::Completed, "small snapshot must build");

  const auto result = ter::run_crossover_point(snapshot.value, nullptr, 64U * 1024U * 1024U);
  if (result.status != ter::CrossoverStatus::Completed) {
    std::cerr << "status=" << static_cast<int>(result.status) << " message=" << result.message
              << '\n';
  }
  require(result.status == ter::CrossoverStatus::Completed, "cold crossover run must complete");
  require(result.report.exactness_passed, "NTT coefficients must equal the direct count path");
  require(result.report.direct_matches_ntt, "direct and NTT selected counts must be identical");

  const auto reference = brute_force_pair_counts(snapshot.value.input, result.report.q);
  std::uint64_t reference_total = 0;
  std::uint64_t reference_occupied = 0;
  for (const auto value : reference) {
    reference_total += value;
    reference_occupied += value != 0U;
  }
  require(reference_total == result.report.a_size * result.report.b_size,
          "brute-force pair-count conservation: sum_t pairCount[t] == |A|*|B|");
  require(reference_occupied == result.report.occupied_pair_residues,
          "brute-force occupied-residue count must match the module's reported value");

  const auto& arithmetic = snapshot.value.input.spec.arithmetic;
  std::uint64_t reference_weighted_sum = 0;
  for (const auto& c : snapshot.value.input.lists[2].entries) {
    const auto target = ter::required_pair_residue(c.q, arithmetic);
    reference_weighted_sum += reference[target];
  }
  std::uint64_t brute_force_raw_triples = 0;
  for (const auto& a : snapshot.value.input.lists[0].entries) {
    for (const auto& b : snapshot.value.input.lists[1].entries) {
      for (const auto& c : snapshot.value.input.lists[2].entries) {
        // Required relationship: qA(a)+qB(b)+qC(c) == tau (mod Q), not 0.
        const auto sum = (static_cast<std::uint64_t>(a.q) + b.q + c.q) & (result.report.q - 1U);
        brute_force_raw_triples += sum == arithmetic.tau;
      }
    }
  }
  require(reference_weighted_sum == brute_force_raw_triples,
          "C-entry-weighted conservation: sum over C of pairCount[target(c)] == raw triples");
}

void amortized_plan_matches_cold_plan() {
  const auto snapshot = ter::build_scaling_snapshot(small_config(0x5005ULL));
  require(snapshot.status == ter::CrossoverStatus::Completed, "small snapshot must build");

  const auto cold = ter::run_crossover_point(snapshot.value, nullptr, 64U * 1024U * 1024U);
  require(cold.status == ter::CrossoverStatus::Completed, "cold run must complete");
  require(cold.report.timing.ntt_plan_build_ns > 0U || cold.report.q <= 2U,
          "cold path should report nonzero plan-build time for a nontrivial Q");

  ter::NttPlan plan;
  std::string message;
  require(ter::build_ntt_plan(cold.report.q, &plan, &message) ==
              ter::CrossoverStatus::Completed,
          "explicit plan build must complete for a supported Q");

  const auto warm = ter::run_crossover_point(snapshot.value, &plan, 64U * 1024U * 1024U);
  require(warm.status == ter::CrossoverStatus::Completed, "amortized run must complete");
  require(warm.report.timing.ntt_plan_build_ns == 0U,
          "amortized path must not charge plan-build time");
  require(warm.report.exactness_passed && warm.report.direct_matches_ntt,
          "amortized path must still be exact");
  require(warm.report.all_residues.zero == cold.report.all_residues.zero &&
              warm.report.all_residues.maximum == cold.report.all_residues.maximum &&
              warm.report.c_weighted_selected.population ==
                  cold.report.c_weighted_selected.population,
          "amortized plan must produce identical coefficients to the cold path");
}

void repeated_execution_does_not_mutate_the_frozen_snapshot() {
  const auto snapshot = ter::build_scaling_snapshot(small_config(0x6006ULL));
  require(snapshot.status == ter::CrossoverStatus::Completed, "small snapshot must build");
  const auto first = ter::run_crossover_point(snapshot.value, nullptr, 64U * 1024U * 1024U);
  const auto second = ter::run_crossover_point(snapshot.value, nullptr, 64U * 1024U * 1024U);
  require(first.status == ter::CrossoverStatus::Completed &&
              second.status == ter::CrossoverStatus::Completed,
          "both repeated runs must complete");
  require(first.report.a_size == second.report.a_size &&
              first.report.occupied_pair_residues == second.report.occupied_pair_residues &&
              first.report.all_residues.maximum == second.report.all_residues.maximum,
          "repeated execution on the same frozen snapshot must reproduce identical logical counts");
}

void unsupported_transform_length_fails_closed() {
  ter::NttPlan plan;
  std::string message;
  require(ter::build_ntt_plan(std::uint64_t{1} << 24U, &plan, &message) ==
              ter::CrossoverStatus::NotApplicable,
          "a transform length beyond 2^23 must fail closed as NotApplicable");

  auto config = tiny_config(0x7007ULL);
  config.r1 = 30U;
  config.r2 = 4U;  // Q = 2^26, exceeds the supported 2^23 ceiling
  const auto rejected = ter::build_scaling_snapshot(config);
  require(rejected.status == ter::CrossoverStatus::InvalidConfiguration,
          "a configuration requesting Q beyond 2^23 must fail closed before generation");
}

void memory_preflight_rejects_undersized_budget() {
  const auto snapshot = ter::build_scaling_snapshot(small_config(0x8008ULL));
  require(snapshot.status == ter::CrossoverStatus::Completed, "small snapshot must build");
  const auto result = ter::run_crossover_point(snapshot.value, nullptr, 1U);
  require(result.status == ter::CrossoverStatus::ResourceLimit,
          "a one-byte memory ceiling must fail closed as ResourceLimit");
}

}  // namespace

int main() {
  deterministic_generation_and_fingerprint_replay();
  distinct_seeds_produce_distinct_snapshots();
  audit_detects_corrupted_digits_and_corrupted_quotient();
  cold_ntt_matches_independent_brute_force_oracle();
  amortized_plan_matches_cold_plan();
  repeated_execution_does_not_mutate_the_frozen_snapshot();
  unsupported_transform_length_fails_closed();
  memory_preflight_rejects_undersized_budget();
  std::cout << "q3report crossover tests passed\n";
  return 0;
}
