// TER-faithful NTT crossover benchmark.
//
// Answers exactly one question: on larger, genuinely TER-shaped L2 lists
// than the frozen Q3REPORT experiment (ter_q3report_bench) tested, does an
// exact count-only cyclic NTT ever beat the equivalent direct O(|A|*|B|)
// count-only path end to end? This binary does not replace, rewrite, or
// relabel the frozen experiment's results.
//
// Every snapshot here is TER_SCALING_EMULATION: real ternary vectors with
// the exact required L2 digit distribution, real instance coefficients, and
// quotient residues derived by the shared quotient-arithmetic module. It is
// never AUTHENTIC_FULLTER.

#include "ter/q3report_crossover.hpp"
#include "ter/limitations.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t kMemoryLimit = ter::limits::crossover::kDefaultMemoryBytes;

[[nodiscard]] std::uint64_t median(std::vector<std::uint64_t> values) {
  std::sort(values.begin(), values.end());
  return values[values.size() / 2U];
}

struct QChoice {
  bool feasible = false;
  std::uint64_t q = 0;
  std::uint8_t r1 = 0;
  std::string skip_reason;
};

// Rounds L*L/lambda to the nearest power of two and checks it against every
// hard ceiling this benchmark's exact single-prime NTT and the generator's
// shape (r1 < n) actually enforce. Declared, not tuned after seeing results.
[[nodiscard]] QChoice choose_q(const std::uint64_t list_size, const long double lambda) {
  QChoice choice;
  const long double pair_universe =
      static_cast<long double>(list_size) * static_cast<long double>(list_size);
  const long double ideal_q = pair_universe / lambda;
  if (ideal_q < 2.0L) {
    choice.skip_reason = "target lambda requires Q < 2, not representable";
    return choice;
  }
  auto exponent = static_cast<int>(std::llround(std::log2(static_cast<double>(ideal_q))));
  exponent = std::clamp(
      exponent, 1,
      static_cast<int>(ter::limits::benchmark::kCrossoverMaximumQuotientExponent));
  const std::uint64_t q = std::uint64_t{1} << exponent;

  if (!ter::limits::ntt::q3report_length_supported(q)) {
    choice.skip_reason = "Q=2^" + std::to_string(exponent) +
                          " exceeds the supported single-prime transform length 2^23";
    return choice;
  }
  const auto r1 = static_cast<std::uint32_t>(ter::limits::benchmark::kCrossoverR2) +
                  static_cast<std::uint32_t>(exponent);
  if (r1 >= ter::limits::benchmark::kCrossoverShapeN) {
    choice.skip_reason =
        "r1=r2+log2(Q)=" + std::to_string(r1) + " would not stay below n=" +
        std::to_string(ter::limits::benchmark::kCrossoverShapeN);
    return choice;
  }
  choice.feasible = true;
  choice.q = q;
  choice.r1 = static_cast<std::uint8_t>(r1);
  return choice;
}

struct PointOutcome {
  bool ran = false;
  std::string classification;  // "Direct wins" / "Approximately tied" / "NTT wins cold" /
                                // "NTT wins only amortized"
};

void print_skip(const std::uint32_t list_size, const long double lambda, const std::string& why) {
  std::cerr << "# SKIP L=" << list_size << " lambda~" << lambda << " reason: " << why << '\n';
}

[[nodiscard]] std::optional<PointOutcome> run_point(const std::uint32_t list_size,
                                                     const long double lambda_target,
                                                     const QChoice& choice,
                                                     std::ostream& csv) {
  bool any_seed_ran = false;
  std::vector<long double> cold_ratio;
  std::vector<long double> amortized_ratio;

  ter::NttPlan shared_plan;
  {
    std::string message;
    const auto status = ter::build_ntt_plan(choice.q, &shared_plan, &message);
    if (status != ter::CrossoverStatus::Completed) {
      print_skip(list_size, lambda_target, "plan build failed: " + message);
      return std::nullopt;
    }
  }

  for (const auto seed : ter::limits::benchmark::kCrossoverSeeds) {
    ter::CrossoverSnapshotConfig config;
    config.n = ter::limits::benchmark::kCrossoverShapeN;
    config.list_size = list_size;
    config.r2 = ter::limits::benchmark::kCrossoverR2;
    config.r1 = choice.r1;
    config.seed = seed;
    config.half_pool_size = ter::limits::benchmark::kCrossoverBenchmarkInitialHalfPool;
    config.max_half_pool_size = ter::limits::benchmark::kCrossoverBenchmarkMaximumHalfPool;
    config.attempt_limit = ter::limits::benchmark::kCrossoverBenchmarkAttemptLimit;
    config.memory_limit_bytes = kMemoryLimit;

    const auto snapshot = ter::build_scaling_snapshot(config);
    if (snapshot.status != ter::CrossoverStatus::Completed) {
      std::cerr << "# SKIP L=" << list_size << " Q=" << choice.q << " seed=" << seed
                << " generation status=" << static_cast<int>(snapshot.status)
                << " reason: " << snapshot.message << '\n';
      continue;
    }

    // Warm-up (discarded) so the first timed repetition is not paying for
    // page faults / allocator warm-up that later repetitions would not pay.
    (void)ter::run_crossover_point(snapshot.value, &shared_plan, kMemoryLimit);

    std::vector<std::uint64_t> direct_setup, direct_count, direct_lookup;
    std::vector<std::uint64_t> ntt_frequency, ntt_transform, ntt_reconstruction, ntt_lookup;
    std::vector<std::uint64_t> plan_build_ns;
    direct_setup.reserve(ter::limits::benchmark::kCrossoverTimedRepetitions);
    direct_count.reserve(ter::limits::benchmark::kCrossoverTimedRepetitions);
    direct_lookup.reserve(ter::limits::benchmark::kCrossoverTimedRepetitions);
    ntt_frequency.reserve(ter::limits::benchmark::kCrossoverTimedRepetitions);
    ntt_transform.reserve(ter::limits::benchmark::kCrossoverTimedRepetitions);
    ntt_reconstruction.reserve(ter::limits::benchmark::kCrossoverTimedRepetitions);
    ntt_lookup.reserve(ter::limits::benchmark::kCrossoverTimedRepetitions);
    plan_build_ns.reserve(ter::limits::benchmark::kCrossoverTimedRepetitions);

    ter::CrossoverRunResult last;
    bool failed = false;
    for (std::uint32_t repetition = 0;
         repetition < ter::limits::benchmark::kCrossoverTimedRepetitions;
         ++repetition) {
      // Amortized series: reuse the Q-specific plan built once above.
      const auto amortized = ter::run_crossover_point(snapshot.value, &shared_plan, kMemoryLimit);
      if (amortized.status != ter::CrossoverStatus::Completed) {
        std::cerr << "# RUN-FAIL L=" << list_size << " Q=" << choice.q << " seed=" << seed
                  << " status=" << static_cast<int>(amortized.status)
                  << " reason: " << amortized.message << '\n';
        failed = true;
        break;
      }
      direct_setup.push_back(amortized.report.timing.direct_setup_ns);
      direct_count.push_back(amortized.report.timing.direct_count_ns);
      direct_lookup.push_back(amortized.report.timing.direct_lookup_ns);
      ntt_frequency.push_back(amortized.report.timing.ntt_frequency_ns);
      ntt_transform.push_back(amortized.report.timing.ntt_transform_ns);
      ntt_reconstruction.push_back(amortized.report.timing.ntt_reconstruction_ns);
      ntt_lookup.push_back(amortized.report.timing.ntt_lookup_ns);
      last = amortized;

      // Independent cold plan-build sample (Q-specific, data-independent).
      ter::NttPlan cold_plan;
      std::string plan_message;
      const auto plan_start = std::chrono::steady_clock::now();
      const auto plan_status = ter::build_ntt_plan(choice.q, &cold_plan, &plan_message);
      const auto plan_ns = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - plan_start)
              .count());
      if (plan_status != ter::CrossoverStatus::Completed) {
        failed = true;
        break;
      }
      plan_build_ns.push_back(plan_ns);
    }
    if (failed) {
      continue;
    }
    any_seed_ran = true;

    const auto direct_total =
        median(direct_setup) + median(direct_count) + median(direct_lookup);
    const auto ntt_amortized_total =
        median(ntt_frequency) + median(ntt_transform) + median(ntt_reconstruction) +
        median(ntt_lookup);
    const auto ntt_cold_total = median(plan_build_ns) + ntt_amortized_total;

    const auto ratio_cold =
        static_cast<long double>(ntt_cold_total) / static_cast<long double>(direct_total);
    const auto ratio_amortized =
        static_cast<long double>(ntt_amortized_total) / static_cast<long double>(direct_total);
    cold_ratio.push_back(ratio_cold);
    amortized_ratio.push_back(ratio_amortized);

    const auto& report = last.report;
    const long double support_fraction =
        report.all_residues.population == 0U
            ? 0.0L
            : static_cast<long double>(report.occupied_pair_residues) /
                  static_cast<long double>(report.all_residues.population);
    const long double unique_zero_rate =
        report.unique_selected.population == 0U
            ? 0.0L
            : static_cast<long double>(report.unique_selected.zero) /
                  static_cast<long double>(report.unique_selected.population);
    const long double weighted_zero_rate =
        report.c_weighted_selected.population == 0U
            ? 0.0L
            : static_cast<long double>(report.c_weighted_selected.zero) /
                  static_cast<long double>(report.c_weighted_selected.population);

    csv << std::fixed << std::setprecision(6) << "TER_SCALING_EMULATION," << seed << ','
        << snapshot.value.fingerprint << ',' << ter::limits::benchmark::kCrossoverShapeN << ','
        << report.a_size << ','
        << report.b_size << ',' << report.c_size << ',' << static_cast<unsigned>(report.r2)
        << ',' << static_cast<unsigned>(report.r1) << ',' << report.q << ',' << report.lambda
        << ',' << support_fraction << ',' << report.predicted_support_fraction << ','
        << unique_zero_rate << ',' << weighted_zero_rate << ',' << report.c_weighted_selected.p50
        << ',' << report.c_weighted_selected.p90 << ',' << report.c_weighted_selected.p99 << ','
        << report.c_weighted_selected.maximum << ',' << median(direct_setup) << ','
        << median(direct_count) << ',' << direct_total << ',' << median(plan_build_ns) << ','
        << median(ntt_frequency) << ',' << median(ntt_transform) << ','
        << median(ntt_reconstruction) << ',' << median(ntt_lookup) << ',' << ntt_cold_total << ','
        << ntt_amortized_total << ',' << ratio_cold << ',' << ratio_amortized << ','
        << report.memory.direct_bytes << ',' << report.memory.ntt_plan_bytes << ','
        << report.memory.ntt_per_run_bytes << ','
        << ter::limits::benchmark::kCrossoverSeeds.size() << ','
        << ter::limits::benchmark::kCrossoverTimedRepetitions
        << ',' << (report.exactness_passed ? 1 : 0) << ",Completed\n";
  }

  if (!any_seed_ran) {
    print_skip(list_size, lambda_target, "no seed produced a usable snapshot at this Q");
    return std::nullopt;
  }

  PointOutcome outcome;
  outcome.ran = true;
  const auto majority_below_one = [](const std::vector<long double>& ratios) {
    std::size_t below = 0;
    for (const auto r : ratios) {
      below += r < 1.0L;
    }
    return static_cast<long double>(below) / static_cast<long double>(ratios.size());
  };
  const auto cold_win_fraction = majority_below_one(cold_ratio);
  const auto amortized_win_fraction = majority_below_one(amortized_ratio);
  long double mean_cold_ratio = 0.0L;
  for (const auto r : cold_ratio) mean_cold_ratio += r;
  mean_cold_ratio /= static_cast<long double>(cold_ratio.size());

  if (cold_win_fraction > 0.5L) {
    outcome.classification = "NTT wins cold";
  } else if (amortized_win_fraction > 0.5L) {
    outcome.classification = "NTT wins only amortized";
  } else if (mean_cold_ratio > 0.98L && mean_cold_ratio < 1.02L) {
    outcome.classification = "Approximately tied";
  } else {
    outcome.classification = "Direct wins";
  }
  return outcome;
}

}  // namespace

int main() {
  std::cout << "snapshot_family,seed,snapshot_fingerprint,n,L_A,L_B,L_C,r2,r1,Q,lambda,"
               "support_fraction,predicted_support_fraction,selected_unique_zero_rate,"
               "selected_weighted_zero_rate,selected_weighted_p50,selected_weighted_p90,"
               "selected_weighted_p99,selected_weighted_max,direct_setup_median_ns,"
               "direct_count_median_ns,direct_total_median_ns,ntt_plan_build_median_ns,"
               "ntt_frequency_median_ns,ntt_transform_median_ns,ntt_reconstruction_median_ns,"
               "ntt_lookup_median_ns,ntt_cold_total_median_ns,ntt_amortized_total_median_ns,"
               "ntt_over_direct_cold,ntt_over_direct_amortized,direct_memory_bytes,"
               "ntt_plan_bytes,ntt_per_run_bytes,seed_repetitions,timing_repetitions,"
               "exactness_passed,status\n";

  std::vector<std::tuple<std::uint32_t, long double, PointOutcome>> summary;
  for (const auto list_size : ter::limits::benchmark::kCrossoverListSizeLadder) {
    for (const auto lambda : ter::limits::benchmark::kCrossoverLambdaTargets) {
      const auto choice = choose_q(list_size, lambda);
      if (!choice.feasible) {
        print_skip(list_size, lambda, choice.skip_reason);
        continue;
      }
      const auto outcome = run_point(list_size, lambda, choice, std::cout);
      if (outcome.has_value()) {
        summary.emplace_back(list_size, lambda, *outcome);
      }
    }
  }

  std::cerr << "\n# --- Crossover summary (per L, target lambda) ---\n";
  std::optional<std::pair<std::uint32_t, long double>> earliest_ntt_win;
  for (const auto& [list_size, lambda, outcome] : summary) {
    std::cerr << "# L=" << list_size << " lambda~" << lambda << " -> " << outcome.classification
              << '\n';
    if (!earliest_ntt_win.has_value() &&
        (outcome.classification == "NTT wins cold" ||
         outcome.classification == "NTT wins only amortized")) {
      earliest_ntt_win = std::make_pair(list_size, lambda);
    }
  }
  if (earliest_ntt_win.has_value()) {
    std::cerr << "# Earliest observed crossover: L=" << earliest_ntt_win->first
              << " lambda~" << earliest_ntt_win->second << '\n';
  } else {
    std::cerr << "# No point in the executed matrix showed direct losing on the majority of "
                 "seeds.\n";
  }
  return 0;
}
