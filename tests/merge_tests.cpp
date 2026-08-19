#include "ter/generator.hpp"
#include "ter/merge.hpp"
#include "ter/validator.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <utility>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

ter::SnapshotConfig merge_config() {
  ter::SnapshotConfig config;
  config.family = ter::ShapeFamily::FiniteSanity;
  config.n = 36;
  config.b2 = 6;
  config.b1 = 10;
  config.seed = 0x8b8b8b8b12344321ULL;
  config.requested_list_size = 8;
  config.mode = ter::GeneratorMode::PlantedMitm;
  config.plant_valid_triple = true;
  config.half_pool_size = 96;
  config.max_half_pool_size = 384;
  config.attempt_limit = 500'000;
  config.memory_limit_bytes = 64U * 1024U * 1024U;
  return config;
}

std::array<ter::ReferenceEntry, 3> planted_entries(const ter::InputSnapshot& snapshot) {
  return {
      snapshot.lists[0].entries[snapshot.planted_entry_ids[0]],
      snapshot.lists[1].entries[snapshot.planted_entry_ids[1]],
      snapshot.lists[2].entries[snapshot.planted_entry_ids[2]],
  };
}

void audit_validator_checks_every_parent_condition() {
  const auto built = ter::build_snapshot(merge_config());
  expect(built.status == ter::Status::Completed, "validator fixture builds");
  if (built.status != ter::Status::Completed) {
    return;
  }
  const auto& snapshot = built.snapshot;
  const auto original = planted_entries(snapshot);
  expect(ter::audit_candidate(snapshot, original[0], original[1], original[2]).valid,
         "planted triple passes the exact audit validator");

  auto wrong_modulus = original;
  wrong_modulus[2].q = static_cast<ter::QuotientResidue>(
      (static_cast<std::uint64_t>(wrong_modulus[2].q) + 1U) &
      snapshot.spec.arithmetic.mask_q);
  expect(ter::audit_candidate(snapshot,
                              wrong_modulus[0],
                              wrong_modulus[1],
                              wrong_modulus[2])
             .reject == ter::RejectReason::Modular,
         "wrong quotient relation is rejected before structural work");

  for (const std::array<std::int8_t, 3> bad_digits :
       {std::array<std::int8_t, 3>{-1, -1, -1},
        std::array<std::int8_t, 3>{-1, -1, 0},
        std::array<std::int8_t, 3>{1, 1, 0},
        std::array<std::int8_t, 3>{1, 1, 1}}) {
    auto invalid = original;
    for (std::size_t child = 0; child < 3U; ++child) {
      invalid[child].digits[0] = bad_digits[child];
    }
    expect(ter::audit_candidate(snapshot, invalid[0], invalid[1], invalid[2]).reject ==
               ter::RejectReason::Structural,
           "parent coordinate sums outside {-1,0,+1} are rejected");
  }

  auto wrong_distribution = original;
  bool changed_distribution = false;
  for (std::size_t coordinate = 0; coordinate < snapshot.spec.shape.n; ++coordinate) {
    const int parent_digit = original[0].digits[coordinate] +
                             original[1].digits[coordinate] +
                             original[2].digits[coordinate];
    if (parent_digit != 0) {
      for (auto& entry : wrong_distribution) {
        entry.digits[coordinate] = 0;
      }
      changed_distribution = true;
      break;
    }
  }
  expect(changed_distribution, "fixture exposes a nonzero parent coordinate");
  expect(ter::audit_candidate(snapshot,
                              wrong_distribution[0],
                              wrong_distribution[1],
                              wrong_distribution[2])
             .reject == ter::RejectReason::Distribution,
         "wrong exact parent distribution is rejected");

  auto wrong_arithmetic = original;
  bool found_arithmetic_mutation = false;
  for (std::size_t first = 0; first < snapshot.spec.shape.n && !found_arithmetic_mutation;
       ++first) {
    const int first_parent = original[0].digits[first] + original[1].digits[first] +
                             original[2].digits[first];
    for (std::size_t second = first + 1U; second < snapshot.spec.shape.n; ++second) {
      const int second_parent = original[0].digits[second] + original[1].digits[second] +
                                original[2].digits[second];
      if (first_parent == second_parent) {
        continue;
      }
      auto candidate = original;
      for (auto& entry : candidate) {
        std::swap(entry.digits[first], entry.digits[second]);
      }
      const auto checked =
          ter::audit_candidate(snapshot, candidate[0], candidate[1], candidate[2]);
      if (checked.reject == ter::RejectReason::Arithmetic) {
        wrong_arithmetic = std::move(candidate);
        found_arithmetic_mutation = true;
        break;
      }
    }
  }
  expect(found_arithmetic_mutation,
         "fixture admits a distribution-preserving arithmetic corruption");
  if (found_arithmetic_mutation) {
    expect(ter::audit_candidate(snapshot,
                                wrong_arithmetic[0],
                                wrong_arithmetic[1],
                                wrong_arithmetic[2])
               .reject == ter::RejectReason::Arithmetic,
           "full M1 audit rejects arithmetic corruption");
  }
}

void practical_baseline_equals_the_cubic_oracle() {
  const auto built = ter::build_snapshot(merge_config());
  expect(built.status == ter::Status::Completed, "merge fixture builds");
  if (built.status != ter::Status::Completed) {
    return;
  }
  const auto cubic = ter::run_cubic_oracle(built.snapshot, ter::OutputPolicy::Correctness);
  const auto baseline =
      ter::run_pair_baseline(built.snapshot, ter::OutputPolicy::Correctness);
  expect(cubic.status == ter::Status::Completed, "cubic oracle completes");
  expect(baseline.status == ter::Status::Completed, "pair baseline completes");
  expect(cubic.parents == baseline.parents,
         "cubic oracle and pair baseline emit the same canonical parent set");
  expect(cubic.valid_provenance == baseline.valid_provenance,
         "cubic oracle and pair baseline preserve exact provenance multiplicity");
  expect(cubic.telemetry.raw_valid_triples == baseline.telemetry.raw_valid_triples,
         "cubic oracle and pair baseline count the same raw valid triples");
  expect(baseline.telemetry.pair_combinations == 64U,
         "pair baseline materializes the cheapest 8x8 pair orientation");
  expect(baseline.telemetry.candidate_triples == baseline.telemetry.structural_checks,
         "every recovered modular triple receives exact validation");
  expect(!baseline.parents.empty(), "planted triple survives into the parent list");
}

// The tiled reference merge must be semantically indistinguishable from the
// existing exact baselines: same parents, same multiplicities, same provenance.
void tiled_reference_merge_equals_the_exact_baselines() {
  const auto built = ter::build_snapshot(merge_config());
  expect(built.status == ter::Status::Completed, "tiled merge fixture builds");
  if (built.status != ter::Status::Completed) {
    return;
  }
  const auto cubic = ter::run_cubic_oracle(built.snapshot, ter::OutputPolicy::Correctness);
  const auto baseline =
      ter::run_pair_baseline(built.snapshot, ter::OutputPolicy::Correctness);
  const auto tiled = ter::run_pair_tiled(built.snapshot, ter::OutputPolicy::Correctness);
  expect(tiled.status == ter::Status::Completed, "tiled merge completes");
  expect(tiled.parents == cubic.parents,
         "tiled merge and cubic oracle emit the same canonical parent set");
  expect(tiled.parents == baseline.parents,
         "tiled merge and pair baseline emit the same canonical parent set");
  expect(tiled.valid_provenance == baseline.valid_provenance,
         "tiled merge preserves exact provenance multiplicity");
  expect(tiled.telemetry.raw_valid_triples == baseline.telemetry.raw_valid_triples,
         "tiled merge counts the same raw valid triples");
  expect(tiled.telemetry.candidate_triples == baseline.telemetry.candidate_triples,
         "tiled merge locates exactly the same modular candidates");
  expect(tiled.telemetry.unique_parents == baseline.telemetry.unique_parents &&
             tiled.telemetry.duplicate_parents == baseline.telemetry.duplicate_parents,
         "tiled merge reports the same parent multiplicity split");
  expect(tiled.telemetry.structural_rejects == baseline.telemetry.structural_rejects &&
             tiled.telemetry.distribution_rejects == baseline.telemetry.distribution_rejects &&
             tiled.telemetry.arithmetic_rejects == baseline.telemetry.arithmetic_rejects,
         "tiled merge reproduces the same rejection breakdown");
  expect(tiled.telemetry.orientation.pair_roles == baseline.telemetry.orientation.pair_roles &&
             tiled.telemetry.orientation.lookup_role ==
                 baseline.telemetry.orientation.lookup_role,
         "tiled merge uses the same pair orientation planner");

  // Structural difference: no pair-record array is materialized.
  expect(tiled.telemetry.pair_records == 0U,
         "the tiled path materializes no pair records");
  expect(baseline.telemetry.pair_records == baseline.telemetry.pair_combinations,
         "the historical baseline still materializes one record per pair");
  expect(tiled.telemetry.pair_combinations == baseline.telemetry.pair_combinations,
         "the logical pair universe is unchanged");
  expect(tiled.telemetry.pair_index_bytes < baseline.telemetry.pair_index_bytes,
         "the tiled path declares a smaller arithmetic search structure");

  // Benchmark policy must agree with the baseline too.
  const auto tiled_bench = ter::run_pair_tiled(built.snapshot, ter::OutputPolicy::Benchmark);
  const auto baseline_bench =
      ter::run_pair_baseline(built.snapshot, ter::OutputPolicy::Benchmark);
  expect(tiled_bench.status == ter::Status::Completed, "tiled benchmark policy completes");
  expect(tiled_bench.parents == baseline_bench.parents,
         "tiled and baseline agree under benchmark policy");
  expect(tiled_bench.valid_provenance.empty(),
         "benchmark policy collects no provenance");
}

void tiled_reference_merge_fails_closed_and_handles_empty_results() {
  auto built = ter::build_snapshot(merge_config());
  expect(built.status == ter::Status::Completed, "tiled resource fixture builds");
  if (built.status != ter::Status::Completed) {
    return;
  }
  built.snapshot.spec.memory_limit_bytes = 1U;
  const auto limited = ter::run_pair_tiled(built.snapshot, ter::OutputPolicy::Benchmark);
  expect(limited.status == ter::Status::ResourceLimit,
         "tiled auxiliary exhaustion reports ResourceLimit");
  expect(limited.parents.empty(), "a resource failure exposes no partial parent list");

  auto empty = ter::build_snapshot(merge_config());
  expect(empty.status == ter::Status::Completed, "tiled empty fixture builds");
  if (empty.status != ter::Status::Completed) {
    return;
  }
  for (auto& list : empty.snapshot.lists) {
    list.entries.clear();
    list.residues_m1.clear();
  }
  const auto result = ter::run_pair_tiled(empty.snapshot, ter::OutputPolicy::Correctness);
  expect(result.status == ter::Status::Completed,
         "an exhaustively empty tiled merge is a successful computation");
  expect(result.parents.empty() && result.telemetry.candidate_triples == 0U,
         "an empty tiled merge reports zero parents and zero candidates");

  auto corrupted = ter::build_snapshot(merge_config());
  corrupted.snapshot.lists[0].entries[0].q ^= 1U;
  expect(ter::run_pair_tiled(corrupted.snapshot, ter::OutputPolicy::Correctness).status !=
             ter::Status::Completed,
         "tiled merge rejects corrupted child metadata");
}

void pair_orientation_is_deterministic_and_minimal() {
  auto built = ter::build_snapshot(merge_config());
  expect(built.status == ter::Status::Completed, "orientation fixture builds");
  if (built.status != ter::Status::Completed) {
    return;
  }
  built.snapshot.lists[0].entries.resize(2U);
  built.snapshot.lists[0].residues_m1.resize(2U);
  built.snapshot.lists[1].entries.resize(5U);
  built.snapshot.lists[1].residues_m1.resize(5U);
  const auto orientation = ter::choose_pair_orientation(built.snapshot);
  expect(orientation.pair_roles == std::array<std::uint8_t, 2>{0, 1} &&
             orientation.lookup_role == 2,
         "planner chooses the lowest entry-product pair with lexical tie-breaking");
}

void resource_failure_never_becomes_a_completed_empty_merge() {
  auto built = ter::build_snapshot(merge_config());
  expect(built.status == ter::Status::Completed, "resource fixture builds");
  if (built.status != ter::Status::Completed) {
    return;
  }
  built.snapshot.spec.memory_limit_bytes = 1U;
  const auto result =
      ter::run_pair_baseline(built.snapshot, ter::OutputPolicy::Benchmark);
  expect(result.status == ter::Status::ResourceLimit,
         "pair-index exhaustion reports ResourceLimit");
  expect(result.parents.empty(), "resource failure exposes no partial parent list");

  built = ter::build_snapshot(merge_config());
  const auto orientation = ter::choose_pair_orientation(built.snapshot);
  const auto pair_count =
      built.snapshot.lists[orientation.pair_roles[0]].entries.size() *
      built.snapshot.lists[orientation.pair_roles[1]].entries.size();
  built.snapshot.spec.memory_limit_bytes =
      (built.snapshot.spec.arithmetic.q + 1U + pair_count) * sizeof(std::uint64_t);
  const auto omitted_cursor =
      ter::run_pair_baseline(built.snapshot, ter::OutputPolicy::Benchmark);
  expect(omitted_cursor.status == ter::Status::ResourceLimit,
         "memory ceiling charges the live CSR cursor copy and output reserve");

  built = ter::build_snapshot(merge_config());
  built.snapshot.spec.memory_limit_bytes = 1U;
  const auto cubic =
      ter::run_cubic_oracle(built.snapshot, ter::OutputPolicy::Correctness);
  expect(cubic.status == ter::Status::ResourceLimit,
         "cubic oracle obeys the declared memory ceiling");
}

void public_merge_rejects_corrupted_child_metadata() {
  auto built = ter::build_snapshot(merge_config());
  expect(built.status == ter::Status::Completed, "corrupt-API fixture builds");
  if (built.status != ter::Status::Completed) {
    return;
  }
  built.snapshot.lists[0].residues_m1[0] ^= 1U;
  const auto cubic =
      ter::run_cubic_oracle(built.snapshot, ter::OutputPolicy::Correctness);
  const auto pair =
      ter::run_pair_baseline(built.snapshot, ter::OutputPolicy::Correctness);
  expect(cubic.status == ter::Status::InvariantViolation,
         "cubic public API rejects a corrupted child snapshot");
  expect(pair.status == ter::Status::InvariantViolation,
         "pair public API rejects a corrupted child snapshot");
}

void a_genuine_empty_result_is_completed() {
  auto built = ter::build_snapshot(merge_config());
  expect(built.status == ter::Status::Completed, "empty-result fixture builds");
  if (built.status != ter::Status::Completed) {
    return;
  }
  built.snapshot.lists[0].entries.clear();
  built.snapshot.lists[0].residues_m1.clear();
  const auto result =
      ter::run_pair_baseline(built.snapshot, ter::OutputPolicy::Correctness);
  expect(result.status == ter::Status::Completed,
         "an exhaustively empty merge is a successful computation");
  expect(result.parents.empty() && result.telemetry.candidate_triples == 0U,
         "a successful empty merge reports zero parents and zero candidates");
}

}  // namespace

int main() {
  audit_validator_checks_every_parent_condition();
  practical_baseline_equals_the_cubic_oracle();
  tiled_reference_merge_equals_the_exact_baselines();
  tiled_reference_merge_fails_closed_and_handles_empty_results();
  pair_orientation_is_deterministic_and_minimal();
  resource_failure_never_becomes_a_completed_empty_merge();
  public_merge_rejects_corrupted_child_metadata();
  a_genuine_empty_result_is_completed();

  if (failures != 0) {
    std::cerr << failures << " merge assertion(s) failed\n";
    return 1;
  }
  std::cout << "merge tests passed\n";
  return 0;
}
