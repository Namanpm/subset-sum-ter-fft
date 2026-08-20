#include "ter/full_ter.hpp"
#include "ter/generator.hpp"
#include "ter/limitations.hpp"
#include "ter/q3report_audit.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t kMemoryLimit = ter::limits::q3report::kDefaultMemoryBytes;
constexpr std::uint64_t kSeed = 0x51335245504f5254ULL;

[[nodiscard]] const char* family_name(const ter::Q3SnapshotFamily family) {
  switch (family) {
    case ter::Q3SnapshotFamily::AuthenticFullTer:
      return "AUTHENTIC_FULLTER";
    case ter::Q3SnapshotFamily::DistributionMatched:
      return "DISTRIBUTION_MATCHED";
    case ter::Q3SnapshotFamily::FixedQMicrobenchmark:
      return "FIXED_Q_MICROBENCH";
  }
  return "UNKNOWN";
}

[[nodiscard]] std::uint64_t median(std::vector<std::uint64_t> values) {
  std::sort(values.begin(), values.end());
  return values[values.size() / 2U];
}

[[nodiscard]] std::uint64_t minimum(const std::vector<std::uint64_t>& values) {
  return *std::min_element(values.begin(), values.end());
}

[[nodiscard]] std::uint64_t maximum(const std::vector<std::uint64_t>& values) {
  return *std::max_element(values.begin(), values.end());
}

[[nodiscard]] ter::Instance balanced_full_instance() {
  const auto built = ter::build_instance(36U, 10U, 0x5143415054555245ULL);
  if (built.status != ter::Status::Completed) {
    std::cerr << "unable to build Full TER benchmark instance\n";
    std::exit(2);
  }
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

[[nodiscard]] ter::FrozenL2MergeSnapshot native_snapshot() {
  ter::FullTerConfig config;
  config.family = ter::ShapeFamily::FiniteSanity;
  config.execution_mode = ter::TerExecutionMode::FullTer;
  config.target_mode = ter::TerTargetMode::GuidedPlanted;
  config.seed = kSeed;
  config.memory_limit_bytes = kMemoryLimit;
  const auto capture = ter::capture_full_ter_l2_merge_snapshot(
      balanced_full_instance(), config, 0U);
  if (capture.status != ter::Status::Completed) {
    std::cerr << "native capture failed: " << capture.message << '\n';
    std::exit(2);
  }
  const auto frozen = ter::freeze_l2_merge_snapshot(
      capture.snapshot, ter::Q3SnapshotFamily::AuthenticFullTer, kSeed);
  if (frozen.status != ter::Q3ReportStatus::Completed) {
    std::cerr << "native freeze failed: " << frozen.message << '\n';
    std::exit(2);
  }
  return frozen.value;
}

[[nodiscard]] ter::FrozenL2MergeSnapshot distribution_matched_snapshot() {
  ter::SnapshotConfig config;
  config.family = ter::ShapeFamily::FiniteSanity;
  config.n = 36U;
  config.b2 = 4U;
  config.b1 = 10U;
  config.seed = kSeed + 1U;
  config.requested_list_size = 64U;
  config.mode = ter::GeneratorMode::PlantedMitm;
  config.plant_valid_triple = true;
  config.half_pool_size = ter::limits::benchmark::kQ3ReportBenchmarkInitialHalfPool;
  config.max_half_pool_size = ter::limits::benchmark::kQ3ReportBenchmarkMaximumHalfPool;
  config.attempt_limit = ter::limits::benchmark::kQ3ReportBenchmarkAttemptLimit;
  config.memory_limit_bytes = kMemoryLimit;
  const auto generated = ter::build_snapshot(config);
  if (generated.status != ter::Status::Completed) {
    std::cerr << "distribution-matched snapshot failed: " << generated.message << '\n';
    std::exit(2);
  }
  const auto frozen = ter::freeze_l2_merge_snapshot(
      generated.snapshot, ter::Q3SnapshotFamily::DistributionMatched, config.seed);
  if (frozen.status != ter::Q3ReportStatus::Completed) {
    std::cerr << "distribution-matched freeze failed: " << frozen.message << '\n';
    std::exit(2);
  }
  return frozen.value;
}

// This preserves genuine L2 vectors but projects their arithmetic to a smaller
// quotient ring. It is strictly a transform-size microbenchmark, never Full TER evidence.
[[nodiscard]] ter::FrozenL2MergeSnapshot fixed_q_projection(
    const ter::FrozenL2MergeSnapshot& native) {
  auto input = native.input;
  constexpr std::uint8_t kR1 = 9U;  // r2=3, Q=64.
  const auto arithmetic = ter::build_arithmetic(
      input.instance.n,
      input.spec.arithmetic.b2,
      kR1,
      input.spec.arithmetic.target_a2,
      input.spec.arithmetic.target_b2,
      input.spec.arithmetic.target_1);
  if (arithmetic.status != ter::Status::Completed) {
    std::cerr << "fixed-Q arithmetic construction failed\n";
    std::exit(2);
  }
  input.instance.b1 = kR1;
  input.instance.residues_m1.resize(input.instance.n);
  for (std::size_t index = 0; index < input.instance.residues_m1.size(); ++index) {
    input.instance.residues_m1[index] = input.instance.values_le[index] & arithmetic.value.mask_m1;
  }
  input.spec.arithmetic = arithmetic.value;
  for (std::size_t list_index = 0; list_index < input.lists.size(); ++list_index) {
    auto& list = input.lists[list_index];
    list.target_m2 = list_index == 0U ? arithmetic.value.target_a2
                                      : (list_index == 1U ? arithmetic.value.target_b2
                                                           : arithmetic.value.target_c2);
    for (std::size_t entry_index = 0; entry_index < list.entries.size(); ++entry_index) {
      const auto residue = ter::inner_product_mod(input.instance.residues_m1,
                                                   list.entries[entry_index].digits,
                                                   arithmetic.value.mask_m1);
      list.residues_m1[entry_index] = residue;
      list.entries[entry_index].q = ter::quotient_residue(
          residue, list.target_m2, arithmetic.value);
    }
  }
  const auto frozen = ter::freeze_l2_merge_snapshot(
      std::move(input), ter::Q3SnapshotFamily::FixedQMicrobenchmark, kSeed + 2U);
  if (frozen.status != ter::Q3ReportStatus::Completed) {
    std::cerr << "fixed-Q freeze failed: " << frozen.message << '\n';
    std::exit(2);
  }
  return frozen.value;
}

void emit_family(const ter::FrozenL2MergeSnapshot& snapshot,
                 const std::string_view descriptor,
                 const std::uint32_t repetitions) {
  ter::Q3ReportConfig config;
  config.memory_limit_bytes = kMemoryLimit;
  const auto warmup = ter::run_q3report_count_only_experiment(snapshot, config);
  if (warmup.status != ter::Q3ReportStatus::Completed) {
    std::cerr << "warmup failed for " << descriptor << ": " << warmup.message << '\n';
    std::exit(2);
  }
  std::vector<std::uint64_t> direct;
  std::vector<std::uint64_t> audit;
  std::vector<std::uint64_t> structural;
  std::vector<std::uint64_t> dedup;
  std::vector<std::uint64_t> scalar;
  std::vector<std::uint64_t> ntt_frequency;
  std::vector<std::uint64_t> ntt_transform;
  std::vector<std::uint64_t> ntt_reconstruct;
  std::vector<std::uint64_t> selected_lookup;
  std::vector<std::uint64_t> full_histogram;
  direct.reserve(repetitions);
  audit.reserve(repetitions);
  structural.reserve(repetitions);
  dedup.reserve(repetitions);
  scalar.reserve(repetitions);
  ntt_frequency.reserve(repetitions);
  ntt_transform.reserve(repetitions);
  ntt_reconstruct.reserve(repetitions);
  selected_lookup.reserve(repetitions);
  full_histogram.reserve(repetitions);
  ter::Q3ReportResult last;
  for (std::uint32_t repetition = 0; repetition < repetitions; ++repetition) {
    last = ter::run_q3report_count_only_experiment(snapshot, config);
    if (last.status != ter::Q3ReportStatus::Completed) {
      std::cerr << "run failed for " << descriptor << ": " << last.message << '\n';
      std::exit(2);
    }
    direct.push_back(last.telemetry.timing.direct_arithmetic_ns);
    audit.push_back(last.telemetry.timing.snapshot_audit_ns);
    structural.push_back(last.telemetry.timing.direct_structural_ns);
    dedup.push_back(last.telemetry.timing.direct_dedup_ns);
    scalar.push_back(last.telemetry.timing.scalar_convolution_ns);
    ntt_frequency.push_back(last.telemetry.timing.ntt_frequency_build_ns);
    ntt_transform.push_back(last.telemetry.timing.ntt_transform_ns);
    ntt_reconstruct.push_back(last.telemetry.timing.ntt_reconstruction_ns);
    selected_lookup.push_back(last.telemetry.timing.selected_target_lookup_ns);
    full_histogram.push_back(last.telemetry.timing.full_histogram_ns);
  }
  const auto ntt_core = median(ntt_frequency) + median(ntt_transform) + median(ntt_reconstruct);
  std::cout << std::fixed << std::setprecision(6)
            << family_name(snapshot.family) << ',' << descriptor << ',' << snapshot.seed << ','
            << last.telemetry.a_size << ',' << last.telemetry.b_size << ','
            << last.telemetry.c_size << ',' << static_cast<unsigned>(last.telemetry.r2) << ','
            << static_cast<unsigned>(last.telemetry.r1) << ',' << last.telemetry.q << ','
            << last.telemetry.snapshot_fingerprint << ',' << repetitions << ','
            << kMemoryLimit << ",998244353-single,1," << last.telemetry.lambda << ','
            << last.telemetry.predicted_support_fraction << ','
            << median(audit) << ',' << median(direct) << ',' << median(structural) << ','
            << median(dedup) << ','
            << median(scalar) << ',' << median(ntt_frequency) << ','
            << median(ntt_transform) << ',' << median(ntt_reconstruct) << ','
            << ntt_core << ',' << minimum(ntt_transform) << ',' << maximum(ntt_transform) << ','
            << median(selected_lookup) << ',' << median(full_histogram) << ','
            << last.telemetry.occupied_pair_residues << ','
            << last.telemetry.unique_selected_targets << ','
            << last.telemetry.unique_selected.zero << ','
            << last.telemetry.c_weighted_selected.zero << ','
            << last.telemetry.raw_arithmetic_triples << ','
            << last.telemetry.structurally_valid_triples << ','
            << last.telemetry.structural_rejects << ','
            << last.telemetry.distribution_rejects << ','
            << last.telemetry.parent_arithmetic_rejects << ','
            << last.telemetry.unique_parents << ','
            << (last.telemetry.exactness_passed ? 1 : 0) << ','
            << (last.telemetry.cheap_kill_passed ? 1 : 0) << ','
            << (last.telemetry.independent_oracle_ran ? 1 : 0) << '\n';
}

}  // namespace

int main(int argc, char** argv) {
  std::uint32_t repetitions = 7U;
  if (argc == 2) {
    repetitions = static_cast<std::uint32_t>(std::strtoul(argv[1], nullptr, 10));
  }
  if (repetitions == 0U) {
    std::cerr << "usage: ter_q3report_bench [positive-repetitions]\n";
    return 2;
  }
  std::cout << "family,descriptor,seed,A,B,C,r2,r1,Q,snapshot_fingerprint,repetitions,"
               "memory_limit_bytes,ntt_strategy,threads,lambda,predicted_support_fraction,"
               "snapshot_audit_median_ns,direct_arithmetic_median_ns,"
               "direct_structural_median_ns,direct_dedup_median_ns,scalar_cyclic_median_ns,"
               "ntt_frequency_median_ns,ntt_transform_median_ns,"
               "ntt_reconstruct_median_ns,ntt_core_median_ns,ntt_transform_min_ns,"
               "ntt_transform_max_ns,selected_lookup_median_ns,full_histogram_median_ns,"
               "occupied_pair_residues,unique_selected_targets,unique_zero_targets,"
               "c_weighted_zero_targets,raw_arithmetic_triples,structurally_valid_triples,"
               "structural_rejects,distribution_rejects,parent_arithmetic_rejects,unique_parents,"
               "exactness_passed,cheap_kill_passed,oracle_ran\n";
  const auto native = native_snapshot();
  emit_family(native, "complete-n36-guided-group0", repetitions);
  emit_family(distribution_matched_snapshot(), "t0-generated-not-fullter", repetitions);
  emit_family(fixed_q_projection(native), "native-vectors-projected-Q64-not-fullter", repetitions);
  return 0;
}
