// TER^2 root-queue generalization benchmark.
//
// Question: does branch-free queue compaction + the radix-13 exact dense leaf
// (QUEUE_DENSE) beat the frozen K2 frontend across genuine TER roots and across
// controlled changes in residual density, and can the crossover be predicted
// from workload telemetry?
//
// Per workload cell:
//   1. build/capture the workload ONCE (never timed);
//   2. build the shared Y bitmaps + group index ONCE (never timed);
//   3. exactness: oracle == K2 == QUEUE_DENSE (== LANE_SKIP where run), on
//      counts, multiset hashes of pairs / groups / entry triples and, while the
//      output is small enough, on the sorted exact pair keys;
//   4. untimed telemetry (guard words, nonzero words, candidate bits, popcount
//      distribution, K2 coarse candidates);
//   5. matched timing: warmups, then samples alternating K2->QUEUE and
//      QUEUE->K2, phase controls rotated after them;
//   6. one CSV row per cell, one row per sample, AC power sampled per cell.
//
// A cell whose outputs disagree is INVALID and carries no timing.  A cell whose
// power state was not AC at both ends is recorded but excluded from aggregates.

#include "ter/limitations.hpp"
#include "ter_microreport8_kernels.hpp"
#include "ter_radix13_packed.hpp"
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
#endif

namespace {

namespace rg = ter::root_generalization;
namespace mr8 = ter::microreport8;
namespace pk = ter::radix13_packed;
namespace tt = ter::translated_tag;

using Clock = std::chrono::steady_clock;

volatile std::uint64_t g_sink = 0U;

constexpr std::uint64_t kReferenceSeed = mr8::kReferenceRootSeed;
constexpr std::uint64_t kDeclaredSeeds[] = {0x1001ULL, 0x2003ULL, 0x3007ULL, 0x4009ULL,
                                            0x500BULL, 0x600DULL, 0x700FULL};

// The n=72 anchor, measured before this experiment existed.  The anchor cell of
// this harness must reproduce these exact counts; that is the identity gate
// proving this harness observes the same root and the same output.
constexpr std::uint64_t kAnchorPairs = 619303U;
constexpr std::uint64_t kAnchorTriples = 619579U;
constexpr std::uint64_t kAnchorGroups = 618857U;
constexpr std::uint64_t kAnchorFullQ = 4718U;
constexpr std::uint64_t kAnchorNonzeroWords = 33310740U;
constexpr std::uint64_t kAnchorCandidateBits = 37370795U;

[[nodiscard]] double ms_since(const Clock::time_point start) noexcept {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
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

[[nodiscard]] std::string fixed(const double value, const int digits = 4) {
  std::ostringstream text;
  text << std::fixed << std::setprecision(digits) << value;
  return text.str();
}

[[nodiscard]] std::string hex(const std::uint64_t value) {
  std::ostringstream text;
  text << "0x" << std::hex << value;
  return text.str();
}

// AC power, sampled at the start and end of every cell.
[[nodiscard]] int ac_line_status() noexcept {
#if defined(_WIN32)
  SYSTEM_POWER_STATUS status{};
  if (GetSystemPowerStatus(&status) == 0) {
    return -1;
  }
  return static_cast<int>(status.ACLineStatus);  // 1 = AC, 0 = battery, 255 = unknown
#else
  return -1;
#endif
}

// ---------------------------------------------------------------------------
// Configuration.
// ---------------------------------------------------------------------------
struct Config {
  std::size_t samples = 7U;
  std::size_t warmups = 3U;
  bool authentic = true;
  bool controlled = true;
  std::vector<std::uint32_t> authentic_n{36U, 72U, 108U};
  std::size_t authentic_seeds = 7U;
  std::size_t n108_seeds = 3U;
  std::size_t synthetic_seeds = 5U;
  std::vector<double> scales{0.25, 0.5, 1.0, 2.0, 4.0};
  bool uniform = true;
  bool profile = true;
  bool phases = true;
  bool probe = false;
  bool pin = true;
  std::string only;  // substring filter on cell id
  std::vector<std::string> snapshots;
  std::size_t exact_key_limit = 16'000'000U;
  std::uint32_t microproblems = 4096U;  // declared subsample; 4096 = full root
  std::string out = "results/root-queue-generalization/raw";
};

[[nodiscard]] std::vector<std::string> split(const std::string_view text) {
  std::vector<std::string> parts;
  std::size_t start = 0U;
  while (start <= text.size()) {
    const std::size_t comma = text.find(',', start);
    const std::size_t end = comma == std::string_view::npos ? text.size() : comma;
    if (end > start) {
      parts.emplace_back(text.substr(start, end - start));
    }
    if (comma == std::string_view::npos) {
      break;
    }
    start = comma + 1U;
  }
  return parts;
}

[[nodiscard]] bool parse(const int argc, char** const argv, Config& config) {
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument{argv[index]};
    const auto value = [&](const std::string_view prefix) {
      return std::string(argument.substr(prefix.size()));
    };
    if (argument.rfind("--samples=", 0U) == 0U) {
      config.samples = std::strtoull(value("--samples=").c_str(), nullptr, 10);
    } else if (argument.rfind("--warmups=", 0U) == 0U) {
      config.warmups = std::strtoull(value("--warmups=").c_str(), nullptr, 10);
    } else if (argument.rfind("--tiers=", 0U) == 0U) {
      const std::vector<std::string> tiers = split(value("--tiers="));
      config.authentic = std::find(tiers.begin(), tiers.end(), "authentic") != tiers.end();
      config.controlled = std::find(tiers.begin(), tiers.end(), "controlled") != tiers.end();
    } else if (argument.rfind("--authentic-n=", 0U) == 0U) {
      config.authentic_n.clear();
      for (const std::string& part : split(value("--authentic-n="))) {
        config.authentic_n.push_back(static_cast<std::uint32_t>(std::strtoul(part.c_str(), nullptr, 10)));
      }
    } else if (argument.rfind("--authentic-seeds=", 0U) == 0U) {
      config.authentic_seeds = std::strtoull(value("--authentic-seeds=").c_str(), nullptr, 10);
    } else if (argument.rfind("--n108-seeds=", 0U) == 0U) {
      config.n108_seeds = std::strtoull(value("--n108-seeds=").c_str(), nullptr, 10);
    } else if (argument.rfind("--synthetic-seeds=", 0U) == 0U) {
      config.synthetic_seeds = std::strtoull(value("--synthetic-seeds=").c_str(), nullptr, 10);
    } else if (argument.rfind("--scales=", 0U) == 0U) {
      config.scales.clear();
      for (const std::string& part : split(value("--scales="))) {
        config.scales.push_back(std::strtod(part.c_str(), nullptr));
      }
    } else if (argument.rfind("--families=", 0U) == 0U) {
      const std::vector<std::string> families = split(value("--families="));
      config.uniform = std::find(families.begin(), families.end(), "uniform") != families.end();
      config.profile = std::find(families.begin(), families.end(), "profile") != families.end();
    } else if (argument == "--no-phases") {
      config.phases = false;
    } else if (argument.rfind("--snapshot=", 0U) == 0U) {
      config.snapshots = split(value("--snapshot="));
    } else if (argument == "--probe") {
      config.probe = true;
    } else if (argument == "--no-pin") {
      config.pin = false;
    } else if (argument.rfind("--only=", 0U) == 0U) {
      config.only = value("--only=");
    } else if (argument.rfind("--microproblems=", 0U) == 0U) {
      config.microproblems = static_cast<std::uint32_t>(
          std::strtoul(value("--microproblems=").c_str(), nullptr, 10));
    } else if (argument.rfind("--exact-key-limit=", 0U) == 0U) {
      config.exact_key_limit = std::strtoull(value("--exact-key-limit=").c_str(), nullptr, 10);
    } else if (argument.rfind("--out=", 0U) == 0U) {
      config.out = value("--out=");
    } else {
      std::cerr << "unknown argument: " << argument << '\n';
      return false;
    }
  }
  if (config.samples == 0U || config.scales.empty() ||
      config.authentic_seeds > std::size(kDeclaredSeeds) ||
      config.n108_seeds > std::size(kDeclaredSeeds) ||
      config.synthetic_seeds > std::size(kDeclaredSeeds)) {
    std::cerr << "invalid configuration (samples > 0, <= 7 declared seeds per tier)\n";
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Cells.
// ---------------------------------------------------------------------------
struct CellSpec {
  rg::WorkloadFamily family = rg::WorkloadFamily::SyntheticUniform;
  std::uint32_t n = 0U;
  std::uint64_t seed = 0U;
  double scale = 1.0;
  bool anchor = false;
  bool lane_skip = false;
  std::string snapshot;  // AUTHENTIC cell replayed from a packed root snapshot
};

struct CellResult {
  std::string id;
  std::string family;
  std::uint32_t n = 0U;
  std::uint64_t seed = 0U;
  double scale = 1.0;
  bool anchor = false;
  std::string status = "Failed";
  std::string detail;
  bool valid = false;
  std::string exactness = "not run";
  bool power_ok = false;
  int ac_start = -1;
  int ac_end = -1;
  double construction_s = 0.0;
  std::uint64_t fingerprint = 0U;

  std::uint64_t a_size = 0U;
  std::uint64_t b_size = 0U;
  std::uint64_t c_size = 0U;
  std::uint64_t p0 = 0U;
  std::uint32_t quotient_bits = 0U;
  std::uint32_t residual_bits = 0U;
  std::uint32_t microproblems = 0U;
  std::uint64_t max_q_distinct = 0U;
  std::uint64_t max_x_distinct = 0U;
  std::uint64_t max_y_distinct = 0U;

  rg::QueueTelemetry telemetry;
  rg::OutputDigest digest;  // the agreed exact output

  Stats k2;
  Stats queue;
  Stats lane_skip;
  Stats k2_kernel_only;
  Stats k2_survivor_only;
  Stats queue_build_only;
  Stats queue_guard_only;
  Stats queue_group_only;
};

[[nodiscard]] double popcount_quantile(const rg::QueueTelemetry& telemetry, const double fraction) {
  if (telemetry.nonzero_guard_words == 0U) {
    return 0.0;
  }
  const auto target = static_cast<std::uint64_t>(
      std::ceil(fraction * static_cast<double>(telemetry.nonzero_guard_words)));
  std::uint64_t running = 0U;
  for (std::size_t bits = 0U; bits < telemetry.popcount_histogram.size(); ++bits) {
    running += telemetry.popcount_histogram[bits];
    if (running >= target) {
      return static_cast<double>(bits);
    }
  }
  return 64.0;
}

[[nodiscard]] std::uint32_t popcount_max(const rg::QueueTelemetry& telemetry) {
  for (std::size_t bits = telemetry.popcount_histogram.size(); bits-- > 0U;) {
    if (telemetry.popcount_histogram[bits] != 0U) {
      return static_cast<std::uint32_t>(bits);
    }
  }
  return 0U;
}

// ---------------------------------------------------------------------------
// Exactness.
// ---------------------------------------------------------------------------
struct ExactRun {
  rg::OutputDigest digest;
  std::vector<std::uint64_t> keys;
  bool keys_complete = false;
};

[[nodiscard]] bool same_output(const rg::OutputDigest& left, const rg::OutputDigest& right) {
  return left.pair_hits == right.pair_hits && left.triples == right.triples &&
         left.full_q == right.full_q && left.residue == right.residue &&
         left.structural == right.structural && left.pair_sum == right.pair_sum &&
         left.triple_sum == right.triple_sum && left.triple_xor == right.triple_xor &&
         left.triple_sum2 == right.triple_sum2;
}

[[nodiscard]] bool clean(const rg::OutputDigest& digest) {
  return digest.residual_mismatches == 0U && digest.directory_misses == 0U;
}

// ---------------------------------------------------------------------------
// Timing.
// ---------------------------------------------------------------------------
struct Entity {
  std::string name;
  std::function<std::uint64_t()> run;
  std::vector<double> samples;
};

void time_entities(const Config& config, std::vector<Entity>& main_pair,
                   std::vector<Entity>& controls, std::ofstream& samples_out,
                   const std::string& cell_id) {
  const auto run_one = [&](Entity& entity, const bool record, const std::size_t sample,
                           const std::size_t position) {
    const auto start = Clock::now();
    const std::uint64_t checksum = entity.run();
    const double ms = ms_since(start);
    g_sink = g_sink + checksum;
    if (record) {
      entity.samples.push_back(ms);
      samples_out << cell_id << ',' << entity.name << ',' << sample << ',' << position << ','
                  << fixed(ms, 6) << '\n';
    }
  };
  const std::size_t rounds = config.warmups + config.samples;
  for (std::size_t round = 0U; round < rounds; ++round) {
    const bool record = round >= config.warmups;
    const std::size_t sample = record ? round - config.warmups : 0U;
    std::size_t position = 0U;
    // K2 -> QUEUE on even rounds, QUEUE -> K2 on odd rounds.
    if ((round & 1U) == 0U) {
      for (Entity& entity : main_pair) {
        run_one(entity, record, sample, position++);
      }
    } else {
      for (std::size_t index = main_pair.size(); index-- > 0U;) {
        run_one(main_pair[index], record, sample, position++);
      }
    }
    for (std::size_t offset = 0U; offset < controls.size(); ++offset) {
      run_one(controls[(offset + round) % controls.size()], record, sample, position++);
    }
  }
}

// ---------------------------------------------------------------------------
// One cell.
// ---------------------------------------------------------------------------
CellResult run_cell(const Config& config, const CellSpec& spec, const rg::ResidualProfile& profile,
                    std::unique_ptr<rg::Workload> prebuilt, rg::RadixTables& tables,
                    std::ofstream& samples_out) {
  CellResult result;
  result.ac_start = ac_line_status();
  std::unique_ptr<rg::Workload> workload = std::move(prebuilt);
  if (workload == nullptr) {
    switch (spec.family) {
      case rg::WorkloadFamily::AuthenticFullTer:
        workload = spec.snapshot.empty()
                       ? rg::capture_authentic(spec.n, spec.seed, /*with_authority=*/true)
                       : rg::load_snapshot_workload(spec.snapshot, /*with_authority=*/true);
        break;
      case rg::WorkloadFamily::SyntheticUniform:
        workload = rg::generate_uniform(spec.scale, spec.seed);
        break;
      case rg::WorkloadFamily::ResidualProfileEmulation:
        workload = rg::generate_profile_emulation(profile, spec.scale, spec.seed);
        break;
    }
  }
  // A snapshot of the reference n=72 root is the anchor: its exact-count and
  // telemetry identity gates apply exactly as to the in-process anchor.
  const bool anchor = spec.anchor || (!spec.snapshot.empty() &&
                                      workload->n == mr8::kReferenceRootN &&
                                      workload->seed == kReferenceSeed);
  result.id = anchor ? workload->id + "_anchor" : workload->id;
  result.family = rg::family_label(spec.family);
  result.n = spec.snapshot.empty() ? spec.n : workload->n;
  result.seed = spec.snapshot.empty() ? spec.seed : workload->seed;
  result.scale = spec.scale;
  result.anchor = anchor;
  result.status = rg::status_label(workload->status);
  result.detail = workload->status_detail;
  result.construction_s = workload->construction_seconds;
  result.quotient_bits = workload->quotient_bits;
  std::cout << "# cell " << result.id << " : " << result.status
            << (result.detail.empty() ? "" : " (" + result.detail + ")") << '\n';
  std::cout.flush();
  if (workload->status != rg::WorkloadStatus::Completed) {
    result.ac_end = ac_line_status();
    return result;
  }
  result.fingerprint = workload->fingerprint;
  result.a_size = workload->a_size;
  result.b_size = workload->b_size;
  result.c_size = workload->c_size;
  result.p0 = workload->pair_universe;
  result.residual_bits = workload->residual_bits;
  const mr8::Dataset& data = *workload->data;
  const tt::AuthorityTables* const authority =
      workload->authority.ok ? &workload->authority : nullptr;

  rg::RootIndex index;
  build_root_index(data, index);
  rg::select_microproblems(index, config.microproblems);
  result.microproblems = index.selected_count;
  for (std::uint32_t family = 0U; family < rg::kFamilies; ++family) {
    result.max_q_distinct = std::max<std::uint64_t>(result.max_q_distinct, index.q_groups[family].size());
    result.max_x_distinct = std::max<std::uint64_t>(result.max_x_distinct, index.x_groups[family].size());
    result.max_y_distinct = std::max<std::uint64_t>(result.max_y_distinct, index.y_groups[family].size());
  }
  // The radix-13 classes (X as sources, Y as targets) are built with a hard
  // capacity: at most kHitCapacity distinct values per class, which bounds one
  // query's witness buffer.  Beyond it the donor builder fails closed, so the
  // cell is reported as outside the architecture's domain, not as a result.
  if (result.max_x_distinct > rg::RadixGeom::kHitCapacity ||
      result.max_y_distinct > rg::RadixGeom::kHitCapacity) {
    result.status = "NotApplicable";
    result.detail = "QUEUE_DENSE capacity: largest X family has " +
                    std::to_string(result.max_x_distinct) + " and largest Y family " +
                    std::to_string(result.max_y_distinct) +
                    " distinct residues; the radix-13 classes hold at most " +
                    std::to_string(rg::RadixGeom::kHitCapacity);
    std::cout << "#   " << result.detail << "\n";
    result.ac_end = ac_line_status();
    return result;
  }

  // --- exactness ------------------------------------------------------------
  const auto exact_run = [&](const std::function<void(rg::RootSink&)>& body) {
    ExactRun run;
    rg::RootSink sink;
    sink.data = &data;
    sink.index = &index;
    sink.authority = authority;
    sink.depth = rg::SinkDepth::Full;
    sink.pair_keys = &run.keys;
    sink.pair_key_limit = config.exact_key_limit;
    sink.reset();
    body(sink);
    run.digest = sink.digest;
    run.keys_complete = !sink.pair_keys_overflowed;
    if (run.keys_complete) {
      std::sort(run.keys.begin(), run.keys.end());
    } else {
      run.keys.clear();
      run.keys.shrink_to_fit();
    }
    return run;
  };
  const ExactRun oracle = exact_run([&](rg::RootSink& sink) { rg::run_oracle(data, index, sink); });
  const ExactRun k2 = exact_run([&](rg::RootSink& sink) { rg::run_k2(data, sink); });
  const ExactRun queue = exact_run([&](rg::RootSink& sink) {
    rg::run_radix(data, index, tables, rg::RadixRoute::QueueDense, rg::RadixStop::Full, sink);
  });
  bool exact = tables.built && clean(oracle.digest) && clean(k2.digest) && clean(queue.digest) &&
               same_output(oracle.digest, k2.digest) && same_output(oracle.digest, queue.digest) &&
               oracle.digest.group_emissions == queue.digest.group_emissions &&
               oracle.digest.group_sum == queue.digest.group_sum;
  bool keys_compared = oracle.keys_complete && k2.keys_complete && queue.keys_complete;
  if (keys_compared) {
    exact = exact && oracle.keys == k2.keys && oracle.keys == queue.keys;
  }
  if (spec.lane_skip) {
    const ExactRun skip = exact_run([&](rg::RootSink& sink) {
      rg::run_radix(data, index, tables, rg::RadixRoute::LaneSkip, rg::RadixStop::Full, sink);
    });
    exact = exact && tables.built && clean(skip.digest) && same_output(oracle.digest, skip.digest) &&
            oracle.digest.group_emissions == skip.digest.group_emissions &&
            oracle.digest.group_sum == skip.digest.group_sum;
    if (keys_compared && skip.keys_complete) {
      exact = exact && oracle.keys == skip.keys;
    }
  }
  result.digest = oracle.digest;
  result.exactness = std::string(exact ? "EXACT" : "DIVERGED") +
                     (keys_compared ? " (sorted pair keys + digests)" : " (digests only)");
  if (anchor && index.selected_count == rg::kFamilies * rg::kFamilies) {
    const bool gate = oracle.digest.pair_hits == kAnchorPairs &&
                      oracle.digest.triples == kAnchorTriples &&
                      oracle.digest.group_emissions == kAnchorGroups &&
                      oracle.digest.full_q == kAnchorFullQ;
    result.exactness += gate ? " anchor-identity=PASS" : " anchor-identity=FAIL";
    exact = exact && gate;
  }
  std::cout << "#   exactness " << result.exactness << " pairs=" << oracle.digest.pair_hits
            << " groups=" << oracle.digest.group_emissions << " triples=" << oracle.digest.triples
            << " fullQ=" << oracle.digest.full_q << '\n';
  std::cout.flush();
  if (!exact) {
    result.status = "INVALID";
    result.detail = "backend outputs disagree; no timing recorded";
    result.ac_end = ac_line_status();
    return result;
  }

  // --- telemetry ------------------------------------------------------------
  result.telemetry = rg::count_telemetry(data, index, tables);
  if (anchor && (result.telemetry.nonzero_guard_words != kAnchorNonzeroWords ||
                      result.telemetry.candidate_bits != kAnchorCandidateBits)) {
    result.status = "INVALID";
    result.detail = "anchor telemetry does not reproduce the measured n=72 geometry";
    result.ac_end = ac_line_status();
    return result;
  }
  if (config.probe) {
    result.valid = true;
    result.ac_end = ac_line_status();
    return result;
  }

  // --- timing ---------------------------------------------------------------
  rg::RootSink full;
  full.data = &data;
  full.index = &index;
  full.authority = authority;
  full.depth = rg::SinkDepth::Full;
  rg::RootSink group_only = full;
  group_only.depth = rg::SinkDepth::GroupOnly;

  std::vector<Entity> main_pair;
  main_pair.push_back(Entity{"k2", [&]() {
                               full.reset();
                               rg::run_k2(data, full);
                               return full.digest.triple_sum;
                             }, {}});
  main_pair.push_back(Entity{"queue_dense", [&]() {
                               full.reset();
                               rg::run_radix(data, index, tables, rg::RadixRoute::QueueDense,
                                             rg::RadixStop::Full, full);
                               return full.digest.triple_sum;
                             }, {}});
  std::vector<Entity> controls;
  if (spec.lane_skip) {
    controls.push_back(Entity{"lane_skip", [&]() {
                                full.reset();
                                rg::run_radix(data, index, tables, rg::RadixRoute::LaneSkip,
                                              rg::RadixStop::Full, full);
                                return full.digest.triple_sum;
                              }, {}});
  }
  if (config.phases) {
    controls.push_back(Entity{"k2_kernel_only", [&]() { return rg::run_k2_kernel_only(data, index); }, {}});
    controls.push_back(
        Entity{"k2_survivor_only", [&]() { return rg::run_k2_survivor_only(data, index); }, {}});
    controls.push_back(Entity{"queue_build_only", [&]() {
                                return rg::run_radix(data, index, tables, rg::RadixRoute::QueueDense,
                                                     rg::RadixStop::BuildOnly, full);
                              }, {}});
    controls.push_back(Entity{"queue_guard_only", [&]() {
                                return rg::run_radix(data, index, tables, rg::RadixRoute::QueueDense,
                                                     rg::RadixStop::GuardQueueOnly, full);
                              }, {}});
    controls.push_back(Entity{"queue_group_only", [&]() {
                                group_only.reset();
                                rg::run_radix(data, index, tables, rg::RadixRoute::QueueDense,
                                              rg::RadixStop::Full, group_only);
                                return group_only.digest.group_sum;
                              }, {}});
  }
  time_entities(config, main_pair, controls, samples_out, result.id);
  samples_out.flush();

  result.k2 = summarize(main_pair[0].samples);
  result.queue = summarize(main_pair[1].samples);
  for (const Entity& entity : controls) {
    const Stats stats = summarize(entity.samples);
    if (entity.name == "lane_skip") {
      result.lane_skip = stats;
    } else if (entity.name == "k2_kernel_only") {
      result.k2_kernel_only = stats;
    } else if (entity.name == "k2_survivor_only") {
      result.k2_survivor_only = stats;
    } else if (entity.name == "queue_build_only") {
      result.queue_build_only = stats;
    } else if (entity.name == "queue_guard_only") {
      result.queue_guard_only = stats;
    } else if (entity.name == "queue_group_only") {
      result.queue_group_only = stats;
    }
  }
  result.ac_end = ac_line_status();
  result.power_ok = result.ac_start == 1 && result.ac_end == 1;
  result.valid = true;
  result.status = "Completed";
  std::cout << "#   K2 " << fixed(result.k2.median, 3) << " ms | QUEUE " << fixed(result.queue.median, 3)
            << " ms | S = " << fixed(result.queue.median > 0.0 ? result.k2.median / result.queue.median : 0.0, 3)
            << "x | rho_q = "
            << fixed(result.telemetry.guard_word_ops > 0U
                         ? static_cast<double>(result.telemetry.nonzero_guard_words) /
                               static_cast<double>(result.telemetry.guard_word_ops)
                         : 0.0,
                     4)
            << (result.power_ok ? "" : "  [POWER NOT AC: excluded from aggregates]") << '\n';
  std::cout.flush();
  return result;
}

// ---------------------------------------------------------------------------
// Output.
// ---------------------------------------------------------------------------
void write_header(std::ofstream& out) {
  out << "cell_id,family,n,seed,scale,anchor,status,detail,valid,exactness,power_ok,ac_start,"
         "ac_end,construction_s,fingerprint,a_size,b_size,c_size,p0,quotient_bits,residual_bits,"
         "queries,guard_word_ops,nonzero_guard_words,queue_entries,candidate_bits,rho_q,"
         "bits_per_queued_word,popcount_mean,popcount_p50,popcount_p95,popcount_max,k2_tiles,"
         "k2_coarse_candidates,pair_hits,group_emissions,triples,full_q,residue,structural,"
         "k2_median_ms,k2_p25_ms,k2_p75_ms,k2_min_ms,queue_median_ms,queue_p25_ms,queue_p75_ms,"
         "queue_min_ms,lane_skip_median_ms,k2_kernel_only_ms,k2_survivor_only_ms,"
         "queue_build_only_ms,queue_guard_only_ms,queue_group_only_ms,speedup_backend,"
         "speedup_steady,k2_sweep_ms,k2_survivor_ms,k2_sink_ms,queue_index_build_ms,"
         "queue_guard_enqueue_ms,queue_consume_ms,queue_authority_ms,samples,microproblems,"
         "max_q_distinct,max_x_distinct,max_y_distinct\n";
}

void write_row(std::ofstream& out, const CellResult& cell) {
  const rg::QueueTelemetry& t = cell.telemetry;
  const double rho = t.guard_word_ops > 0U ? static_cast<double>(t.nonzero_guard_words) /
                                                 static_cast<double>(t.guard_word_ops)
                                           : 0.0;
  const double per_word = t.nonzero_guard_words > 0U
                              ? static_cast<double>(t.candidate_bits) /
                                    static_cast<double>(t.nonzero_guard_words)
                              : 0.0;
  const double speedup = cell.queue.median > 0.0 ? cell.k2.median / cell.queue.median : 0.0;
  const double queue_steady = cell.queue.median - cell.queue_build_only.median;
  const double speedup_steady = queue_steady > 0.0 ? cell.k2.median / queue_steady : 0.0;
  std::string detail = cell.detail;
  std::replace(detail.begin(), detail.end(), ',', ';');
  std::replace(detail.begin(), detail.end(), '\n', ' ');
  out << cell.id << ',' << cell.family << ',' << cell.n << ',' << hex(cell.seed) << ','
      << fixed(cell.scale, 3) << ',' << (cell.anchor ? 1 : 0) << ',' << cell.status << ','
      << detail << ',' << (cell.valid ? 1 : 0) << ',' << cell.exactness << ','
      << (cell.power_ok ? 1 : 0) << ',' << cell.ac_start << ',' << cell.ac_end << ','
      << fixed(cell.construction_s, 3) << ',' << hex(cell.fingerprint) << ',' << cell.a_size << ','
      << cell.b_size << ',' << cell.c_size << ',' << cell.p0 << ',' << cell.quotient_bits << ','
      << cell.residual_bits << ',' << t.queries << ',' << t.guard_word_ops << ','
      << t.nonzero_guard_words << ',' << t.nonzero_guard_words << ',' << t.candidate_bits << ','
      << fixed(rho, 6) << ',' << fixed(per_word, 6) << ',' << fixed(per_word, 6) << ','
      << fixed(popcount_quantile(t, 0.50), 0) << ',' << fixed(popcount_quantile(t, 0.95), 0)
      << ',' << popcount_max(t) << ',' << t.k2_tiles << ',' << t.k2_coarse_candidates << ','
      << cell.digest.pair_hits << ',' << cell.digest.group_emissions << ',' << cell.digest.triples
      << ',' << cell.digest.full_q << ',' << cell.digest.residue << ',' << cell.digest.structural
      << ',' << fixed(cell.k2.median) << ',' << fixed(cell.k2.p25) << ',' << fixed(cell.k2.p75)
      << ',' << fixed(cell.k2.minimum) << ',' << fixed(cell.queue.median) << ','
      << fixed(cell.queue.p25) << ',' << fixed(cell.queue.p75) << ',' << fixed(cell.queue.minimum)
      << ',' << fixed(cell.lane_skip.median) << ',' << fixed(cell.k2_kernel_only.median) << ','
      << fixed(cell.k2_survivor_only.median) << ',' << fixed(cell.queue_build_only.median) << ','
      << fixed(cell.queue_guard_only.median) << ',' << fixed(cell.queue_group_only.median) << ','
      << fixed(speedup, 6) << ',' << fixed(speedup_steady, 6) << ','
      << fixed(cell.k2_kernel_only.median) << ','
      << fixed(cell.k2_survivor_only.median - cell.k2_kernel_only.median) << ','
      << fixed(cell.k2.median - cell.k2_survivor_only.median) << ','
      << fixed(cell.queue_build_only.median) << ','
      << fixed(cell.queue_guard_only.median - cell.queue_build_only.median) << ','
      << fixed(cell.queue_group_only.median - cell.queue_guard_only.median) << ','
      << fixed(cell.queue.median - cell.queue_group_only.median) << ',' << cell.k2.samples << ','
      << cell.microproblems << ',' << cell.max_q_distinct << ',' << cell.max_x_distinct << ','
      << cell.max_y_distinct << '\n';
  out.flush();
}

}  // namespace

int main(int argc, char** argv) {
  Config config;
  if (!parse(argc, argv, config)) {
    return EXIT_FAILURE;
  }

  std::cout << "# ter_root_queue_generalization_bench\n";
  std::cout << "# configure_time_commit=" << TER_CONFIGURE_TIME_COMMIT
            << " configure_time_dirty=" << (TER_CONFIGURE_TIME_DIRTY ? "yes" : "no") << '\n';
#if defined(_MSC_FULL_VER)
  std::cout << "# compiler=MSVC " << _MSC_FULL_VER << '\n';
#elif defined(__clang__)
  std::cout << "# compiler=clang " << __clang_version__ << '\n';
#elif defined(__GNUC__)
  std::cout << "# compiler=gcc " << __VERSION__ << '\n';
#endif
  const mr8::IsaReport isa = mr8::detect_isa();
  const bool executable = isa.usable && isa.bmi2 && mr8::avx2::translation_unit_is_avx2() &&
                          ter::radix::avx2::translation_unit_is_avx2() &&
                          pk::bmi2::translation_unit_is_bmi2();
  std::cout << "# cpu=" << (isa.brand[0] != '\0' ? isa.brand : "unknown")
            << " avx2+bmi2 executable=" << (executable ? "yes" : "NO") << '\n';
  if (!executable) {
    std::cerr << "FATAL: the AVX2 K2 kernel and the AVX2/BMI2 radix routes are not all "
                 "executable here; no backend may silently fall back\n";
    return EXIT_FAILURE;
  }
  std::string affinity = "not requested";
#if defined(_WIN32)
  if (config.pin) {
    const bool pinned = SetThreadAffinityMask(GetCurrentThread(), static_cast<DWORD_PTR>(1)) != 0;
    const bool priority = SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS) != 0;
    affinity = pinned ? (priority ? "cpu0 + high priority" : "cpu0") : "failed";
  }
#endif
  std::cout << "# thread_affinity=" << affinity << " ac_line_status=" << ac_line_status()
            << " memory_limit_mib="
            << ter::limits::benchmark::kFullTerMemoryBytes / (1024ULL * 1024ULL)
            << " warmups=" << config.warmups << " samples=" << config.samples << '\n';
  std::cout.flush();

  std::error_code error;
  std::filesystem::create_directories(config.out, error);
  const auto path_for = [&](const char* name) { return std::filesystem::path(config.out) / name; };
  std::ofstream cells_out(path_for("cells.csv"));
  std::ofstream samples_out(path_for("samples.csv"));
  write_header(cells_out);
  samples_out << "cell_id,entity,sample,position,ms\n";

  rg::RadixTables tables;
  tables.allocate();

  // The anchor root is captured first: it is both the identity-gate cell and
  // the source of the residual profile for RESIDUAL_PROFILE_EMULATION.
  std::unique_ptr<rg::Workload> anchor;
  rg::ResidualProfile profile;
  if (config.authentic || config.controlled) {
    std::cout << "# capturing the n=72 reference anchor root...\n";
    std::cout.flush();
    anchor = rg::capture_authentic(mr8::kReferenceRootN, kReferenceSeed, /*with_authority=*/true);
    if (anchor->status != rg::WorkloadStatus::Completed) {
      std::cerr << "FATAL: anchor capture failed: " << anchor->status_detail << '\n';
      return EXIT_FAILURE;
    }
    profile = rg::measure_profile(*anchor->data, mr8::kReferenceRootN, kReferenceSeed);
  }

  // Order: anchor and n < 108 authentic cells, then the controlled sweep, then
  // the n >= 108 captures (slow, and possibly resource-limited) last.
  std::vector<CellSpec> specs;
  // Snapshot cells first: each is an AUTHENTIC root replayed in this fresh
  // process from a file written by ter_root_snapshot_capture.
  for (const std::string& path : config.snapshots) {
    CellSpec spec{rg::WorkloadFamily::AuthenticFullTer, 0U, 0U, 1.0, false, true, path};
    specs.push_back(spec);
  }
  const auto add_authentic = [&](const bool large) {
    for (const std::uint32_t n : config.authentic_n) {
      if ((n >= 108U) != large) {
        continue;
      }
      const std::size_t seeds = n >= 108U ? config.n108_seeds : config.authentic_seeds;
      for (std::size_t index = 0U; index < seeds; ++index) {
        specs.push_back(CellSpec{rg::WorkloadFamily::AuthenticFullTer, n, kDeclaredSeeds[index],
                                 1.0, false, index == 0U});
      }
    }
  };
  if (config.authentic) {
    specs.push_back(CellSpec{rg::WorkloadFamily::AuthenticFullTer, mr8::kReferenceRootN,
                             kReferenceSeed, 1.0, true, true});
    add_authentic(false);
  }
  if (config.controlled) {
    for (const double scale : config.scales) {
      for (std::size_t index = 0U; index < config.synthetic_seeds; ++index) {
        if (config.uniform) {
          specs.push_back(CellSpec{rg::WorkloadFamily::SyntheticUniform, 0U,
                                   kDeclaredSeeds[index], scale, false, index == 0U});
        }
        if (config.profile) {
          specs.push_back(CellSpec{rg::WorkloadFamily::ResidualProfileEmulation, 0U,
                                   kDeclaredSeeds[index], scale, false, index == 0U});
        }
      }
    }
  }
  if (config.authentic) {
    add_authentic(true);
  }

  std::vector<CellResult> results;
  const auto campaign_start = Clock::now();
  for (const CellSpec& spec : specs) {
    // Filter by the id the cell will carry, before any expensive construction.
    std::string planned;
    if (!spec.snapshot.empty()) {
      planned = "snapshot:" + spec.snapshot;
    } else if (spec.family == rg::WorkloadFamily::AuthenticFullTer) {
      planned = "authentic_n" + std::to_string(spec.n) + "_s" + hex(spec.seed) +
                (spec.anchor ? "_anchor" : "");
    } else {
      planned = std::string(spec.family == rg::WorkloadFamily::SyntheticUniform ? "uniform"
                                                                                 : "profile") +
                "_x" + fixed(spec.scale, 2) + "_s" + hex(spec.seed);
    }
    if (!config.only.empty() && planned.find(config.only) == std::string::npos) {
      continue;
    }
    std::unique_ptr<rg::Workload> prebuilt;
    if (spec.anchor) {
      prebuilt = std::move(anchor);
    }
    results.push_back(run_cell(config, spec, profile, std::move(prebuilt), tables, samples_out));
    write_row(cells_out, results.back());
  }

  // Console aggregate (the analysis script is the authority for the paper
  // tables; this is a sanity summary of the same rows).
  std::vector<double> logs;
  std::size_t wins = 0U;
  for (const CellResult& cell : results) {
    if (cell.valid && cell.power_ok && cell.queue.median > 0.0) {
      const double speedup = cell.k2.median / cell.queue.median;
      logs.push_back(std::log(speedup));
      wins += speedup > 1.0 ? 1U : 0U;
    }
  }
  double geometric = 0.0;
  if (!logs.empty()) {
    double sum = 0.0;
    for (const double value : logs) {
      sum += value;
    }
    geometric = std::exp(sum / static_cast<double>(logs.size()));
  }
  std::cout << "\n# cells=" << results.size() << " timed_valid_ac=" << logs.size()
            << " queue_wins=" << wins << " geometric_mean_speedup=" << fixed(geometric, 4)
            << " campaign_minutes=" << fixed(ms_since(campaign_start) / 60000.0, 2) << '\n';
  std::cout << "# checksum sink = " << g_sink << '\n';
  std::cout << "# artifacts: " << config.out << '\n';
  return EXIT_SUCCESS;
}
