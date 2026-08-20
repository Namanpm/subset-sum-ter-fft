// TER-Q3REPORT paper-scaling dense-NTT crossover benchmark (V2).
//
// stdout carries the machine-readable CSV described in Section 25.
// stderr carries human-readable progress and a compact decision summary.
//
// This is a screening experiment.  It never claims an exponent improvement and
// never extrapolates beyond the points it actually executed.

#include "ter/arithmetic.hpp"
#include "ter/full_ter.hpp"
#include "ter/instance.hpp"
#include "ter/limitations.hpp"
#include "ter/q3report_paper_crossover.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::uint64_t kAnchorSeed = 0x51335245504f5254ULL;

using Schedule = ter::limits::benchmark::PaperCrossoverSchedule;

// Section 23.  Declared before any timing result was seen.
[[nodiscard]] Schedule schedule_for(const std::uint64_t list_size,
                                    const bool extended_16384) noexcept {
  return ter::limits::benchmark::paper_crossover_schedule_for(list_size, extended_16384);
}

void emit_header() {
  std::cout
      << "snapshot_family,seed,fingerprint,list_size_requested,L_A,L_B,L_C,effective_n,shape_n,"
         "l2_negative,l2_zero,l2_positive,l3_half_negative,l3_half_zero,l3_half_positive,"
         "r2,r1,q_bits,Q,lambda,ntt_prime,ntt_primitive_root,ntt_max_power,support_fraction,"
         "selected_weighted_zero_rate,selected_weighted_one_rate,selected_weighted_le2_rate,"
         "selected_weighted_le4_rate,selected_weighted_p50,selected_weighted_p90,"
         "selected_weighted_p99,selected_weighted_max,raw_arithmetic_triples,"
         "direct_full_total_median_ns,direct_production_total_median_ns,ntt_plan_build_median_ns,"
         "ntt_warm_total_median_ns,ntt_cold_total_median_ns,ntt_three_merge_amortized_ns,"
         "ntt_transform_median_ns,ntt_over_direct_full,ntt_over_direct_production,"
         "direct_full_peak_bytes,direct_production_peak_bytes,ntt_plan_bytes,ntt_run_bytes,"
         "ntt_peak_bytes,seed_count,timing_repetitions,exactness_passed,status,"
         "direct_full_total_min_ns,direct_full_total_max_ns,direct_production_total_min_ns,"
         "direct_production_total_max_ns,ntt_warm_total_min_ns,ntt_warm_total_max_ns,"
         "poisson_support_estimate,occupied_pair_residues,half_domain,half_pool_a,half_pool_b,"
         "half_pool_c,compatible_pairs_a,compatible_pairs_b,compatible_pairs_c,"
         // Appended for DIRECT_PRODUCTION_TILED.  Every column above this line is
         // historical and is never renamed, reordered or overwritten.
         "direct_tiled_total_median_ns,direct_tiled_setup_median_ns,"
         "direct_tiled_scan_median_ns,direct_tiled_peak_bytes,direct_tiled_tile_bits,"
         "direct_tiled_nonempty_windows,direct_tiled_executed_pairs,ntt_over_direct_tiled,"
         "direct_tiled_total_min_ns,direct_tiled_total_max_ns,direct_tiled_window_count,"
         "direct_tiled_auxiliary_bytes\n";
}

[[nodiscard]] double rate(const std::uint64_t part, const std::uint64_t whole) noexcept {
  return whole == 0U ? 0.0 : static_cast<double>(part) / static_cast<double>(whole);
}

[[nodiscard]] double ratio(const std::uint64_t numerator, const std::uint64_t denominator) noexcept {
  return denominator == 0U ? 0.0
                           : static_cast<double>(numerator) / static_cast<double>(denominator);
}

void emit_row(const ter::PaperCrossoverSnapshot& snapshot,
              const ter::PaperCrossoverResult& result,
              const Schedule& schedule) {
  const auto& telemetry = result.telemetry;
  const auto& histogram = telemetry.selected_weighted;
  const std::uint64_t amortized = telemetry.ntt_three_merge_amortized_ns;
  std::cout << std::fixed << std::setprecision(9) << ter::paper_crossover_family_name(snapshot.family)
            << ',' << snapshot.seed << ',' << snapshot.fingerprint << ','
            << snapshot.requested_list_size << ',' << telemetry.a_size << ',' << telemetry.b_size
            << ',' << telemetry.c_size << ',' << telemetry.effective_n << ',' << telemetry.shape_n
            << ',' << telemetry.level2.negative << ',' << telemetry.level2.zero << ','
            << telemetry.level2.positive << ',' << telemetry.level3_active_half.negative << ','
            << telemetry.level3_active_half.zero << ',' << telemetry.level3_active_half.positive
            << ',' << telemetry.r2 << ',' << telemetry.r1 << ',' << telemetry.q_bits << ','
            << telemetry.q << ',' << static_cast<double>(telemetry.lambda) << ','
            << telemetry.prime.prime << ',' << telemetry.prime.primitive_root << ','
            << telemetry.prime.max_power << ',' << static_cast<double>(telemetry.support_fraction)
            << ',' << rate(histogram.zero, histogram.population) << ','
            << rate(histogram.one, histogram.population) << ','
            << rate(histogram.at_most_two, histogram.population) << ','
            << rate(histogram.at_most_four, histogram.population) << ',' << histogram.p50 << ','
            << histogram.p90 << ',' << histogram.p99 << ',' << histogram.maximum << ','
            << telemetry.raw_arithmetic_triples << ',' << telemetry.direct_full.total.median_ns
            << ',' << telemetry.direct_production.total.median_ns << ','
            << telemetry.components.ntt_plan_build.median_ns << ','
            << telemetry.ntt_warm.total.median_ns << ',' << telemetry.ntt_cold.total.median_ns
            << ',' << amortized << ',' << telemetry.components.ntt_transform.median_ns << ','
            << ratio(amortized, telemetry.direct_full.total.median_ns) << ','
            << ratio(amortized, telemetry.direct_production.total.median_ns) << ','
            << telemetry.direct_full.peak_bytes << ',' << telemetry.direct_production.peak_bytes
            << ',' << telemetry.ntt_plan_bytes << ',' << telemetry.ntt_run_bytes << ','
            << telemetry.ntt_peak_bytes << ',' << schedule.seeds << ',' << schedule.repetitions
            << ',' << (telemetry.exactness_passed ? 1 : 0) << ','
            << ter::paper_crossover_status_name(result.status) << ','
            << telemetry.direct_full.total.minimum_ns << ','
            << telemetry.direct_full.total.maximum_ns << ','
            << telemetry.direct_production.total.minimum_ns << ','
            << telemetry.direct_production.total.maximum_ns << ','
            << telemetry.ntt_warm.total.minimum_ns << ',' << telemetry.ntt_warm.total.maximum_ns
            << ',' << static_cast<double>(telemetry.poisson_support_estimate) << ','
            << telemetry.occupied_pair_residues << ',' << snapshot.generation.half_domain << ','
            << snapshot.generation.half_pool_size[0] << ',' << snapshot.generation.half_pool_size[1]
            << ',' << snapshot.generation.half_pool_size[2] << ','
            << snapshot.generation.compatible_pairs[0] << ','
            << snapshot.generation.compatible_pairs[1] << ','
            << snapshot.generation.compatible_pairs[2] << ','
            // Appended DIRECT_PRODUCTION_TILED fields.
            << telemetry.direct_tiled.total.median_ns << ','
            << telemetry.components.direct_tiled_setup.median_ns << ','
            << telemetry.components.direct_tiled_scan.median_ns << ','
            << telemetry.direct_tiled.peak_bytes << ','
            << telemetry.direct_tiled_tiling.tile_bits << ','
            << telemetry.direct_tiled_tiling.nonempty_windows << ','
            << telemetry.direct_tiled_tiling.executed_pairs << ','
            << ratio(amortized, telemetry.direct_tiled.total.median_ns) << ','
            << telemetry.direct_tiled.total.minimum_ns << ','
            << telemetry.direct_tiled.total.maximum_ns << ','
            << telemetry.direct_tiled_tiling.window_count << ','
            << telemetry.direct_tiled_tiling.auxiliary_peak_bytes << '\n';
  std::cout.flush();
}

[[nodiscard]] ter::Instance balanced_full_instance() {
  const auto built = ter::build_instance(36U, 10U, 0x5143415054555245ULL);
  if (built.status != ter::Status::Completed) {
    std::cerr << "unable to build the Full TER anchor instance\n";
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

// Section 21: correctness continuity against genuine Full TER captures.
void run_authentic_anchors() {
  ter::FullTerConfig config;
  config.family = ter::ShapeFamily::FiniteSanity;
  config.execution_mode = ter::TerExecutionMode::FullTer;
  config.target_mode = ter::TerTargetMode::GuidedPlanted;
  config.seed = kAnchorSeed;
  config.memory_limit_bytes = ter::limits::q3report::kDefaultMemoryBytes;
  for (std::uint32_t group = 0; group < 3U; ++group) {
    const auto capture =
        ter::capture_full_ter_l2_merge_snapshot(balanced_full_instance(), config, group);
    if (capture.status != ter::Status::Completed) {
      std::cerr << "anchor group " << group << " not capturable: " << capture.message << '\n';
      continue;
    }
    const auto adopted = ter::adopt_authentic_full_ter_snapshot(capture.snapshot,
                                                                capture.parameters, kAnchorSeed);
    if (adopted.status != ter::PaperCrossoverStatus::Completed) {
      std::cerr << "anchor group " << group << " not adoptable: " << adopted.message << '\n';
      continue;
    }
    ter::PaperCrossoverConfig run_config;
    run_config.memory_limit_bytes = ter::limits::paper_crossover::kDefaultMemoryBytes;
    run_config.warmup_repetitions = ter::limits::paper_crossover::kDefaultWarmupRepetitions;
    run_config.timed_repetitions = ter::limits::paper_crossover::kDefaultTimedRepetitions;
    const auto result = ter::run_paper_crossover_experiment(adopted.value, run_config);
    const Schedule schedule{1U, run_config.timed_repetitions, true};
    emit_row(adopted.value, result, schedule);
    std::cerr << "anchor group " << group << ": " << ter::paper_crossover_status_name(result.status)
              << " exact=" << (result.telemetry.exactness_passed ? 1 : 0)
              << " conservation=" << (result.telemetry.pair_conservation_passed ? 1 : 0)
              << (result.telemetry.triple_conservation_passed ? 1 : 0)
              << " cold==warm=" << (result.telemetry.cold_warm_identical ? 1 : 0) << ' '
              << result.message << '\n';
  }
}

struct PointSummary {
  std::uint64_t list_size = 0;
  std::uint32_t completed_seeds = 0;
  std::uint32_t amortized_wins = 0;
  double best_ratio = 0.0;
  bool executable = false;
};

}  // namespace

int main(int argc, char** argv) {
  bool extended_16384 = false;
  bool emit_csv_header = true;
  bool anchors_only = false;
  std::uint64_t only_list_size = 0;
  // --seed-index selects one declared seed of the declared schedule so that the
  // same matrix can be executed in separate processes.  It never changes which
  // seeds, schedules or rows the matrix contains.
  std::uint32_t only_seed_index = 0xFFFFFFFFU;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument{argv[index]};
    if (argument == "--extend-16384") {
      extended_16384 = true;
    } else if (argument == "--no-header") {
      emit_csv_header = false;
    } else if (argument == "--anchors-only") {
      anchors_only = true;
    } else if (argument.rfind("--only=", 0U) == 0U) {
      only_list_size = std::strtoull(argument.data() + 7, nullptr, 10);
    } else if (argument.rfind("--seed-index=", 0U) == 0U) {
      only_seed_index = static_cast<std::uint32_t>(std::strtoul(argument.data() + 13, nullptr, 10));
    } else {
      std::cerr << "usage: ter_q3report_paper_crossover_bench [--extend-16384] [--only=L]"
                   " [--seed-index=K] [--anchors-only] [--no-header]\n";
      return 2;
    }
  }

  if (emit_csv_header) {
    emit_header();
  }
  if (only_list_size == 0U || anchors_only) {
    run_authentic_anchors();
  }
  if (anchors_only) {
    return 0;
  }

  std::vector<PointSummary> summaries;
  for (const std::uint64_t list_size : ter::declared_paper_scaling_ladder()) {
    if (only_list_size != 0U && list_size != only_list_size) {
      continue;
    }
    const auto schedule = schedule_for(list_size, extended_16384);
    PointSummary summary;
    summary.list_size = list_size;
    summary.executable = schedule.executable;
    const auto seeds = ter::declared_paper_scaling_seeds();
    for (std::uint32_t index = 0; index < schedule.seeds && index < seeds.size(); ++index) {
      if (only_seed_index != 0xFFFFFFFFU && index != only_seed_index) {
        continue;
      }
      ter::PaperCrossoverSnapshotConfig snapshot_config;
      snapshot_config.requested_list_size = list_size;
      snapshot_config.seed = seeds[index];
      snapshot_config.memory_limit_bytes = ter::limits::paper_crossover::kDefaultMemoryBytes;
      const auto built = ter::build_paper_scaling_snapshot(snapshot_config);
      if (built.status != ter::PaperCrossoverStatus::Completed) {
        std::cerr << "L=" << list_size << " seed=" << seeds[index]
                  << " snapshot failed: " << ter::paper_crossover_status_name(built.status) << ' '
                  << built.message << '\n';
        continue;
      }
      const auto audited = ter::audit_paper_crossover_snapshot(built.value);
      if (audited.status != ter::PaperCrossoverStatus::Completed) {
        std::cerr << "L=" << list_size << " seed=" << seeds[index]
                  << " audit failed: " << audited.message << '\n';
        continue;
      }
      ter::PaperCrossoverConfig run_config;
      run_config.memory_limit_bytes = ter::limits::paper_crossover::kDefaultMemoryBytes;
      run_config.warmup_repetitions = 1U;
      run_config.timed_repetitions = schedule.repetitions;
      // A non-executable declared point returns NotApplicable from the prime
      // selection before any buffer is allocated; its row is still emitted.
      const auto result = ter::run_paper_crossover_experiment(built.value, run_config);
      emit_row(built.value, result, schedule);
      if (result.status == ter::PaperCrossoverStatus::Completed) {
        ++summary.completed_seeds;
        const std::uint64_t amortized = result.telemetry.ntt_three_merge_amortized_ns;
        const std::uint64_t production = result.telemetry.direct_production.total.median_ns;
        if (production != 0U && amortized < production) {
          ++summary.amortized_wins;
        }
        const double observed = production == 0U
                                    ? 0.0
                                    : static_cast<double>(production) /
                                          static_cast<double>(amortized == 0U ? 1U : amortized);
        summary.best_ratio = std::max(summary.best_ratio, observed);
      }
      std::cerr << "L=" << list_size << " seed=" << seeds[index] << " -> "
                << ter::paper_crossover_status_name(result.status)
                << " direct_prod=" << result.telemetry.direct_production.total.median_ns << "ns"
                << " direct_tiled=" << result.telemetry.direct_tiled.total.median_ns << "ns"
                << " direct_full=" << result.telemetry.direct_full.total.median_ns << "ns"
                << " ntt_amort=" << result.telemetry.ntt_three_merge_amortized_ns << "ns"
                << " exact=" << (result.telemetry.exactness_passed ? 1 : 0) << ' '
                << result.message << '\n';
    }
    summaries.push_back(summary);
  }

  std::cerr << "\n-- amortized NTT versus DIRECT_PRODUCTION (speedup > 1 favours NTT) --\n";
  for (const auto& summary : summaries) {
    std::cerr << "L=" << summary.list_size << " completed_seeds=" << summary.completed_seeds
              << " amortized_wins=" << summary.amortized_wins << " best_speedup="
              << std::fixed << std::setprecision(4) << summary.best_ratio
              << (summary.executable ? "" : " (declared point, NTT NotApplicable)") << '\n';
  }
  return 0;
}
