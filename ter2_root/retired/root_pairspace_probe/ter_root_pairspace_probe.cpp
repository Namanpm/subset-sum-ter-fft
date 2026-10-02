// Pair-space structure probe for a genuine captured Full TER root.
//
// Purpose: decide, from measurement rather than from a uniformity model,
// whether exact low-digit region pruning (the proposed "Stage D") can reduce
// the A x B pair work of the root merge.
//
// For a radix width k the root condition
//
//     q_a + q_b + q_c == T   (mod 2^Q)
//
// implies, on the low k bits alone,
//
//     (q_a + q_b + q_c) mod 2^k == T mod 2^k,
//
// so for a rectangle A_i x B_j (every member of A_i has low digit i, every
// member of B_j has low digit j) the required C low digit is exactly
//
//     d(i,j) = (T0 - i - j) mod 2^k.
//
// That is an exact, no-false-negative test: if bucket C_d is empty, no pair in
// the whole rectangle can complete to any c in C. This probe measures how much
// pair mass that rejects, and — critically — what it costs to enumerate the
// rectangles in the first place.
//
// It reports work counts only. It makes no wall-clock claim.

#include "ter/full_ter.hpp"
#include "ter/limitations.hpp"
#include "ter_root_capture_adapter.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] std::uint64_t elapsed_ns(const Clock::time_point start) noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
}

void add_value(ter::Instance& instance, std::uint32_t index) {
  std::uint64_t carry = 0;
  const std::size_t base = static_cast<std::size_t>(index) * instance.words_per_value;
  for (std::uint32_t word = 0; word < instance.words_per_value; ++word) {
    const std::uint64_t original = instance.root_target_le[word];
    const std::uint64_t first = original + instance.values_le[base + word];
    const std::uint64_t carry_first = first < original ? 1U : 0U;
    const std::uint64_t second = first + carry;
    const std::uint64_t carry_second = second < first ? 1U : 0U;
    instance.root_target_le[word] = second;
    carry = carry_first | carry_second;
  }
}

ter::Instance benchmark_instance(std::uint32_t n, std::uint64_t seed) {
  auto built =
      ter::build_instance(n, ter::limits::benchmark::kFullTerFixtureModulusBits, seed);
  if (built.status != ter::Status::Completed) {
    return {};
  }
  auto instance = std::move(built.value);
  std::fill(instance.planted_root.begin(), instance.planted_root.end(), std::uint8_t{0});
  std::fill(instance.root_target_le.begin(), instance.root_target_le.end(), 0ULL);
  const std::uint32_t half_width = instance.n / 2U;
  const std::uint32_t planted_per_half = instance.n / 4U;
  for (std::uint32_t half = 0; half < 2U; ++half) {
    const std::uint32_t begin = half * half_width;
    for (std::uint32_t offset = 0; offset < planted_per_half; ++offset) {
      const std::uint32_t coordinate = begin + offset;
      instance.planted_root[coordinate] = 1U;
      add_value(instance, coordinate);
    }
  }
  const std::uint32_t top_word_bits = instance.n % 64U;
  if (top_word_bits != 0U) {
    instance.root_target_le.back() &= (std::uint64_t{1} << top_word_bits) - 1U;
  }
  return instance;
}

struct Histogram {
  std::vector<std::uint32_t> counts;   // per low-k digit
  std::uint64_t occupied = 0U;
};

[[nodiscard]] Histogram build_histogram(
    const std::vector<ter::TerPartitionEntry>& records, const std::uint32_t bits) {
  Histogram histogram;
  histogram.counts.assign(std::size_t{1} << bits, 0U);
  const std::uint32_t mask = bits == 32U ? 0xFFFFFFFFU : (std::uint32_t{1} << bits) - 1U;
  for (const auto& record : records) {
    ++histogram.counts[static_cast<std::size_t>(record.low_k & mask)];
  }
  for (const auto count : histogram.counts) {
    histogram.occupied += count != 0U ? 1U : 0U;
  }
  return histogram;
}

[[nodiscard]] int report_pair_space(
    const std::vector<std::vector<ter::TerPartitionEntry>>& lists,
    const std::uint32_t target_low_in,
    const std::uint32_t quotient_width,
    const std::string_view label) {
  const std::uint64_t a_size = lists[0].size();
  const std::uint64_t b_size = lists[1].size();
  const std::uint64_t c_size = lists[2].size();
  const std::uint64_t pair_mass = a_size * b_size;
  const std::uint32_t quotient_bits = quotient_width;

  // The low-k target digit. Low bits of a modular sum depend only on the low
  // bits of the operands, so truncating the root target is exact here.
  const std::uint32_t target_low = target_low_in;

  std::cout << "# root=" << label << " Q=" << quotient_bits
            << " |A|=" << a_size << " |B|=" << b_size << " |C|=" << c_size << '\n';
  std::cout << "# P0 = |A|*|B| = " << pair_mass << " logical pairs\n";
  std::cout << "bits,A_occupied,B_occupied,C_occupied,regions_considered,"
               "regions_rejected_exact,regions_surviving,rho_region,"
               "rejected_pair_mass,opened_pair_mass,rho_open,pair_work_reduction,"
               "phi_region_visits,summary_entries,summary_build_ns,region_prune_ns,"
               "total_work,vs_P0\n";

  for (const std::uint32_t bits : {8U, 10U, 12U, 14U, 16U, 18U, 20U, 22U, 24U}) {
    const Histogram a_hist = build_histogram(lists[0], bits);
    const Histogram b_hist = build_histogram(lists[1], bits);
    const Histogram c_hist = build_histogram(lists[2], bits);
    const std::uint32_t mask = (std::uint32_t{1} << bits) - 1U;
    const std::uint32_t target_digit = target_low & mask;

    // Only occupied buckets are ever visited; empty ones cost nothing.
    std::vector<std::uint32_t> a_used;
    std::vector<std::uint32_t> b_used;
    a_used.reserve(static_cast<std::size_t>(a_hist.occupied));
    b_used.reserve(static_cast<std::size_t>(b_hist.occupied));
    for (std::uint32_t digit = 0; digit <= mask; ++digit) {
      if (a_hist.counts[digit] != 0U) {
        a_used.push_back(digit);
      }
      if (b_hist.counts[digit] != 0U) {
        b_used.push_back(digit);
      }
    }

    // Regions are disjoint over A x B: every pair (a,b) lands in exactly one
    // (low_k(q_a), low_k(q_b)) rectangle, and the low-k congruence fixes a
    // single required C digit per rectangle. So opened_mass counts each
    // original pair at most once and there is no target multiplicity.
    const auto summary_start = Clock::now();
    // The summary is the exact C occupancy directory built above; time its
    // construction separately from the region sweep.
    std::uint64_t summary_touch = 0U;
    for (const auto count : c_hist.counts) {
      summary_touch += count != 0U ? 1U : 0U;
    }
    const std::uint64_t summary_ns = elapsed_ns(summary_start);

    const auto prune_start = Clock::now();
    std::uint64_t surviving = 0U;
    std::uint64_t rejected = 0U;
    std::uint64_t opened_mass = 0U;
    for (const std::uint32_t i : a_used) {
      const std::uint64_t a_count = a_hist.counts[i];
      for (const std::uint32_t j : b_used) {
        const std::uint32_t required = (target_digit - i - j) & mask;
        if (c_hist.counts[required] != 0U) {
          ++surviving;
          opened_mass += a_count * b_hist.counts[j];
        } else {
          ++rejected;
        }
      }
    }
    const std::uint64_t prune_ns = elapsed_ns(prune_start);

    // Phi is the enumeration cost of this naive sweep: one visit per occupied
    // A bucket x occupied B bucket. It is reported apart from rho_open so a
    // cheaper enumeration can be evaluated against the same pruning power.
    const std::uint64_t regions_considered = surviving + rejected;
    const std::uint64_t phi_region_visits = regions_considered;
    const std::uint64_t rejected_mass = pair_mass - opened_mass;
    const double rho_open =
        pair_mass == 0U ? 0.0
                        : static_cast<double>(opened_mass) / static_cast<double>(pair_mass);
    const double rho_region =
        regions_considered == 0U
            ? 0.0
            : static_cast<double>(surviving) / static_cast<double>(regions_considered);
    const double pair_work_reduction =
        opened_mass == 0U ? 0.0
                          : static_cast<double>(pair_mass) / static_cast<double>(opened_mass);
    const std::uint64_t total_work = phi_region_visits + opened_mass;
    const double vs_p0 = pair_mass == 0U
                             ? 0.0
                             : static_cast<double>(total_work) / static_cast<double>(pair_mass);
    // Invariant: a disjoint decomposition must conserve pair mass exactly.
    if (opened_mass + rejected_mass != pair_mass) {
      std::cerr << "pair-mass conservation violated at bits=" << bits << '\n';
      return EXIT_FAILURE;
    }

    std::cout << bits << ',' << a_hist.occupied << ',' << b_hist.occupied << ','
              << c_hist.occupied << ',' << regions_considered << ',' << rejected << ','
              << surviving << ',' << rho_region << ',' << rejected_mass << ','
              << opened_mass << ',' << rho_open << ',' << pair_work_reduction << ','
              << phi_region_visits << ',' << summary_touch << ',' << summary_ns << ','
              << prune_ns << ',' << total_work << ',' << vs_p0 << '\n';
    std::cout.flush();
  }
  return EXIT_SUCCESS;
}

}  // namespace

int main(const int argc, char** const argv) {
  std::uint32_t root_n = 72U;
  std::uint64_t root_seed = 19797034556212562ULL;
  bool synthetic = false;
  std::uint64_t synthetic_a = 30000U;
  std::uint64_t synthetic_b = 30000U;
  std::uint64_t synthetic_c = 30000U;
  for (int argument = 1; argument < argc; ++argument) {
    const std::string_view value(argv[argument]);
    if (value.starts_with("--root-n=")) {
      root_n = static_cast<std::uint32_t>(std::strtoul(argv[argument] + 9, nullptr, 10));
    } else if (value.starts_with("--root-seed=")) {
      root_seed = std::strtoull(argv[argument] + 12, nullptr, 10);
    } else if (value == "--synthetic") {
      synthetic = true;
    } else if (value.starts_with("--a=")) {
      synthetic_a = std::strtoull(argv[argument] + 4, nullptr, 10);
    } else if (value.starts_with("--b=")) {
      synthetic_b = std::strtoull(argv[argument] + 4, nullptr, 10);
    } else if (value.starts_with("--c=")) {
      synthetic_c = std::strtoull(argv[argument] + 4, nullptr, 10);
    } else {
      std::cerr << "usage: ter_root_pairspace_probe [--root-n=N] [--root-seed=S]\n"
                   "       ter_root_pairspace_probe --synthetic --a=N --b=N --c=N\n";
      return EXIT_FAILURE;
    }
  }
  if (synthetic && (synthetic_a == 0U || synthetic_b == 0U || synthetic_c == 0U)) {
    std::cerr << "synthetic list sizes must be positive\n";
    return EXIT_FAILURE;
  }

  // Synthetic mode exists to test the regime the closed form predicts should
  // win: exact region pruning pays only when |C| << sqrt(|A|*|B|). Genuine TER
  // roots are balanced (|A| ~ |B| ~ |C|), so that condition is never met there;
  // this lets the boundary be measured instead of argued.
  if (synthetic) {
    std::vector<std::vector<ter::TerPartitionEntry>> lists(3);
    const std::array<std::uint64_t, 3> sizes{synthetic_a, synthetic_b, synthetic_c};
    std::uint64_t state = root_seed | 1ULL;
    const auto next = [&state]() {
      state += 0x9E3779B97F4A7C15ULL;
      std::uint64_t z = state;
      z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
      z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
      return static_cast<std::uint32_t>((z ^ (z >> 31)) & 0xFFFFFFFFU);
    };
    for (std::size_t group = 0; group < 3U; ++group) {
      lists[group].reserve(static_cast<std::size_t>(sizes[group]));
      for (std::uint64_t index = 0; index < sizes[group]; ++index) {
        lists[group].push_back({next(), static_cast<ter::EntryId>(index)});
      }
    }
    return report_pair_space(lists, 0U, 47U, "synthetic-uniform");
  }

  const auto instance = benchmark_instance(root_n, root_seed);
  if (instance.n == 0U) {
    std::cerr << "failed to build probe instance\n";
    return EXIT_FAILURE;
  }
  ter::FullTerRootCapture capture;
  ter::FullTerConfig config;
  config.family = ter::ShapeFamily::FiniteSanity;
  config.execution_mode = ter::TerExecutionMode::FullTer;
  config.target_mode = ter::TerTargetMode::Random;
  config.seed = root_seed;
  config.attempt_limit = ter::limits::full_ter::kDefaultAttemptLimit;
  config.memory_limit_bytes = ter::limits::benchmark::kFullTerMemoryBytes;
  config.root_capture = &capture;
  const auto solved = ter::solve_full_ter(instance, config);
  if (capture.status != ter::Status::Completed) {
    std::cerr << "root capture did not complete: " << capture.message << '\n';
    return EXIT_FAILURE;
  }

  std::vector<std::vector<ter::TerPartitionEntry>> lists(3);
  for (std::size_t group = 0; group < 3U; ++group) {
    auto converted = ter::root_partition_bench::capture_list_to_records(capture, group);
    if (converted.status != ter::Status::Completed) {
      std::cerr << "list conversion failed: " << converted.message << '\n';
      return EXIT_FAILURE;
    }
    lists[group] = std::move(converted.records);
  }
  return report_pair_space(lists, capture.root_target_words_le.empty() ? 0U : capture.root_target_words_le[0], capture.parameters.n - capture.parameters.r1, "captured");
}
