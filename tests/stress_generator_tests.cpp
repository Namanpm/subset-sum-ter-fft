#include "ter/stress_generator.hpp"
#include "ter/validator.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string_view>

namespace {

int failures = 0;

void expect(const bool condition, const std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

ter::TerStressConfig base_config() {
  ter::TerStressConfig config;
  config.source.n = 36U;
  config.source.b2 = 6U;
  config.source.b1 = 10U;
  config.source.seed = 0x5a17'5eed'1bad'f00dULL;
  config.source.mode = ter::GeneratorMode::PlantedMitm;
  config.source.plant_valid_triple = true;
  config.source.half_pool_size = 192U;
  config.source.max_half_pool_size = 768U;
  config.source.attempt_limit = 1'000'000U;
  config.source.memory_limit_bytes = 128U * 1024U * 1024U;
  config.candidate_pool_size = 48U;
  config.requested_list_sizes = {12U, 12U, 12U};
  config.prefilter_bits = 2U;
  config.solution_mode = ter::StressSolutionMode::SinglePlantedSolution;
  return config;
}

bool lists_equal(const ter::InputSnapshot& left, const ter::InputSnapshot& right) {
  for (std::size_t role = 0U; role < 3U; ++role) {
    if (left.lists[role].entries != right.lists[role].entries ||
        left.lists[role].residues_m1 != right.lists[role].residues_m1) {
      return false;
    }
  }
  return true;
}

void uniform_control_is_audited_and_replays() {
  auto config = base_config();
  config.mode = ter::StressMode::BucketSkew;
  config.bucket_profile = ter::BucketProfile::Uniform;
  const auto first = ter::build_stress_snapshot(config);
  const auto replay = ter::build_stress_snapshot(config);
  expect(first.status == ter::Status::Completed, "uniform stress fixture completes");
  expect(replay.status == ter::Status::Completed, "uniform fixture replays");
  if (first.status != ter::Status::Completed || replay.status != ter::Status::Completed) {
    return;
  }
  expect(ter::audit_snapshot_children(first.snapshot).status == ter::Status::Completed,
         "uniform fixture has independently audited children");
  expect(lists_equal(first.snapshot, replay.snapshot),
         "stress snapshot replay preserves selected child lists");
  expect(first.telemetry.buckets.normalized_entropy >= 0.50,
         "uniform control produces a measurable spread across actual quotient buckets");
}

void skew_and_imbalance_are_measured() {
  auto config = base_config();
  config.mode = ter::StressMode::Composite;
  config.bucket_profile = ter::BucketProfile::HotBuckets;
  config.hot_bucket_count = 1U;
  config.target_hot_mass_percent = 75U;
  config.hot_mass_tolerance_percent = 25U;
  config.requested_list_sizes = {8U, 12U, 16U};
  config.solution_mode = ter::StressSolutionMode::NoPlantedSolution;
  const auto result = ter::build_stress_snapshot(config);
  expect(result.status == ter::Status::Completed, "skewed imbalanced stress fixture completes");
  if (result.status != ter::Status::Completed) {
    return;
  }
  expect(result.telemetry.actual_list_sizes == config.requested_list_sizes,
         "stress fixture preserves independently requested A/B/C sizes");
  expect(result.telemetry.buckets.hot_mass_percent >= 50U,
         "hot-bucket workload is measurably concentrated");
  expect(result.telemetry.guaranteed_planted_witnesses == 0U,
         "no-planted mode does not claim an unplanted witness");
}

void group_size_mode_requires_and_preserves_real_imbalance() {
  auto config = base_config();
  config.mode = ter::StressMode::GroupSizeSkew;
  config.bucket_profile = ter::BucketProfile::Uniform;
  config.requested_list_sizes = {8U, 12U, 16U};
  const auto result = ter::build_stress_snapshot(config);
  expect(result.status == ter::Status::Completed, "group-size stress fixture completes");
  if (result.status == ter::Status::Completed) {
    expect(result.telemetry.actual_list_sizes == config.requested_list_sizes,
           "group-size mode preserves requested unequal list sizes");
  }

  config.requested_list_sizes = {12U, 12U, 12U};
  expect(ter::build_stress_snapshot(config).status == ter::Status::InvalidConfiguration,
         "group-size mode rejects an input with no group-size skew");
}

void quotient_width_boundaries_are_explicit() {
  for (const std::uint8_t bits : {std::uint8_t{31U}, std::uint8_t{32U}, std::uint8_t{33U}}) {
    const auto boundary = ter::describe_quotient_boundary(bits);
    expect(boundary.quotient_bits == bits, "boundary report preserves requested quotient width");
    expect(boundary.native_merge_applicable == (bits <= 32U),
           "boundary report matches the native 32-bit quotient representation");
    expect(boundary.requires_widened_quotient_representation == (bits > 32U),
           "boundary report does not pretend that a wider quotient fits natively");
  }
}

void native_width_boundary_fixtures_use_actual_quotient_residues() {
  for (const std::uint8_t quotient_bits : {std::uint8_t{31U}, std::uint8_t{32U}}) {
    auto config = base_config();
    config.source.b2 = 1U;
    config.source.b1 = static_cast<std::uint8_t>(quotient_bits + 1U);
    config.mode = ter::StressMode::QuotientBoundary;
    config.quotient_boundary_bits = quotient_bits;
    config.prefilter_bits = 2U;
    config.candidate_pool_size = 24U;
    config.requested_list_sizes = {8U, 8U, 8U};
    const auto result = ter::build_stress_snapshot(config);
    expect(result.status == ter::Status::Completed,
           "native-width boundary fixture completes through normal TER generation");
    if (result.status == ter::Status::Completed) {
      expect(result.telemetry.quotient_bits == quotient_bits,
             "boundary fixture reports the actual arithmetic quotient width");
      expect(ter::audit_snapshot_children(result.snapshot).status == ter::Status::Completed,
             "boundary fixture arithmetic sidecars are independently recomputed");
    }
  }
}

void impossible_request_refuses_instead_of_shrinking() {
  auto config = base_config();
  config.candidate_pool_size = 12U;
  config.requested_list_sizes = {24U, 24U, 24U};
  const auto result = ter::build_stress_snapshot(config);
  expect(result.status == ter::Status::InvalidConfiguration,
         "oversized requested list is rejected before generation");
}

void declared_memory_budget_covers_source_and_selected_fixture() {
  auto config = base_config();
  config.source.memory_limit_bytes = 512U;
  const auto result = ter::build_stress_snapshot(config);
  expect(result.status == ter::Status::ResourceLimit,
         "stress resource preflight refuses source-plus-output memory before generation");
}

void multiplicity_is_certified_by_the_independent_oracle() {
  auto config = base_config();
  config.mode = ter::StressMode::SolutionMultiplicity;
  config.bucket_profile = ter::BucketProfile::Uniform;
  config.candidate_pool_size = 64U;
  config.requested_list_sizes = {24U, 24U, 24U};
  config.solution_mode = ter::StressSolutionMode::MultiplePlantedSolutions;
  config.required_distinct_witnesses = 2U;
  const auto result = ter::build_stress_snapshot(config);
  expect(result.status == ter::Status::Completed,
         "multiple-witness stress fixture reaches its certified lower bound");
  if (result.status != ter::Status::Completed) {
    return;
  }
  expect(result.telemetry.exact_solution_count_known,
         "bounded cubic certification reports its exact local witness count");
  expect(result.telemetry.deliberately_planted_witnesses == 2U,
         "multiple-witness mode deliberately retains the requested witness lower bound");
  expect(result.certified_witnesses.size() >= 2U,
         "multiple-witness stress fixture exposes two independently auditable witnesses");
  for (const auto& witness : result.certified_witnesses) {
    const auto audited = ter::audit_candidate(result.snapshot,
                                              result.snapshot.lists[0].entries[witness.a],
                                              result.snapshot.lists[1].entries[witness.b],
                                              result.snapshot.lists[2].entries[witness.c]);
    expect(audited.valid, "every certified stress witness independently validates");
  }
}

void independent_audit_catches_stress_sidecar_corruption() {
  auto result = ter::build_stress_snapshot(base_config());
  expect(result.status == ter::Status::Completed, "corruption stress fixture builds");
  if (result.status != ter::Status::Completed) {
    return;
  }
  result.snapshot.lists[1].residues_m1[0] ^= 1U;
  expect(ter::audit_snapshot_children(result.snapshot).status == ter::Status::InvariantViolation,
         "independent audit rejects a corrupted stress sidecar");
}

void composite_n72_geometry_is_a_valid_synthetic_merge_fixture() {
  auto config = base_config();
  config.source.n = 72U;
  config.source.b2 = 6U;
  config.source.b1 = 25U;
  config.source.half_pool_size = 128U;
  config.source.max_half_pool_size = 512U;
  config.candidate_pool_size = 32U;
  config.requested_list_sizes = {8U, 12U, 16U};
  config.mode = ter::StressMode::Composite;
  config.bucket_profile = ter::BucketProfile::HotBuckets;
  config.hot_bucket_count = 1U;
  config.target_hot_mass_percent = 50U;
  config.hot_mass_tolerance_percent = 50U;
  const auto result = ter::build_stress_snapshot(config);
  expect(result.status == ter::Status::Completed,
         "n=72 composite stress geometry completes as a valid merge fixture");
  if (result.status == ter::Status::Completed) {
    expect(result.telemetry.quotient_bits == 19U,
           "n=72 fixture reports the selected actual quotient width");
    expect(result.telemetry.actual_list_sizes == config.requested_list_sizes,
           "n=72 fixture preserves its deliberately imbalanced geometry");
    expect(ter::audit_snapshot_children(result.snapshot).status == ter::Status::Completed,
           "n=72 synthetic fixture preserves exact TER child invariants");
  }
}

}  // namespace

int main() {
  uniform_control_is_audited_and_replays();
  skew_and_imbalance_are_measured();
  group_size_mode_requires_and_preserves_real_imbalance();
  quotient_width_boundaries_are_explicit();
  native_width_boundary_fixtures_use_actual_quotient_residues();
  impossible_request_refuses_instead_of_shrinking();
  declared_memory_budget_covers_source_and_selected_fixture();
  multiplicity_is_certified_by_the_independent_oracle();
  independent_audit_catches_stress_sidecar_corruption();
  composite_n72_geometry_is_a_valid_synthetic_merge_fixture();
  if (failures != 0) {
    std::cerr << failures << " stress-generator assertion(s) failed\n";
    return 1;
  }
  std::cout << "stress generator tests passed\n";
  return 0;
}
