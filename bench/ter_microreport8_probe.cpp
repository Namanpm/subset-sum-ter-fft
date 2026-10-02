// TER^2 MicroReport8 research spike.
//
// Answers exactly one question: can a genuinely packed, zero-false-negative
// 8x8 reporter process authentic n=72 TER^2 residual tiles cheaply and
// selectively enough to justify building an architecture around it?
//
// This is an isolated benchmark. It does not integrate TER^2, does not touch
// the production L1->L0 reporter, and makes no wall-clock claim about FullTER.
// Every speedup it prints is a projected isolated-front-end model, labelled as
// such at the point of printing.
//
// Evidence classes, never merged:
//   MEASURED/PROJECTED  - same-executable timings on this machine, this build.
//   MODEL_ONLY          - the historical 57.4625M blocked-bitmap work reference,
//                         which is a prior logical-work model over different
//                         primitive work classes and is NOT calibrated against
//                         measured nanoseconds here.

#include "ter/tiled_q3report.hpp"
#include "ter_microreport8_dataset.hpp"
#include "ter_microreport8_kernels.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
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

namespace ter::microreport8 {

// ---------------------------------------------------------------------------
// Portable batch drivers. These stay in ordinary compilation so the scalar
// baseline is exactly what a portable build would run.
// ---------------------------------------------------------------------------
std::uint64_t scalar_pair_batch(const Tile* const tiles, const std::size_t count,
                                const TargetView& view, std::uint64_t checksum) {
  // One accumulator per column gives eight independent dependency chains, so
  // the instrumentation costs about a quarter of a cycle per lane instead of a
  // full multiply latency. Folding once per lane would put a rotate-multiply on
  // the critical path and inflate this baseline, which would in turn flatter
  // every packed kernel it is divided into.
  std::uint64_t column[kTileDim] = {0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U};
  for (std::size_t index = 0U; index < count; ++index) {
    const Tile& tile = tiles[index];
    const std::uint32_t* const exact = view.exact[tile.u];
    for (std::uint32_t i = 0U; i < tile.x_count; ++i) {
      for (std::uint32_t j = 0U; j < tile.q_count; ++j) {
        column[j] += pair_probe(tile.x[i], tile.q[j], tile.kappa, exact) ? 1U : 0U;
      }
    }
  }
  for (const std::uint64_t partial : column) {
    checksum = fold(checksum, partial);
  }
  return checksum;
}

std::uint64_t scalar_report8_batch(const Tile* const tiles, const std::size_t count,
                                   const TargetView& view, std::uint64_t checksum) {
  for (std::size_t index = 0U; index < count; ++index) {
    checksum = fold(checksum, scalar_report8(tiles[index], view));
  }
  return checksum;
}

std::uint64_t k0_sum_floor_portable_batch(const Tile* const tiles, const std::size_t count,
                                          const TargetView& view, std::uint64_t checksum) {
  for (std::size_t index = 0U; index < count; ++index) {
    checksum = fold(checksum, k0_sum_floor_portable(tiles[index], view));
  }
  return checksum;
}

std::uint64_t k1_exact_gather_portable_batch(const Tile* const tiles, const std::size_t count,
                                             const TargetView& view, std::uint64_t checksum) {
  for (std::size_t index = 0U; index < count; ++index) {
    checksum = fold(checksum, k1_exact_gather_portable(tiles[index], view));
  }
  return checksum;
}

std::uint64_t k3_signature_shuffle_portable_batch(const Tile* const tiles,
                                                  const std::size_t count,
                                                  const TargetView& view,
                                                  std::uint64_t checksum) {
  for (std::size_t index = 0U; index < count; ++index) {
    checksum = fold(checksum, k3_signature_shuffle_portable(tiles[index], view));
  }
  return checksum;
}

std::uint64_t k2_coarse_gather_portable_batch(const Tile* const tiles, const std::size_t count,
                                              const TargetView& view, std::uint64_t checksum) {
  for (std::size_t index = 0U; index < count; ++index) {
    checksum = fold(checksum, k2_coarse_gather_portable(tiles[index], view));
  }
  return checksum;
}

std::uint64_t verify_candidate_batch(const CandidateLane* const lanes, const std::size_t count,
                                     const TargetView& view, std::uint64_t checksum) {
  const auto probe = [&view](const CandidateLane& lane) {
    return pair_probe(lane.x, lane.q, lane.meta & 1U,
                      view.exact[(lane.meta >> 1U) & (kSplitRadix - 1U)])
               ? std::uint64_t{1}
               : std::uint64_t{0};
  };
  // Four independent chains, for the same reason as scalar_pair_batch.
  std::uint64_t a0 = 0U;
  std::uint64_t a1 = 0U;
  std::uint64_t a2 = 0U;
  std::uint64_t a3 = 0U;
  std::size_t index = 0U;
  for (; index + 4U <= count; index += 4U) {
    a0 += probe(lanes[index]);
    a1 += probe(lanes[index + 1U]);
    a2 += probe(lanes[index + 2U]);
    a3 += probe(lanes[index + 3U]);
  }
  for (; index < count; ++index) {
    a0 += probe(lanes[index]);
  }
  return fold(fold(fold(fold(checksum, a0), a1), a2), a3);
}

namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] double elapsed_ns(const Clock::time_point start) noexcept {
  return std::chrono::duration<double, std::nano>(Clock::now() - start).count();
}

// The final observable sink. Every batch result reaches it, so no kernel's work
// can be eliminated as dead, and the value is printed as a checksum.
volatile std::uint64_t g_sink = 0U;

// The reference blocked-cyclic-bitmap work model. MODEL_ONLY: a prior logical
// work count over different primitive classes, not measured nanoseconds.
constexpr double kBlockedBitmapModelWork = 57.4625e6;
constexpr double kIdealTileModel = 9.7026e6;
constexpr double kIdealOutputModel = 0.6201e6;

struct Stats {
  double median = 0.0;
  double p25 = 0.0;
  double p75 = 0.0;
  double iqr = 0.0;
  double minimum = 0.0;
  std::size_t samples = 0U;
};

[[nodiscard]] double quantile(const std::vector<double>& sorted, const double fraction) {
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
  stats.p25 = quantile(samples, 0.25);
  stats.median = quantile(samples, 0.50);
  stats.p75 = quantile(samples, 0.75);
  stats.iqr = stats.p75 - stats.p25;
  return stats;
}

// ---------------------------------------------------------------------------
// Kernel registry.
// ---------------------------------------------------------------------------
struct KernelRecord {
  std::string id;
  std::string description;
  bool exact = false;          // reports no false positives by construction
  bool control_only = false;   // never bears a verdict
  TileKernel tile_fn = nullptr;
  TileKernel portable_fn = nullptr;
  BatchKernel batch_fn = nullptr;

  // Full authentic sweep counters.
  std::uint64_t candidate_lanes = 0U;
  std::uint64_t false_positive_lanes = 0U;
  std::uint64_t false_negative_lanes = 0U;
  std::uint64_t padding_violations = 0U;
  std::uint64_t portable_mismatches = 0U;
  std::uint64_t candidate_checksum = 0U;

  // Candidate verification stream (subsampled from the full sweep).
  std::vector<CandidateLane> verify_stream;

  Stats tile_ns;
  Stats verify_ns;
  double c8 = 0.0;
  double c_verify = 0.0;
  double w_eff = 0.0;
  double s_eff = 0.0;
  double t_leaf_ns = 0.0;
  double s_leaf = 0.0;
  bool verdict_eligible = false;
};

[[nodiscard]] std::string json_escape(const std::string_view value) {
  std::string out;
  out.reserve(value.size() + 8U);
  for (const char character : value) {
    switch (character) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(character) < 0x20U) {
          out += ' ';
        } else {
          out += character;
        }
        break;
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

void emit_stats(std::ostream& out, const std::string_view name, const Stats& stats) {
  out << "\"" << name << "\":{\"median_ns\":" << number(stats.median)
      << ",\"p25_ns\":" << number(stats.p25) << ",\"p75_ns\":" << number(stats.p75)
      << ",\"iqr_ns\":" << number(stats.iqr) << ",\"min_ns\":" << number(stats.minimum)
      << ",\"samples\":" << stats.samples << "}";
}

// ---------------------------------------------------------------------------
// Deterministic generator for the synthetic sections.
// ---------------------------------------------------------------------------
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

struct Geometry {
  std::uint64_t tile_count = 0U;
  std::uint64_t full_tiles = 0U;
  std::uint64_t partial_tiles = 0U;
  std::uint64_t valid_lanes = 0U;
  std::uint64_t true_lanes = 0U;
  std::uint64_t empty_microproblems = 0U;
  std::uint64_t truth_checksum = 0U;
  std::uint64_t crosscheck_observed = 0U;
  std::array<std::uint64_t, kTileLanes + 1U> lane_histogram{};
  double fragmentation = 0.0;
  double mean_valid_lanes = 0.0;
  double median_valid_lanes = 0.0;
};

}  // namespace
}  // namespace ter::microreport8

int main(const int argc, char** const argv) {
  using namespace ter;
  using namespace ter::microreport8;

  std::uint32_t root_n = kReferenceRootN;
  std::uint64_t root_seed = kReferenceRootSeed;
  std::size_t sample_count = 51U;
  std::size_t verify_cap = 1000000U;
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
    } else if (value.starts_with("--verify-cap=")) {
      verify_cap = static_cast<std::size_t>(std::strtoull(argv[argument] + 13, nullptr, 10));
    } else if (value.starts_with("--out=")) {
      output_directory = argv[argument] + 6;
    } else if (value == "--no-production-anchor") {
      run_production_anchor = false;
    } else if (value == "--no-pin") {
      pin_to_one_core = false;
    } else {
      std::cerr << "usage: ter_microreport8_probe [--root-n=N] [--root-seed=S]\n"
                   "       [--samples=N] [--verify-cap=N] [--out=DIR]\n"
                   "       [--no-production-anchor] [--no-pin]\n";
      return EXIT_FAILURE;
    }
  }
  if (sample_count < 5U) {
    std::cerr << "at least 5 timing samples are required\n";
    return EXIT_FAILURE;
  }

  const IsaReport isa = detect_isa();
  const bool avx2_compiled = avx2::translation_unit_is_avx2();
  const bool avx2_executed = avx2_compiled && isa.usable;

  std::cout << "# ter_microreport8_probe -- TER^2 leaf reporter feasibility spike\n";
  std::cout << "# configure_time_commit=" << TER_CONFIGURE_TIME_COMMIT
            << " configure_time_dirty=" << (TER_CONFIGURE_TIME_DIRTY ? "yes" : "no") << '\n';
  std::cout << "# cpu=" << (isa.brand[0] != '\0' ? isa.brand : "unknown") << '\n';
  std::cout << "# isa_compiled=" << (avx2_compiled ? "AVX2" : "portable")
            << " isa_available=" << (isa.usable ? "AVX2" : "portable")
            << " isa_executed=" << (avx2_executed ? "AVX2" : "portable")
            << " (osxsave=" << isa.osxsave << " avx=" << isa.avx
            << " ymm_state=" << isa.ymm_state_enabled << " avx2=" << isa.avx2
            << " bmi2=" << isa.bmi2 << " avx512f=" << isa.avx512f << ")\n";
  // This is a hybrid performance/efficiency-core CPU with no SMT. Left to the
  // scheduler, a batch can migrate between core types mid-sample and the
  // resulting spread swamps the effect being measured. Pin to one core and
  // raise priority so the samples describe one core's behaviour.
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
  // Phase 1: authentic capture and exact TER^2 decomposition.
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
            << " P0=" << data->pair_universe << '\n';
  std::cout << "# target_source=quotient_tau tau_low32=" << data->tau_low32
            << " tau_low25=" << data->tau_low25 << " tau_derivation_verified="
            << (data->tau_derivation_verified ? "true" : "false") << " ("
            << data->tau_verification_detail << ")\n";
  std::cout << "# geometry_matches_reference="
            << (data->geometry_matches_reference ? "true" : "false")
            << " capture_ms=" << number(capture_ns / 1.0e6) << '\n';
  std::cout.flush();

  const double pair_universe = static_cast<double>(data->pair_universe);

  // -------------------------------------------------------------------------
  // Phase 2: independent decomposition cross-check.
  //
  // tiled_q3report_count() is the repository's already-tested exact reporter.
  // Running it on the low-25 projection gives the number of captured triples
  // with q_a + q_b + q_c == tau (mod 2^25), counted with posting multiplicity.
  // The 4096-microproblem decomposition must reproduce it exactly, which
  // validates tau's use, d = tau - q_c, the 6/19 split, u = (s+t)&63,
  // kappa = (s+t)>>6, the mod-2^19 wrap, the microproblem enumeration and the
  // posting multiplicity. Production's own low_prefilter_candidates at the same
  // width is the third, independent leg: it uses PRODUCTION's tau, so agreement
  // validates this spike's tau end to end rather than merely internally.
  // -------------------------------------------------------------------------
  std::cout << "# running independent tiled_q3report_count cross-check...\n";
  std::cout.flush();
  TiledQ3ReportConfig tiled_config;
  tiled_config.preferred_tile_bits = limits::tiled::kDefaultPreferredTileBits;
  const auto crosscheck = tiled_q3report_count(
      data->a_low25, data->b_low25, data->c_low25, data->tau_low25,
      std::uint64_t{1} << kProjectionBits, tiled_config);
  if (crosscheck.status != TiledQ3ReportStatus::Completed) {
    std::cerr << "FATAL: cross-check reporter failed: "
              << tiled_q3report_status_name(crosscheck.status) << " " << crosscheck.message
              << '\n';
    return EXIT_FAILURE;
  }
  const std::uint64_t crosscheck_expected = crosscheck.telemetry.raw_triples;
  std::cout << "# decomposition_crosscheck_expected=" << crosscheck_expected << '\n';
  if (data->production_anchor_available) {
    std::cout << "# production_low_prefilter_candidates("
              << kProjectionBits << " bits)=" << data->production_low_prefilter_candidates
              << " production_valid_root_count=" << data->production_valid_root_count << '\n';
  } else {
    std::cout << "# production anchor unavailable: " << data->production_anchor_status << '\n';
  }
  std::cout.flush();

  // -------------------------------------------------------------------------
  // Phase 3: kernel registry.
  // -------------------------------------------------------------------------
  std::vector<KernelRecord> kernels;
  {
    KernelRecord k0;
    k0.id = "K0_sum_floor";
    k0.description =
        "SUM-MATERIALIZATION FLOOR CONTROL for the 32-bit-lane scheme: materializes the "
        "64 exact residual sums as eight vectors of eight 32-bit lanes and emits a mask "
        "from an always-true predicate. It bounds only the sum-generation scheme K1 and "
        "K2 share. It is NOT a lower bound for every conceivable packed reporter, and "
        "this run demonstrates why: K3's byte-lane scheme produces the same 64 lanes in "
        "two vector groups instead of eight and measures cheaper than this control. "
        "Never bears a verdict.";
    k0.exact = false;
    k0.control_only = true;
    k0.tile_fn = avx2_executed ? &avx2::k0_sum_floor : &k0_sum_floor_portable;
    k0.portable_fn = &k0_sum_floor_portable;
    k0.batch_fn = avx2_executed ? &avx2::k0_sum_floor_batch : &k0_sum_floor_portable_batch;
    kernels.push_back(std::move(k0));

    KernelRecord k1;
    k1.id = "K1_exact_gather";
    k1.description =
        "Primary exact candidate: eight vpgatherdd loads pull the 2^19-bit exact Y "
        "bitmap words for eight lanes at a time; vpsrlvd aligns the membership bit "
        "and vmovmskps assembles the row. Exact, so no false positives.";
    k1.exact = true;
    k1.tile_fn = avx2_executed ? &avx2::k1_exact_gather : &k1_exact_gather_portable;
    k1.portable_fn = &k1_exact_gather_portable;
    k1.batch_fn = avx2_executed ? &avx2::k1_exact_gather_batch
                                : &k1_exact_gather_portable_batch;
    kernels.push_back(std::move(k1));

    KernelRecord k3;
    k3.id = "K3_signature_shuffle";
    k3.description =
        "Low-memory control and superset reporter: 32 lanes per group, byte-lane "
        "vpaddb sums and a two-vpshufb 256-bucket nibble membership test entirely in "
        "registers. Zero memory traffic beyond the tile. It measures the cost of a "
        "register-only filter on this geometry; it is not a proof of the minimum "
        "possible cost of every future filter architecture.";
    k3.exact = false;
    k3.tile_fn = avx2_executed ? &avx2::k3_signature_shuffle : &k3_signature_shuffle_portable;
    k3.portable_fn = &k3_signature_shuffle_portable;
    k3.batch_fn = avx2_executed ? &avx2::k3_signature_shuffle_batch
                                : &k3_signature_shuffle_portable_batch;
    kernels.push_back(std::move(k3));

    KernelRecord k2;
    k2.id = "K2_coarse_gather";
    k2.description =
        "Superset reporter trading exactness for cache residency: identical gather "
        "shape to K1 but against a block-of-8 coarse bitmap one eighth the size.";
    k2.exact = false;
    k2.tile_fn = avx2_executed ? &avx2::k2_coarse_gather : &k2_coarse_gather_portable;
    k2.portable_fn = &k2_coarse_gather_portable;
    k2.batch_fn = avx2_executed ? &avx2::k2_coarse_gather_batch
                                : &k2_coarse_gather_portable_batch;
    kernels.push_back(std::move(k2));
  }

  // -------------------------------------------------------------------------
  // Phase 4: full authentic sweep.
  //
  // Every one of the real tiles is visited: geometry, truth, per-kernel
  // differential correctness, lane accounting and the decomposition
  // cross-check all come from this single exhaustive pass. Nothing here is
  // sampled.
  // -------------------------------------------------------------------------
  std::cout << "# sweeping every authentic tile (truth, differential, accounting)...\n";
  std::cout.flush();
  Geometry geometry;
  std::vector<std::uint32_t> multiplicity(kResidualUniverse, 0U);
  std::uint32_t loaded_family = kSplitRadix;  // none loaded yet
  const auto sweep_start = Clock::now();
  {
    for_each_tile(*data, [&](const Tile& tile, const MicroproblemKey& key) {
      if (key.u != loaded_family) {
        if (loaded_family < kSplitRadix) {
          for (const std::uint32_t value : data->targets[loaded_family].values) {
            multiplicity[value] = 0U;
          }
        }
        for (const std::uint32_t value : data->targets[key.u].values) {
          ++multiplicity[value];
        }
        loaded_family = key.u;
      }
      const std::uint64_t valid = tile_valid_mask(tile);
      const std::uint64_t truth = scalar_report8(tile, data->view);
      const auto valid_lanes = static_cast<std::uint32_t>(std::popcount(valid));
      ++geometry.tile_count;
      geometry.valid_lanes += valid_lanes;
      ++geometry.lane_histogram[valid_lanes];
      if (valid_lanes == kTileLanes) {
        ++geometry.full_tiles;
      } else {
        ++geometry.partial_tiles;
      }
      geometry.true_lanes += static_cast<std::uint64_t>(std::popcount(truth));
      geometry.truth_checksum = fold(geometry.truth_checksum, truth);

      // Only true lanes can carry nonzero multiplicity, so the exact posting
      // total costs one lookup per true lane rather than one per lane.
      std::uint64_t remaining = truth;
      while (remaining != 0U) {
        const auto lane = static_cast<std::uint32_t>(std::countr_zero(remaining));
        remaining &= remaining - 1U;
        const std::uint32_t sum =
            (tile.x[lane >> 3U] + tile.q[lane & 7U] + tile.kappa) & kResidualMask;
        geometry.crosscheck_observed += multiplicity[sum];
      }

      for (KernelRecord& kernel : kernels) {
        const std::uint64_t reported = kernel.tile_fn(tile, data->view);
        kernel.candidate_checksum = fold(kernel.candidate_checksum, reported);
        kernel.candidate_lanes += static_cast<std::uint64_t>(std::popcount(reported & valid));
        kernel.false_negative_lanes +=
            static_cast<std::uint64_t>(std::popcount(truth & ~reported));
        kernel.false_positive_lanes +=
            static_cast<std::uint64_t>(std::popcount(reported & ~truth & valid));
        kernel.padding_violations +=
            static_cast<std::uint64_t>(std::popcount(reported & ~valid));
        if (avx2_executed) {
          // Whenever both paths exist they must agree bit for bit.
          kernel.portable_mismatches +=
              kernel.portable_fn(tile, data->view) != reported ? 1U : 0U;
        }
      }
    });
  }
  const double sweep_ns = elapsed_ns(sweep_start);

  for (std::uint32_t u = 0U; u < kSplitRadix; ++u) {
    for (std::uint32_t s = 0U; s < kSplitRadix; ++s) {
      const MicroproblemKey key = microproblem_for(u, s);
      if (data->x_family[key.t].empty() || data->q_family[key.s].empty()) {
        ++geometry.empty_microproblems;
      }
    }
  }
  geometry.fragmentation =
      static_cast<double>(kTileLanes * geometry.tile_count) / pair_universe;
  geometry.mean_valid_lanes =
      static_cast<double>(geometry.valid_lanes) / static_cast<double>(geometry.tile_count);
  {
    // Median valid lanes per tile, straight from the histogram.
    const std::uint64_t half = geometry.tile_count / 2U;
    std::uint64_t running = 0U;
    for (std::size_t lanes = 0U; lanes <= kTileLanes; ++lanes) {
      running += geometry.lane_histogram[lanes];
      if (running > half) {
        geometry.median_valid_lanes = static_cast<double>(lanes);
        break;
      }
    }
  }

  const bool crosscheck_pass = geometry.crosscheck_observed == crosscheck_expected &&
                               (!data->production_anchor_available ||
                                data->production_low_prefilter_candidates == crosscheck_expected);
  std::cout << "# tiles=" << geometry.tile_count << " valid_lanes=" << geometry.valid_lanes
            << " true_lanes=" << geometry.true_lanes
            << " F_tile=" << number(geometry.fragmentation)
            << " sweep_ms=" << number(sweep_ns / 1.0e6) << '\n';
  std::cout << "# decomposition_crosscheck_observed=" << geometry.crosscheck_observed
            << " expected=" << crosscheck_expected
            << " pass=" << (crosscheck_pass ? "true" : "false") << '\n';
  std::cout.flush();

  if (geometry.valid_lanes != data->pair_universe) {
    std::cerr << "FATAL: tile decomposition lost pair mass: " << geometry.valid_lanes
              << " valid lanes vs P0=" << data->pair_universe << '\n';
    return EXIT_FAILURE;
  }
  if (!crosscheck_pass) {
    std::cerr << "FATAL: decomposition cross-check failed. Observed "
              << geometry.crosscheck_observed << ", tiled_q3report_count expected "
              << crosscheck_expected;
    if (data->production_anchor_available) {
      std::cerr << ", production low_prefilter_candidates "
                << data->production_low_prefilter_candidates;
    }
    std::cerr << ". Refusing to benchmark an unvalidated decomposition.\n";
    return EXIT_FAILURE;
  }

  bool any_false_negative = false;
  for (const KernelRecord& kernel : kernels) {
    if (kernel.false_negative_lanes != 0U) {
      any_false_negative = true;
    }
    std::cout << "# " << kernel.id << ": candidates=" << kernel.candidate_lanes
              << " fp=" << kernel.false_positive_lanes
              << " fn=" << kernel.false_negative_lanes
              << " padding_violations=" << kernel.padding_violations
              << " portable_mismatches=" << kernel.portable_mismatches << '\n';
  }
  std::cout.flush();

  // -------------------------------------------------------------------------
  // Phase 5: candidate verification streams.
  //
  // Second exhaustive pass. A per-kernel stride is computed from the exact
  // candidate counts measured above so the retained sample is spread uniformly
  // across the whole authentic stream rather than truncated at its head.
  // -------------------------------------------------------------------------
  std::cout << "# collecting candidate verification streams...\n";
  std::cout.flush();
  std::vector<std::uint64_t> stride(kernels.size(), 1U);
  std::vector<std::uint64_t> seen(kernels.size(), 0U);
  for (std::size_t index = 0U; index < kernels.size(); ++index) {
    const std::uint64_t total = kernels[index].candidate_lanes;
    stride[index] = total > verify_cap ? (total + verify_cap - 1U) / verify_cap : 1U;
    kernels[index].verify_stream.reserve(
        static_cast<std::size_t>(std::min<std::uint64_t>(total, verify_cap) + 1U));
  }
  for_each_tile(*data, [&](const Tile& tile, const MicroproblemKey&) {
    const std::uint64_t valid = tile_valid_mask(tile);
    for (std::size_t index = 0U; index < kernels.size(); ++index) {
      KernelRecord& kernel = kernels[index];
      std::uint64_t reported = kernel.tile_fn(tile, data->view) & valid;
      while (reported != 0U) {
        const auto lane = static_cast<std::uint32_t>(std::countr_zero(reported));
        reported &= reported - 1U;
        if (seen[index] % stride[index] == 0U) {
          kernel.verify_stream.push_back(
              CandidateLane{tile.x[lane >> 3U], tile.q[lane & 7U],
                            static_cast<std::uint32_t>(tile.kappa) |
                                (static_cast<std::uint32_t>(tile.u) << 1U)});
        }
        ++seen[index];
      }
    }
  });

  // -------------------------------------------------------------------------
  // Phase 6: benchmark sample stream.
  //
  // Grouped by u so a run of tiles reuses one active Y_u structure, and strided
  // within each (s, t) so the whole residual value range is represented rather
  // than only the smallest values.
  // -------------------------------------------------------------------------
  constexpr std::size_t kTilesPerMicroproblem = 16U;
  std::vector<Tile> sample;
  sample.reserve(kSplitRadix * kSplitRadix * kTilesPerMicroproblem);
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
        const std::size_t index = step * total / take;
        make_tile(*data, key, index / q_blocks, index % q_blocks, tile);
        sample.push_back(tile);
      }
    }
  }
  std::uint64_t sample_valid_lanes = 0U;
  for (const Tile& tile : sample) {
    sample_valid_lanes += static_cast<std::uint64_t>(std::popcount(tile_valid_mask(tile)));
  }
  std::cout << "# benchmark sample: " << sample.size() << " tiles, " << sample_valid_lanes
            << " valid lanes, " << (sample.size() * sizeof(Tile)) / 1024U << " KiB\n";
  std::cout.flush();

  // -------------------------------------------------------------------------
  // Phase 7: batched timing.
  // -------------------------------------------------------------------------
  double clock_overhead_ns = 0.0;
  double clock_resolution_ns = 0.0;
  {
    // The median back-to-back gap is frequently zero because the platform clock
    // ticks coarsely, so the smallest NONZERO gap is recorded as the effective
    // resolution and used for the guard. Reporting only the median would make
    // the guard vacuous.
    constexpr std::size_t kProbes = 20000U;
    std::vector<double> gaps;
    gaps.reserve(kProbes);
    for (std::size_t probe = 0U; probe < kProbes; ++probe) {
      const auto start = Clock::now();
      gaps.push_back(elapsed_ns(start));
    }
    clock_overhead_ns = summarize_samples(gaps).median;
    clock_resolution_ns = 0.0;
    for (const double gap : gaps) {
      if (gap > 0.0 && (clock_resolution_ns == 0.0 || gap < clock_resolution_ns)) {
        clock_resolution_ns = gap;
      }
    }
  }

  struct Timed {
    std::string id;
    BatchKernel batch = nullptr;
    const Tile* tiles = nullptr;
    const CandidateLane* lanes = nullptr;
    std::size_t count = 0U;
    double units = 0.0;  // work units per batch call
    std::size_t repeats = 1U;
    std::vector<double> samples;
  };

  std::vector<Timed> timed;
  timed.push_back(Timed{"scalar_local_pair", &scalar_pair_batch, sample.data(), nullptr,
                        sample.size(), static_cast<double>(sample_valid_lanes), 1U, {}});
  timed.push_back(Timed{"scalar_report8", &scalar_report8_batch, sample.data(), nullptr,
                        sample.size(), static_cast<double>(sample.size()), 1U, {}});
  for (const KernelRecord& kernel : kernels) {
    timed.push_back(Timed{kernel.id, kernel.batch_fn, sample.data(), nullptr, sample.size(),
                          static_cast<double>(sample.size()), 1U, {}});
  }
  for (const KernelRecord& kernel : kernels) {
    if (kernel.verify_stream.empty()) {
      continue;
    }
    timed.push_back(Timed{kernel.id + "_verify", nullptr, nullptr, kernel.verify_stream.data(),
                          kernel.verify_stream.size(),
                          static_cast<double>(kernel.verify_stream.size()), 1U, {}});
  }

  const auto run_batch = [&](const Timed& entry, std::uint64_t checksum) {
    for (std::size_t repeat = 0U; repeat < entry.repeats; ++repeat) {
      checksum = entry.batch != nullptr
                     ? entry.batch(entry.tiles, entry.count, data->view, checksum)
                     : verify_candidate_batch(entry.lanes, entry.count, data->view, checksum);
    }
    return checksum;
  };

  // Calibrate repeats so every timed region is at least 20 ms, which is many
  // orders of magnitude above the platform clock's effective resolution.
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
  // Guard against timing the clock rather than the kernel. The median
  // back-to-back call gap is routinely zero because the platform clock ticks
  // coarsely, so the smallest observed nonzero gap -- the effective resolution
  // -- is what the timed region has to dominate.
  const double clock_guard_ns = std::max(clock_resolution_ns, clock_overhead_ns);
  if (kTargetRegionNs < 1000.0 * clock_guard_ns) {
    std::cerr << "FATAL: timing region is not large enough relative to the platform clock ("
              << number(clock_guard_ns) << " ns)\n";
    return EXIT_FAILURE;
  }

  std::cout << "# timing (" << sample_count
            << " samples, rotated order, clock resolution " << number(clock_resolution_ns)
            << " ns, median call overhead " << number(clock_overhead_ns) << " ns)...\n";
  std::cout.flush();
  for (std::size_t round = 0U; round < sample_count; ++round) {
    for (std::size_t offset = 0U; offset < timed.size(); ++offset) {
      // Rotate the order each round so no entity systematically inherits
      // another's cache state.
      Timed& entry = timed[(offset + round) % timed.size()];
      std::uint64_t checksum = g_sink;
      const auto start = Clock::now();
      checksum = run_batch(entry, checksum);
      const double region = elapsed_ns(start);
      g_sink = checksum;
      entry.samples.push_back(region /
                              (entry.units * static_cast<double>(entry.repeats)));
    }
  }

  std::vector<Stats> timed_stats;
  timed_stats.reserve(timed.size());
  for (Timed& entry : timed) {
    timed_stats.push_back(summarize_samples(std::move(entry.samples)));
  }
  const auto stats_for = [&](const std::string_view id) {
    for (std::size_t index = 0U; index < timed.size(); ++index) {
      if (timed[index].id == id) {
        return timed_stats[index];
      }
    }
    return Stats{};
  };

  const Stats pair_stats = stats_for("scalar_local_pair");
  const Stats report8_stats = stats_for("scalar_report8");
  const double t_pair = pair_stats.median;
  if (t_pair <= 0.0) {
    std::cerr << "FATAL: scalar pair baseline did not produce a positive median\n";
    return EXIT_FAILURE;
  }
  const double c_scalar8 = report8_stats.median / t_pair;

  // Baseline self-consistency. ScalarReport8 performs exactly the same per-lane
  // exact probes over the same tiles, with one checksum fold per tile instead of
  // a per-column accumulator, so its per-lane cost must land close to t_pair. A
  // large gap would mean the baseline is measuring its own instrumentation
  // rather than the pair probe, which would silently flatter every kernel that
  // is divided by it.
  const double mean_sample_lanes =
      static_cast<double>(sample_valid_lanes) / static_cast<double>(sample.size());
  const double report8_per_lane_ns = report8_stats.median / mean_sample_lanes;
  const double baseline_agreement =
      t_pair > 0.0 ? report8_per_lane_ns / t_pair : 0.0;

  double worst_relative_iqr = pair_stats.median > 0.0 ? pair_stats.iqr / pair_stats.median : 1.0;
  for (std::size_t index = 0U; index < kernels.size(); ++index) {
    KernelRecord& kernel = kernels[index];
    kernel.tile_ns = stats_for(kernel.id);
    kernel.verify_ns = stats_for(kernel.id + "_verify");
    kernel.c8 = kernel.tile_ns.median / t_pair;
    kernel.c_verify = kernel.verify_ns.median / t_pair;
    kernel.w_eff = static_cast<double>(geometry.tile_count) * kernel.c8 +
                   static_cast<double>(kernel.candidate_lanes) * kernel.c_verify;
    kernel.s_eff = kernel.w_eff > 0.0 ? pair_universe / kernel.w_eff : 0.0;
    kernel.t_leaf_ns = static_cast<double>(geometry.tile_count) * kernel.tile_ns.median +
                       static_cast<double>(kernel.candidate_lanes) * kernel.verify_ns.median;
    kernel.s_leaf = kernel.t_leaf_ns > 0.0 ? (pair_universe * t_pair) / kernel.t_leaf_ns : 0.0;
    kernel.verdict_eligible = !kernel.control_only && kernel.false_negative_lanes == 0U &&
                              kernel.padding_violations == 0U &&
                              kernel.portable_mismatches == 0U && kernel.tile_ns.samples > 0U &&
                              kernel.verify_ns.samples > 0U;
    if (kernel.tile_ns.median > 0.0) {
      worst_relative_iqr =
          std::max(worst_relative_iqr, kernel.tile_ns.iqr / kernel.tile_ns.median);
    }
  }
  const std::string timing_stability = worst_relative_iqr > 0.10 ? "poor" : "acceptable";

  // -------------------------------------------------------------------------
  // Phase 8: synthetic evidence. Correctness and stress only; it never feeds
  // the verdict, which comes solely from the authentic section.
  // -------------------------------------------------------------------------
  std::uint64_t synthetic_cases = 0U;
  std::uint64_t synthetic_false_negatives = 0U;
  std::uint64_t synthetic_brute_mismatches = 0U;
  std::uint64_t stress_cases = 0U;
  std::uint64_t stress_false_negatives = 0U;
  {
    auto synthetic_targets = std::make_unique<std::array<TargetSet, kSplitRadix>>();
    TargetView synthetic_view;
    SplitMix64 rng(0xA5A5A5A5DEADBEEFULL);
    // Density sweep: family f holds roughly 2^(f/4) distinct residuals, so the
    // sweep covers the empty, singleton, sparse and dense regimes.
    std::array<std::vector<std::uint32_t>, kSplitRadix> synthetic_values;
    for (std::uint32_t family = 0U; family < kSplitRadix; ++family) {
      const std::size_t count =
          family == 0U ? 0U : static_cast<std::size_t>(1ULL << (family / 4U));
      auto& values = synthetic_values[family];
      values.reserve(count);
      for (std::size_t item = 0U; item < count; ++item) {
        values.push_back(static_cast<std::uint32_t>(rng.next() & kResidualMask));
      }
      std::vector<Posting> postings;
      postings.reserve(values.size());
      for (const std::uint32_t value : values) {
        postings.push_back(Posting{value, 0U});
      }
      std::sort(postings.begin(), postings.end(),
                [](const Posting& left, const Posting& right) {
                  return left.value < right.value;
                });
      TargetSet& target = (*synthetic_targets)[family];
      build_target(postings, target);
      synthetic_view.exact[family] = target.exact.data();
      synthetic_view.coarse[family] = target.coarse.data();
      synthetic_view.sig07[family] = target.sig07.data();
      synthetic_view.sig815[family] = target.sig815.data();
    }

    for (std::size_t trial = 0U; trial < 200000U; ++trial) {
      Tile tile{};
      tile.x_count = static_cast<std::uint8_t>(1U + (rng.next() % kTileDim));
      tile.q_count = static_cast<std::uint8_t>(1U + (rng.next() % kTileDim));
      tile.kappa = static_cast<std::uint8_t>(rng.next() & 1U);
      tile.u = static_cast<std::uint8_t>(rng.next() % kSplitRadix);
      const auto& values = (*synthetic_targets)[tile.u].values;
      for (std::size_t lane = 0U; lane < kTileDim; ++lane) {
        // A third of the tiles are seeded from real target values so hits are
        // constructed rather than left to chance, and boundaries are forced.
        const std::uint64_t choice = rng.next() % 8U;
        std::uint32_t value = static_cast<std::uint32_t>(rng.next() & kResidualMask);
        if (choice == 0U) {
          value = 0U;
        } else if (choice == 1U) {
          value = kResidualMask;
        } else if (choice == 2U && !values.empty()) {
          value = values[static_cast<std::size_t>(rng.next() % values.size())];
        }
        tile.x[lane] = lane < tile.x_count ? value : 0U;
        tile.q[lane] = lane < tile.q_count
                           ? static_cast<std::uint32_t>(rng.next() & kResidualMask)
                           : 0U;
      }
      const std::uint64_t truth = scalar_report8(tile, synthetic_view);
      const std::uint64_t brute =
          brute_report8(tile, values.data(), values.size()) & tile_valid_mask(tile);
      synthetic_brute_mismatches += (truth & tile_valid_mask(tile)) != brute ? 1U : 0U;
      for (const KernelRecord& kernel : kernels) {
        const std::uint64_t reported = kernel.tile_fn(tile, synthetic_view);
        synthetic_false_negatives +=
            static_cast<std::uint64_t>(std::popcount(truth & ~reported));
        synthetic_false_negatives +=
            static_cast<std::uint64_t>(std::popcount(reported & ~tile_valid_mask(tile)));
        ++synthetic_cases;
      }
      if (tile.u >= 40U) {
        ++stress_cases;
        // kernels[1] is K1, the primary exact candidate.
        stress_false_negatives += static_cast<std::uint64_t>(std::popcount(
            truth & ~kernels[1].tile_fn(tile, synthetic_view)));
      }
    }
  }

  // -------------------------------------------------------------------------
  // Phase 9: models and verdict.
  // -------------------------------------------------------------------------
  const KernelRecord* best = nullptr;
  for (const KernelRecord& kernel : kernels) {
    if (!kernel.verdict_eligible) {
      continue;
    }
    if (best == nullptr || kernel.t_leaf_ns < best->t_leaf_ns) {
      best = &kernel;
    }
  }

  // Optimistic component-recombination bound. It pairs the cheapest measured
  // tile cost with the most selective measured candidate count, across
  // different kernels. No kernel achieves both, and the two properties are in
  // direct tension: K3 is cheap precisely because it touches no memory, which
  // is exactly why it is unselective. The bound is therefore not attainable by
  // any measured implementation; it exists to show how much headroom the whole
  // measured component set has, even under the most generous recombination.
  double cheapest_tile_ns = 0.0;
  double most_selective_candidates = 0.0;
  double most_selective_verify_ns = 0.0;
  for (const KernelRecord& kernel : kernels) {
    if (!kernel.verdict_eligible) {
      continue;
    }
    if (cheapest_tile_ns == 0.0 || kernel.tile_ns.median < cheapest_tile_ns) {
      cheapest_tile_ns = kernel.tile_ns.median;
    }
    if (most_selective_candidates == 0.0 ||
        static_cast<double>(kernel.candidate_lanes) < most_selective_candidates) {
      most_selective_candidates = static_cast<double>(kernel.candidate_lanes);
      most_selective_verify_ns = kernel.verify_ns.median;
    }
  }
  const double recombination_leaf_ns =
      static_cast<double>(geometry.tile_count) * cheapest_tile_ns +
      most_selective_candidates * most_selective_verify_ns;
  const double recombination_speedup =
      recombination_leaf_ns > 0.0 ? (pair_universe * t_pair) / recombination_leaf_ns : 0.0;
  const double recombination_work =
      static_cast<double>(geometry.tile_count) * (cheapest_tile_ns / t_pair) +
      most_selective_candidates * (most_selective_verify_ns / t_pair);

  std::string verdict;
  std::string justification;
  if (any_false_negative || synthetic_false_negatives != 0U) {
    verdict = "INCORRECT";
    justification =
        "At least one reporter dropped a true lane or reported on a padding lane. "
        "Correctness dominates every performance consideration.";
  } else if (!avx2_executed) {
    verdict = "UNRESOLVED";
    justification =
        "The packed AVX2 reporters were not executable on this host (isa_executed is "
        "portable), so no genuinely packed implementation was measured. The harness "
        "and the correctness evidence stand; the performance question is open.";
  } else if (best == nullptr) {
    verdict = "UNRESOLVED";
    justification =
        "No verdict-eligible packed reporter completed both the differential sweep and "
        "the timing protocol.";
  } else if (best->s_eff >= 30.0) {
    verdict = "PROMISING";
  } else if (best->w_eff < kBlockedBitmapModelWork) {
    verdict = "VIABLE";
  } else {
    verdict = "TOO_SLOW";
  }
  if (verdict == "PROMISING" || verdict == "VIABLE" || verdict == "TOO_SLOW") {
    justification = "Best validated verdict-eligible reporter " + best->id +
                    " after charging its own candidate verification cost.";
  }

  // -------------------------------------------------------------------------
  // Phase 10: artifacts.
  // -------------------------------------------------------------------------
  std::filesystem::path directory;
  if (output_directory.empty()) {
    directory = std::filesystem::path("results") / "microreport8" / "raw";
  } else {
    directory = std::filesystem::path(output_directory);
  }
  std::error_code directory_error;
  std::filesystem::create_directories(directory, directory_error);

  const auto ideal_work = [&](const double c8) {
    return kIdealTileModel * c8 + kIdealOutputModel;
  };

  std::ostringstream json;
  json << std::setprecision(10);
  json << "{\n";
  json << "\"schema\":\"ter_microreport8_probe\",\"schema_version\":1,\n";
  json << "\"configure_time_commit\":\"" << json_escape(TER_CONFIGURE_TIME_COMMIT)
       << "\",\"configure_time_dirty\":" << (TER_CONFIGURE_TIME_DIRTY ? "true" : "false")
       << ",\n";
  json << "\"executable_sha256\":\"not_computed\",\"executable_path\":\""
       << json_escape(argc > 0 ? argv[0] : "") << "\",\n";
  json << "\"cpu_brand\":\"" << json_escape(isa.brand) << "\",\n";
  json << "\"isa_compiled\":\"" << (avx2_compiled ? "AVX2" : "portable")
       << "\",\"isa_available\":\"" << (isa.usable ? "AVX2" : "portable")
       << "\",\"isa_executed\":\"" << (avx2_executed ? "AVX2" : "portable") << "\",\n";
  json << "\"isa_detail\":{\"cpuid\":" << (isa.cpuid_available ? "true" : "false")
       << ",\"osxsave\":" << (isa.osxsave ? "true" : "false")
       << ",\"avx\":" << (isa.avx ? "true" : "false")
       << ",\"ymm_state_enabled\":" << (isa.ymm_state_enabled ? "true" : "false")
       << ",\"avx2\":" << (isa.avx2 ? "true" : "false")
       << ",\"bmi2\":" << (isa.bmi2 ? "true" : "false")
       << ",\"avx512f\":" << (isa.avx512f ? "true" : "false") << "},\n";

  json << "\"n\":" << data->n << ",\"root_seed\":" << data->seed << ",\"r1\":" << data->r1
       << ",\"quotient_bits\":" << data->quotient_bits << ",\"k\":" << kProjectionBits
       << ",\"residual_bits\":" << kResidualBits << ",\n";
  json << "\"a_size\":" << data->a_size << ",\"b_size\":" << data->b_size
       << ",\"c_size\":" << data->c_size << ",\"pair_universe\":" << data->pair_universe
       << ",\"geometry_matches_reference\":"
       << (data->geometry_matches_reference ? "true" : "false") << ",\n";

  json << "\"target_source\":\"quotient_tau\",\"tau_low32\":" << data->tau_low32
       << ",\"tau_low25\":" << data->tau_low25 << ",\"tau_derivation_verified\":"
       << (data->tau_derivation_verified ? "true" : "false")
       << ",\"tau_verification_detail\":\"" << json_escape(data->tau_verification_detail)
       << "\",\n";

  json << "\"decomposition_crosscheck_expected\":" << crosscheck_expected
       << ",\"decomposition_crosscheck_observed\":" << geometry.crosscheck_observed
       << ",\"decomposition_crosscheck_pass\":" << (crosscheck_pass ? "true" : "false")
       << ",\"production_anchor_available\":"
       << (data->production_anchor_available ? "true" : "false")
       << ",\"production_low_prefilter_candidates\":"
       << data->production_low_prefilter_candidates
       << ",\"production_valid_root_count\":" << data->production_valid_root_count
       << ",\"production_anchor_status\":\"" << json_escape(data->production_anchor_status)
       << "\",\n";

  const auto emit_family = [&](const std::string_view name, const FamilySummary& summary) {
    json << "\"" << name << "\":{\"postings\":" << summary.postings
         << ",\"min\":" << summary.minimum << ",\"max\":" << summary.maximum
         << ",\"mean\":" << number(summary.mean) << ",\"median\":" << number(summary.median)
         << ",\"p90\":" << number(summary.p90) << ",\"p95\":" << number(summary.p95)
         << ",\"p99\":" << number(summary.p99)
         << ",\"empty_families\":" << summary.empty_families
         << ",\"distinct_values\":" << summary.distinct_values
         << ",\"duplicate_values\":" << summary.duplicate_values
         << ",\"max_multiplicity\":" << summary.max_multiplicity << ",\"sizes\":[";
    for (std::size_t index = 0U; index < kSplitRadix; ++index) {
      json << (index == 0U ? "" : ",") << summary.sizes[index];
    }
    json << "]}";
  };
  emit_family("q_families", data->q_summary);
  json << ",\n";
  emit_family("x_families", data->x_summary);
  json << ",\n";
  emit_family("y_families", data->y_summary);
  json << ",\n";

  json << "\"microproblems\":" << (kSplitRadix * kSplitRadix)
       << ",\"empty_microproblems\":" << geometry.empty_microproblems
       << ",\"ideal_unfragmented_tile_model\":" << number(kIdealTileModel)
       << ",\"tile_count_real\":" << geometry.tile_count
       << ",\"fragmentation_factor\":" << number(geometry.fragmentation)
       << ",\"full_tiles\":" << geometry.full_tiles
       << ",\"partial_tiles\":" << geometry.partial_tiles
       << ",\"valid_lanes\":" << geometry.valid_lanes
       << ",\"true_lanes\":" << geometry.true_lanes
       << ",\"mean_valid_lanes\":" << number(geometry.mean_valid_lanes)
       << ",\"median_valid_lanes\":" << number(geometry.median_valid_lanes)
       << ",\"truth_checksum\":" << geometry.truth_checksum << ",\"valid_lane_histogram\":[";
  for (std::size_t lanes = 0U; lanes <= kTileLanes; ++lanes) {
    json << (lanes == 0U ? "" : ",") << geometry.lane_histogram[lanes];
  }
  json << "],\n";

  json << "\"per_active_y_bitmap_bytes\":" << data->per_active_y_bitmap_bytes
       << ",\"all_y_bitmap_bytes\":" << data->all_y_bitmap_bytes
       << ",\"y_bitmap_policy\":\"all 64 built once before timing and retained; tiles are "
          "enumerated grouped by u so a run reuses one active bitmap\""
       << ",\"y_bitmap_build_ns\":" << data->target_build_ns << ",\n";

  json << "\"baseline_kind\":\"scalar_local_pair\",\"baseline_description\":\"(x + q + kappa) "
          "mod 2^19 followed by exact membership in the same Y_u bitmap, over the same "
          "authentic lane stream, in this executable\",\n";
  json << "\"production_pair_baseline\":\"unavailable\",\"production_pair_baseline_reason\":"
          "\"the production root pair probe is fused into ternary_tiled_l1_to_l0's visitor "
          "over 47-bit quotients and BigInt residues; no faithful isolated version exists "
          "without modifying production, so none is reported\",\n";
  json << "\"thread_affinity\":\"" << json_escape(affinity_state)
       << "\",\"clock_resolution_ns\":" << number(clock_resolution_ns)
       << ",\"clock_overhead_ns\":" << number(clock_overhead_ns)
       << ",\"timing_samples\":" << sample_count
       << ",\"benchmark_sample_tiles\":" << sample.size()
       << ",\"benchmark_sample_valid_lanes\":" << sample_valid_lanes
       << ",\"timing_stability\":\"" << timing_stability
       << "\",\"worst_relative_iqr\":" << number(worst_relative_iqr) << ",\n";
  emit_stats(json, "scalar_local_pair_ns", pair_stats);
  json << ",\n";
  emit_stats(json, "scalar_report8_ns", report8_stats);
  json << ",\n\"c_scalar8\":" << number(c_scalar8)
       << ",\"scalar_report8_per_lane_ns\":" << number(report8_per_lane_ns)
       << ",\"baseline_agreement_ratio\":" << number(baseline_agreement) << ",\n";

  json << "\"kernels\":[\n";
  for (std::size_t index = 0U; index < kernels.size(); ++index) {
    const KernelRecord& kernel = kernels[index];
    const double density =
        geometry.valid_lanes == 0U
            ? 0.0
            : static_cast<double>(kernel.candidate_lanes) /
                  static_cast<double>(geometry.valid_lanes);
    const double negatives =
        static_cast<double>(std::max<std::uint64_t>(1U, geometry.valid_lanes -
                                                            geometry.true_lanes));
    json << (index == 0U ? "" : ",\n") << "{\"kernel_id\":\"" << kernel.id
         << "\",\"description\":\"" << json_escape(kernel.description)
         << "\",\"kernel_exact\":" << (kernel.exact ? "true" : "false")
         << ",\"kernel_control_only\":" << (kernel.control_only ? "true" : "false")
         << ",\"kernel_verdict_eligible\":" << (kernel.verdict_eligible ? "true" : "false")
         << ",\"valid_lanes\":" << geometry.valid_lanes
         << ",\"true_lanes\":" << geometry.true_lanes
         << ",\"candidate_lanes\":" << kernel.candidate_lanes
         << ",\"false_positive_lanes\":" << kernel.false_positive_lanes
         << ",\"false_negative_lanes\":" << kernel.false_negative_lanes
         << ",\"padding_violations\":" << kernel.padding_violations
         << ",\"portable_avx2_mismatches\":" << kernel.portable_mismatches
         << ",\"candidate_checksum\":" << kernel.candidate_checksum
         << ",\"candidate_density\":" << number(density) << ",\"false_positive_rate\":"
         << number(static_cast<double>(kernel.false_positive_lanes) / negatives)
         << ",\"verify_stream_lanes\":" << kernel.verify_stream.size() << ",";
    emit_stats(json, "tile_ns", kernel.tile_ns);
    json << ",";
    emit_stats(json, "verify_ns", kernel.verify_ns);
    json << ",\"C8\":" << number(kernel.c8) << ",\"Cverify\":" << number(kernel.c_verify)
         << ",\"W_ideal_model_only\":" << number(ideal_work(kernel.c8))
         << ",\"S_ideal_model_only\":" << number(pair_universe / ideal_work(kernel.c8))
         << ",\"W_eff\":" << number(kernel.w_eff) << ",\"S_eff\":" << number(kernel.s_eff)
         << ",\"projected_microreport_leaf_ns\":" << number(kernel.t_leaf_ns)
         << ",\"projected_leaf_speedup\":" << number(kernel.s_leaf) << "}";
  }
  json << "\n],\n";

  const double projected_current_pair_ns = pair_universe * t_pair;
  json << "\"projected_current_pair_ns\":" << number(projected_current_pair_ns)
       << ",\"projected_microreport_leaf_ns\":"
       << (best != nullptr ? number(best->t_leaf_ns) : std::string("null"))
       << ",\"projected_leaf_speedup\":"
       << (best != nullptr ? number(best->s_leaf) : std::string("null"))
       << ",\"projection_class\":\"PROJECTED ISOLATED FRONT-END, NOT FULLTER WALL CLOCK\",\n";
  json << "\"optimistic_recombination_bound\":{\"note\":\"combines the cheapest measured "
          "tile cost with the most selective measured candidate count across DIFFERENT "
          "kernels; no measured kernel achieves both and the two properties are in "
          "direct tension, so this is an unattainable upper bound on the measured "
          "component set, not a result\",\"cheapest_tile_ns\":"
       << number(cheapest_tile_ns)
       << ",\"most_selective_candidate_lanes\":" << number(most_selective_candidates)
       << ",\"leaf_ns\":" << number(recombination_leaf_ns)
       << ",\"W_eff\":" << number(recombination_work)
       << ",\"speedup\":" << number(recombination_speedup) << "},\n";
  json << "\"blocked_bitmap_reference_work\":" << number(kBlockedBitmapModelWork)
       << ",\"blocked_bitmap_reference_class\":\"MODEL_ONLY\""
       << ",\"blocked_bitmap_reference_note\":\"a prior logical-work model over different "
          "primitive work classes; it is NOT calibrated to measured nanoseconds and beating "
          "it in pair-probe-equivalent units does not demonstrate wall-clock superiority "
          "over any real blocked-bitmap implementation\",\n";

  json << "\"synthetic_correctness\":{\"cases\":" << synthetic_cases
       << ",\"false_negative_or_padding_lanes\":" << synthetic_false_negatives
       << ",\"oracle_vs_brute_force_mismatches\":" << synthetic_brute_mismatches << "},\n";
  json << "\"synthetic_stress\":{\"dense_target_cases\":" << stress_cases
       << ",\"false_negative_lanes\":" << stress_false_negatives << "},\n";

  json << "\"best_kernel\":"
       << (best != nullptr ? "\"" + best->id + "\"" : std::string("null"))
       << ",\"best_kernel_exact_or_superset\":"
       << (best != nullptr ? (best->exact ? "\"exact\"" : "\"superset\"") : std::string("null"))
       << ",\n";
  json << "\"verdict\":\"" << verdict << "\",\"verdict_justification\":\""
       << json_escape(justification) << "\",\"final_checksum\":" << g_sink << "\n";
  json << "}\n";

  const std::filesystem::path json_path = directory / "microreport8.json";
  {
    std::ofstream stream(json_path, std::ios::binary);
    stream << json.str();
  }
  const std::filesystem::path csv_path = directory / "microreport8_kernels.csv";
  {
    std::ofstream stream(csv_path, std::ios::binary);
    stream << "kernel_id,exact,control_only,verdict_eligible,valid_lanes,true_lanes,"
              "candidate_lanes,false_positive_lanes,false_negative_lanes,padding_violations,"
              "candidate_density,tile_ns_median,tile_ns_iqr,verify_ns_median,C8,Cverify,"
              "W_eff,S_eff,projected_leaf_ns,projected_leaf_speedup\n";
    for (const KernelRecord& kernel : kernels) {
      stream << kernel.id << ',' << (kernel.exact ? 1 : 0) << ','
             << (kernel.control_only ? 1 : 0) << ',' << (kernel.verdict_eligible ? 1 : 0) << ','
             << geometry.valid_lanes << ',' << geometry.true_lanes << ','
             << kernel.candidate_lanes << ',' << kernel.false_positive_lanes << ','
             << kernel.false_negative_lanes << ',' << kernel.padding_violations << ','
             << number(static_cast<double>(kernel.candidate_lanes) /
                       static_cast<double>(geometry.valid_lanes))
             << ',' << number(kernel.tile_ns.median) << ',' << number(kernel.tile_ns.iqr) << ','
             << number(kernel.verify_ns.median) << ',' << number(kernel.c8) << ','
             << number(kernel.c_verify) << ',' << number(kernel.w_eff) << ','
             << number(kernel.s_eff) << ',' << number(kernel.t_leaf_ns) << ','
             << number(kernel.s_leaf) << '\n';
    }
  }

  // -------------------------------------------------------------------------
  // Human-readable summary.
  // -------------------------------------------------------------------------
  std::cout << "\n=== AUTHENTIC_N72 ===\n";
  std::cout << "tiles=" << geometry.tile_count << "  ideal_model=" << number(kIdealTileModel)
            << "  F_tile=" << number(geometry.fragmentation) << "  full=" << geometry.full_tiles
            << "  partial=" << geometry.partial_tiles << '\n';
  std::cout << "valid_lanes=" << geometry.valid_lanes << "  true_lanes=" << geometry.true_lanes
            << "  mean_valid/tile=" << number(geometry.mean_valid_lanes)
            << "  median_valid/tile=" << number(geometry.median_valid_lanes) << '\n';
  std::cout << "scalar_local_pair_ns median=" << number(pair_stats.median)
            << " iqr=" << number(pair_stats.iqr) << "  scalar_report8_ns median="
            << number(report8_stats.median) << "  C_scalar8=" << number(c_scalar8) << '\n';
  std::cout << "baseline self-consistency: ScalarReport8 per lane = "
            << number(report8_per_lane_ns) << " ns, ratio to scalar_local_pair = "
            << number(baseline_agreement) << " (1.0 means the two agree)\n";
  std::cout << "timing_stability=" << timing_stability
            << " (worst relative IQR " << number(worst_relative_iqr) << ")\n\n";
  std::cout << std::left << std::setw(22) << "kernel" << std::setw(12) << "tile_ns"
            << std::setw(10) << "C8" << std::setw(14) << "candidates" << std::setw(11)
            << "density" << std::setw(10) << "Cverify" << std::setw(14) << "W_eff"
            << std::setw(10) << "S_eff" << "eligible\n";
  for (const KernelRecord& kernel : kernels) {
    std::cout << std::left << std::setw(22) << kernel.id << std::setw(12)
              << number(kernel.tile_ns.median) << std::setw(10) << number(kernel.c8)
              << std::setw(14) << kernel.candidate_lanes << std::setw(11)
              << number(static_cast<double>(kernel.candidate_lanes) /
                        static_cast<double>(geometry.valid_lanes))
              << std::setw(10) << number(kernel.c_verify) << std::setw(14)
              << number(kernel.w_eff) << std::setw(10) << number(kernel.s_eff)
              << (kernel.control_only ? "control-only"
                                      : (kernel.verdict_eligible ? "yes" : "no"))
              << '\n';
  }
  std::cout << "\n--- MEASURED / PROJECTED FROM SAME-EXECUTABLE TIMINGS ---\n";
  std::cout << "T_current_pair = P0 * t_pair = " << number(projected_current_pair_ns / 1.0e6)
            << " ms\n";
  if (best != nullptr) {
    std::cout << "T_leaf(" << best->id << ") = " << number(best->t_leaf_ns / 1.0e6)
              << " ms   S_leaf = " << number(best->s_leaf) << "x\n";
  }
  std::cout << "PROJECTED ISOLATED FRONT-END ONLY. NOT a FullTER wall-clock speedup.\n";
  std::cout << "optimistic recombination bound (cheapest measured tile + most selective\n"
               "measured mask, achieved by no single kernel and mutually in tension):\n"
               "  W_eff <= " << number(recombination_work) << "   S_eff <= "
            << number(recombination_speedup) << "x\n";
  std::cout << "\n--- MODEL_ONLY BLOCKED-BITMAP COMPARISON ---\n";
  std::cout << "W_BB = " << number(kBlockedBitmapModelWork)
            << " pair-equivalent units (prior logical-work model, different primitive\n"
               "classes, NOT calibrated to measured nanoseconds here).\n";
  std::cout << "\n=== SYNTHETIC_CORRECTNESS ===\ncases=" << synthetic_cases
            << " false_negative_or_padding_lanes=" << synthetic_false_negatives
            << " oracle_vs_brute_mismatches=" << synthetic_brute_mismatches << '\n';
  std::cout << "=== SYNTHETIC_STRESS ===\ndense_target_cases=" << stress_cases
            << " false_negative_lanes=" << stress_false_negatives << '\n';
  std::cout << "\nVERDICT: " << verdict << '\n' << justification << '\n';
  std::cout << "artifacts: " << json_path.string() << " , " << csv_path.string() << '\n';
  return EXIT_SUCCESS;
}
