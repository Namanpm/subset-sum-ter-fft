// A64-CHQR root benchmark (TER_n108_A64_CHQR_Compiler_God_Spec).
//
// One snapshot per fresh process.  Per run:
//   1. load the packed root snapshot (checksummed) and the authority tables,
//      derive the full Q-bit keys (never timed);
//   2. build the shared Y bitmaps + group index, select a declared
//      deterministic microproblem subset (never timed);
//   3. exactness, untimed: frozen K2 (25-bit filter -> exact sink authority),
//      K2_WIDE (frozen K2 kernel + full-key check before the sink) and A64.
//      A64 == K2_WIDE on every triple hash; every A64 triple passes the low-32
//      and residue authorities; A64's residue/structural survivors, with their
//      multiset hash, equal frozen K2's.  Any disagreement: INVALID, no timing;
//   4. optional width sweep (spec 11.2): A64 refined to k bits, single untimed
//      runs, projected hits vs K2_WIDE's measured histogram vs 2^-(k-25);
//   5. matched timing: warmups, then samples alternating
//      K2 -> K2_WIDE -> A64 and A64 -> K2_WIDE -> K2.
//
// Kill criterion (spec 20): refinement work (digit pairs + directly resolved
// pairs) within 10x of the logical pair count |Q_s||X_t| is pair-bound.

#include "ter_a64_chqr.hpp"
#include "ter_microreport8_kernels.hpp"
#include "ter_root_generalization_workload.hpp"
#include "ter_root_queue_kernels.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif

namespace {

namespace a64 = ter::a64;
namespace rg = ter::root_generalization;
namespace mr8 = ter::microreport8;

using Clock = std::chrono::steady_clock;

volatile std::uint64_t g_sink = 0U;

constexpr double kKillRatio = 0.1;

[[nodiscard]] double ms_since(const Clock::time_point start) noexcept {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

[[nodiscard]] std::string fixed(const double value, const int digits = 3) {
  std::ostringstream text;
  text << std::fixed << std::setprecision(digits) << value;
  return text.str();
}

[[nodiscard]] std::string hex(const std::uint64_t value) {
  std::ostringstream text;
  text << "0x" << std::hex << value;
  return text.str();
}

[[nodiscard]] int ac_line_status() noexcept {
#if defined(_WIN32)
  SYSTEM_POWER_STATUS status{};
  if (GetSystemPowerStatus(&status) == 0) {
    return -1;
  }
  return static_cast<int>(status.ACLineStatus);
#else
  return -1;
#endif
}

[[nodiscard]] double peak_working_set_mib() noexcept {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS counters{};
  if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) == 0) {
    return 0.0;
  }
  return static_cast<double>(counters.PeakWorkingSetSize) / (1024.0 * 1024.0);
#else
  return 0.0;
#endif
}

struct Stats {
  double median = 0.0;
  double p25 = 0.0;
  double p75 = 0.0;
  double minimum = 0.0;
  double maximum = 0.0;
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

[[nodiscard]] Stats summarize(std::vector<double> samples) {
  Stats stats;
  if (samples.empty()) {
    return stats;
  }
  std::sort(samples.begin(), samples.end());
  stats.samples = samples.size();
  stats.minimum = samples.front();
  stats.maximum = samples.back();
  stats.p25 = quantile(samples, 0.25);
  stats.median = quantile(samples, 0.50);
  stats.p75 = quantile(samples, 0.75);
  return stats;
}

struct Config {
  std::string snapshot;
  std::string out_dir = "results/a64-chqr";
  std::string label;
  std::uint32_t microproblems = 4096U;
  std::size_t warmups = 3U;
  std::size_t samples = 7U;
  std::uint64_t threshold = 128U;
  std::uint32_t digit_bits = 13U;
  std::uint32_t a64_k = 0U;  // 0: F
  bool width_sweep = false;
  bool timing = true;
  bool with_queue = false;              // current QUEUE_DENSE entity, where its capacity allows
  std::vector<std::uint64_t> threshold_scan;
};

[[nodiscard]] bool parse(int argc, char** argv, Config& config) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const auto value = [&](const std::string_view prefix) {
      return std::string(arg.substr(prefix.size()));
    };
    if (arg.rfind("--snapshot=", 0) == 0) {
      config.snapshot = value("--snapshot=");
    } else if (arg.rfind("--out-dir=", 0) == 0) {
      config.out_dir = value("--out-dir=");
    } else if (arg.rfind("--label=", 0) == 0) {
      config.label = value("--label=");
    } else if (arg.rfind("--microproblems=", 0) == 0) {
      config.microproblems = static_cast<std::uint32_t>(std::stoul(value("--microproblems=")));
    } else if (arg.rfind("--warmups=", 0) == 0) {
      config.warmups = std::stoul(value("--warmups="));
    } else if (arg.rfind("--samples=", 0) == 0) {
      config.samples = std::stoul(value("--samples="));
    } else if (arg.rfind("--threshold=", 0) == 0) {
      config.threshold = std::stoull(value("--threshold="));
    } else if (arg.rfind("--digit-bits=", 0) == 0) {
      config.digit_bits = static_cast<std::uint32_t>(std::stoul(value("--digit-bits=")));
    } else if (arg.rfind("--a64-k=", 0) == 0) {
      config.a64_k = static_cast<std::uint32_t>(std::stoul(value("--a64-k=")));
    } else if (arg == "--width-sweep") {
      config.width_sweep = true;
    } else if (arg == "--no-timing") {
      config.timing = false;
    } else if (arg == "--with-queue") {
      config.with_queue = true;
    } else if (arg.rfind("--threshold-scan=", 0) == 0) {
      std::stringstream list(value("--threshold-scan="));
      std::string item;
      while (std::getline(list, item, ',')) {
        config.threshold_scan.push_back(std::stoull(item));
      }
    } else {
      std::cerr << "unknown argument: " << arg << '\n';
      return false;
    }
  }
  return !config.snapshot.empty() && config.digit_bits >= 1U && config.digit_bits <= 16U;
}

struct Entity {
  std::string name;
  std::function<std::uint64_t()> run;
  std::vector<double> samples;
};

[[nodiscard]] std::string tasks_text(const a64::Telemetry& t, const std::uint32_t levels) {
  std::string text;
  for (std::uint32_t level = 0U; level <= levels; ++level) {
    text += (level == 0U ? "" : "/") + std::to_string(t.tasks[level]);
  }
  return text;
}

[[nodiscard]] double refine_ratio(const a64::Telemetry& t) {
  return t.logical_pairs == 0U ? 0.0
                               : static_cast<double>(t.digit_pairs + t.direct_pairs) /
                                     static_cast<double>(t.logical_pairs);
}

}  // namespace

int main(int argc, char** argv) {
  Config config;
  if (!parse(argc, argv, config)) {
    std::cerr << "usage: ter_a64_chqr_bench --snapshot=PATH [--microproblems=K] [--warmups=W]"
                 " [--samples=S] [--threshold=T] [--digit-bits=B] [--a64-k=K] [--width-sweep]"
                 " [--no-timing] [--out-dir=DIR] [--label=L]\n";
    return 2;
  }
  const int ac_start = ac_line_status();
  std::filesystem::create_directories(config.out_dir);

  // --- workload (never timed) ---------------------------------------------
  const auto load_start = Clock::now();
  const auto workload = rg::load_snapshot_workload(config.snapshot, /*with_authority=*/true);
  const double load_s = ms_since(load_start) / 1000.0;
  std::cout << "# snapshot " << config.snapshot << " : " << rg::status_label(workload->status)
            << (workload->status_detail.empty() ? "" : " (" + workload->status_detail + ")") << '\n';
  if (workload->status != rg::WorkloadStatus::Completed || !workload->authority.ok) {
    return 3;
  }
  const a64::FamilyKeys keys = a64::build_family_keys(*workload);
  if (!keys.ok) {
    std::cout << "# NotApplicable: full keys unavailable (Q=" << workload->full_quotient_bits << ")\n";
    return 3;
  }
  const mr8::Dataset& data = *workload->data;
  rg::RootIndex index;
  rg::build_root_index(data, index);
  rg::select_microproblems(index, config.microproblems);
  const std::string id =
      (config.label.empty() ? workload->id : config.label) + "_mp" + std::to_string(index.selected_count);
  std::cout << "# n=" << workload->n << " seed=" << hex(workload->seed) << " |A|=" << workload->a_size
            << " |B|=" << workload->b_size << " |C|=" << workload->c_size << " Q=" << keys.full_bits
            << " F=" << keys.fast_bits << " W=" << keys.residual_bits << " hi=" << keys.hi_bits
            << " microproblems=" << index.selected_count << " load_s=" << fixed(load_s, 1) << '\n';
  std::cout.flush();

  const std::uint32_t full_k = keys.fast_bits;
  const std::uint32_t a64_k = config.a64_k == 0U ? full_k : std::min(config.a64_k, full_k);
  a64::Config a64_config;
  a64_config.profile = a64::default_profile(a64_k - mr8::kSplitBits, config.digit_bits);
  a64_config.direct_threshold = config.threshold;
  std::string profile_text;
  for (std::uint32_t level = 0U; level < a64_config.profile.levels; ++level) {
    profile_text += (level == 0U ? "" : "-") + std::to_string(a64_config.profile.widths[level]);
  }

  const auto make_sink = [&]() {
    rg::RootSink sink;
    sink.data = &data;
    sink.index = &index;
    sink.authority = &workload->authority;
    sink.depth = rg::SinkDepth::Full;
    sink.reset();
    return sink;
  };

  // --- exactness (untimed) -------------------------------------------------
  rg::RootSink k2 = make_sink();
  auto start = Clock::now();
  rg::run_k2(data, k2);
  const double k2_once = ms_since(start);
  rg::RootSink wide = make_sink();
  a64::Telemetry wide_t;
  start = Clock::now();
  a64::run_k2_wide(data, index, keys, wide, &wide_t, /*sweep=*/true);
  const double wide_once = ms_since(start);
  rg::RootSink tree = make_sink();
  a64::Telemetry tree_t;
  start = Clock::now();
  a64::run_a64(data, index, keys, a64_config, tree, &tree_t);
  const double a64_once = ms_since(start);

  // Authentic cross-check against the donor low-32 quotient authority: A64
  // refined to exactly 32 bits must find exactly frozen K2's full_q survivors.
  a64::Config k32_config = a64_config;
  k32_config.profile = a64::default_profile(32U - mr8::kSplitBits, config.digit_bits);
  rg::RootSink k32 = make_sink();
  a64::Telemetry k32_t;
  a64::run_a64(data, index, keys, k32_config, k32, &k32_t);

  const rg::OutputDigest& dk = k2.digest;
  const rg::OutputDigest& dw = wide.digest;
  const rg::OutputDigest& da = tree.digest;
  const bool same_wide = da.triples == dw.triples && da.triple_sum == dw.triple_sum &&
                         da.triple_xor == dw.triple_xor && da.triple_sum2 == dw.triple_sum2;
  const bool authority_clean = da.triples == da.full_q && da.triples == da.residue;
  const bool same_residue = da.residue == dk.residue && da.residue_sum == dk.residue_sum &&
                            da.structural == dk.structural && dw.residue == dk.residue &&
                            dw.residue_sum == dk.residue_sum;
  const bool clean = dk.directory_misses == 0U && dk.residual_mismatches == 0U &&
                     dw.directory_misses == 0U && dw.residual_mismatches == 0U;
  const bool projections = wide_t.projected_hits == dk.triples && k32_t.projected_hits == dk.full_q;
  const bool exact = same_wide && authority_clean && same_residue && clean && projections &&
                     tree_t.full_hits == da.triples && wide_t.full_hits == dw.triples;
  std::cout << "# exactness " << (exact ? "EXACT" : "DIVERGED") << " | frozen K2: triples(25-bit)="
            << dk.triples << " fullQ32=" << dk.full_q << " residue=" << dk.residue
            << " structural=" << dk.structural << " | K2_WIDE: triples=" << dw.triples
            << " | A64: triples=" << da.triples << " residue=" << da.residue
            << " structural=" << da.structural << " | K2_WIDE hits25=" << wide_t.projected_hits
            << " A64(k=32) hits32=" << k32_t.projected_hits << '\n';
  std::cout << "# single-shot ms: K2 " << fixed(k2_once, 1) << " | K2_WIDE " << fixed(wide_once, 1)
            << " | A64 " << fixed(a64_once, 1) << '\n';
  std::cout << "# A64 k=" << a64_k << " digits=" << profile_text << " threshold=" << config.threshold
            << " tasks/level=" << tasks_text(tree_t, a64_config.profile.levels)
            << " digit_pairs=" << tree_t.digit_pairs << " direct_pairs=" << tree_t.direct_pairs
            << " logical_pairs=" << tree_t.logical_pairs << " refine/logical="
            << fixed(refine_ratio(tree_t), 4) << " projected=" << tree_t.projected_hits
            << " fast=" << tree_t.fast_hits << " full=" << tree_t.full_hits
            << " index_MiB=" << fixed(static_cast<double>(tree_t.index_bytes) / 1048576.0, 1) << '\n';
  std::cout.flush();

  // --- width sweep ----------------------------------------------------------
  const std::uint64_t n25 = wide_t.projected_hits;
  std::ofstream sweep_out;
  if (config.width_sweep) {
    sweep_out.open(config.out_dir + "/" + id + "_width_sweep.csv");
    sweep_out << "id,k,projected_measured_k2wide,projected_measured_a64,model_2pow,ratio_to_model,"
                 "fast_hits,full_hits,digit_pairs,direct_pairs,logical_pairs,refine_ratio,tasks,"
                 "a64_single_ms\n";
    const std::uint32_t widths[] = {25U, 29U, 33U, 37U, 41U, 45U, 48U, 49U, 52U, 56U, 60U, 64U};
    for (const std::uint32_t k : widths) {
      if (k > full_k) {
        continue;
      }
      std::uint64_t measured = 0U;
      for (std::uint32_t c = k - mr8::kSplitBits; c <= keys.residual_bits; ++c) {
        measured += wide_t.sweep[c];
      }
      a64::Config sweep_config = a64_config;
      sweep_config.profile = a64::default_profile(k - mr8::kSplitBits, config.digit_bits);
      rg::RootSink sweep_sink = make_sink();
      a64::Telemetry t;
      start = Clock::now();
      a64::run_a64(data, index, keys, sweep_config, sweep_sink, &t);
      const double ms = ms_since(start);
      const bool same = sweep_sink.digest.triple_sum == da.triple_sum &&
                        sweep_sink.digest.triples == da.triples && t.projected_hits == measured;
      const double model = static_cast<double>(n25) / std::ldexp(1.0, static_cast<int>(k - 25U));
      sweep_out << id << ',' << k << ',' << measured << ',' << t.projected_hits << ','
                << fixed(model, 1) << ',' << fixed(model > 0.0 ? static_cast<double>(measured) / model : 0.0, 4)
                << ',' << t.fast_hits << ',' << t.full_hits << ',' << t.digit_pairs << ','
                << t.direct_pairs << ',' << t.logical_pairs << ',' << fixed(refine_ratio(t), 4) << ','
                << tasks_text(t, sweep_config.profile.levels) << ',' << fixed(ms, 1) << '\n';
      std::cout << "# sweep k=" << k << " projected=" << measured << " model=" << fixed(model, 1)
                << " refine/logical=" << fixed(refine_ratio(t), 4) << " a64_ms=" << fixed(ms, 1)
                << (same ? "" : "  [MISMATCH]") << '\n';
      std::cout.flush();
      if (!same) {
        std::cout << "# INVALID: width-sweep run disagrees\n";
        return 4;
      }
    }
  }

  if (!exact) {
    std::cout << "# INVALID: backend outputs disagree; no timing recorded\n";
    return 4;
  }

  // --- direct-resolution threshold scan (single untimed-protocol runs) ------
  if (!config.threshold_scan.empty()) {
    std::ofstream scan_out(config.out_dir + "/" + id + "_threshold_scan.csv");
    scan_out << "id,k,threshold,a64_single_ms,tasks,digit_pairs,direct_pairs,refine_ratio\n";
    for (const std::uint64_t threshold : config.threshold_scan) {
      a64::Config scan_config = a64_config;
      scan_config.direct_threshold = threshold;
      rg::RootSink scan_sink = make_sink();
      a64::Telemetry t;
      start = Clock::now();
      a64::run_a64(data, index, keys, scan_config, scan_sink, &t);
      const double ms = ms_since(start);
      const bool same = scan_sink.digest.triple_sum == da.triple_sum &&
                        scan_sink.digest.triples == da.triples &&
                        scan_sink.digest.residue_sum == dk.residue_sum;
      scan_out << id << ',' << a64_k << ',' << threshold << ',' << fixed(ms, 1) << ','
               << tasks_text(t, scan_config.profile.levels) << ',' << t.digit_pairs << ','
               << t.direct_pairs << ',' << fixed(refine_ratio(t), 4) << '\n';
      std::cout << "# threshold=" << threshold << " a64_ms=" << fixed(ms, 1)
                << " tasks/level=" << tasks_text(t, scan_config.profile.levels)
                << " digit_pairs=" << t.digit_pairs << " direct_pairs=" << t.direct_pairs
                << (same ? "" : "  [MISMATCH]") << '\n';
      std::cout.flush();
      if (!same) {
        std::cout << "# INVALID: threshold-scan run disagrees\n";
        return 4;
      }
    }
  }

  // --- optional current queue (QUEUE_DENSE), only inside its capacity ---------
  rg::RadixTables tables;
  bool queue_ok = false;
  std::string queue_status = "not requested";
  if (config.with_queue) {
    std::uint64_t max_x = 0U;
    std::uint64_t max_y = 0U;
    for (std::uint32_t family = 0U; family < rg::kFamilies; ++family) {
      max_x = std::max<std::uint64_t>(max_x, index.x_groups[family].size());
      max_y = std::max<std::uint64_t>(max_y, index.y_groups[family].size());
    }
    if (max_x > rg::RadixGeom::kHitCapacity || max_y > rg::RadixGeom::kHitCapacity) {
      queue_status = "NotApplicable (capacity " + std::to_string(rg::RadixGeom::kHitCapacity) +
                     "; max X " + std::to_string(max_x) + ", max Y " + std::to_string(max_y) + ")";
    } else {
      tables.allocate();
      rg::RootSink queue_sink = make_sink();
      rg::run_radix(data, index, tables, rg::RadixRoute::QueueDense, rg::RadixStop::Full, queue_sink);
      queue_ok = tables.built && queue_sink.digest.triples == dk.triples &&
                 queue_sink.digest.triple_sum == dk.triple_sum &&
                 queue_sink.digest.residue_sum == dk.residue_sum &&
                 queue_sink.digest.full_q == dk.full_q;
      queue_status = queue_ok ? "EXACT (== frozen K2)" : "DIVERGED";
      if (!queue_ok) {
        std::cout << "# INVALID: QUEUE_DENSE disagrees with frozen K2\n";
        return 4;
      }
    }
    std::cout << "# queue_dense " << queue_status << '\n';
  }

  // --- matched timing -------------------------------------------------------
  std::vector<Entity> entities;
  rg::RootSink timed = make_sink();
  entities.push_back(Entity{"k2", [&]() {
                              timed.reset();
                              rg::run_k2(data, timed);
                              return timed.digest.residue_sum;
                            }, {}});
  entities.push_back(Entity{"k2_wide", [&]() {
                              timed.reset();
                              a64::run_k2_wide(data, index, keys, timed, nullptr, false);
                              return timed.digest.residue_sum;
                            }, {}});
  entities.push_back(Entity{"a64", [&]() {
                              timed.reset();
                              a64::run_a64(data, index, keys, a64_config, timed, nullptr);
                              return timed.digest.residue_sum;
                            }, {}});
  if (queue_ok) {
    entities.push_back(Entity{"queue_dense", [&]() {
                                timed.reset();
                                rg::run_radix(data, index, tables, rg::RadixRoute::QueueDense,
                                              rg::RadixStop::Full, timed);
                                return timed.digest.residue_sum;
                              }, {}});
  }
  std::ofstream samples_out(config.out_dir + "/" + id + "_samples.csv");
  samples_out << "id,entity,sample,position,ms\n";
  if (config.timing) {
    const std::size_t rounds = config.warmups + config.samples;
    for (std::size_t round = 0U; round < rounds; ++round) {
      const bool record = round >= config.warmups;
      for (std::size_t position = 0U; position < entities.size(); ++position) {
        Entity& entity =
            entities[(round & 1U) == 0U ? position : entities.size() - 1U - position];
        start = Clock::now();
        const std::uint64_t checksum = entity.run();
        const double ms = ms_since(start);
        g_sink = g_sink + checksum;
        if (checksum != dk.residue_sum) {
          std::cout << "# INVALID: timed " << entity.name << " residue hash drifted\n";
          return 4;
        }
        if (record) {
          entity.samples.push_back(ms);
          samples_out << id << ',' << entity.name << ',' << round - config.warmups << ',' << position
                      << ',' << fixed(ms, 4) << '\n';
        }
      }
      samples_out.flush();
    }
  }
  const Stats sk = summarize(entities[0].samples);
  const Stats sw = summarize(entities[1].samples);
  const Stats sa = summarize(entities[2].samples);
  const Stats sq = queue_ok ? summarize(entities[3].samples) : Stats{};
  const int ac_end = ac_line_status();
  const bool power_ok = ac_start == 1 && ac_end == 1;
  const bool killed = refine_ratio(tree_t) >= kKillRatio;

  const std::string summary_path = config.out_dir + "/summary.csv";
  const bool fresh = !std::filesystem::exists(summary_path);
  std::ofstream summary(summary_path, std::ios::app);
  if (fresh) {
    summary << "id,n,seed,a_size,b_size,c_size,Q,F,W,hi_bits,microproblems,a64_k,digit_profile,"
               "threshold,exactness,k2_triples25,k2_fullq32,residue,structural,residue_sum,"
               "a64_tasks,a64_digit_pairs,a64_direct_pairs,logical_pairs,refine_ratio,kill,"
               "a64_projected,a64_fast,a64_full,a64_index_mib,k25_projected,"
               "k2_median_ms,k2_p25_ms,k2_p75_ms,k2_wide_median_ms,k2_wide_p25_ms,k2_wide_p75_ms,"
               "a64_median_ms,a64_p25_ms,a64_p75_ms,samples,warmups,speedup_k2_over_a64,"
               "speedup_k2_over_k2wide,speedup_k2wide_over_a64,queue_status,queue_median_ms,queue_p25_ms,"
               "queue_p75_ms,peak_ws_mib,ac_start,ac_end,power_ok\n";
  }
  const auto ratio = [](const double num, const double den) { return den > 0.0 ? num / den : 0.0; };
  summary << id << ',' << workload->n << ',' << hex(workload->seed) << ',' << workload->a_size << ','
          << workload->b_size << ',' << workload->c_size << ',' << keys.full_bits << ','
          << keys.fast_bits << ',' << keys.residual_bits << ',' << keys.hi_bits << ','
          << index.selected_count << ',' << a64_k << ',' << profile_text << ',' << config.threshold
          << ',' << (exact ? "EXACT" : "DIVERGED") << ',' << dk.triples << ',' << dk.full_q << ','
          << dk.residue << ',' << dk.structural << ',' << hex(dk.residue_sum) << ','
          << tasks_text(tree_t, a64_config.profile.levels) << ',' << tree_t.digit_pairs << ','
          << tree_t.direct_pairs << ',' << tree_t.logical_pairs << ',' << fixed(refine_ratio(tree_t), 4)
          << ',' << (killed ? "PAIR_BOUND" : "PASS") << ',' << tree_t.projected_hits << ','
          << tree_t.fast_hits << ',' << tree_t.full_hits << ','
          << fixed(static_cast<double>(tree_t.index_bytes) / 1048576.0, 1) << ',' << n25 << ','
          << fixed(sk.median) << ',' << fixed(sk.p25) << ',' << fixed(sk.p75) << ','
          << fixed(sw.median) << ',' << fixed(sw.p25) << ',' << fixed(sw.p75) << ','
          << fixed(sa.median) << ',' << fixed(sa.p25) << ',' << fixed(sa.p75) << ',' << sk.samples
          << ',' << config.warmups << ',' << fixed(ratio(sk.median, sa.median), 4) << ','
          << fixed(ratio(sk.median, sw.median), 4) << ',' << fixed(ratio(sw.median, sa.median), 4)
          << ',' << queue_status << ',' << fixed(sq.median) << ',' << fixed(sq.p25) << ','
          << fixed(sq.p75) << ',' << fixed(peak_working_set_mib(), 1) << ',' << ac_start << ',' << ac_end << ','
          << (power_ok ? 1 : 0) << '\n';

  std::cout << "# K2 " << fixed(sk.median) << " ms [" << fixed(sk.p25) << ", " << fixed(sk.p75)
            << "] | K2_WIDE " << fixed(sw.median) << " ms [" << fixed(sw.p25) << ", "
            << fixed(sw.p75) << "] | A64 " << fixed(sa.median) << " ms [" << fixed(sa.p25) << ", "
            << fixed(sa.p75) << "] | K2/A64 = " << fixed(ratio(sk.median, sa.median), 3)
            << "x | K2/K2_WIDE = " << fixed(ratio(sk.median, sw.median), 3)
            << "x" << (queue_ok ? " | QUEUE " + fixed(sq.median) + " ms" : std::string())
            << " | kill=" << (killed ? "PAIR_BOUND" : "PASS")
            << " | peak_ws_MiB=" << fixed(peak_working_set_mib(), 0)
            << (power_ok ? "" : " [POWER NOT AC]") << '\n';
  return 0;
}
