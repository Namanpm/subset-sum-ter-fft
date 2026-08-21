#include "ter/limitations.hpp"
#include "ter/merge.hpp"
#include "ter/stress_generator.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Options {
  std::uint32_t n = 36U;
  std::uint8_t b1 = 10U;
  std::uint8_t b2 = 6U;
  std::array<std::uint32_t, 3> sizes{32U, 64U, 96U};
  std::uint32_t candidate_pool = 256U;
  std::uint32_t half_pool = 512U;
  std::uint32_t max_half_pool = 2'048U;
  std::uint64_t memory_mib = 256U;
  std::uint64_t seed = 0x535452455353ULL;
  std::uint32_t repetitions = 1U;
  std::uint8_t prefilter_bits = 2U;
  std::uint32_t hot_bucket_count = 1U;
  std::uint32_t hot_mass_percent = 60U;
  std::uint32_t hot_tolerance_percent = 15U;
  ter::BucketProfile profile = ter::BucketProfile::HotBuckets;
  ter::StressSolutionMode solution_mode = ter::StressSolutionMode::SinglePlantedSolution;
};

[[nodiscard]] std::uint64_t median(std::vector<std::uint64_t> values) {
  std::sort(values.begin(), values.end());
  return values[values.size() / 2U];
}

[[nodiscard]] std::uint64_t percentile(std::vector<std::uint64_t> values,
                                        const std::uint32_t percent) {
  std::sort(values.begin(), values.end());
  const std::size_t index = static_cast<std::size_t>(
      (static_cast<std::uint64_t>(values.size() - 1U) * percent + 50U) / 100U);
  return values[index];
}

template <typename Integer>
[[nodiscard]] bool parse_integer(const std::string_view text, Integer& value) {
  Integer parsed{};
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (error != std::errc{} || end != text.data() + text.size()) {
    return false;
  }
  value = parsed;
  return true;
}

[[nodiscard]] bool parse_sizes(const std::string_view text,
                               std::array<std::uint32_t, 3>& sizes) {
  std::size_t begin = 0U;
  for (std::size_t index = 0U; index < sizes.size(); ++index) {
    const std::size_t end = text.find(',', begin);
    if ((index + 1U < sizes.size() && end == std::string_view::npos) ||
        (index + 1U == sizes.size() && end != std::string_view::npos) ||
        !parse_integer(text.substr(begin, end == std::string_view::npos ? end : end - begin),
                       sizes[index])) {
      return false;
    }
    begin = end == std::string_view::npos ? end : end + 1U;
  }
  return true;
}

[[nodiscard]] bool parse_options(const int argc, char** argv, Options& options) {
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument{argv[index]};
    const auto value = [&argument](const std::string_view key) {
      return argument.rfind(key, 0U) == 0U ? argument.substr(key.size()) : std::string_view{};
    };
    if (argument == "--uniform") {
      options.profile = ter::BucketProfile::Uniform;
    } else if (argument == "--hot") {
      options.profile = ter::BucketProfile::HotBuckets;
    } else if (argument == "--no-plant") {
      options.solution_mode = ter::StressSolutionMode::NoPlantedSolution;
    } else if (!value("--n=").empty()) {
      if (!parse_integer(value("--n="), options.n)) return false;
    } else if (!value("--b1=").empty()) {
      if (!parse_integer(value("--b1="), options.b1)) return false;
    } else if (!value("--b2=").empty()) {
      if (!parse_integer(value("--b2="), options.b2)) return false;
    } else if (!value("--sizes=").empty()) {
      if (!parse_sizes(value("--sizes="), options.sizes)) return false;
    } else if (!value("--candidate-pool=").empty()) {
      if (!parse_integer(value("--candidate-pool="), options.candidate_pool)) return false;
    } else if (!value("--half-pool=").empty()) {
      if (!parse_integer(value("--half-pool="), options.half_pool)) return false;
    } else if (!value("--max-half-pool=").empty()) {
      if (!parse_integer(value("--max-half-pool="), options.max_half_pool)) return false;
    } else if (!value("--memory-mib=").empty()) {
      if (!parse_integer(value("--memory-mib="), options.memory_mib)) return false;
    } else if (!value("--seed=").empty()) {
      if (!parse_integer(value("--seed="), options.seed)) return false;
    } else if (!value("--repetitions=").empty()) {
      if (!parse_integer(value("--repetitions="), options.repetitions)) return false;
    } else if (!value("--prefilter-bits=").empty()) {
      if (!parse_integer(value("--prefilter-bits="), options.prefilter_bits)) return false;
    } else if (!value("--hot-buckets=").empty()) {
      if (!parse_integer(value("--hot-buckets="), options.hot_bucket_count)) return false;
    } else if (!value("--hot-mass=").empty()) {
      if (!parse_integer(value("--hot-mass="), options.hot_mass_percent)) return false;
    } else if (!value("--hot-tolerance=").empty()) {
      if (!parse_integer(value("--hot-tolerance="), options.hot_tolerance_percent)) return false;
    } else {
      return false;
    }
  }
  return options.repetitions != 0U;
}

void usage() {
  std::cerr << "usage: ter_stress_bench [--uniform|--hot] [--no-plant] [--n=N] [--b1=B1] "
               "[--b2=B2] [--sizes=A,B,C] [--candidate-pool=N] [--half-pool=N] "
               "[--max-half-pool=N] [--memory-mib=N] [--seed=N] [--repetitions=N] "
               "[--prefilter-bits=N] [--hot-buckets=N] [--hot-mass=N] "
               "[--hot-tolerance=N]\n";
}

}  // namespace

int main(const int argc, char** argv) {
  Options options;
  if (!parse_options(argc, argv, options)) {
    usage();
    return 2;
  }
  ter::TerStressConfig config;
  config.source.n = options.n;
  config.source.b2 = options.b2;
  config.source.b1 = options.b1;
  config.source.seed = options.seed;
  config.source.mode = ter::GeneratorMode::PlantedMitm;
  config.source.plant_valid_triple = true;
  config.source.half_pool_size = options.half_pool;
  config.source.max_half_pool_size = options.max_half_pool;
  config.source.memory_limit_bytes = options.memory_mib * ter::limits::kMiB;
  config.candidate_pool_size = options.candidate_pool;
  config.requested_list_sizes = options.sizes;
  config.mode = ter::StressMode::Composite;
  config.bucket_profile = options.profile;
  config.hot_bucket_count = options.hot_bucket_count;
  config.target_hot_mass_percent = options.hot_mass_percent;
  config.hot_mass_tolerance_percent = options.hot_tolerance_percent;
  config.prefilter_bits = options.prefilter_bits;
  config.solution_mode = options.solution_mode;

  const auto generation_start = std::chrono::steady_clock::now();
  const auto generated = ter::build_stress_snapshot(config);
  const auto generation_ns = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - generation_start)
          .count());
  if (generated.status != ter::Status::Completed) {
    std::cerr << "stress generation failed status=" << static_cast<unsigned>(generated.status)
              << " message=" << generated.message << '\n';
    return 1;
  }

  const auto warmup = ter::run_pair_tiled(generated.snapshot, ter::OutputPolicy::Benchmark);
  if (warmup.status != ter::Status::Completed) {
    std::cerr << "stress warmup failed: " << warmup.message << '\n';
    return 1;
  }
  std::vector<std::uint64_t> totals;
  std::vector<std::uint64_t> scans;
  totals.reserve(options.repetitions);
  scans.reserve(options.repetitions);
  ter::MergeOutput last;
  for (std::uint32_t repetition = 0U; repetition < options.repetitions; ++repetition) {
    const auto output = ter::run_pair_tiled(generated.snapshot, ter::OutputPolicy::Benchmark);
    if (output.status != ter::Status::Completed) {
      std::cerr << "stress timing run failed: " << output.message << '\n';
      return 1;
    }
    if (!last.parents.empty() && output.parents != last.parents) {
      std::cerr << "stress timing run changed the parent result\n";
      return 1;
    }
    totals.push_back(output.telemetry.total_ns);
    scans.push_back(output.telemetry.candidate_loop_ns);
    last = output;
  }
  const auto& buckets = generated.telemetry.buckets;
  std::cout << "fixture_family,seed,n,level,r1,r2,quotient_bits,A_size,B_size,C_size,"
               "bucket_profile,candidate_w,candidate_tile_bits,bucket_count,nonempty_buckets,"
               "hot_mass_percent,normalized_entropy,generation_ns,repetitions,"
               "merge_p10_ns,merge_median_ns,merge_p90_ns,scan_median_ns,"
               "actual_survivor_count,auxiliary_memory_bytes,candidate_triples\n";
  std::cout << "ADVERSARIAL_STRESS," << options.seed << ',' << options.n << ','
            << "L2_TO_L1," << static_cast<unsigned>(options.b1) << ','
            << static_cast<unsigned>(options.b2) << ','
            << static_cast<unsigned>(generated.telemetry.quotient_bits) << ','
            << generated.telemetry.actual_list_sizes[0] << ','
            << generated.telemetry.actual_list_sizes[1] << ','
            << generated.telemetry.actual_list_sizes[2] << ','
            << (options.profile == ter::BucketProfile::HotBuckets ? "hot" : "uniform") << ','
            << static_cast<unsigned>(options.prefilter_bits) << ','
            << ter::limits::tiled::kDefaultPreferredTileBits << ','
            << buckets.bucket_count << ',' << buckets.nonempty_bucket_count << ','
            << buckets.hot_mass_percent << ',' << buckets.normalized_entropy << ','
            << generation_ns << ',' << options.repetitions << ',' << percentile(totals, 10U) << ','
            << median(totals) << ',' << percentile(totals, 90U) << ',' << median(scans) << ','
            << last.telemetry.raw_valid_triples << ',' << last.telemetry.pair_index_bytes << ','
            << last.telemetry.candidate_triples << '\n';
  return 0;
}
