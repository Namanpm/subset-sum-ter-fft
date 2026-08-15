#include "ter/generator.hpp"
#include "ter/validator.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string_view>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

ter::SnapshotConfig planted_config() {
  ter::SnapshotConfig config;
  config.family = ter::ShapeFamily::FiniteSanity;
  config.n = 36;
  config.b2 = 6;
  config.b1 = 10;
  config.seed = 0xdecafbad12345678ULL;
  config.requested_list_size = 12;
  config.mode = ter::GeneratorMode::PlantedMitm;
  config.plant_valid_triple = true;
  config.half_pool_size = 96;
  config.max_half_pool_size = 384;
  config.attempt_limit = 500'000;
  config.memory_limit_bytes = 64U * 1024U * 1024U;
  return config;
}

bool lists_equal(const ter::TerList& left, const ter::TerList& right) {
  return left.role == right.role && left.target_m2 == right.target_m2 &&
         left.entries == right.entries && left.residues_m1 == right.residues_m1;
}

void planted_mitm_emits_three_authentic_lists() {
  const auto result = ter::build_snapshot(planted_config());
  expect(result.status == ter::Status::Completed, "planted MITM snapshot completes");
  if (result.status != ter::Status::Completed) {
    return;
  }
  const auto& snapshot = result.snapshot;
  expect(snapshot.has_planted_triple, "snapshot records a planted valid triple");
  for (const auto& list : snapshot.lists) {
    expect(list.entries.size() == 12U, "each child list has the exact requested size");
    expect(list.entries.size() == list.residues_m1.size(),
           "cold arithmetic sidecar is keyed by entry ID");
  }
  const auto audit = ter::audit_snapshot_children(snapshot);
  expect(audit.status == ter::Status::Completed,
         "every emitted child passes independent structural and arithmetic audit");

  const auto& a = snapshot.lists[0].entries[snapshot.planted_entry_ids[0]];
  const auto& b = snapshot.lists[1].entries[snapshot.planted_entry_ids[1]];
  const auto& c = snapshot.lists[2].entries[snapshot.planted_entry_ids[2]];
  const auto q_sum = static_cast<ter::QuotientResidue>(
      (static_cast<std::uint64_t>(a.q) + b.q + c.q) & snapshot.spec.arithmetic.mask_q);
  expect(q_sum == snapshot.spec.arithmetic.tau,
         "planted entries satisfy the stronger level-1 quotient target");
}

void generation_replays_exactly() {
  const auto first = ter::build_snapshot(planted_config());
  const auto replay = ter::build_snapshot(planted_config());
  expect(first.status == ter::Status::Completed && replay.status == ter::Status::Completed,
         "both deterministic snapshots complete");
  if (first.status != ter::Status::Completed || replay.status != ter::Status::Completed) {
    return;
  }
  expect(first.snapshot.instance.values_le == replay.snapshot.instance.values_le,
         "snapshot replay preserves the density-one instance");
  for (std::size_t index = 0; index < 3U; ++index) {
    expect(lists_equal(first.snapshot.lists[index], replay.snapshot.lists[index]),
           "snapshot replay preserves canonical child entries and metadata");
  }
  expect(first.telemetry.attempts == replay.telemetry.attempts,
         "deterministic generation preserves logical telemetry");
}

void child_audit_detects_arithmetic_corruption() {
  auto result = ter::build_snapshot(planted_config());
  expect(result.status == ter::Status::Completed, "corruption fixture builds");
  if (result.status != ter::Status::Completed) {
    return;
  }
  result.snapshot.lists[0].residues_m1[0] ^= 1U;
  expect(ter::audit_snapshot_children(result.snapshot).status ==
             ter::Status::InvariantViolation,
         "audit recomputation rejects corrupted arithmetic sidecar data");
}

void attempt_exhaustion_is_not_an_empty_completed_list() {
  auto config = planted_config();
  config.mode = ter::GeneratorMode::RejectionReference;
  config.plant_valid_triple = false;
  config.attempt_limit = 0;
  config.requested_list_size = 1;
  const auto result = ter::build_snapshot(config);
  expect(result.status == ter::Status::GenerationIncomplete,
         "attempt exhaustion reports GenerationIncomplete");
}

void invalid_modulus_width_is_rejected_before_bit_shifts() {
  auto config = planted_config();
  config.b1 = 64;
  const auto result = ter::build_snapshot(config);
  expect(result.status == ter::Status::InvalidConfiguration,
         "an unsafe modulus width is rejected before target masks are formed");
}

void impossible_generator_allocations_fail_before_materialization() {
  auto huge_modulus = planted_config();
  huge_modulus.family = ter::ShapeFamily::FiniteSanity;
  huge_modulus.n = 72;
  huge_modulus.b2 = 62;
  huge_modulus.b1 = 63;
  huge_modulus.requested_list_size = 1;
  huge_modulus.half_pool_size = 1;
  huge_modulus.max_half_pool_size = 1;
  expect(ter::build_snapshot(huge_modulus).status == ter::Status::ResourceLimit,
         "enormous M2 index is rejected without arithmetic wraparound");

  auto huge_pool = planted_config();
  huge_pool.requested_list_size = 1;
  huge_pool.half_pool_size = std::numeric_limits<std::uint32_t>::max();
  huge_pool.max_half_pool_size = huge_pool.half_pool_size;
  expect(ter::build_snapshot(huge_pool).status == ter::Status::ResourceLimit,
         "impossible MITM pools are rejected before sampling or allocation");

  auto exhaustive_output = planted_config();
  exhaustive_output.mode = ter::GeneratorMode::ExhaustiveReference;
  exhaustive_output.b2 = 1;
  exhaustive_output.b1 = 5;
  exhaustive_output.requested_list_size = 1;
  exhaustive_output.memory_limit_bytes = 8U * 1024U * 1024U;
  expect(ter::build_snapshot(exhaustive_output).status == ter::Status::ResourceLimit,
         "complete exhaustive output is budgeted before vector materialization");

  auto huge_instance = planted_config();
  huge_instance.n = 1'000'008U;
  huge_instance.requested_list_size = 1;
  expect(ter::build_snapshot(huge_instance).status == ter::Status::ResourceLimit,
         "density-one instance storage is budgeted before O(n squared) allocation");

  auto cumulative_exhaustive = planted_config();
  cumulative_exhaustive.mode = ter::GeneratorMode::ExhaustiveReference;
  cumulative_exhaustive.b2 = 10;
  cumulative_exhaustive.b1 = 14;
  cumulative_exhaustive.requested_list_size = 1;
  cumulative_exhaustive.memory_limit_bytes = 1'100U * 1024U;
  expect(ter::build_snapshot(cumulative_exhaustive).status ==
             ter::Status::ResourceLimit,
         "later exhaustive roles include already-resident complete child slices");

  auto cumulative_rejection = planted_config();
  cumulative_rejection.mode = ter::GeneratorMode::RejectionReference;
  cumulative_rejection.requested_list_size = 1'000;
  cumulative_rejection.memory_limit_bytes = 300U * 1024U;
  expect(ter::build_snapshot(cumulative_rejection).status ==
             ter::Status::ResourceLimit,
         "rejection set and copy peaks include already-resident child lists");

  auto planted_rank_sets = planted_config();
  planted_rank_sets.family = ter::ShapeFamily::PaperRounded;
  planted_rank_sets.n = 360;
  planted_rank_sets.b2 = 8;
  planted_rank_sets.b1 = 18;
  planted_rank_sets.requested_list_size = 512;
  planted_rank_sets.half_pool_size = 1'024;
  planted_rank_sets.max_half_pool_size = 1'024;
  planted_rank_sets.memory_limit_bytes = 900U * 1024U;
  expect(ter::build_snapshot(planted_rank_sets).status == ter::Status::ResourceLimit,
         "planted MITM budgets simultaneous rank-selection sets");
}

void exhaustive_reference_builds_the_complete_tiny_domain_slice() {
  auto config = planted_config();
  config.mode = ter::GeneratorMode::ExhaustiveReference;
  config.b2 = 16;
  config.b1 = 20;
  config.requested_list_size = 1;
  const auto result = ter::build_snapshot(config);
  expect(result.status == ter::Status::Completed,
         "exhaustive reference fixture completes at n=36");
  if (result.status != ter::Status::Completed) {
    return;
  }
  expect(ter::audit_snapshot_children(result.snapshot).status == ter::Status::Completed,
         "exhaustive child lists pass the same independent audit");
  for (std::size_t role = 0; role < 3U; ++role) {
    const auto& planted = result.snapshot.planted_vectors[role];
    const auto& entries = result.snapshot.lists[role].entries;
    expect(std::any_of(entries.begin(), entries.end(), [&planted](const auto& entry) {
             return entry.digits == planted;
           }),
           "exhaustive target slice contains its planted authentic child");
  }
}

}  // namespace

int main() {
  planted_mitm_emits_three_authentic_lists();
  generation_replays_exactly();
  child_audit_detects_arithmetic_corruption();
  attempt_exhaustion_is_not_an_empty_completed_list();
  invalid_modulus_width_is_rejected_before_bit_shifts();
  impossible_generator_allocations_fail_before_materialization();
  exhaustive_reference_builds_the_complete_tiny_domain_slice();

  if (failures != 0) {
    std::cerr << failures << " generator assertion(s) failed\n";
    return 1;
  }
  std::cout << "generator tests passed\n";
  return 0;
}
