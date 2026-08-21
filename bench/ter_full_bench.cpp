#include "ter/full_ter.hpp"
#include "ter/limitations.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>
#include <vector>

namespace {

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
  auto built = ter::build_instance(n,
                                   ter::limits::benchmark::kFullTerFixtureModulusBits,
                                   seed);
  if (built.status != ter::Status::Completed) {
    return {};
  }
  auto instance = std::move(built.value);
  std::fill(instance.planted_root.begin(),
            instance.planted_root.end(),
            std::uint8_t{0});
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

std::uint64_t median(std::vector<std::uint64_t> values) {
  std::sort(values.begin(), values.end());
  return values[values.size() / 2U];
}

}  // namespace

int main(const int argc, char** const argv) {
  std::uint32_t benchmark_n = ter::limits::benchmark::kFullTerFixtureN;
  for (int argument = 1; argument < argc; ++argument) {
    const std::string_view value(argv[argument]);
    constexpr std::string_view kDimensionPrefix{"--n="};
    if (!value.starts_with(kDimensionPrefix)) {
      std::cerr << "usage: ter_full_bench [--n=<finite TER dimension>]\n";
      return EXIT_FAILURE;
    }
    std::uint32_t parsed = 0U;
    for (const char character : value.substr(kDimensionPrefix.size())) {
      if (character < '0' || character > '9' ||
          parsed > (std::numeric_limits<std::uint32_t>::max() -
                    static_cast<std::uint32_t>(character - '0')) / 10U) {
        std::cerr << "invalid --n value\n";
        return EXIT_FAILURE;
      }
      parsed = parsed * 10U + static_cast<std::uint32_t>(character - '0');
    }
    benchmark_n = parsed;
  }
  constexpr std::uint64_t seed = ter::limits::benchmark::kFullTerFixtureSeed;
  const auto instance = benchmark_instance(benchmark_n, seed);
  if (instance.n == 0U) {
    std::cerr << "failed to build benchmark instance\n";
    return EXIT_FAILURE;
  }
  ter::FullTerConfig config;
  config.family = ter::ShapeFamily::FiniteSanity;
  config.execution_mode = ter::TerExecutionMode::FullTer;
  config.target_mode = ter::TerTargetMode::Random;
  config.seed = seed;
  config.attempt_limit = ter::limits::full_ter::kDefaultAttemptLimit;
  config.memory_limit_bytes = ter::limits::benchmark::kFullTerMemoryBytes;
  config.audit = false;

  const auto warmup = ter::solve_full_ter(instance, config);
  if (!ter::limits::benchmark::full_ter_warmup_acceptable(warmup.status)) {
    std::cerr << "full TER warmup failed: " << warmup.message << '\n';
    return EXIT_FAILURE;
  }
  std::vector<std::uint64_t> totals;
  std::vector<std::uint64_t> l3;
  std::vector<std::uint64_t> binary;
  std::vector<std::uint64_t> middle;
  std::vector<std::uint64_t> root;
  std::vector<std::uint64_t> verification;
  for (std::uint32_t repetition = 0;
       repetition < ter::limits::benchmark::kFullTerTimedRepetitions;
       ++repetition) {
    const auto measured = ter::solve_full_ter(instance, config);
    if (measured.status != warmup.status ||
        measured.witness != warmup.witness ||
        measured.telemetry.fingerprint != warmup.telemetry.fingerprint) {
      std::cerr << "full TER benchmark changed its exact result\n";
      return EXIT_FAILURE;
    }
    totals.push_back(measured.telemetry.total_solver_ns);
    l3.push_back(measured.telemetry.level3_generation_ns);
    binary.push_back(measured.telemetry.attempts[0].level3_to_level2.elapsed_ns);
    middle.push_back(measured.telemetry.attempts[0].level2_to_level1.elapsed_ns);
    root.push_back(measured.telemetry.attempts[0].level1_to_level0.elapsed_ns);
    verification.push_back(measured.telemetry.attempts[0].verification_ns);
  }

#if defined(_MSC_VER)
  std::cout << "# compiler=MSVC-" << _MSC_VER;
#else
  std::cout << "# compiler=non-MSVC";
#endif
#if defined(NDEBUG)
  std::cout << " build=Release";
#else
  std::cout << " build=Debug";
#endif
#if defined(TER_NATIVE_OPT_ENABLED)
  std::cout << " native_opt=on";
#else
  std::cout << " native_opt=off";
#endif
  std::cout << " threads=1 mode=FullTer target=Random family=FiniteSanity\n";
  std::cout << "status,n,r1,r2,L3_left,L3_right,L2_sizes,L1_sizes,L0,"
               "l3_generation_ns,l3_to_l2_ns,l2_to_l1_ns,l1_to_l0_ns,"
               "verification_ns,total_solver_ns,"
               "binary_pairs,binary_modular,binary_structural_rejects,"
               "binary_distribution_rejects,binary_survivors,binary_deduplicated,"
               "middle_pairs,middle_modular,middle_structural_rejects,"
               "middle_distribution_rejects,middle_survivors,middle_deduplicated,"
               "root_pairs,root_modular,root_structural_rejects,"
               "root_distribution_rejects,root_survivors,root_deduplicated,"
               "l3_list_bytes,arithmetic_bytes,l2_list_bytes,l1_list_bytes,l0_list_bytes,"
               "l2_active_bytes,l1_active_bytes,root_active_bytes,"
               "peak_working_bytes,hot_entry_bytes,packed_vector_bytes,fingerprint,"
               "l2_to_l1_tiled_used,l2_to_l1_pair_records,l2_to_l1_tile_bits,"
               "l2_to_l1_tiled_pair_probes,l2_to_l1_tiled_nonempty_windows,"
               "l2_to_l1_tiled_auxiliary_bytes,l2_to_l1_tiled_setup_ns,"
               "l2_to_l1_tiled_scan_ns,"
               "l1_to_l0_tiled_used,l1_to_l0_pair_records,l1_to_l0_quotient_bits,"
               "l1_to_l0_prefilter_bits,l1_to_l0_low_prefilter_candidates,"
               "l1_to_l0_full_quotient_rejects,l1_to_l0_tile_bits,"
               "l1_to_l0_tiled_pair_probes,l1_to_l0_tiled_nonempty_windows,"
               "l1_to_l0_tiled_auxiliary_bytes,l1_to_l0_tiled_setup_ns,"
               "l1_to_l0_tiled_scan_ns\n";
  const auto& attempt = warmup.telemetry.attempts[0];
  std::cout << (warmup.status == ter::Status::Found ? "Found"
                                                    : "AttemptLimitReached")
            << ',' << instance.n << ','
            << warmup.telemetry.parameters.r1 << ','
            << warmup.telemetry.parameters.r2 << ','
            << warmup.telemetry.physical_level3_sizes[0] << ','
            << warmup.telemetry.physical_level3_sizes[1] << ",\"";
  for (std::size_t index = 0; index < attempt.level2_sizes.size(); ++index) {
    std::cout << (index == 0U ? "" : ":") << attempt.level2_sizes[index];
  }
  std::cout << "\",\"";
  for (std::size_t index = 0; index < attempt.level1_sizes.size(); ++index) {
    std::cout << (index == 0U ? "" : ":") << attempt.level1_sizes[index];
  }
  std::cout << "\"," << attempt.level0_size << ',' << median(l3) << ','
            << median(binary) << ',' << median(middle) << ',' << median(root) << ','
            << median(verification) << ',' << median(totals) << ','
            << attempt.level3_to_level2.pair_candidates << ','
            << attempt.level3_to_level2.modular_candidates << ','
            << attempt.level3_to_level2.structural_rejects << ','
            << attempt.level3_to_level2.distribution_rejects << ','
            << attempt.level3_to_level2.survivors_before_dedup << ','
            << attempt.level3_to_level2.survivors_after_dedup << ','
            << attempt.level2_to_level1.pair_candidates << ','
            << attempt.level2_to_level1.modular_candidates << ','
            << attempt.level2_to_level1.structural_rejects << ','
            << attempt.level2_to_level1.distribution_rejects << ','
            << attempt.level2_to_level1.survivors_before_dedup << ','
            << attempt.level2_to_level1.survivors_after_dedup << ','
            << attempt.level1_to_level0.pair_candidates << ','
            << attempt.level1_to_level0.modular_candidates << ','
            << attempt.level1_to_level0.structural_rejects << ','
            << attempt.level1_to_level0.distribution_rejects << ','
            << attempt.level1_to_level0.survivors_before_dedup << ','
            << attempt.level1_to_level0.survivors_after_dedup << ','
            << warmup.telemetry.level3_allocated_bytes << ','
            << warmup.telemetry.arithmetic_allocated_bytes << ','
            << attempt.level2_allocated_bytes << ','
            << attempt.level1_allocated_bytes << ','
            << attempt.level0_allocated_bytes << ','
            << attempt.level3_to_level2.allocated_bytes << ','
            << attempt.level2_to_level1.allocated_bytes << ','
            << attempt.level1_to_level0.allocated_bytes << ','
            << warmup.telemetry.peak_working_bytes << ','
            << warmup.telemetry.hot_entry_bytes << ','
            << warmup.telemetry.packed_vector_bytes << ','
            << warmup.telemetry.fingerprint << ','
            << (attempt.level2_to_level1.tiled_used ? 1 : 0) << ','
            << attempt.level2_to_level1.pair_records << ','
            << attempt.level2_to_level1.tile_bits << ','
            << attempt.level2_to_level1.tiled_pair_probes << ','
            << attempt.level2_to_level1.tiled_nonempty_windows << ','
            << attempt.level2_to_level1.tiled_auxiliary_bytes << ','
            << attempt.level2_to_level1.tiled_setup_ns << ','
            << attempt.level2_to_level1.tiled_scan_ns << ','
            << (attempt.level1_to_level0.tiled_used ? 1 : 0) << ','
            << attempt.level1_to_level0.pair_records << ','
            << attempt.level1_to_level0.quotient_bits << ','
            << attempt.level1_to_level0.prefilter_bits << ','
            << attempt.level1_to_level0.low_prefilter_candidates << ','
            << attempt.level1_to_level0.full_quotient_rejects << ','
            << attempt.level1_to_level0.tile_bits << ','
            << attempt.level1_to_level0.tiled_pair_probes << ','
            << attempt.level1_to_level0.tiled_nonempty_windows << ','
            << attempt.level1_to_level0.tiled_auxiliary_bytes << ','
            << attempt.level1_to_level0.tiled_setup_ns << ','
            << attempt.level1_to_level0.tiled_scan_ns << '\n';
  return EXIT_SUCCESS;
}
