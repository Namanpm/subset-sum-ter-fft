// Deterministic cache-tile parameter sweep for the exact tiled Q3REPORT locator.
//
// This is intentionally a merge-kernel benchmark, not a full TER-solver benchmark.
// Each w uses a fresh n=72 TER-shaped L2 sample with exact child digit distributions;
// SB is the requested tile-width exponent.  The independently computed raw triple
// count is checked before timings are emitted.

#include "ter/q3report_paper_crossover.hpp"
#include "ter/tiled_q3report.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::array<std::uint32_t, 9> kQuotientBits{16U, 18U, 20U, 22U, 24U,
                                                      26U, 28U, 30U, 32U};
constexpr std::array<std::uint32_t, 7> kRequestedTileBits{10U, 12U, 14U, 15U,
                                                           16U, 18U, 20U};
constexpr std::uint32_t kShapeN = 72U;
constexpr std::uint32_t kR2 = 6U;
constexpr std::uint64_t kListSize = 4'096U;
// Default experiment controls.  --seed-base and --repetitions may override
// these; nothing else about the declared grid is configurable.
constexpr std::uint64_t kDefaultSeedBase = 0x1001ULL;
constexpr std::uint64_t kMemoryBytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr std::uint32_t kDefaultTimedRepetitions = 3U;

[[nodiscard]] std::uint64_t elapsed_ns(const Clock::time_point start) noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
}

[[nodiscard]] std::uint64_t median(std::vector<std::uint64_t> samples) {
  std::sort(samples.begin(), samples.end());
  return samples[samples.size() / 2U];
}

[[nodiscard]] std::vector<ter::TiledQEntry> tiled_entries(const ter::TerList& list) {
  std::vector<ter::TiledQEntry> result;
  result.reserve(list.entries.size());
  for (const auto& entry : list.entries) {
    result.push_back(ter::TiledQEntry{entry.q, entry.id});
  }
  return result;
}

// A separate, simple exact count: sort the C target keys and binary-search each
// pair. It deliberately does not share the tile code or its bucket traversal.
[[nodiscard]] std::uint64_t independent_raw_count(const std::vector<ter::TiledQEntry>& a,
                                                   const std::vector<ter::TiledQEntry>& b,
                                                   const std::vector<ter::TiledQEntry>& c,
                                                   const std::uint64_t tau,
                                                   const std::uint64_t q) {
  const std::uint64_t mask = q - 1U;
  std::vector<std::uint64_t> keys;
  keys.reserve(c.size());
  for (const auto& entry : c) {
    keys.push_back((tau - static_cast<std::uint64_t>(entry.q)) & mask);
  }
  std::sort(keys.begin(), keys.end());
  std::uint64_t raw = 0U;
  for (const auto& left : a) {
    for (const auto& right : b) {
      const std::uint64_t sum = (static_cast<std::uint64_t>(left.q) + right.q) & mask;
      const auto range = std::equal_range(keys.begin(), keys.end(), sum);
      raw += static_cast<std::uint64_t>(range.second - range.first);
    }
  }
  return raw;
}

void fail(const std::string& message) {
  std::cerr << "fatal: " << message << '\n';
  std::exit(2);
}

// Strict decimal parse for the experiment-control options.  A trailing
// character, an empty value or an out-of-range value is a refusal, never a
// silently truncated experiment parameter.
[[nodiscard]] bool parse_u64(const std::string& text, std::uint64_t* const out) noexcept {
  if (text.empty()) {
    return false;
  }
  for (const char character : text) {
    if (character < '0' || character > '9') {
      return false;
    }
  }
  errno = 0;
  char* end = nullptr;
  const unsigned long long value = std::strtoull(text.c_str(), &end, 10);
  if (errno != 0 || end != text.c_str() + text.size()) {
    return false;
  }
  *out = static_cast<std::uint64_t>(value);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  std::uint32_t only_w = 0U;
  std::uint32_t only_sb = 0U;
  std::uint64_t seed_base = kDefaultSeedBase;
  std::uint32_t timed_repetitions = kDefaultTimedRepetitions;
  for (int index = 1; index < argc; ++index) {
    const std::string argument{argv[index]};
    constexpr const char* kPrefix = "--w=";
    if (argument.rfind(kPrefix, 0U) == 0U) {
      only_w = static_cast<std::uint32_t>(std::strtoul(argument.c_str() + 4, nullptr, 10));
    } else if (argument.rfind("--SB=", 0U) == 0U) {
      only_sb = static_cast<std::uint32_t>(std::strtoul(argument.c_str() + 5, nullptr, 10));
    } else if (argument.rfind("--seed-base=", 0U) == 0U) {
      if (!parse_u64(argument.substr(12U), &seed_base)) {
        std::cerr << "fatal: --seed-base requires a decimal 64-bit value\n";
        return 2;
      }
    } else if (argument.rfind("--repetitions=", 0U) == 0U) {
      std::uint64_t requested = 0U;
      if (!parse_u64(argument.substr(14U), &requested) || requested == 0U ||
          requested > 0xFFFFFFFFULL) {
        std::cerr << "fatal: --repetitions requires a positive 32-bit value\n";
        return 2;
      }
      timed_repetitions = static_cast<std::uint32_t>(requested);
    } else {
      std::cerr << "usage: ter_tiled_parameter_sweep [--w=W] [--SB=N]"
                   " [--seed-base=U64] [--repetitions=N]\n";
      return 2;
    }
  }
  std::cout << "family,shape_n,list_size,seed,w,SB,effective_SB,Q,tau,raw_triples,"
               "setup_median_ns,scan_median_ns,total_median_ns,auxiliary_peak_bytes,"
               "bucket_count,window_count,executed_pairs,nonempty_windows,"
               "independent_exactness_passed\n";
  for (const std::uint32_t w : kQuotientBits) {
    if (only_w != 0U && only_w != w) {
      continue;
    }
    ter::PaperCrossoverSnapshotConfig config;
    config.requested_list_size = kListSize;
    config.seed = seed_base + w;
    config.initial_half_pool = 256U;
    config.max_half_pool = 65'536U;
    config.memory_limit_bytes = kMemoryBytes;
    config.override_shape_n = kShapeN;
    config.override_r2 = kR2;
    config.override_r1 = kR2 + w;
    const auto built = ter::build_paper_scaling_snapshot(config);
    if (built.status != ter::PaperCrossoverStatus::Completed) {
      fail("w=" + std::to_string(w) + " snapshot: " + built.message);
    }
    const auto audited = ter::audit_paper_crossover_snapshot(built.value);
    if (audited.status != ter::PaperCrossoverStatus::Completed) {
      fail("w=" + std::to_string(w) + " snapshot audit: " + audited.message);
    }
    const auto& arithmetic = built.value.input.spec.arithmetic;
    const auto a = tiled_entries(built.value.input.lists[0]);
    const auto b = tiled_entries(built.value.input.lists[1]);
    const auto c = tiled_entries(built.value.input.lists[2]);
    const std::uint64_t oracle = independent_raw_count(a, b, c, arithmetic.tau, arithmetic.q);

    for (const std::uint32_t requested_sb : kRequestedTileBits) {
      if (only_sb != 0U && only_sb != requested_sb) {
        continue;
      }
      ter::TiledQ3ReportConfig tiled_config;
      tiled_config.preferred_tile_bits = requested_sb;
      tiled_config.memory_limit_bytes = kMemoryBytes;
      const auto warmup = ter::tiled_q3report_count(a, b, c, arithmetic.tau, arithmetic.q,
                                                    tiled_config);
      if (warmup.status != ter::TiledQ3ReportStatus::Completed ||
          warmup.telemetry.raw_triples != oracle) {
        fail("w=" + std::to_string(w) + " SB=" + std::to_string(requested_sb) +
             " warmup failed exactness");
      }
      std::vector<std::uint64_t> setup_samples;
      std::vector<std::uint64_t> scan_samples;
      std::vector<std::uint64_t> total_samples;
      setup_samples.reserve(timed_repetitions);
      scan_samples.reserve(timed_repetitions);
      total_samples.reserve(timed_repetitions);
      ter::TiledQ3ReportResult last;
      for (std::uint32_t repetition = 0U; repetition < timed_repetitions; ++repetition) {
        const auto total_start = Clock::now();
        last = ter::tiled_q3report_count(a, b, c, arithmetic.tau, arithmetic.q, tiled_config);
        const auto total = elapsed_ns(total_start);
        if (last.status != ter::TiledQ3ReportStatus::Completed ||
            last.telemetry.raw_triples != oracle) {
          fail("w=" + std::to_string(w) + " SB=" + std::to_string(requested_sb) +
               " timed run failed exactness");
        }
        setup_samples.push_back(last.telemetry.setup_ns);
        scan_samples.push_back(last.telemetry.scan_ns);
        total_samples.push_back(total);
      }
      std::cout << "TER_SHAPED_L2_MERGE_MICROBENCH," << kShapeN << ',' << kListSize << ','
                << built.value.seed << ',' << w << ',' << requested_sb << ','
                << last.telemetry.tile_bits << ',' << arithmetic.q << ',' << arithmetic.tau << ','
                << oracle << ',' << median(std::move(setup_samples)) << ','
                << median(std::move(scan_samples)) << ',' << median(std::move(total_samples))
                << ',' << last.telemetry.auxiliary_peak_bytes << ','
                << last.telemetry.bucket_count << ',' << last.telemetry.window_count << ','
                << last.telemetry.executed_pairs << ',' << last.telemetry.nonempty_windows
                << ",1\n";
      std::cout.flush();
    }
  }
  return 0;
}
