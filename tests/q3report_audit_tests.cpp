#include "ter/full_ter.hpp"
#include "ter/generator.hpp"
#include "ter/q3report_audit.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <numeric>
#include <string_view>
#include <utility>

namespace {

void require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

ter::SnapshotConfig test_config() {
  ter::SnapshotConfig config;
  config.family = ter::ShapeFamily::FiniteSanity;
  config.n = 36U;
  config.b2 = 4U;
  config.b1 = 10U;
  config.seed = 0x51335245504f5254ULL;
  config.requested_list_size = 8U;
  config.mode = ter::GeneratorMode::PlantedMitm;
  config.plant_valid_triple = true;
  config.half_pool_size = 96U;
  config.max_half_pool_size = 384U;
  config.attempt_limit = 500'000U;
  config.memory_limit_bytes = 64U * 1024U * 1024U;
  return config;
}

ter::Instance balanced_full_instance() {
  const auto built = ter::build_instance(36U, 10U, 0x5143415054555245ULL);
  require(built.status == ter::Status::Completed, "native capture test instance must build");
  auto instance = built.value;
  std::fill(instance.planted_root.begin(), instance.planted_root.end(), std::uint8_t{0});
  instance.root_target_le.assign(instance.words_per_value, 0U);
  for (std::uint32_t half = 0; half < 2U; ++half) {
    for (std::uint32_t offset = 0; offset < 9U; ++offset) {
      const auto coordinate = half * 18U + offset;
      instance.planted_root[coordinate] = 1U;
      instance.root_target_le[0] += instance.values_le[coordinate];
    }
  }
  instance.root_target_le[0] &= (std::uint64_t{1} << 36U) - 1U;
  return instance;
}

void count_only_ntt_matches_scalar_reference_on_frozen_snapshot() {
  const auto generated = ter::build_snapshot(test_config());
  require(generated.status == ter::Status::Completed, "test snapshot must build");
  const auto frozen = ter::freeze_l2_merge_snapshot(
      generated.snapshot, ter::Q3SnapshotFamily::DistributionMatched, test_config().seed);
  require(frozen.status == ter::Q3ReportStatus::Completed,
          "valid source snapshot must freeze");

  const auto result = ter::run_q3report_count_only_experiment(frozen.value, {});
  if (result.status != ter::Q3ReportStatus::Completed) {
    std::cerr << "Q3 result status=" << static_cast<int>(result.status)
              << " message=" << result.message << '\n';
  }
  require(result.status == ter::Q3ReportStatus::Completed,
          "count-only experiment must complete");
  require(result.telemetry.scalar_pair_counts == result.telemetry.ntt_pair_counts,
          "every NTT coefficient must equal the scalar cyclic reference");
  require(std::accumulate(result.telemetry.scalar_pair_counts.begin(),
                          result.telemetry.scalar_pair_counts.end(), std::uint64_t{0}) ==
              result.telemetry.pair_universe,
          "cyclic pair histogram must conserve the complete A-times-B universe");
  require(result.telemetry.raw_arithmetic_triples > 0U,
          "planted snapshot must contain arithmetic triples");
}

void deterministic_seed_suite_preserves_all_exactness_checks() {
  constexpr std::array<std::uint64_t, 4> seeds{
      0x1001U, 0x2003U, 0x3007U, 0x4009U};
  for (const auto seed : seeds) {
    auto config = test_config();
    config.seed = seed;
    const auto generated = ter::build_snapshot(config);
    require(generated.status == ter::Status::Completed,
            "every deterministic seed snapshot must build");
    const auto frozen = ter::freeze_l2_merge_snapshot(
        generated.snapshot, ter::Q3SnapshotFamily::DistributionMatched, seed);
    const auto result = ter::run_q3report_count_only_experiment(frozen.value, {});
    require(result.status == ter::Q3ReportStatus::Completed &&
                result.telemetry.independent_oracle_ran && result.telemetry.exactness_passed,
            "every deterministic seed must agree across oracle, direct, scalar, and NTT");
  }
}

void signed_tau_uses_widened_negative_arithmetic() {
  const auto generated = ter::build_snapshot(test_config());
  require(generated.status == ter::Status::Completed,
          "signed-tau fixture source snapshot must build");
  auto snapshot = generated.snapshot;
  // Each L2 vector has (+4,-2), so constant coefficient 342 produces child
  // residue 684. Each valid L1 parent has (+8,-2), hence parent residue 4.
  // Therefore tau=(4-12-12-12)/16=-2 mod 64 = 62.
  std::fill(snapshot.instance.residues_m1.begin(),
            snapshot.instance.residues_m1.end(), 342U);
  const auto arithmetic = ter::build_arithmetic(snapshot.instance.n, 4U, 10U,
                                                 12U, 12U, 4U);
  require(arithmetic.status == ter::Status::Completed && arithmetic.value.target_c2 == 12U,
          "signed-tau fixture arithmetic must derive exactly");
  snapshot.spec.arithmetic = arithmetic.value;
  for (auto& list : snapshot.lists) {
    list.target_m2 = 12U;
    for (std::size_t entry = 0; entry < list.entries.size(); ++entry) {
      list.residues_m1[entry] = 684U;
      list.entries[entry].q = ter::quotient_residue(684U, 12U, arithmetic.value);
    }
  }
  const auto frozen = ter::freeze_l2_merge_snapshot(
      std::move(snapshot), ter::Q3SnapshotFamily::DistributionMatched, 0x5349474e45445441ULL);
  const auto result = ter::run_q3report_count_only_experiment(frozen.value, {});
  require(result.status == ter::Q3ReportStatus::Completed,
          "negative signed-tau fixture must complete exactly");
  require(result.telemetry.tau == 62U,
          "telemetry tau must use exact signed quotient arithmetic");
}

void authentic_fullter_capture_matches_the_same_exact_authorities() {
  ter::FullTerConfig config;
  config.family = ter::ShapeFamily::FiniteSanity;
  config.execution_mode = ter::TerExecutionMode::FullTer;
  config.target_mode = ter::TerTargetMode::GuidedPlanted;
  config.seed = 79U;
  config.memory_limit_bytes = 64U * 1024U * 1024U;
  const auto captured = ter::capture_full_ter_l2_merge_snapshot(
      balanced_full_instance(), config, 0U);
  if (captured.status != ter::Status::Completed) {
    std::cerr << "capture status=" << static_cast<int>(captured.status)
              << " message=" << captured.message << '\n';
  }
  require(captured.status == ter::Status::Completed,
          "complete Full TER must expose a genuine captured L2 merge");
  const auto frozen = ter::freeze_l2_merge_snapshot(
      captured.snapshot, ter::Q3SnapshotFamily::AuthenticFullTer, config.seed);
  const auto result = ter::run_q3report_count_only_experiment(frozen.value, {});
  if (result.status != ter::Q3ReportStatus::Completed) {
    std::cerr << "authentic Q3 result status=" << static_cast<int>(result.status)
              << " message=" << result.message << '\n';
  }
  require(result.status == ter::Q3ReportStatus::Completed,
          "genuine Full TER snapshot must pass count-only exact checks");
  require(result.telemetry.independent_oracle_ran,
          "small genuine Full TER capture must receive the independent cubic oracle");
  require(result.telemetry.scalar_pair_counts == result.telemetry.ntt_pair_counts,
          "genuine Full TER NTT must equal every scalar coefficient");

  const auto replay = ter::capture_full_ter_l2_merge_snapshot(
      balanced_full_instance(), config, 0U);
  require(replay.status == ter::Status::Completed,
          "complete Full TER capture must replay deterministically");
  const auto replay_frozen = ter::freeze_l2_merge_snapshot(
      replay.snapshot, ter::Q3SnapshotFamily::AuthenticFullTer, config.seed);
  require(replay_frozen.value.fingerprint == frozen.value.fingerprint,
          "native capture fingerprint must replay exactly");
}

void malformed_or_resource_limited_snapshots_fail_closed() {
  const auto generated = ter::build_snapshot(test_config());
  require(generated.status == ter::Status::Completed, "negative-test snapshot must build");
  auto frozen = ter::freeze_l2_merge_snapshot(
      generated.snapshot, ter::Q3SnapshotFamily::DistributionMatched, test_config().seed);
  frozen.value.input.lists[0].entries[0].q ^= 1U;
  const auto corrupted = ter::run_q3report_count_only_experiment(frozen.value, {});
  require(corrupted.status == ter::Q3ReportStatus::InvalidSnapshot,
          "post-freeze mutation must fail the snapshot fingerprint audit");

  const auto clean = ter::freeze_l2_merge_snapshot(
      generated.snapshot, ter::Q3SnapshotFamily::DistributionMatched, test_config().seed);
  ter::Q3ReportConfig tiny_memory;
  tiny_memory.memory_limit_bytes = 1U;
  const auto limited = ter::run_q3report_count_only_experiment(clean.value, tiny_memory);
  require(limited.status == ter::Q3ReportStatus::ResourceLimit,
          "declared resource ceilings must fail before the experiment runs");

  const auto default_snapshot = ter::freeze_l2_merge_snapshot(
      {}, ter::Q3SnapshotFamily::DistributionMatched, 0U);
  const auto default_result = ter::run_q3report_count_only_experiment(default_snapshot.value, {});
  require(default_result.status == ter::Q3ReportStatus::InvalidConfiguration ||
              default_result.status == ter::Q3ReportStatus::InvalidSnapshot,
          "default representable snapshot must never report completion");
}

}  // namespace

int main() {
  count_only_ntt_matches_scalar_reference_on_frozen_snapshot();
  deterministic_seed_suite_preserves_all_exactness_checks();
  signed_tau_uses_widened_negative_arithmetic();
  authentic_fullter_capture_matches_the_same_exact_authorities();
  malformed_or_resource_limited_snapshots_fail_closed();
  std::cout << "q3report audit tests passed\n";
  return 0;
}
