// TER^2 residual windowing research spike.
//
// Answers one question: can residual-window locality remove the cost /
// selectivity tradeoff that produced the MicroReport8 TOO_SLOW verdict?
//
// Two possibilities are tested, not one:
//   (A) the existing rank-ordered 8x8 tiles already have useful numeric
//       locality, so a tiny window-local exact target beats scattered gathers;
//   (B) they do not, but re-tiling by numeric VALUE ranges creates locality
//       and lets whole source rectangles be rejected before any leaf reporter.
//
// This is an isolated benchmark. It does not integrate TER^2, does not touch
// the production L1->L0 reporter, does not replace tiled_q3report, and makes no
// wall-clock claim about FullTER. The MicroReport8 dataset builder, kernels and
// truth oracle are used as unmodified donors.
//
// Evidence classes, never merged:
//   MEASURED             counted exhaustively over authentic tiles, or timed on
//                        this machine in this executable.
//   PROJECTED            a measured per-unit cost multiplied by a measured
//                        count. Labelled at the point of printing.
//   PRIOR_MEASURED       the MicroReport8 run's timings, same machine, same
//                        fixture, different executable. Used as reference
//                        constants and always named as such.
//   MODEL_ONLY           the historical 57.4625M blocked-bitmap work reference,
//                        a logical-work model over different primitive classes,
//                        NOT calibrated to nanoseconds here.

#include "ter/tiled_q3report.hpp"
#include "ter_microreport8_dataset.hpp"
#include "ter_microreport8_kernels.hpp"
#include "ter_windowing_geometry.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#ifndef TER_CONFIGURE_TIME_COMMIT
#define TER_CONFIGURE_TIME_COMMIT "unknown"
#endif
#ifndef TER_CONFIGURE_TIME_DIRTY
#define TER_CONFIGURE_TIME_DIRTY 1
#endif

#define TER_STRINGIFY_IMPL(value) #value
#define TER_STRINGIFY(value) TER_STRINGIFY_IMPL(value)

namespace ter::windowing {

using namespace ter::microreport8;

namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] double elapsed_ns(const Clock::time_point start) noexcept {
  return std::chrono::duration<double, std::nano>(Clock::now() - start).count();
}

volatile std::uint64_t g_sink = 0U;

// ---------------------------------------------------------------------------
// PRIOR_MEASURED constants from the MicroReport8 run (same machine, same
// authentic root, same fixture; a different executable). They are reference
// points, never re-derived here and never presented as this run's measurements.
// ---------------------------------------------------------------------------
constexpr double kPriorScalarPairNs = 0.6605;         // ns per exact pair probe
constexpr double kPriorTotalMs = 410.175;             // P0 * t_pair
constexpr double kPriorK2TotalMs = 138.524;           // previous best front end
constexpr double kPriorK2Speedup = 2.961;
constexpr double kPriorK3TileNs = 2.900;              // cheapest packed tile cost
constexpr double kPriorK1TileNs = 15.471;             // exact tile cost
constexpr std::uint64_t kPriorTileCount = 9890364ULL;
constexpr std::uint64_t kPriorPairUniverse = 620966064ULL;
constexpr double kBlockedBitmapModelWork = 57.4625e6;  // MODEL_ONLY

// Window-size classes. Index i covers slots in (kThresholds[i-1], kThresholds[i]].
constexpr std::array<std::uint32_t, 17> kThresholds{
    8U,    16U,    32U,    64U,     128U,    256U,    512U,    1024U,   2048U,
    4096U, 8192U,  16384U, 32768U,  65536U,  131072U, 262144U, 0xFFFFFFFFU};
constexpr std::size_t kClassCount = kThresholds.size();

[[nodiscard]] std::size_t window_class(const std::uint32_t slots) noexcept {
  for (std::size_t index = 0U; index < kClassCount; ++index) {
    if (slots <= kThresholds[index]) {
      return index;
    }
  }
  return kClassCount - 1U;
}

[[nodiscard]] std::string class_label(const std::size_t index) {
  if (index + 1U == kClassCount) {
    return ">262144";
  }
  return "<=" + std::to_string(kThresholds[index]);
}

constexpr std::array<std::uint32_t, 7> kReuseWidths{64U, 128U, 256U, 512U, 1024U, 2048U, 4096U};

// ---------------------------------------------------------------------------
// Exact histogram over a bounded integer range. Quantiles are computed from the
// full distribution, never from a stored sample, so nothing about the geometry
// result is subsampled.
// ---------------------------------------------------------------------------
struct Histogram {
  std::vector<std::uint64_t> counts;
  std::uint64_t total = 0U;
  std::uint64_t minimum = 0U;
  std::uint64_t maximum = 0U;
  long double sum = 0.0L;
  bool empty = true;

  explicit Histogram(const std::size_t range) : counts(range, 0U) {}

  void add(const std::uint32_t value, const std::uint64_t weight = 1U) {
    counts[value] += weight;
    total += weight;
    sum += static_cast<long double>(value) * static_cast<long double>(weight);
    if (empty) {
      minimum = value;
      maximum = value;
      empty = false;
    } else {
      minimum = std::min<std::uint64_t>(minimum, value);
      maximum = std::max<std::uint64_t>(maximum, value);
    }
  }

  [[nodiscard]] double mean() const {
    return total == 0U ? 0.0 : static_cast<double>(sum / static_cast<long double>(total));
  }

  [[nodiscard]] double quantile(const double fraction) const {
    if (total == 0U) {
      return 0.0;
    }
    const auto want = static_cast<std::uint64_t>(
        std::max(1.0, std::min(static_cast<double>(total),
                               std::ceil(fraction * static_cast<double>(total)))));
    std::uint64_t running = 0U;
    for (std::size_t value = 0U; value < counts.size(); ++value) {
      running += counts[value];
      if (running >= want) {
        return static_cast<double>(value);
      }
    }
    return static_cast<double>(counts.size() - 1U);
  }
};

// ---------------------------------------------------------------------------
// Small distinct-set stamp used for counting unique aligned window identities
// without allocating a hash table per configuration.
// ---------------------------------------------------------------------------
struct BitSet {
  std::vector<std::uint64_t> words;
  std::uint64_t population = 0U;

  explicit BitSet(const std::size_t bits) : words((bits + 63U) / 64U, 0U) {}

  bool insert(const std::size_t index) {
    const std::uint64_t bit = std::uint64_t{1} << (index & 63U);
    std::uint64_t& word = words[index >> 6U];
    if ((word & bit) != 0U) {
      return false;
    }
    word |= bit;
    ++population;
    return true;
  }

  void clear() {
    std::fill(words.begin(), words.end(), 0U);
    population = 0U;
  }
};

[[nodiscard]] std::string json_escape(const std::string_view value) {
  std::string out;
  out.reserve(value.size() + 8U);
  for (const char character : value) {
    if (character == '"') {
      out += "\\\"";
    } else if (character == '\\') {
      out += "\\\\";
    } else if (static_cast<unsigned char>(character) < 0x20U) {
      out += ' ';
    } else {
      out += character;
    }
  }
  return out;
}

[[nodiscard]] std::string number(const double value) {
  std::ostringstream stream;
  stream << std::setprecision(10) << value;
  std::string text = stream.str();
  if (text == "inf" || text == "-inf" || text == "nan" || text == "-nan") {
    return "null";
  }
  return text;
}

class SplitMix64 {
 public:
  explicit SplitMix64(const std::uint64_t seed) noexcept : state_(seed) {}
  [[nodiscard]] std::uint64_t next() noexcept {
    std::uint64_t value = (state_ += 0x9e3779b97f4a7c15ULL);
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
  }

 private:
  std::uint64_t state_;
};

// Reservoir size for the per-configuration rectangle timing sample. Large
// enough to spill out of L1/L2 so the measured cost is not a best case, small
// enough that 19 configurations fit in memory alongside the histograms.
constexpr std::size_t kReservoirCap = 65536U;

// Compiler and standard-library identity, so a timing figure can never be
// compared across builds without the difference being visible.
#if defined(_MSC_VER)
constexpr const char* kBuildIdentity = "MSVC " TER_STRINGIFY(_MSC_FULL_VER) ", C++20";
#elif defined(__clang__)
constexpr const char* kBuildIdentity = "clang " __VERSION__ ", C++20";
#elif defined(__GNUC__)
constexpr const char* kBuildIdentity = "gcc " __VERSION__ ", C++20";
#else
constexpr const char* kBuildIdentity = "unknown compiler, C++20";
#endif

struct Stats {
  double median = 0.0;
  double p25 = 0.0;
  double p75 = 0.0;
  double iqr = 0.0;
  double minimum = 0.0;
  double mean = 0.0;
  double maximum = 0.0;
  double p90 = 0.0;
  double p95 = 0.0;
  double p99 = 0.0;
  std::size_t samples = 0U;
};

[[nodiscard]] double sample_quantile(const std::vector<double>& sorted, const double fraction) {
  if (sorted.empty()) {
    return 0.0;
  }
  const double position = fraction * static_cast<double>(sorted.size() - 1U);
  const auto lower = static_cast<std::size_t>(position);
  const std::size_t upper = lower + 1U < sorted.size() ? lower + 1U : lower;
  const double weight = position - static_cast<double>(lower);
  return sorted[lower] * (1.0 - weight) + sorted[upper] * weight;
}

[[nodiscard]] Stats summarize_samples(std::vector<double> samples) {
  Stats stats;
  if (samples.empty()) {
    return stats;
  }
  std::sort(samples.begin(), samples.end());
  stats.samples = samples.size();
  stats.minimum = samples.front();
  stats.maximum = samples.back();
  long double sum = 0.0L;
  for (const double sample : samples) sum += sample;
  stats.mean = static_cast<double>(sum / samples.size());
  stats.p90 = sample_quantile(samples, 0.90);
  stats.p95 = sample_quantile(samples, 0.95);
  stats.p99 = sample_quantile(samples, 0.99);
  stats.p25 = sample_quantile(samples, 0.25);
  stats.median = sample_quantile(samples, 0.50);
  stats.p75 = sample_quantile(samples, 0.75);
  stats.iqr = stats.p75 - stats.p25;
  return stats;
}

// ---------------------------------------------------------------------------
// Phase 1 accumulators.
// ---------------------------------------------------------------------------
struct WindowGeometry {
  std::uint64_t tiles = 0U;
  std::uint64_t full_tiles = 0U;
  std::uint64_t partial_tiles = 0U;
  std::uint64_t valid_lanes = 0U;
  std::uint64_t true_lanes = 0U;
  std::uint64_t true_postings = 0U;
  std::uint64_t routing_disagreements = 0U;
  std::uint64_t retile_true_lane_checks = 0U;
  std::uint64_t retile_false_negatives = 0U;

  // Contract counters. Any nonzero value invalidates the run.
  std::uint64_t normalization_violations = 0U;
  std::uint64_t box_underestimates = 0U;
  std::uint64_t empty_windows = 0U;

  Histogram slots_all{kModulus + 2U};
  Histogram slots_full{kModulus + 2U};
  Histogram slots_partial{kModulus + 2U};
  Histogram box_all{2U * kModulus + 2U};
  Histogram box_full{kModulus + 2U};
  Histogram box_partial{kModulus + 2U};
  std::vector<double> ratios; // exhaustive uncensored ratios, diagnostic only
  std::uint64_t ratio_exact = 0U;

  // Lane mass per window-size class, so a 1-lane partial tile is never counted
  // as equivalent work to a full 64-lane tile.
  std::array<std::uint64_t, kClassCount> class_tiles{};
  std::array<std::uint64_t, kClassCount> class_lanes{};
  std::array<std::uint64_t, kClassCount> class_true_lanes{};
  std::array<long double, kClassCount> class_density_sum{};
  std::array<Histogram, kClassCount> class_y_distinct{
      Histogram{1024U}, Histogram{1024U}, Histogram{1024U}, Histogram{1024U}, Histogram{1024U},
      Histogram{1024U}, Histogram{1024U}, Histogram{1024U}, Histogram{1024U}, Histogram{1024U},
      Histogram{1024U}, Histogram{1024U}, Histogram{1024U}, Histogram{1024U}, Histogram{1024U},
      Histogram{1024U}, Histogram{1024U}};
  std::array<std::uint64_t, kClassCount> class_y_postings{};
  std::vector<Histogram> class_y_posting_hist{kClassCount, Histogram{33506U}};

  // Memory locality of the existing exact 2^19-bit Y bitmap, over each tile's
  // 64 sums: distinct bits, distinct 32-bit words, distinct 64-byte lines,
  // distinct 4-KiB pages.
  std::array<std::uint64_t, kTileLanes + 1U> touch_bits{};
  std::array<std::uint64_t, kTileLanes + 1U> touch_words{};
  std::array<std::uint64_t, kTileLanes + 1U> touch_lines{};
  std::array<std::uint64_t, kTileLanes + 1U> touch_pages{};
  std::array<std::uint64_t, kTileLanes + 1U> physical_lines{};
  std::array<std::uint64_t, kTileLanes + 1U> physical_pages{};
  std::uint64_t wrap_tiles = 0U;

  // Window reuse between consecutive tiles of one (s, t, u) packet.
  std::uint64_t consecutive_pairs = 0U;
  std::uint64_t same_start = 0U;
  long double overlap_sum = 0.0L;
  std::array<std::uint64_t, kReuseWidths.size()> same_region{};
  std::array<std::uint64_t, kReuseWidths.size()> fits_width_tiles{};
  std::array<std::uint64_t, kReuseWidths.size()> fits_width_lanes{};
  std::array<std::uint64_t, kReuseWidths.size()> fits_aligned_tiles{};
  std::array<std::uint64_t, kReuseWidths.size()> fits_aligned_lanes{};
  std::array<std::uint64_t, kReuseWidths.size()> unique_aligned{};
  std::array<std::uint64_t, kReuseWidths.size()> unique_start_regions{};
};

// ---------------------------------------------------------------------------
// Phase 2: value-retiling configuration result.
// ---------------------------------------------------------------------------
struct TilingResult {
  std::uint32_t bx = 0U;
  std::uint32_t bq = 0U;
  std::uint64_t occupied_x_bins = 0U;
  std::uint64_t occupied_q_bins = 0U;
  std::uint64_t rectangles = 0U;
  std::uint64_t rectangles_rejected = 0U;
  std::uint64_t rectangles_surviving = 0U;
  std::uint64_t mass_rejected = 0U;
  std::uint64_t mass_surviving = 0U;
  std::uint64_t tiles = 0U;
  std::uint64_t full_tiles = 0U;
  std::uint64_t partial_tiles = 0U;
  std::uint64_t surviving_lanes_padded = 0U;
  std::uint64_t unique_exact_windows = 0U;
  std::uint64_t unique_aligned_windows = 0U;
  std::array<std::uint64_t, kClassCount> class_rectangles{};
  std::array<std::uint64_t, kClassCount> class_tiles{};
  std::array<std::uint64_t, kClassCount> class_lanes{};
  double window_slots_mean = 0.0;
  long double window_slots_sum = 0.0L;
  std::uint32_t window_slots_max = 0U;

  // Uniform reservoir over this configuration's rectangles, used to measure the
  // cost of the rejection test itself rather than assume it.
  std::vector<RectProbe> reservoir;
  std::vector<std::uint64_t> reservoir_ordinals;
  std::uint64_t reservoir_seen = 0U;
  Stats rect_ns;

  // Routing-inclusive projections. A rectangle test is real work: charging only
  // the surviving tiles would credit the architecture with rejecting pairs it
  // had to examine one at a time to reject.
  double routing_ms = 0.0;       // N_rect * measured t_rect
  double leaf_floor_ms = 0.0;    // N_tiles * measured-K3 tile cost
  double ceiling_measured = 0.0; // speedup vs scalar, measured t_rect
  double ceiling_floor = 0.0;    // hypothetical t_rect = t_pair, NOT a lower bound
  double mixed_cost_class_ms = 0.0; // measured routing, tiny regions at K3 cost,
                                    // larger regions at prior K2 tile cost; no verification

  [[nodiscard]] double pairs_per_rectangle() const {
    return rectangles == 0U ? 0.0
                            : static_cast<double>(kPriorPairUniverse) /
                                  static_cast<double>(rectangles);
  }

  [[nodiscard]] double fragmentation() const {
    return mass_surviving == 0U
               ? 0.0
               : static_cast<double>(tiles * kTileLanes) / static_cast<double>(mass_surviving);
  }
  [[nodiscard]] double budget_ns(const double speedup) const {
    return tiles == 0U ? 0.0
                       : (kPriorTotalMs * 1.0e6) / (speedup * static_cast<double>(tiles));
  }
  [[nodiscard]] double k3_floor_ms() const {
    return static_cast<double>(tiles) * kPriorK3TileNs / 1.0e6;
  }
  [[nodiscard]] double k3_floor_speedup() const {
    const double total = k3_floor_ms();
    return total > 0.0 ? kPriorTotalMs / total : 0.0;
  }
};

// ---------------------------------------------------------------------------
// Timed work units.
//
// Both drivers keep several independent accumulator chains so the measurement
// is of the work itself and not of a serialized reduction. Folding once per
// unit would put a rotate-multiply latency on the critical path and inflate the
// per-unit cost, which would flatter every ratio computed against it.
// ---------------------------------------------------------------------------
[[nodiscard]] std::uint64_t pair_probe_batch(const Tile* const tiles, const std::size_t count,
                                             const TargetView& view,
                                             const std::uint64_t checksum) {
  std::uint64_t column[kTileDim] = {0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U};
  for (std::size_t entry = 0U; entry < count; ++entry) {
    const Tile& tile = tiles[entry];
    const std::uint32_t* const exact = view.exact[tile.u];
    for (std::uint32_t i = 0U; i < tile.x_count; ++i) {
      for (std::uint32_t j = 0U; j < tile.q_count; ++j) {
        column[j] += pair_probe(tile.x[i], tile.q[j], tile.kappa, exact) ? 1U : 0U;
      }
    }
  }
  std::uint64_t total = 0U;
  for (const std::uint64_t value : column) {
    total += value;
  }
  return fold(checksum, total);
}

[[nodiscard]] std::uint64_t rect_probe_batch(const RectProbe* const probes,
                                             const std::size_t count, const TargetView& view,
                                             const std::uint64_t checksum) {
  std::uint64_t lane[4] = {0U, 0U, 0U, 0U};
  for (std::size_t entry = 0U; entry < count; ++entry) {
    const RectProbe& probe = probes[entry];
    lane[entry & 3U] +=
        range_nonempty(view.exact[probe.u], probe.start, probe.slots) ? 1U : 0U;
  }
  return fold(checksum, lane[0] + lane[1] + lane[2] + lane[3]);
}

[[nodiscard]] std::string budget_label(const double ns_per_tile) {
  if (ns_per_tile >= 4.0) {
    return "easy-ish";
  }
  if (ns_per_tile >= 2.0) {
    return "difficult-but-plausible";
  }
  if (ns_per_tile >= 1.0) {
    return "extremely-aggressive";
  }
  return "not-worth-implementing";
}

}  // namespace
}  // namespace ter::windowing

int main(const int argc, char** const argv) {
  using namespace ter;
  using namespace ter::microreport8;
  using namespace ter::windowing;

  std::uint32_t root_n = kReferenceRootN;
  std::uint64_t root_seed = kReferenceRootSeed;
  std::size_t sample_count = 51U;
  std::string output_directory;
  bool run_production_anchor = true;
  bool pin_to_one_core = true;

  for (int argument = 1; argument < argc; ++argument) {
    const std::string_view value(argv[argument]);
    if (value.starts_with("--root-n=")) {
      root_n = static_cast<std::uint32_t>(std::strtoul(argv[argument] + 9, nullptr, 10));
    } else if (value.starts_with("--root-seed=")) {
      root_seed = std::strtoull(argv[argument] + 12, nullptr, 10);
    } else if (value.starts_with("--samples=")) {
      sample_count = static_cast<std::size_t>(std::strtoull(argv[argument] + 10, nullptr, 10));
    } else if (value.starts_with("--out=")) {
      output_directory = argv[argument] + 6;
    } else if (value == "--no-production-anchor") {
      run_production_anchor = false;
    } else if (value == "--no-pin") {
      pin_to_one_core = false;
    } else {
      std::cerr << "usage: ter_windowing_probe [--root-n=N] [--root-seed=S] [--samples=N]\n"
                   "       [--out=DIR] [--no-production-anchor] [--no-pin]\n";
      return EXIT_FAILURE;
    }
  }
  if (sample_count < 51U) {
    std::cerr << "at least 51 timing samples are required\n";
    return EXIT_FAILURE;
  }
  if (!output_directory.empty()) {
    const std::filesystem::path destination(output_directory);
    if (std::filesystem::exists(destination) && !std::filesystem::is_empty(destination)) {
      std::cerr << "refusing to overwrite a nonempty results directory\n";
      return EXIT_FAILURE;
    }
  }

  const IsaReport isa = detect_isa();
  std::cout << "# ter_windowing_probe -- TER^2 residual windowing spike\n";
  std::cout << "# configure_time_commit=" << TER_CONFIGURE_TIME_COMMIT
            << " configure_time_dirty=" << (TER_CONFIGURE_TIME_DIRTY ? "yes" : "no") << '\n';
  std::cout << "# cpu=" << (isa.brand[0] != '\0' ? isa.brand : "unknown") << '\n';

  std::string affinity_state = "not attempted";
  if (pin_to_one_core) {
#if defined(_WIN32)
    const bool affinity =
        SetThreadAffinityMask(GetCurrentThread(), static_cast<DWORD_PTR>(1)) != 0;
    const bool priority = SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS) != 0;
    affinity_state = affinity ? (priority ? "cpu0 + high priority" : "cpu0, priority unchanged")
                              : "failed";
#else
    affinity_state = "unsupported on this platform";
#endif
  } else {
    affinity_state = "disabled by --no-pin";
  }
  std::cout << "# thread_affinity=" << affinity_state << '\n';
  std::cout.flush();

  // -------------------------------------------------------------------------
  // Fixture gate. The windowing result is only meaningful on the same authentic
  // root the MicroReport8 verdict was measured against, so every invariant that
  // run established is re-checked before any geometry is computed.
  // -------------------------------------------------------------------------
  std::cout << "# capturing authentic root (n=" << root_n << ", seed=" << root_seed << ")...\n";
  std::cout.flush();
  const auto capture_start = Clock::now();
  const auto data = build_dataset(root_n, root_seed, run_production_anchor);
  const double capture_ns = elapsed_ns(capture_start);
  if (!data->ok) {
    std::cerr << "FATAL: " << data->message << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "# |A|=" << data->a_size << " |B|=" << data->b_size << " |C|=" << data->c_size
            << " Q=" << data->quotient_bits << " r1=" << data->r1
            << " P0=" << data->pair_universe << " capture_ms=" << number(capture_ns / 1.0e6)
            << '\n';
  std::cout << "# tau_low32=" << data->tau_low32 << " tau_low25=" << data->tau_low25
            << " tau_derivation_verified=" << (data->tau_derivation_verified ? "true" : "false")
            << '\n';
  std::cout.flush();

  std::vector<std::string> fixture_failures;
  const auto require = [&](const bool condition, const std::string& detail) {
    if (!condition) {
      fixture_failures.push_back(detail);
    }
  };
  require(data->n == 72U, "n != 72");
  require(data->seed == 19797034556212562ULL, "seed mismatch");
  require(data->quotient_bits == 47U, "Q != 47");
  require(data->r1 == 25U, "r1 != 25");
  require(kProjectionBits == 25U, "k != 25");
  require(kResidualBits == 19U, "residual width != 19");
  require(data->a_size == 19109U, "|A| != 19109");
  require(data->b_size == 32496U, "|B| != 32496");
  require(data->c_size == 33505U, "|C| != 33505");
  require(data->pair_universe == kPriorPairUniverse, "P0 != 620966064");
  require(data->tau_low25 == 5005954U, "tau_low25 != 5005954");
  require(data->tau_derivation_verified, "tau derivation not verified");
  require(kSplitRadix * kSplitRadix == 4096U, "microproblem count != 4096");

  std::cout << "# running independent tiled_q3report_count cross-check...\n";
  std::cout.flush();
  TiledQ3ReportConfig tiled_config;
  tiled_config.preferred_tile_bits = limits::tiled::kDefaultPreferredTileBits;
  const auto crosscheck = tiled_q3report_count(data->a_low25, data->b_low25, data->c_low25,
                                               data->tau_low25,
                                               std::uint64_t{1} << kProjectionBits, tiled_config);
  if (crosscheck.status != TiledQ3ReportStatus::Completed) {
    std::cerr << "FATAL: cross-check reporter failed: "
              << tiled_q3report_status_name(crosscheck.status) << " " << crosscheck.message
              << '\n';
    return EXIT_FAILURE;
  }
  const std::uint64_t crosscheck_expected = crosscheck.telemetry.raw_triples;
  require(crosscheck_expected == 619579ULL, "tiled_q3report_count != 619579");
  require(data->production_anchor_available, "production low-25 anchor unavailable");
  if (data->production_anchor_available) {
    require(data->production_low_prefilter_candidates == 619579ULL,
            "production low-25 count != 619579");
  }
  std::cout << "# decomposition_crosscheck=" << crosscheck_expected
            << " production_low25=" << data->production_low_prefilter_candidates
            << " (" << data->production_anchor_status << ")\n";
  if (!fixture_failures.empty()) {
    std::cerr << "FATAL: the authenticated fixture invariants no longer hold:\n";
    for (const std::string& failure : fixture_failures) {
      std::cerr << "  - " << failure << '\n';
    }
    return EXIT_FAILURE;
  }
  std::cout << "# fixture gate: all authenticated invariants hold\n";
  std::cout.flush();

  // -------------------------------------------------------------------------
  // Phase 1 + Phase 2 share one pass over the 64 target families, so the
  // 2 MiB prefix index of Y_u is built exactly once per family rather than once
  // per configuration. Index construction is preprocessing and is reported
  // separately; it is never inside a timed kernel region.
  // -------------------------------------------------------------------------
  std::vector<std::uint32_t> bin_widths{16U,   32U,   64U,    128U,   256U,   512U,  1024U,
                                        2048U, 4096U, 8192U,  16384U, 32768U, 65536U};
  std::vector<std::pair<std::uint32_t, std::uint32_t>> configurations;
  configurations.reserve(bin_widths.size() + 8U);
  for (const std::uint32_t width : bin_widths) {
    configurations.emplace_back(width, width);
  }
  // Asymmetric probes. Q families are roughly 1.7x sparser than X families, so
  // an equal-count bin is wider on the Q side; these test whether matching the
  // bin widths to the densities helps.
  const std::array<std::pair<std::uint32_t, std::uint32_t>, 6> asymmetric{
      std::pair{4096U, 8192U},   std::pair{8192U, 16384U}, std::pair{16384U, 32768U},
      std::pair{8192U, 4096U},   std::pair{16384U, 8192U}, std::pair{32768U, 16384U}};
  for (const auto& entry : asymmetric) {
    configurations.push_back(entry);
  }

  WindowGeometry geometry;
  geometry.ratios.reserve(kPriorTileCount);
  for (auto& histogram : geometry.class_y_distinct) histogram.counts.resize(33506U, 0U);
  std::vector<TilingResult> tiling(configurations.size());
  for (std::size_t index = 0U; index < configurations.size(); ++index) {
    tiling[index].bx = configurations[index].first;
    tiling[index].bq = configurations[index].second;
  }

  std::vector<BitSet> reuse_seen;
  std::vector<BitSet> reuse_visited;
  reuse_seen.reserve(kReuseWidths.size());
  for (const std::uint32_t width : kReuseWidths) {
    reuse_seen.emplace_back(static_cast<std::size_t>(kSplitRadix) * (kModulus / width));
    reuse_visited.emplace_back(static_cast<std::size_t>(kSplitRadix) * (kModulus / width));
  }
  std::vector<std::vector<std::uint64_t>> exact_window_seen;
  std::vector<BitSet> aligned_window_seen;
  exact_window_seen.reserve(configurations.size());
  aligned_window_seen.reserve(configurations.size());
  for (const auto& configuration : configurations) {
    // (kappa, window_start) for the tight window; (kappa, bin-sum) for the
    // aligned window a shared local bitmap would actually cover.
    exact_window_seen.emplace_back();
    const std::size_t bins = kModulus / std::min(configuration.first, configuration.second);
    aligned_window_seen.emplace_back(2U * 2U * bins);
  }

  std::cout << "# Phase 1 + Phase 2: sweeping all authentic tiles and "
            << configurations.size() << " value-tiling configurations...\n";
  std::cout.flush();
  const auto sweep_start = Clock::now();
  double index_build_ns = 0.0;

  std::vector<std::uint32_t> y_values;
  FamilyIndex index;
  std::uint64_t rectangle_scan_total = 0U;
  SplitMix64 reservoir_rng(0x9E3779B97F4A7C15ULL);

  for (std::uint32_t u = 0U; u < kSplitRadix; ++u) {
    y_values.clear();
    y_values.reserve(data->y_family[u].size());
    for (const Posting& posting : data->y_family[u]) {
      y_values.push_back(posting.value);
    }
    const auto index_start = Clock::now();
    index.build(y_values.data(), y_values.size());
    index_build_ns += elapsed_ns(index_start);

    for (std::size_t configuration = 0U; configuration < configurations.size(); ++configuration) {
      exact_window_seen[configuration].clear();
      aligned_window_seen[configuration].clear();
    }

    for (std::uint32_t s = 0U; s < kSplitRadix; ++s) {
      const MicroproblemKey key = microproblem_for(u, s);
      const std::vector<Posting>& xs = data->x_family[key.t];
      const std::vector<Posting>& qs = data->q_family[key.s];
      if (xs.empty() || qs.empty()) {
        continue;
      }

      // ---------------------------------------------------------------------
      // Phase 1: every rank tile of this microproblem.
      // ---------------------------------------------------------------------
      const std::size_t x_blocks = block_count(xs.size());
      const std::size_t q_blocks = block_count(qs.size());
      Tile tile{};
      Window previous{};
      bool have_previous = false;
      for (std::size_t x_block = 0U; x_block < x_blocks; ++x_block) {
        for (std::size_t q_block = 0U; q_block < q_blocks; ++q_block) {
          make_tile(*data, key, x_block, q_block, tile);
          const std::uint64_t valid = tile_valid_mask(tile);
          const auto lanes = static_cast<std::uint64_t>(std::popcount(valid));
          const bool full = tile.x_count == kTileDim && tile.q_count == kTileDim;
          ++geometry.tiles;
          geometry.valid_lanes += lanes;
          if (full) {
            ++geometry.full_tiles;
          } else {
            ++geometry.partial_tiles;
          }

          std::uint32_t sums[kTileLanes];
          const std::size_t total = tile_sorted_sums(tile, sums);
          const std::size_t distinct = unique_in_place(sums, total);
          const Window window = minimal_circular_window(sums, distinct);
          if (!window.valid) {
            ++geometry.empty_windows;
            continue;
          }

          // Contract: every valid sum must normalize into [0, slots).
          for (std::size_t entry = 0U; entry < distinct; ++entry) {
            if (normalize(sums[entry], window.start) >= window.slots) {
              ++geometry.normalization_violations;
            }
          }

          const Window box = tile_source_box_window(tile);
          for (std::size_t entry = 0U; entry < distinct; ++entry) {
            if (normalize(sums[entry], box.start) > box.span) {
              ++geometry.box_underestimates;
            }
          }
          if (box.slots < window.slots) {
            ++geometry.box_underestimates;
          }

          geometry.slots_all.add(window.slots);
          geometry.wrap_tiles += window.wraps ? 1U : 0U;
          if (full) {
            geometry.slots_full.add(window.slots);
            geometry.box_full.add(box.slots);
          } else {
            geometry.slots_partial.add(window.slots);
            geometry.box_partial.add(box.slots);
          }
          geometry.box_all.add(std::min<std::uint32_t>(box.slots, 2U * kModulus));

          const double ratio =
              static_cast<double>(box.slots) / static_cast<double>(window.slots);
          if (box.slots == window.slots) {
            ++geometry.ratio_exact;
          }
          geometry.ratios.push_back(ratio);

          // Distinct bitmap footprint over the tile's sums, from one sort.
          std::uint32_t bits = 1U;
          std::uint32_t words = 1U;
          std::uint32_t lines = 1U;
          std::uint32_t pages = 1U;
          for (std::size_t entry = 1U; entry < distinct; ++entry) {
            ++bits;
            words += (sums[entry] >> 5U) != (sums[entry - 1U] >> 5U) ? 1U : 0U;
            lines += (sums[entry] >> 9U) != (sums[entry - 1U] >> 9U) ? 1U : 0U;
            pages += (sums[entry] >> 15U) != (sums[entry - 1U] >> 15U) ? 1U : 0U;
          }
          ++geometry.touch_bits[bits];
          ++geometry.touch_words[words];
          ++geometry.touch_lines[lines];
          ++geometry.touch_pages[pages];
          // Actual allocation alignment affects cache lines/pages. Retain the
          // historical zero-offset layout histogram separately for comparison.
          const auto base = reinterpret_cast<std::uintptr_t>(data->view.exact[tile.u]);
          std::uint32_t actual_lines = 1U, actual_pages = 1U;
          for (std::size_t entry = 1U; entry < distinct; ++entry) {
            const auto current = base + (sums[entry] >> 5U) * sizeof(std::uint32_t);
            const auto prior = base + (sums[entry - 1U] >> 5U) * sizeof(std::uint32_t);
            actual_lines += current / 64U != prior / 64U ? 1U : 0U;
            actual_pages += current / 4096U != prior / 4096U ? 1U : 0U;
          }
          ++geometry.physical_lines[actual_lines];
          ++geometry.physical_pages[actual_pages];

          const std::uint32_t y_distinct = index.distinct_in(window);
          const std::uint32_t y_postings = index.postings_in(window);
          const auto true_lanes =
              static_cast<std::uint64_t>(std::popcount(scalar_report8(tile, data->view)));
          geometry.true_lanes += true_lanes;
          auto true_mask = scalar_report8(tile, data->view);
          while (true_mask != 0U) {
            const auto lane = static_cast<std::uint32_t>(std::countr_zero(true_mask));
            const auto sum = (tile.x[lane / 8U] + tile.q[lane % 8U] + tile.kappa) & kResidualMask;
            geometry.true_postings += index.posting_range(sum, sum + 1U);
            // Check every authentic true lane in every retiling configuration,
            // independently locating its source-bin boundaries from raw postings.
            for (const auto& config : configurations) {
              const auto x_bin_start = (tile.x[lane / 8U] / config.first) * config.first;
              const auto q_bin_start = (tile.q[lane % 8U] / config.second) * config.second;
              const auto less_value = [](const Posting& p, const std::uint32_t v) { return p.value < v; };
              const auto xl = std::lower_bound(xs.begin(), xs.end(), x_bin_start, less_value);
              const auto xh = std::lower_bound(xs.begin(), xs.end(), x_bin_start + config.first, less_value);
              const auto ql = std::lower_bound(qs.begin(), qs.end(), q_bin_start, less_value);
              const auto qh = std::lower_bound(qs.begin(), qs.end(), q_bin_start + config.second, less_value);
              const Window region = bin_sum_window(xl->value, (xh - 1)->value,
                                                    ql->value, (qh - 1)->value, key.kappa);
              ++geometry.retile_true_lane_checks;
              if (!window_contains(region, sum) || index.distinct_in(region) == 0U)
                ++geometry.retile_false_negatives;
            }
            true_mask &= true_mask - 1U;
          }

          const std::size_t klass = window_class(window.slots);
          ++geometry.class_tiles[klass];
          geometry.class_lanes[klass] += lanes;
          geometry.class_true_lanes[klass] += true_lanes;
          geometry.class_density_sum[klass] +=
              static_cast<long double>(y_distinct) / static_cast<long double>(window.slots);
          geometry.class_y_distinct[klass].add(y_distinct);
          geometry.class_y_posting_hist[klass].add(y_postings);
          geometry.class_y_postings[klass] += y_postings;

          for (std::size_t width = 0U; width < kReuseWidths.size(); ++width) {
            const std::uint32_t length = kReuseWidths[width];
            if (reuse_visited[width].insert(static_cast<std::size_t>(u) * (kModulus / length) +
                                            window.start / length)) {
              ++geometry.unique_start_regions[width];
            }
            if (window.slots <= length) {
              ++geometry.fits_width_tiles[width];
              geometry.fits_width_lanes[width] += lanes;
              const std::uint32_t region = window.start / length;
              const bool aligned_fit =
                  !window.wraps && (window.end / length) == region;
              if (aligned_fit) {
                ++geometry.fits_aligned_tiles[width];
                geometry.fits_aligned_lanes[width] += lanes;
                if (reuse_seen[width].insert(static_cast<std::size_t>(u) * (kModulus / length) +
                                             region)) {
                  ++geometry.unique_aligned[width];
                }
              }
            }
          }

          if (have_previous) {
            ++geometry.consecutive_pairs;
            if (previous.start == window.start) {
              ++geometry.same_start;
            }
            const std::uint32_t intersection = window_intersection_slots(previous, window);
            const std::uint64_t union_slots =
                static_cast<std::uint64_t>(previous.slots) + window.slots - intersection;
            geometry.overlap_sum += union_slots == 0U
                                        ? 0.0L
                                        : static_cast<long double>(intersection) /
                                              static_cast<long double>(union_slots);
            for (std::size_t width = 0U; width < kReuseWidths.size(); ++width) {
              const std::uint32_t length = kReuseWidths[width];
              if (previous.start / length == window.start / length) {
                ++geometry.same_region[width];
              }
            }
          }
          previous = window;
          have_previous = true;
        }
      }

      // ---------------------------------------------------------------------
      // Phase 2: value-based retiling of this microproblem.
      //
      // Bins are half-open numeric ranges [b*B, (b+1)*B) of the residual
      // universe, so a bin never wraps. Within a bin the tight sum interval is
      // [xlo + qlo + kappa, xhi + qhi + kappa], which is exact and may wrap; a
      // rectangle is rejectable only when Y_u has no residue in that interval,
      // which is an exact test and therefore cannot lose a true pair.
      // ---------------------------------------------------------------------
      for (std::size_t configuration = 0U; configuration < configurations.size();
           ++configuration) {
        TilingResult& result = tiling[configuration];
        const std::uint32_t bx = result.bx;
        const std::uint32_t bq = result.bq;
        const std::uint32_t aligned_stride = std::min(bx, bq);

        // Walk the two sorted families as runs of equal bin id.
        std::size_t x_begin = 0U;
        while (x_begin < xs.size()) {
          const std::uint32_t x_bin = xs[x_begin].value / bx;
          std::size_t x_end = x_begin;
          while (x_end < xs.size() && xs[x_end].value / bx == x_bin) {
            ++x_end;
          }
          const auto x_count = static_cast<std::uint32_t>(x_end - x_begin);
          const std::uint32_t x_lo = xs[x_begin].value;
          const std::uint32_t x_hi = xs[x_end - 1U].value;
          ++result.occupied_x_bins;
          // The Q runs are re-walked for every X bin, so occupied Q bins are
          // counted only on the first pass or they would be multiplied by the
          // number of occupied X bins.
          const bool first_x_bin = x_begin == 0U;

          std::size_t q_begin = 0U;
          while (q_begin < qs.size()) {
            const std::uint32_t q_bin = qs[q_begin].value / bq;
            std::size_t q_end = q_begin;
            while (q_end < qs.size() && qs[q_end].value / bq == q_bin) {
              ++q_end;
            }
            const auto q_count = static_cast<std::uint32_t>(q_end - q_begin);
            const std::uint32_t q_lo = qs[q_begin].value;
            const std::uint32_t q_hi = qs[q_end - 1U].value;
            if (first_x_bin) {
              ++result.occupied_q_bins;
            }

            ++result.rectangles;
            ++rectangle_scan_total;
            const std::uint64_t mass =
                static_cast<std::uint64_t>(x_count) * static_cast<std::uint64_t>(q_count);
            const Window sum_window = bin_sum_window(x_lo, x_hi, q_lo, q_hi, key.kappa);

            // Uniform reservoir (Algorithm R) over every rectangle this
            // configuration issues, so the timed rejection cost describes the
            // whole population rather than a prefix of it.
            const RectProbe probe{sum_window.start, sum_window.slots, u};
            if (result.reservoir.size() < kReservoirCap) {
              result.reservoir.push_back(probe);
              result.reservoir_ordinals.push_back(result.reservoir_seen);
            } else {
              const std::uint64_t slot = reservoir_rng.next() % (result.reservoir_seen + 1U);
              if (slot < kReservoirCap) {
                result.reservoir[static_cast<std::size_t>(slot)] = probe;
                result.reservoir_ordinals[static_cast<std::size_t>(slot)] = result.reservoir_seen;
              }
            }
            ++result.reservoir_seen;

            const bool retained = index.distinct_in(sum_window) != 0U;
            geometry.routing_disagreements +=
                range_nonempty(data->view.exact[u], sum_window.start, sum_window.slots) != retained
                    ? 1U : 0U;
            if (!retained) {
              ++result.rectangles_rejected;
              result.mass_rejected += mass;
            } else {
              ++result.rectangles_surviving;
              result.mass_surviving += mass;
              const std::uint64_t x_tiles = (x_count + kTileDim - 1U) / kTileDim;
              const std::uint64_t q_tiles = (q_count + kTileDim - 1U) / kTileDim;
              const std::uint64_t full_x = x_count / kTileDim;
              const std::uint64_t full_q = q_count / kTileDim;
              result.tiles += x_tiles * q_tiles;
              result.full_tiles += full_x * full_q;
              result.partial_tiles += x_tiles * q_tiles - full_x * full_q;
              result.surviving_lanes_padded += x_tiles * q_tiles * kTileLanes;
              const std::size_t klass = window_class(sum_window.slots);
              ++result.class_rectangles[klass];
              result.class_tiles[klass] += x_tiles * q_tiles;
              result.class_lanes[klass] += mass;
              result.window_slots_sum +=
                  static_cast<long double>(sum_window.slots) *
                  static_cast<long double>(x_tiles * q_tiles);
              result.window_slots_max =
                  std::max(result.window_slots_max, sum_window.slots);
              exact_window_seen[configuration].push_back(window_identity(sum_window));
              const std::size_t aligned_id =
                  static_cast<std::size_t>(key.kappa) * (kModulus / aligned_stride) +
                  (((x_bin * bx + q_bin * bq) & kResidualMask) / aligned_stride);
              if (aligned_window_seen[configuration].insert(aligned_id)) {
                ++result.unique_aligned_windows;
              }
            }
            q_begin = q_end;
          }
          x_begin = x_end;
        }
      }
    }
    for (std::size_t configuration = 0U; configuration < tiling.size(); ++configuration) {
      auto& identities = exact_window_seen[configuration];
      std::sort(identities.begin(), identities.end());
      tiling[configuration].unique_exact_windows += static_cast<std::uint64_t>(
          std::unique(identities.begin(), identities.end()) - identities.begin());
    }
    if ((u % 8U) == 7U) {
      std::cout << "#   families " << (u + 1U) << "/64 done ("
                << number(elapsed_ns(sweep_start) / 1.0e9) << " s)\n";
      std::cout.flush();
    }
  }
  const double sweep_ns = elapsed_ns(sweep_start);
  std::cout << "# sweep complete in " << number(sweep_ns / 1.0e9) << " s (index build "
            << number(index_build_ns / 1.0e6) << " ms, " << rectangle_scan_total
            << " rectangle tests)\n";
  std::cout.flush();

  // -------------------------------------------------------------------------
  // Correctness gates on the geometry pass itself.
  // -------------------------------------------------------------------------
  bool geometry_ok = true;
  const auto gate = [&](const bool condition, const std::string_view detail) {
    if (!condition) {
      geometry_ok = false;
      std::cerr << "GATE FAILURE: " << detail << '\n';
    }
  };
  gate(geometry.normalization_violations == 0U,
       "a valid sum did not normalize into [0, window_slots)");
  gate(geometry.box_underestimates == 0U, "the source-box bound underestimated a sum window");
  gate(geometry.empty_windows == 0U, "a tile produced an empty window");
  gate(geometry.tiles == kPriorTileCount, "tile count differs from the MicroReport8 sweep");
  gate(geometry.valid_lanes == kPriorPairUniverse, "valid lane mass != P0");
  gate(geometry.true_lanes == 619303ULL, "true lane count differs from MicroReport8");
  gate(geometry.true_postings == crosscheck_expected, "multiplicity-aware decomposition mismatch");
  gate(geometry.routing_disagreements == 0U, "bitmap routing disagrees with prefix authority");
  gate(geometry.retile_false_negatives == 0U, "an authentic true lane was rejected by retiling");
  gate(geometry.retile_true_lane_checks == geometry.true_lanes * configurations.size(),
       "not every authentic true lane was checked in every configuration");
  for (const TilingResult& result : tiling) {
    gate(result.mass_rejected + result.mass_surviving == kPriorPairUniverse,
         "value-tiling pair-mass conservation failed for Bx=" + std::to_string(result.bx));
  }
  if (!geometry_ok) {
    std::cerr << "FATAL: geometry gates failed; no verdict is issued.\n";
    return EXIT_FAILURE;
  }
  std::cout << "# geometry gates: all pass (pair mass conserved in every configuration)\n";
  const Stats ratio_stats = summarize_samples(std::move(geometry.ratios));

  for (TilingResult& result : tiling) {
    result.window_slots_mean =
        result.tiles == 0U
            ? 0.0
            : static_cast<double>(result.window_slots_sum / static_cast<long double>(result.tiles));
    // Preserve authentic traversal order in the uniform sample. Algorithm R's
    // storage slots are randomized; timing those directly destroys family reuse.
    std::vector<std::size_t> order(result.reservoir.size());
    for (std::size_t i = 0U; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](const auto a, const auto b) {
      return result.reservoir_ordinals[a] < result.reservoir_ordinals[b];
    });
    std::vector<RectProbe> ordered;
    ordered.reserve(order.size());
    for (const auto i : order) ordered.push_back(result.reservoir[i]);
    result.reservoir = std::move(ordered);
  }

  // -------------------------------------------------------------------------
  // Routing cost measurement.
  //
  // Section 16 requires that binning and routing be charged, not assumed free.
  // A value-retiled architecture has to answer one range-emptiness query per
  // source rectangle before it can skip anything, so that query is timed here
  // on real rectangles against the real Y bitmaps, in this executable, using
  // the same batched methodology as the MicroReport8 run. The scalar pair probe
  // is timed alongside it so the ratio is same-executable and so the prior
  // 0.6605 ns baseline can be checked rather than trusted.
  // -------------------------------------------------------------------------
  constexpr std::size_t kTilesPerMicroproblem = 16U;
  std::vector<Tile> tile_sample;
  tile_sample.reserve(kSplitRadix * kSplitRadix * kTilesPerMicroproblem);
  for (std::uint32_t u = 0U; u < kSplitRadix; ++u) {
    for (std::uint32_t s = 0U; s < kSplitRadix; ++s) {
      const MicroproblemKey key = microproblem_for(u, s);
      const std::size_t x_blocks = block_count(data->x_family[key.t].size());
      const std::size_t q_blocks = block_count(data->q_family[key.s].size());
      const std::size_t total = x_blocks * q_blocks;
      if (total == 0U) {
        continue;
      }
      const std::size_t take = std::min(total, kTilesPerMicroproblem);
      Tile tile{};
      for (std::size_t step = 0U; step < take; ++step) {
        const std::size_t position = step * total / take;
        make_tile(*data, key, position / q_blocks, position % q_blocks, tile);
        tile_sample.push_back(tile);
      }
    }
  }
  std::uint64_t sample_valid_lanes = 0U;
  for (const Tile& tile : tile_sample) {
    sample_valid_lanes += static_cast<std::uint64_t>(std::popcount(tile_valid_mask(tile)));
  }

  double clock_overhead_ns = 0.0;
  double clock_resolution_ns = 0.0;
  {
    constexpr std::size_t kProbes = 20000U;
    std::vector<double> gaps;
    gaps.reserve(kProbes);
    for (std::size_t probe = 0U; probe < kProbes; ++probe) {
      const auto start = Clock::now();
      gaps.push_back(elapsed_ns(start));
    }
    clock_overhead_ns = summarize_samples(gaps).median;
    for (const double gap : gaps) {
      if (gap > 0.0 && (clock_resolution_ns == 0.0 || gap < clock_resolution_ns)) {
        clock_resolution_ns = gap;
      }
    }
  }

  struct Timed {
    std::string id;
    const Tile* tiles = nullptr;
    const RectProbe* probes = nullptr;
    std::size_t count = 0U;
    double units = 0.0;
    std::size_t repeats = 1U;
    std::vector<double> samples;
  };
  std::vector<Timed> timed;
  timed.push_back(Timed{"scalar_local_pair", tile_sample.data(), nullptr, tile_sample.size(),
                        static_cast<double>(sample_valid_lanes), 1U, {}});
  for (const TilingResult& result : tiling) {
    if (result.reservoir.empty()) {
      continue;
    }
    timed.push_back(Timed{"rect_" + std::to_string(result.bx) + "_" + std::to_string(result.bq),
                          nullptr, result.reservoir.data(), result.reservoir.size(),
                          static_cast<double>(result.reservoir.size()), 1U, {}});
  }

  const auto run_batch = [&](const Timed& entry, std::uint64_t checksum) {
    for (std::size_t repeat = 0U; repeat < entry.repeats; ++repeat) {
      checksum = entry.tiles != nullptr
                     ? pair_probe_batch(entry.tiles, entry.count, data->view, checksum)
                     : rect_probe_batch(entry.probes, entry.count, data->view, checksum);
    }
    return checksum;
  };

  constexpr double kTargetRegionNs = 2.0e7;
  for (Timed& entry : timed) {
    const auto start = Clock::now();
    g_sink = run_batch(entry, g_sink);
    const double once = elapsed_ns(start);
    entry.repeats = once <= 0.0
                        ? 1U
                        : static_cast<std::size_t>(std::max(1.0, kTargetRegionNs / once));
    entry.samples.reserve(sample_count);
  }
  const double clock_guard_ns = std::max(clock_resolution_ns, clock_overhead_ns);
  if (kTargetRegionNs < 1000.0 * clock_guard_ns) {
    std::cerr << "FATAL: timing region is not large enough relative to the platform clock ("
              << number(clock_guard_ns) << " ns)\n";
    return EXIT_FAILURE;
  }

  std::cout << "# timing routing and baseline (" << sample_count
            << " samples, rotated order, clock resolution " << number(clock_resolution_ns)
            << " ns)...\n";
  std::cout.flush();
  for (std::size_t round = 0U; round < sample_count; ++round) {
    for (std::size_t offset = 0U; offset < timed.size(); ++offset) {
      Timed& entry = timed[(offset + round) % timed.size()];
      std::uint64_t checksum = g_sink;
      const auto start = Clock::now();
      checksum = run_batch(entry, checksum);
      const double region = elapsed_ns(start);
      g_sink = checksum;
      entry.samples.push_back(region / (entry.units * static_cast<double>(entry.repeats)));
    }
  }

  std::vector<Stats> timed_stats;
  timed_stats.reserve(timed.size());
  for (Timed& entry : timed) {
    timed_stats.push_back(summarize_samples(entry.samples));
  }
  const auto stats_for = [&](const std::string_view id) {
    for (std::size_t position = 0U; position < timed.size(); ++position) {
      if (timed[position].id == id) {
        return timed_stats[position];
      }
    }
    return Stats{};
  };

  const Stats pair_stats = stats_for("scalar_local_pair");
  const double t_pair = pair_stats.median;
  if (t_pair <= 0.0) {
    std::cerr << "FATAL: scalar pair baseline did not produce a positive median\n";
    return EXIT_FAILURE;
  }
  double worst_relative_iqr = pair_stats.iqr / pair_stats.median;
  for (TilingResult& result : tiling) {
    result.rect_ns = stats_for("rect_" + std::to_string(result.bx) + "_" +
                               std::to_string(result.bq));
    if (result.rect_ns.median > 0.0) {
      worst_relative_iqr = std::max(worst_relative_iqr, result.rect_ns.iqr / result.rect_ns.median);
    }
    result.routing_ms = static_cast<double>(result.rectangles) * result.rect_ns.median / 1.0e6;
    result.leaf_floor_ms = result.k3_floor_ms();
    const double measured_total = result.routing_ms + result.leaf_floor_ms;
    result.ceiling_measured = measured_total > 0.0 ? kPriorTotalMs / measured_total : 0.0;
    // Sensitivity scenario only: t_rect = t_pair is not a hardware lower bound.
    const double floor_total =
        static_cast<double>(result.rectangles) * t_pair / 1.0e6 + result.leaf_floor_ms;
    result.ceiling_floor = floor_total > 0.0 ? kPriorTotalMs / floor_total : 0.0;
    std::uint64_t tiny_tiles = 0U;
    for (std::size_t klass = 0U; klass <= 7U; ++klass) tiny_tiles += result.class_tiles[klass];
    result.mixed_cost_class_ms = result.routing_ms +
        (static_cast<double>(tiny_tiles) * kPriorK3TileNs +
         static_cast<double>(result.tiles - tiny_tiles) * 13.50479126) / 1.0e6;
  }
  const std::string timing_stability = worst_relative_iqr > 0.10 ? "poor" : "acceptable";
  const double baseline_agreement = t_pair / kPriorScalarPairNs;

  // Cost-class illustration only. K3 is unselective: this is not an exact
  // rank reporter and must never be a hurdle imposed on a selective candidate.
  const double rank_k3_ms = static_cast<double>(kPriorTileCount) * kPriorK3TileNs / 1.0e6;
  const double rank_k3_ceiling = kPriorTotalMs / rank_k3_ms;

  std::cout << "# t_pair(this executable)=" << number(t_pair) << " ns (prior "
            << number(kPriorScalarPairNs) << " ns, agreement "
            << number(baseline_agreement) << "), timing_stability=" << timing_stability
            << " worst_relative_iqr=" << number(worst_relative_iqr) << '\n';
  std::cout << "# rank-tile ceiling at the measured K3 cost class: " << number(rank_k3_ms)
            << " ms -> " << number(rank_k3_ceiling) << "x (routing-free, nothing to route)\n";
  std::cout.flush();

  // -------------------------------------------------------------------------
  // Phase 1 report.
  // -------------------------------------------------------------------------
  const auto lane_fraction = [&](const std::uint64_t lanes) {
    return static_cast<double>(lanes) / static_cast<double>(geometry.valid_lanes);
  };
  const auto tile_fraction = [&](const std::uint64_t tiles) {
    return static_cast<double>(tiles) / static_cast<double>(geometry.tiles);
  };

  std::array<std::uint64_t, kClassCount> cumulative_tiles{};
  std::array<std::uint64_t, kClassCount> cumulative_lanes{};
  {
    std::uint64_t tiles_running = 0U;
    std::uint64_t lanes_running = 0U;
    for (std::size_t index2 = 0U; index2 < kClassCount; ++index2) {
      tiles_running += geometry.class_tiles[index2];
      lanes_running += geometry.class_lanes[index2];
      cumulative_tiles[index2] = tiles_running;
      cumulative_lanes[index2] = lanes_running;
    }
  }

  std::cout << "\n=== CURRENT_RANK_TILE_WINDOW_GEOMETRY ===\n";
  std::cout << "tiles=" << geometry.tiles << " full=" << geometry.full_tiles
            << " partial=" << geometry.partial_tiles << " valid_lanes=" << geometry.valid_lanes
            << " true_lanes=" << geometry.true_lanes << '\n';
  std::cout << "\nwindow-size class          tiles      tile_frac        lanes      lane_frac"
               "   cum_tile_frac   cum_lane_frac\n";
  for (std::size_t index2 = 0U; index2 < kClassCount; ++index2) {
    std::cout << std::left << std::setw(20) << class_label(index2) << std::right
              << std::setw(12) << geometry.class_tiles[index2] << std::setw(15)
              << number(tile_fraction(geometry.class_tiles[index2])) << std::setw(14)
              << geometry.class_lanes[index2] << std::setw(15)
              << number(lane_fraction(geometry.class_lanes[index2])) << std::setw(16)
              << number(tile_fraction(cumulative_tiles[index2])) << std::setw(16)
              << number(lane_fraction(cumulative_lanes[index2])) << '\n';
  }

  const auto describe = [&](const std::string_view name, const Histogram& histogram) {
    std::cout << std::left << std::setw(22) << name << std::right << " min=" << histogram.minimum
              << " mean=" << number(histogram.mean())
              << " median=" << number(histogram.quantile(0.50))
              << " p75=" << number(histogram.quantile(0.75))
              << " p90=" << number(histogram.quantile(0.90))
              << " p95=" << number(histogram.quantile(0.95))
              << " p99=" << number(histogram.quantile(0.99)) << " max=" << histogram.maximum
              << '\n';
  };
  std::cout << "\nwindow quantiles (slots):\n";
  describe("exact_all", geometry.slots_all);
  describe("exact_full_tiles", geometry.slots_full);
  describe("exact_partial_tiles", geometry.slots_partial);
  describe("source_box_all", geometry.box_all);

  std::cout << "\nsource-box overestimate W_box/W_exact: mean="
            << number(ratio_stats.mean)
            << " median=" << number(ratio_stats.median)
            << " p90=" << number(ratio_stats.p90)
            << " p95=" << number(ratio_stats.p95)
            << " p99=" << number(ratio_stats.p99)
            << " max=" << number(ratio_stats.maximum)
            << " fraction_exact=" << number(tile_fraction(geometry.ratio_exact)) << '\n';

  std::cout << "\nY-in-window by window class:\n";
  std::cout << "class                  tiles    mean|Y|   median      p90      p95      p99"
               "      max   mean_density  true_lane_density\n";
  for (std::size_t index2 = 0U; index2 < kClassCount; ++index2) {
    const Histogram& histogram = geometry.class_y_distinct[index2];
    if (histogram.total == 0U) {
      continue;
    }
    const double density = static_cast<double>(
        geometry.class_density_sum[index2] / static_cast<long double>(histogram.total));
    const double true_density =
        geometry.class_lanes[index2] == 0U
            ? 0.0
            : static_cast<double>(geometry.class_true_lanes[index2]) /
                  static_cast<double>(geometry.class_lanes[index2]);
    std::cout << std::left << std::setw(16) << class_label(index2) << std::right << std::setw(12)
              << histogram.total << std::setw(11) << number(histogram.mean()) << std::setw(9)
              << number(histogram.quantile(0.50)) << std::setw(9)
              << number(histogram.quantile(0.90)) << std::setw(9)
              << number(histogram.quantile(0.95)) << std::setw(9)
              << number(histogram.quantile(0.99)) << std::setw(9) << histogram.maximum
              << std::setw(15) << number(density) << std::setw(19) << number(true_density)
              << '\n';
  }

  const auto touch_report = [&](const std::string_view name,
                                const std::array<std::uint64_t, kTileLanes + 1U>& histogram) {
    std::cout << std::left << std::setw(16) << name << std::right;
    std::uint64_t running = 0U;
    long double weighted = 0.0L;
    for (std::size_t bucket = 0U; bucket <= kTileLanes; ++bucket) {
      weighted += static_cast<long double>(bucket) * static_cast<long double>(histogram[bucket]);
    }
    for (const std::uint32_t threshold : {1U, 2U, 4U, 8U, 16U, 32U}) {
      running = 0U;
      for (std::size_t bucket = 0U; bucket <= threshold; ++bucket) {
        running += histogram[bucket];
      }
      std::cout << "  <=" << threshold << ":" << number(tile_fraction(running));
    }
    std::uint64_t above = 0U;
    for (std::size_t bucket = 33U; bucket <= kTileLanes; ++bucket) {
      above += histogram[bucket];
    }
    std::cout << "  >32:" << number(tile_fraction(above)) << "  mean="
              << number(static_cast<double>(weighted /
                                            static_cast<long double>(geometry.tiles)))
              << '\n';
  };
  std::cout << "\nexact 2^19-bit Y bitmap footprint per tile (fraction of tiles):\n";
  touch_report("distinct_bits", geometry.touch_bits);
  touch_report("distinct_words", geometry.touch_words);
  touch_report("distinct_lines", geometry.touch_lines);
  touch_report("distinct_pages", geometry.touch_pages);

  std::cout << "\nwindow reuse (consecutive tiles in one (s,t,u) packet: "
            << geometry.consecutive_pairs << " pairs)\n";
  std::cout << "same_window_start=" << number(static_cast<double>(geometry.same_start) /
                                              static_cast<double>(geometry.consecutive_pairs))
            << " mean_overlap="
            << number(static_cast<double>(geometry.overlap_sum /
                                          static_cast<long double>(geometry.consecutive_pairs)))
            << '\n';
  std::cout << "     L   fits_L_tiles   fits_L_lanes  aligned_tiles  aligned_lanes"
               "   unique_windows   tiles/window   same_region_rate\n";
  for (std::size_t width = 0U; width < kReuseWidths.size(); ++width) {
    const double per_window =
        geometry.unique_aligned[width] == 0U
            ? 0.0
            : static_cast<double>(geometry.fits_aligned_tiles[width]) /
                  static_cast<double>(geometry.unique_aligned[width]);
    std::cout << std::right << std::setw(6) << kReuseWidths[width] << std::setw(15)
              << number(tile_fraction(geometry.fits_width_tiles[width])) << std::setw(15)
              << number(lane_fraction(geometry.fits_width_lanes[width])) << std::setw(15)
              << number(tile_fraction(geometry.fits_aligned_tiles[width])) << std::setw(15)
              << number(lane_fraction(geometry.fits_aligned_lanes[width])) << std::setw(17)
              << geometry.unique_aligned[width] << std::setw(15) << number(per_window)
              << std::setw(19)
              << number(static_cast<double>(geometry.same_region[width]) /
                        static_cast<double>(geometry.consecutive_pairs))
              << '\n';
  }

  // -------------------------------------------------------------------------
  // Phase 2 report and gate.
  // -------------------------------------------------------------------------
  std::cout << "\n=== VALUE_RETILING_MODEL ===\n";
  std::cout << "leaf-only view (rectangle rejection charged at zero -- NOT an architecture):\n";
  std::cout << "   Bx      Bq   rectangles   rej_rect_frac   rej_mass_frac        tiles"
               "   tiles/old   fragmentation   ns@10x   ns@20x   ns@30x   K3floor_x\n";
  for (const TilingResult& result : tiling) {
    std::cout << std::right << std::setw(6) << result.bx << std::setw(8) << result.bq
              << std::setw(13) << result.rectangles << std::setw(16)
              << number(static_cast<double>(result.rectangles_rejected) /
                        static_cast<double>(result.rectangles))
              << std::setw(16)
              << number(static_cast<double>(result.mass_rejected) /
                        static_cast<double>(kPriorPairUniverse))
              << std::setw(13) << result.tiles << std::setw(12)
              << number(static_cast<double>(result.tiles) /
                        static_cast<double>(kPriorTileCount))
              << std::setw(16) << number(result.fragmentation()) << std::setw(9)
              << number(result.budget_ns(10.0)) << std::setw(9) << number(result.budget_ns(20.0))
              << std::setw(9) << number(result.budget_ns(30.0)) << std::setw(12)
              << number(result.k3_floor_speedup()) << '\n';
  }

  std::cout << "\noptimistic query-component projection (sparse warmed source-order sample; binning, traversal, reporter build and verification omitted):\n";
  std::cout << "   Bx      Bq   pairs/rect   t_rect_ns   routing_ms   leaf_ms   total_ms"
               "   ceiling_x   floor_ceiling_x   vs_rank_" << number(rank_k3_ceiling) << "x\n";
  for (const TilingResult& result : tiling) {
    const double total = result.routing_ms + result.leaf_floor_ms;
    std::cout << std::right << std::setw(6) << result.bx << std::setw(8) << result.bq
              << std::setw(13) << number(result.pairs_per_rectangle()) << std::setw(12)
              << number(result.rect_ns.median) << std::setw(13) << number(result.routing_ms)
              << std::setw(10) << number(result.leaf_floor_ms) << std::setw(11)
              << number(total) << std::setw(12) << number(result.ceiling_measured)
              << std::setw(18) << number(result.ceiling_floor) << std::setw(14)
              << (result.ceiling_floor > rank_k3_ceiling ? "BETTER" : "worse") << '\n';
  }

  // Rank the mixed cost-class projection: local regions at K3 cost, larger
  // regions at the existing K2 tile cost, plus measured rectangle routing.
  std::size_t best = 0U;
  for (std::size_t index2 = 1U; index2 < tiling.size(); ++index2) {
    if (tiling[index2].mixed_cost_class_ms < tiling[best].mixed_cost_class_ms) {
      best = index2;
    }
  }
  std::size_t fewest_tiles = 0U;
  for (std::size_t index2 = 1U; index2 < tiling.size(); ++index2) {
    if (tiling[index2].tiles < tiling[fewest_tiles].tiles) {
      fewest_tiles = index2;
    }
  }
  const TilingResult& winner = tiling[best];
  const TilingResult& smallest = tiling[fewest_tiles];

  // Research gate, not an impossibility proof. "Material" here is an explicit
  // 1.5x engineering heuristic relative to the REAL K2 result. A hypothetical
  // exact K3 on all rank tiles is not a valid comparator. The mixed estimate
  // deliberately gives bin construction, bitmap build and verification zero
  // cost; it can reject this measured design, but cannot establish a speedup.
  constexpr double kMaterialGain = 1.5;
  bool phase3_triggered = false;
  bool route_a = false;
  bool route_b = false;
  std::string gate_reason;
  {
    const double mass_in_1024 = lane_fraction(cumulative_lanes[7]);
    const double mass_in_4096 = lane_fraction(cumulative_lanes[9]);
    const double rank_mixed_ms =
        (static_cast<double>(cumulative_tiles[7]) * kPriorK3TileNs +
         static_cast<double>(geometry.tiles - cumulative_tiles[7]) * 13.50479126) / 1.0e6;
    route_a = rank_mixed_ms < kPriorK2TotalMs / kMaterialGain;
    route_b = winner.mixed_cost_class_ms < kPriorK2TotalMs / kMaterialGain;
    phase3_triggered = route_a || route_b;

    std::ostringstream reason;
    reason << "route_A (window-local leaf on the existing rank tiles): authentic lane mass "
              "with window <= 1024 slots = "
           << number(mass_in_1024) << ", <= 4096 slots = " << number(mass_in_4096)
           << "; median window = "
           << number(geometry.slots_all.quantile(0.50)) << " slots -> "
           << (route_a ? "ELIGIBLE" : "NOT ELIGIBLE")
           << ". route_B (value retiling): the configuration with the fewest tiles is Bx="
           << smallest.bx << " Bq=" << smallest.bq << " at " << smallest.tiles << " tiles ("
           << number(static_cast<double>(smallest.tiles) / static_cast<double>(kPriorTileCount))
           << "x rank), but it issues " << smallest.rectangles << " rectangle tests for "
           << kPriorPairUniverse << " source pairs -- " << number(smallest.pairs_per_rectangle())
           << " pairs per test. Charging the measured range-query implementation, tiny "
              "regions (<=1024 slots) at 2.9 ns/tile and larger regions at the prior K2 "
              "tile cost, the best optimistic mixed projection over "
           << tiling.size() << " configurations is Bx=" << winner.bx << " Bq=" << winner.bq
           << " is " << number(winner.mixed_cost_class_ms) << " ms, versus the real K2 "
           << kPriorK2TotalMs << " ms. The material-gain heuristic requires < "
           << number(kPriorK2TotalMs / kMaterialGain) << " ms -> "
           << (route_b ? "ELIGIBLE" : "NOT ELIGIBLE")
           << ". No universal routing lower bound or optimal-grouping theorem is claimed. "
              "Omitted construction/verification costs can only increase this modeled cost.";
    gate_reason = reason.str();
  }

  std::cout << "\n=== PHASE_3_GATE ===\n" << gate_reason << "\nphase3_triggered="
            << (phase3_triggered ? "true" : "false") << '\n';

  // Only a geometry verdict can be issued here. WINDOW_LOCALITY_VIABLE,
  // WINDOW_LOCALITY_PROMISING and BREAKTHROUGH all require an implemented,
  // measured, zero-false-negative reporter, so none of them can be awarded from
  // a model.
  const std::string verdict = route_b ? "VALUE_RETILING_REQUIRED"
                              : route_a ? "PHASE3_REQUIRED_WINDOW_LOCAL_LEAF"
                                        : "WINDOWING_GEOMETRY_UNPROMISING";
  std::cout << "\nPRIMARY VERDICT: " << verdict << '\n';

  // -------------------------------------------------------------------------
  // Output artifacts.
  // -------------------------------------------------------------------------
  if (!output_directory.empty()) {
    std::error_code error;
    std::filesystem::create_directories(output_directory, error);
    const std::filesystem::path root(output_directory);

    std::ofstream json(root / "window_geometry.json");
    json << std::setprecision(10);
    json << "{\"schema_version\":2"
         << ",\"experiment\":\"ter2_residual_windowing\""
         << ",\"configure_time_commit\":\"" << TER_CONFIGURE_TIME_COMMIT << "\""
         << ",\"configure_time_dirty\":" << (TER_CONFIGURE_TIME_DIRTY ? "true" : "false")
         << ",\"executable_sha256\":\"not_computed\""
         << ",\"build_identity\":\"" << json_escape(kBuildIdentity) << "\""
#ifdef NDEBUG
         << ",\"build_mode\":\"Release\""
#else
         << ",\"build_mode\":\"Debug\""
#endif
         << ",\"cpu\":\"" << json_escape(isa.brand) << "\""
         << ",\"isa_available\":\"" << (isa.usable ? "AVX2" : "portable") << "\""
         << ",\"thread_affinity\":\"" << json_escape(affinity_state) << "\""
         << ",\"timing\":{\"samples\":" << sample_count
         << ",\"timing_stability\":\"" << timing_stability << "\""
         << ",\"worst_relative_iqr\":" << number(worst_relative_iqr)
         << ",\"clock_resolution_ns\":" << number(clock_resolution_ns)
         << ",\"clock_overhead_ns\":" << number(clock_overhead_ns)
         << ",\"scalar_pair_ns\":{\"median\":" << number(pair_stats.median)
         << ",\"p25\":" << number(pair_stats.p25) << ",\"p75\":" << number(pair_stats.p75)
         << ",\"iqr\":" << number(pair_stats.iqr) << ",\"min\":" << number(pair_stats.minimum)
         << ",\"samples\":" << pair_stats.samples << "}"
         << ",\"baseline_agreement_vs_prior\":" << number(baseline_agreement)
         << ",\"rank_tile_k3_ceiling_ms\":" << number(rank_k3_ms)
         << ",\"rank_tile_k3_ceiling_speedup\":" << number(rank_k3_ceiling) << "}"
         << ",\"fixture\":{\"n\":" << data->n << ",\"seed\":" << data->seed
         << ",\"quotient_bits\":" << data->quotient_bits << ",\"r1\":" << data->r1
         << ",\"projection_bits\":" << kProjectionBits
         << ",\"residual_bits\":" << kResidualBits << ",\"a_size\":" << data->a_size
         << ",\"b_size\":" << data->b_size << ",\"c_size\":" << data->c_size
         << ",\"pair_universe\":" << data->pair_universe
         << ",\"tau_low32\":" << data->tau_low32 << ",\"tau_low25\":" << data->tau_low25
         << ",\"tau_derivation_verified\":"
         << (data->tau_derivation_verified ? "true" : "false")
         << ",\"decomposition_crosscheck\":" << crosscheck_expected
         << ",\"production_low25\":" << data->production_low_prefilter_candidates
         << ",\"gate_passed\":true}"
         << ",\"prior_measured\":{\"source\":\"MicroReport8, same machine and fixture\""
         << ",\"scalar_pair_ns\":" << number(kPriorScalarPairNs)
         << ",\"total_ms\":" << number(kPriorTotalMs)
         << ",\"k2_total_ms\":" << number(kPriorK2TotalMs)
         << ",\"k2_speedup\":" << number(kPriorK2Speedup)
         << ",\"k3_tile_ns\":" << number(kPriorK3TileNs)
         << ",\"k1_tile_ns\":" << number(kPriorK1TileNs)
         << ",\"tile_count\":" << kPriorTileCount << "}"
         << ",\"model_only\":{\"blocked_bitmap_work\":" << number(kBlockedBitmapModelWork)
         << ",\"note\":\"logical-work model over different primitive classes; not "
            "calibrated to nanoseconds here\"}"
         << ",\"geometry\":{\"tiles\":" << geometry.tiles
         << ",\"full_tiles\":" << geometry.full_tiles
         << ",\"partial_tiles\":" << geometry.partial_tiles
         << ",\"valid_lanes\":" << geometry.valid_lanes
         << ",\"true_lanes\":" << geometry.true_lanes
         << ",\"true_postings\":" << geometry.true_postings
         << ",\"routing_disagreements\":" << geometry.routing_disagreements
         << ",\"retile_true_lane_checks\":" << geometry.retile_true_lane_checks
         << ",\"retile_false_negatives\":" << geometry.retile_false_negatives
         << ",\"wrap_tiles\":" << geometry.wrap_tiles
         << ",\"normalization_violations\":" << geometry.normalization_violations
         << ",\"box_underestimates\":" << geometry.box_underestimates
         << ",\"sweep_seconds\":" << number(sweep_ns / 1.0e9)
         << ",\"index_build_ms\":" << number(index_build_ns / 1.0e6) << "}";

    json << ",\"window_thresholds\":[";
    for (std::size_t index2 = 0U; index2 < kClassCount; ++index2) {
      json << (index2 == 0U ? "" : ",") << "{\"class\":\"" << class_label(index2)
           << "\",\"tiles\":" << geometry.class_tiles[index2]
           << ",\"tile_fraction\":" << number(tile_fraction(geometry.class_tiles[index2]))
           << ",\"lanes\":" << geometry.class_lanes[index2]
           << ",\"lane_fraction\":" << number(lane_fraction(geometry.class_lanes[index2]))
           << ",\"cumulative_tile_fraction\":" << number(tile_fraction(cumulative_tiles[index2]))
           << ",\"cumulative_lane_fraction\":" << number(lane_fraction(cumulative_lanes[index2]))
           << ",\"true_lanes\":" << geometry.class_true_lanes[index2]
           << ",\"y_in_window_mean\":" << number(geometry.class_y_distinct[index2].mean())
           << ",\"y_in_window_median\":"
           << number(geometry.class_y_distinct[index2].quantile(0.50))
           << ",\"y_in_window_p90\":" << number(geometry.class_y_distinct[index2].quantile(0.90))
           << ",\"y_in_window_p95\":" << number(geometry.class_y_distinct[index2].quantile(0.95))
           << ",\"y_in_window_p99\":" << number(geometry.class_y_distinct[index2].quantile(0.99))
           << ",\"y_in_window_max\":" << geometry.class_y_distinct[index2].maximum
           << ",\"mean_target_density\":"
           << number(geometry.class_tiles[index2] == 0U ? 0.0 :
               static_cast<double>(geometry.class_density_sum[index2] / geometry.class_tiles[index2]))
           << ",\"true_lane_density\":"
           << number(geometry.class_lanes[index2] == 0U ? 0.0 :
               static_cast<double>(geometry.class_true_lanes[index2]) / geometry.class_lanes[index2])
           << ",\"y_postings_in_window\":" << geometry.class_y_postings[index2]
           << ",\"y_postings_mean\":" << number(geometry.class_y_posting_hist[index2].mean())
           << ",\"y_postings_median\":" << number(geometry.class_y_posting_hist[index2].quantile(0.50))
           << ",\"y_postings_p90\":" << number(geometry.class_y_posting_hist[index2].quantile(0.90))
           << ",\"y_postings_p95\":" << number(geometry.class_y_posting_hist[index2].quantile(0.95))
           << ",\"y_postings_p99\":" << number(geometry.class_y_posting_hist[index2].quantile(0.99))
           << ",\"y_postings_max\":" << geometry.class_y_posting_hist[index2].maximum << "}";
    }
    json << "]";

    const auto quantile_object = [&](const Histogram& histogram, const double offset = 0.0) {
      std::ostringstream out;
      out << std::setprecision(10) << "{\"min\":" << number(static_cast<double>(histogram.minimum) + offset)
          << ",\"mean\":" << number(histogram.mean() + offset)
          << ",\"median\":" << number(histogram.quantile(0.50) + offset)
          << ",\"p75\":" << number(histogram.quantile(0.75) + offset)
          << ",\"p90\":" << number(histogram.quantile(0.90) + offset)
          << ",\"p95\":" << number(histogram.quantile(0.95) + offset)
          << ",\"p99\":" << number(histogram.quantile(0.99) + offset)
          << ",\"max\":" << number(static_cast<double>(histogram.maximum) + offset)
          << "}";
      return out.str();
    };
    json << ",\"window_quantiles\":{\"exact_all\":" << quantile_object(geometry.slots_all)
         << ",\"exact_full_tiles\":" << quantile_object(geometry.slots_full)
         << ",\"exact_partial_tiles\":" << quantile_object(geometry.slots_partial)
         << ",\"source_box_all\":" << quantile_object(geometry.box_all)
         << ",\"source_box_full\":" << quantile_object(geometry.box_full)
         << ",\"source_box_partial\":" << quantile_object(geometry.box_partial)
         << ",\"exact_span_all\":" << quantile_object(geometry.slots_all, -1.0)
         << ",\"exact_span_full\":" << quantile_object(geometry.slots_full, -1.0)
         << ",\"exact_span_partial\":" << quantile_object(geometry.slots_partial, -1.0) << "}";

    json << ",\"source_box_error\":{\"mean\":" << number(ratio_stats.mean)
         << ",\"median\":" << number(ratio_stats.median)
         << ",\"p90\":" << number(ratio_stats.p90)
         << ",\"p95\":" << number(ratio_stats.p95)
         << ",\"p99\":" << number(ratio_stats.p99)
         << ",\"max\":" << number(ratio_stats.maximum)
         << ",\"fraction_exact\":" << number(tile_fraction(geometry.ratio_exact)) << "}";

    const auto touch_object = [&](const std::array<std::uint64_t, kTileLanes + 1U>& histogram) {
      std::ostringstream out;
      out << std::setprecision(10) << "{";
      bool first = true;
      for (const std::uint32_t threshold : {1U, 2U, 4U, 8U, 16U, 32U}) {
        std::uint64_t running = 0U;
        for (std::size_t bucket = 0U; bucket <= threshold; ++bucket) {
          running += histogram[bucket];
        }
        out << (first ? "" : ",") << "\"le_" << threshold << "\":"
            << number(tile_fraction(running));
        first = false;
      }
      std::uint64_t above = 0U;
      long double weighted = 0.0L;
      for (std::size_t bucket = 0U; bucket <= kTileLanes; ++bucket) {
        weighted += static_cast<long double>(bucket) * static_cast<long double>(histogram[bucket]);
        if (bucket > 32U) {
          above += histogram[bucket];
        }
      }
      out << ",\"gt_32\":" << number(tile_fraction(above)) << ",\"mean\":"
          << number(static_cast<double>(weighted / static_cast<long double>(geometry.tiles)))
          << ",\"histogram\":[";
      for (std::size_t i = 0U; i < histogram.size(); ++i) out << (i == 0U ? "" : ",") << histogram[i];
      out << "]}";
      return out.str();
    };
    json << ",\"bitmap_footprint\":{\"distinct_bits\":" << touch_object(geometry.touch_bits)
         << ",\"distinct_words\":" << touch_object(geometry.touch_words)
         << ",\"distinct_cache_lines\":" << touch_object(geometry.touch_lines)
         << ",\"distinct_pages\":" << touch_object(geometry.touch_pages)
         << ",\"actual_allocation_cache_lines\":" << touch_object(geometry.physical_lines)
         << ",\"actual_allocation_pages\":" << touch_object(geometry.physical_pages)
         << ",\"note\":\"legacy distinct_cache_lines/pages assume zero-aligned bitmap; actual_allocation fields use observed addresses\"}";

    json << ",\"window_reuse\":{\"consecutive_pairs\":" << geometry.consecutive_pairs
         << ",\"same_window_start\":"
         << number(static_cast<double>(geometry.same_start) /
                   static_cast<double>(geometry.consecutive_pairs))
         << ",\"mean_overlap\":"
         << number(static_cast<double>(geometry.overlap_sum /
                                       static_cast<long double>(geometry.consecutive_pairs)))
         << ",\"by_width\":[";
    for (std::size_t width = 0U; width < kReuseWidths.size(); ++width) {
      const double per_window =
          geometry.unique_aligned[width] == 0U
              ? 0.0
              : static_cast<double>(geometry.fits_aligned_tiles[width]) /
                    static_cast<double>(geometry.unique_aligned[width]);
      json << (width == 0U ? "" : ",") << "{\"L\":" << kReuseWidths[width]
           << ",\"fits_tile_fraction\":"
           << number(tile_fraction(geometry.fits_width_tiles[width]))
           << ",\"fits_lane_fraction\":"
           << number(lane_fraction(geometry.fits_width_lanes[width]))
           << ",\"aligned_tile_fraction\":"
           << number(tile_fraction(geometry.fits_aligned_tiles[width]))
           << ",\"aligned_lane_fraction\":"
           << number(lane_fraction(geometry.fits_aligned_lanes[width]))
           << ",\"unique_windows\":" << geometry.unique_aligned[width]
           << ",\"unique_start_regions_visited\":" << geometry.unique_start_regions[width]
           << ",\"tiles_per_visited_start_region\":"
           << number(static_cast<double>(geometry.tiles) / geometry.unique_start_regions[width])
           << ",\"lanes_per_visited_start_region\":"
           << number(static_cast<double>(geometry.valid_lanes) / geometry.unique_start_regions[width])
           << ",\"lanes_per_fitting_window\":"
           << number(geometry.unique_aligned[width] == 0U ? 0.0 :
               static_cast<double>(geometry.fits_aligned_lanes[width]) / geometry.unique_aligned[width])
           << ",\"tiles_per_window\":" << number(per_window) << ",\"same_region_rate\":"
           << number(static_cast<double>(geometry.same_region[width]) /
                     static_cast<double>(geometry.consecutive_pairs))
           << "}";
    }
    json << "]}";

    json << ",\"value_tiling\":[";
    for (std::size_t index2 = 0U; index2 < tiling.size(); ++index2) {
      const TilingResult& result = tiling[index2];
      json << (index2 == 0U ? "" : ",") << "{\"bx\":" << result.bx << ",\"bq\":" << result.bq
           << ",\"occupied_x_bins\":" << result.occupied_x_bins
           << ",\"occupied_q_bins\":" << result.occupied_q_bins
           << ",\"rectangles\":" << result.rectangles
           << ",\"rectangles_rejected\":" << result.rectangles_rejected
           << ",\"rectangles_surviving\":" << result.rectangles_surviving
           << ",\"mass_rejected\":" << result.mass_rejected
           << ",\"mass_surviving\":" << result.mass_surviving
           << ",\"mass_rejected_fraction\":"
           << number(static_cast<double>(result.mass_rejected) /
                     static_cast<double>(kPriorPairUniverse))
           << ",\"tiles\":" << result.tiles << ",\"full_tiles\":" << result.full_tiles
           << ",\"partial_tiles\":" << result.partial_tiles
           << ",\"fragmentation\":" << number(result.fragmentation())
           << ",\"tiles_vs_rank\":"
           << number(static_cast<double>(result.tiles) / static_cast<double>(kPriorTileCount))
           << ",\"unique_exact_windows\":" << result.unique_exact_windows
           << ",\"unique_aligned_windows\":" << result.unique_aligned_windows
           << ",\"window_slots_mean\":" << number(result.window_slots_mean)
           << ",\"window_slots_max\":" << result.window_slots_max
           << ",\"mixed_cost_class_ms\":" << number(result.mixed_cost_class_ms)
           << ",\"mixed_speedup_vs_scalar\":" << number(kPriorTotalMs / result.mixed_cost_class_ms)
           << ",\"mixed_speedup_vs_k2\":" << number(kPriorK2TotalMs / result.mixed_cost_class_ms)
           << ",\"tiles_per_exact_window\":" << number(result.unique_exact_windows == 0U ? 0.0 :
               static_cast<double>(result.tiles) / result.unique_exact_windows)
           << ",\"max_ns_per_tile\":{\"x10\":" << number(result.budget_ns(10.0))
           << ",\"x20\":" << number(result.budget_ns(20.0))
           << ",\"x30\":" << number(result.budget_ns(30.0))
           << ",\"x40\":" << number(result.budget_ns(40.0))
           << ",\"x50\":" << number(result.budget_ns(50.0)) << "}"
           << ",\"budget_label_10x\":\"" << budget_label(result.budget_ns(10.0)) << "\""
           << ",\"routing\":{\"pairs_per_rectangle\":" << number(result.pairs_per_rectangle())
           << ",\"t_rect_ns_median\":" << number(result.rect_ns.median)
           << ",\"t_rect_ns_iqr\":" << number(result.rect_ns.iqr)
           << ",\"t_rect_ns_p25\":" << number(result.rect_ns.p25)
           << ",\"t_rect_ns_p75\":" << number(result.rect_ns.p75)
           << ",\"t_rect_ns_min\":" << number(result.rect_ns.minimum)
           << ",\"reservoir\":" << result.reservoir.size()
           << ",\"routing_ms\":" << number(result.routing_ms)
           << ",\"leaf_ms_at_k3_cost\":" << number(result.leaf_floor_ms)
           << ",\"total_ms_measured_routing\":"
           << number(result.routing_ms + result.leaf_floor_ms)
           << ",\"ceiling_measured_routing\":" << number(result.ceiling_measured)
           << ",\"ceiling_floor_routing\":" << number(result.ceiling_floor)
           << ",\"speedup_vs_k2_floor\":"
           << number(result.ceiling_floor > 0.0
                         ? kPriorK2TotalMs /
                               (static_cast<double>(result.rectangles) * t_pair / 1.0e6 +
                                result.leaf_floor_ms)
                         : 0.0)
           << ",\"beats_unchanged_rank_tiling\":"
           << (result.ceiling_floor > rank_k3_ceiling ? "true" : "false")
           << ",\"note\":\"measured bitmap range query on source-order reservoir. Legacy floor fields are a hypothetical t_rect=t_pair sensitivity scenario, NOT a lower bound. K3 leaf costs assume an unimplemented exact reporter; binning/build/verification omitted.\"}"
           << ",\"optimistic_k3_cost_class\":{\"label\":\"OPTIMISTIC COST-CLASS PROJECTION, "
              "NOT AN ACHIEVED REPORTER\",\"total_ms\":"
           << number(result.k3_floor_ms()) << ",\"speedup_vs_scalar\":"
           << number(result.k3_floor_speedup()) << ",\"speedup_vs_k2\":"
           << number(result.k3_floor_ms() > 0.0 ? kPriorK2TotalMs / result.k3_floor_ms() : 0.0)
           << "}"
           << ",\"surviving_window_classes\":[";
      for (std::size_t klass = 0U; klass < kClassCount; ++klass) {
        json << (klass == 0U ? "" : ",") << "{\"class\":\"" << class_label(klass)
             << "\",\"regions\":" << result.class_rectangles[klass]
             << ",\"tiles\":" << result.class_tiles[klass]
             << ",\"lanes\":" << result.class_lanes[klass] << "}";
      }
      json << "]}";
    }
    json << "]";

    json << ",\"best_configuration\":{\"bx\":" << winner.bx << ",\"bq\":" << winner.bq
         << ",\"tiles\":" << winner.tiles
         << ",\"ceiling_floor_routing\":" << number(winner.ceiling_floor)
         << ",\"mixed_cost_class_ms\":" << number(winner.mixed_cost_class_ms)
         << ",\"reason\":\"lowest optimistic mixed cost: measured routing plus tiny regions at K3 cost and larger regions at K2 tile cost; not an achieved reporter\"}"
         << ",\"fewest_tiles_configuration\":{\"bx\":" << smallest.bx
         << ",\"bq\":" << smallest.bq << ",\"tiles\":" << smallest.tiles
         << ",\"rectangles\":" << smallest.rectangles
         << ",\"pairs_per_rectangle\":" << number(smallest.pairs_per_rectangle())
         << ",\"ceiling_floor_routing\":" << number(smallest.ceiling_floor) << "}"
         << ",\"route_a_viable\":" << (route_a ? "true" : "false")
         << ",\"route_b_viable\":" << (route_b ? "true" : "false")
         << ",\"phase3_triggered\":" << (phase3_triggered ? "true" : "false")
         << ",\"phase3_gate_reason\":\"" << json_escape(gate_reason) << "\""
         << ",\"verdict\":\"" << verdict << "\"}";
    json.close();
    if (!json) { std::cerr << "failed writing geometry JSON\n"; return EXIT_FAILURE; }
    std::ofstream raw_samples(root / "timing_samples.csv");
    raw_samples << "work_unit,sample,ns_per_unit\n" << std::setprecision(17);
    for (const Timed& entry : timed) {
      for (std::size_t i = 0U; i < entry.samples.size(); ++i)
        raw_samples << entry.id << ',' << i << ',' << entry.samples[i] << '\n';
    }
    raw_samples.close();
    if (!raw_samples) { std::cerr << "failed writing timing samples\n"; return EXIT_FAILURE; }

    std::ofstream csv(root / "window_geometry.csv");
    csv << "class,tiles,tile_fraction,lanes,lane_fraction,cumulative_tile_fraction,"
           "cumulative_lane_fraction,true_lanes,y_in_window_mean,y_in_window_median,"
           "y_in_window_p90,y_in_window_p99,y_in_window_max\n";
    csv << std::setprecision(10);
    for (std::size_t index2 = 0U; index2 < kClassCount; ++index2) {
      csv << class_label(index2) << ',' << geometry.class_tiles[index2] << ','
          << tile_fraction(geometry.class_tiles[index2]) << ','
          << geometry.class_lanes[index2] << ','
          << lane_fraction(geometry.class_lanes[index2]) << ','
          << tile_fraction(cumulative_tiles[index2]) << ','
          << lane_fraction(cumulative_lanes[index2]) << ','
          << geometry.class_true_lanes[index2] << ','
          << geometry.class_y_distinct[index2].mean() << ','
          << geometry.class_y_distinct[index2].quantile(0.50) << ','
          << geometry.class_y_distinct[index2].quantile(0.90) << ','
          << geometry.class_y_distinct[index2].quantile(0.99) << ','
          << geometry.class_y_distinct[index2].maximum << '\n';
    }
    csv.close();

    std::ofstream sweep(root / "value_tiling_sweep.csv");
    sweep << "bx,bq,occupied_x_bins,occupied_q_bins,rectangles,rectangles_rejected,"
             "rectangles_surviving,mass_rejected,mass_surviving,mass_rejected_fraction,tiles,"
             "full_tiles,partial_tiles,fragmentation,tiles_vs_rank,unique_exact_windows,"
             "unique_aligned_windows,window_slots_mean,window_slots_max,ns_10x,ns_20x,ns_30x,"
             "ns_40x,ns_50x,k3_floor_ms,k3_floor_speedup,pairs_per_rectangle,t_rect_ns,"
             "routing_ms,total_ms_measured,ceiling_measured,ceiling_floor,"
             "beats_unchanged_rank_tiling\n";
    sweep << std::setprecision(10);
    for (const TilingResult& result : tiling) {
      sweep << result.bx << ',' << result.bq << ',' << result.occupied_x_bins << ','
            << result.occupied_q_bins << ',' << result.rectangles << ','
            << result.rectangles_rejected << ',' << result.rectangles_surviving << ','
            << result.mass_rejected << ',' << result.mass_surviving << ','
            << static_cast<double>(result.mass_rejected) /
                   static_cast<double>(kPriorPairUniverse)
            << ',' << result.tiles << ',' << result.full_tiles << ',' << result.partial_tiles
            << ',' << result.fragmentation() << ','
            << static_cast<double>(result.tiles) / static_cast<double>(kPriorTileCount) << ','
            << result.unique_exact_windows << ',' << result.unique_aligned_windows << ','
            << result.window_slots_mean << ',' << result.window_slots_max << ','
            << result.budget_ns(10.0) << ',' << result.budget_ns(20.0) << ','
            << result.budget_ns(30.0) << ',' << result.budget_ns(40.0) << ','
            << result.budget_ns(50.0) << ',' << result.k3_floor_ms() << ','
            << result.k3_floor_speedup() << ',' << result.pairs_per_rectangle() << ','
            << result.rect_ns.median << ',' << result.routing_ms << ','
            << (result.routing_ms + result.leaf_floor_ms) << ',' << result.ceiling_measured
            << ',' << result.ceiling_floor << ','
            << (result.ceiling_floor > rank_k3_ceiling ? 1 : 0) << '\n';
    }
    sweep.close();

    std::ofstream summary(root / "summary.txt");
    summary << std::setprecision(10);
    summary << "TER^2 residual windowing spike\n";
    summary << "verdict: " << verdict << "\n\n";
    summary << "authentic tiles       " << geometry.tiles << "\n";
    summary << "valid lanes (P0)      " << geometry.valid_lanes << "\n";
    summary << "true lanes            " << geometry.true_lanes << "\n";
    summary << "median window slots   " << geometry.slots_all.quantile(0.50) << "\n";
    summary << "mean window slots     " << geometry.slots_all.mean() << "\n";
    summary << "lane mass <=1024      " << lane_fraction(cumulative_lanes[7]) << "\n";
    summary << "lane mass <=4096      " << lane_fraction(cumulative_lanes[9]) << "\n";
    summary << "t_pair this exe       " << t_pair << " ns (prior " << kPriorScalarPairNs
            << " ns, agreement " << baseline_agreement << ")\n";
    summary << "timing stability      " << timing_stability << " (worst relative IQR "
            << worst_relative_iqr << ")\n";
    summary << "fewest-tile config    Bx=" << smallest.bx << " Bq=" << smallest.bq << " tiles="
            << smallest.tiles << " ("
            << static_cast<double>(smallest.tiles) / static_cast<double>(kPriorTileCount)
            << "x rank tiles) but " << smallest.rectangles << " rectangle tests at "
            << smallest.pairs_per_rectangle() << " pairs per test\n";
    summary << "best mixed projection Bx=" << winner.bx << " Bq=" << winner.bq << " cost="
            << winner.mixed_cost_class_ms << " ms (optimistic, not achieved)\n";
    summary << "t_rect=t_pair fields are sensitivity scenarios, not lower bounds.\n";
    summary << "phase 3 triggered     " << (phase3_triggered ? "yes" : "no") << "\n\n";
    summary << gate_reason << "\n";
    summary.close();

    std::cout << "\n# wrote window_geometry.json, window_geometry.csv, value_tiling_sweep.csv "
                 "and summary.txt to "
              << output_directory << '\n';
  }

  std::cout << "checksum=" << g_sink << '\n';
  return EXIT_SUCCESS;
}
