// ARWLF root benchmark: one packed root snapshot per fresh process.
//
//   1. load the snapshot (checksummed), authority tables and full Q-bit keys;
//      build the shared Y bitmaps + group index; select the declared
//      deterministic microproblem subset (never timed);
//   2. exactness, untimed:
//        frozen K2 (25-bit filter -> RootSink authority chain);
//        K2_WIDE (frozen K2 kernel + full-key check) with its width histogram;
//        ARWLF production; ARWLF refined to exactly quotient bits [0, 32);
//        QUEUE_DENSE where its capacity allows; the dispatcher.
//      Gates: ARWLF == K2_WIDE on every triple hash; ARWLF k=32 hits == frozen
//      K2's low-32 survivors; ARWLF projected hits at k = 19 + mid == K2_WIDE's
//      histogram; residue/structural survivors (+ multiset hash) of every
//      backend == frozen K2's.  Any disagreement: INVALID, no timing;
//   3. matched timing: warmups, then samples alternating forward/reverse over
//      K2, K2_WIDE, ARWLF, DISPATCH (and QUEUE_DENSE), plus the K2 kernel-only
//      control (tiles + frozen kernel, no survivor test: K2's sweep floor, not
//      an exact backend).

#include "ter_a64_chqr.hpp"
#include "ter_arwlf.hpp"
#include "ter_bench_host.hpp"
#include "ter_microreport8_kernels.hpp"
#include "ter_root_generalization_workload.hpp"
#include "ter_root_queue_kernels.hpp"

#include <algorithm>
#include <chrono>
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

namespace {

namespace aw = ter::arwlf;
namespace a64 = ter::a64;
namespace rg = ter::root_generalization;
namespace mr8 = ter::microreport8;

using ter::bench_host::ac_line_status;
using ter::bench_host::current_cpu;
using ter::bench_host::efficiency_classes;
using ter::bench_host::peak_working_set_mib;
using ter::bench_host::pin_to_p_core;

using Clock = std::chrono::steady_clock;

volatile std::uint64_t g_sink = 0U;

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

struct Stats {
  double median = 0.0;
  double p25 = 0.0;
  double p75 = 0.0;
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
  stats.p25 = quantile(samples, 0.25);
  stats.median = quantile(samples, 0.50);
  stats.p75 = quantile(samples, 0.75);
  return stats;
}

struct Config {
  std::string snapshot;
  std::string out_dir = "results/arwlf";
  std::string label;
  std::uint32_t microproblems = 4096U;
  std::size_t warmups = 3U;
  std::size_t samples = 7U;
  bool timing = true;
  std::string pin = "none";  // none | p-core
  std::string entities;      // comma list; empty = all
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
    } else if (arg.rfind("--pin=", 0) == 0) {
      config.pin = value("--pin=");
    } else if (arg.rfind("--entities=", 0) == 0) {
      config.entities = value("--entities=");
    } else if (arg == "--no-timing") {
      config.timing = false;
    } else {
      std::cerr << "unknown argument: " << arg << '\n';
      return false;
    }
  }
  return !config.snapshot.empty() && (config.pin == "none" || config.pin == "p-core");
}

struct Entity {
  std::string name;
  std::function<std::uint64_t()> run;
  bool exact = true;  // false: a control whose checksum is not the answer
  std::vector<double> samples;
};

}  // namespace

int main(int argc, char** argv) {
  Config config;
  if (!parse(argc, argv, config)) {
    std::cerr << "usage: ter_arwlf_bench --snapshot=PATH [--microproblems=K] [--warmups=W]"
                 " [--samples=S] [--no-timing] [--out-dir=DIR] [--label=L]\n";
    return 2;
  }
  const int ac_start = ac_line_status();
  const std::vector<int> classes = efficiency_classes();
  const auto class_of = [&](const int cpu) {
    return cpu >= 0 && static_cast<std::size_t>(cpu) < classes.size() ? classes[static_cast<std::size_t>(cpu)] : -1;
  };
  int pinned_cpu = -1;
  if (config.pin == "p-core") {
    pinned_cpu = pin_to_p_core(classes);
    if (pinned_cpu < 0) {
      std::cout << "# pin=p-core FAILED: refusing to run an unpinned sample under a pinned label\n";
      return 5;
    }
  }
  std::cout << "# pin=" << config.pin << " cpu=" << current_cpu()
            << " efficiency_class=" << class_of(current_cpu()) << '\n';
  std::filesystem::create_directories(config.out_dir);

  // --- workload (never timed) ---------------------------------------------
  const auto workload = rg::load_snapshot_workload(config.snapshot, /*with_authority=*/true);
  std::cout << "# snapshot " << config.snapshot << " : " << rg::status_label(workload->status)
            << (workload->status_detail.empty() ? "" : " (" + workload->status_detail + ")") << '\n';
  if (workload->status != rg::WorkloadStatus::Completed || !workload->authority.ok) {
    return 3;
  }
  const a64::FamilyKeys keys = a64::build_family_keys(*workload);
  if (!keys.ok) {
    std::cout << "# NotApplicable: full keys unavailable\n";
    return 3;
  }
  const mr8::Dataset& data = *workload->data;
  rg::RootIndex index;
  rg::build_root_index(data, index);
  rg::select_microproblems(index, config.microproblems);
  const std::string id = (config.label.empty() ? workload->id : config.label) + "_mp" +
                         std::to_string(index.selected_count);
  // Physical 16 x 16 packets: the per-packet cost unit of every ARWLF report.
  const double packets = aw::selected_packets(keys, index);
  std::cout << "# n=" << workload->n << " seed=" << hex(workload->seed) << " |A|=" << workload->a_size
            << " |B|=" << workload->b_size << " |C|=" << workload->c_size << " Q=" << keys.full_bits
            << " W=" << keys.residual_bits << " microproblems=" << index.selected_count
            << " | packets=" << packets << '\n';
  std::cout.flush();

  const auto make_sink = [&]() {
    rg::RootSink sink;
    sink.data = &data;
    sink.index = &index;
    sink.authority = &workload->authority;
    sink.reset();
    return sink;
  };

  // --- exactness (untimed) -------------------------------------------------
  auto shot = Clock::now();
  rg::RootSink k2 = make_sink();
  rg::run_k2(data, k2);
  const double k2_once = ms_since(shot);
  rg::RootSink wide = make_sink();
  a64::Telemetry wide_t;
  shot = Clock::now();
  a64::run_k2_wide(data, index, keys, wide, &wide_t, /*sweep=*/true);
  const double wide_once = ms_since(shot);
  // Counted run (exact lane / packet geometry) for the gates; a plain run
  // (what is timed) for the single shot and as a second answer.
  rg::RootSink arwlf = make_sink();
  aw::Telemetry at;
  aw::Config counted;
  counted.account = true;
  const aw::Status a_status = aw::run_arwlf(data, index, keys, counted, arwlf, &at);
  rg::RootSink plain = make_sink();
  shot = Clock::now();
  const aw::Status p_status = aw::run_arwlf(data, index, keys, aw::Config{}, plain, nullptr);
  const double arwlf_once = ms_since(shot);
  aw::Config k32_config;
  k32_config.check_bits = 13U;
  rg::RootSink k32 = make_sink();
  aw::Telemetry k32_t;
  const aw::Status k32_status = aw::run_arwlf(data, index, keys, k32_config, k32, &k32_t);

  rg::RadixTables tables;
  tables.allocate();
  rg::RootSink queue = make_sink();
  std::uint64_t max_x = 0U;
  std::uint64_t max_y = 0U;
  for (std::uint32_t f = 0U; f < rg::kFamilies; ++f) {
    max_x = std::max<std::uint64_t>(max_x, index.x_groups[f].size());
    max_y = std::max<std::uint64_t>(max_y, index.y_groups[f].size());
  }
  const bool queue_applicable =
      max_x <= rg::RadixGeom::kHitCapacity && max_y <= rg::RadixGeom::kHitCapacity;
  double queue_once = 0.0;
  if (queue_applicable) {
    shot = Clock::now();
    rg::run_radix(data, index, tables, rg::RadixRoute::QueueDense, rg::RadixStop::Full, queue);
    queue_once = ms_since(shot);
  }
  rg::RootSink dispatched = make_sink();
  shot = Clock::now();
  const aw::Dispatch decision = aw::dispatch_and_run(data, index, keys, &tables, dispatched);
  const double dispatch_once = ms_since(shot);
  shot = Clock::now();
  const std::uint64_t kernel_checksum = rg::run_k2_kernel_only(data, index);
  const double kernel_once = ms_since(shot);
  g_sink = g_sink + kernel_checksum;

  const rg::OutputDigest& dk = k2.digest;
  const rg::OutputDigest& dw = wide.digest;
  const rg::OutputDigest& da = arwlf.digest;
  std::uint64_t sweep_at_mid = 0U;
  for (std::uint32_t c = aw::kLeafBits + at.mid_bits; c <= keys.residual_bits; ++c) {
    sweep_at_mid += wide_t.sweep[c];
  }
  const auto same_answer = [&](const rg::OutputDigest& d) {
    return d.residue == dk.residue && d.residue_sum == dk.residue_sum && d.structural == dk.structural;
  };
  const bool gate_status = a_status == aw::Status::Completed && p_status == aw::Status::Completed &&
                           k32_status == aw::Status::Completed;
  const bool gate_wide = da.triples == dw.triples && da.triple_sum == dw.triple_sum &&
                         da.triple_xor == dw.triple_xor && da.triple_sum2 == dw.triple_sum2 &&
                         at.full_hits == da.triples;
  const bool gate_k32 = k32_t.projected_hits == dk.full_q;
  const bool gate_mid = at.projected_hits == sweep_at_mid;
  const bool gate_answer = same_answer(dw) && same_answer(da) && same_answer(plain.digest) &&
                           plain.digest.triple_sum == da.triple_sum && same_answer(dispatched.digest) &&
                           (!queue_applicable || (same_answer(queue.digest) &&
                                                  queue.digest.triple_sum == dk.triple_sum));
  const bool gate_clean = dk.directory_misses == 0U && dk.residual_mismatches == 0U &&
                          dw.directory_misses == 0U && dw.residual_mismatches == 0U;
  const bool exact = gate_status && gate_wide && gate_k32 && gate_mid && gate_answer && gate_clean;
  std::cout << "# exactness " << (exact ? "EXACT" : "DIVERGED") << " | frozen K2 hits25="
            << dk.triples << " fullQ32=" << dk.full_q << " residue=" << dk.residue
            << " | ARWLF k=32 hits=" << k32_t.projected_hits << " | ARWLF k=" << 19U + at.mid_bits
            << " hits=" << at.projected_hits << " vs K2_WIDE histogram " << sweep_at_mid
            << " | ARWLF triples=" << da.triples << " K2_WIDE triples=" << dw.triples
            << " | queue " << (queue_applicable ? "EXACT-checked" : "NotApplicable (capacity)")
            << " | dispatcher chose " << aw::backend_label(decision.chosen) << " ran "
            << aw::backend_label(decision.ran) << '\n';
  std::cout << "# single-shot ms (sweep-instrumented K2_WIDE): K2 " << fixed(k2_once, 1) << " | K2_WIDE "
            << fixed(wide_once, 1) << " | ARWLF " << fixed(arwlf_once, 1) << " ("
            << fixed(packets > 0.0 ? arwlf_once * 1e6 / packets : 0.0, 2) << " ns/packet) | QUEUE " << fixed(queue_once, 1) << " | DISPATCH "
            << fixed(dispatch_once, 1) << " (ran " << aw::backend_label(decision.ran) << ") | K2 kernel-only "
            << fixed(kernel_once, 1) << '\n';
  std::cout << "# gates status=" << gate_status << " wide=" << gate_wide << " k32=" << gate_k32
            << " mid=" << gate_mid << " answer=" << gate_answer << " clean=" << gate_clean << '\n';
  const double lane_util =
      at.rows == 0U ? 0.0 : static_cast<double>(at.lanes) / (16.0 * static_cast<double>(at.rows));
  std::cout << "# ARWLF telemetry: leaf_triples=" << at.leaf_triples << " packets=" << at.packets
            << " lanes=" << at.lanes << " logical_pairs=" << at.logical_pairs << " rows=" << at.rows
            << " main_rows=" << at.main_rows << " lane_utilization=" << fixed(lane_util, 4)
            << " build_ms=" << fixed(static_cast<double>(at.build_ns) * 1e-6, 2)
            << " survivors=" << at.filter_survivors << " survivor_rate="
            << (at.lanes == 0U ? 0.0 : static_cast<double>(at.filter_survivors) / static_cast<double>(at.lanes))
            << " mid_bits=" << at.mid_bits << " chunks=" << at.chunks << " avx2=" << at.avx2
            << " index_MiB=" << fixed(static_cast<double>(at.index_bytes) / 1048576.0, 1) << '\n';
  std::cout << "# dispatcher predicted ms: queue=" << decision.predicted_ms[0]
            << " arwlf=" << decision.predicted_ms[1] << " k2_wide=" << decision.predicted_ms[2]
            << " probe_ms=" << fixed(decision.probe_ms, 1) << " probe_mps=" << decision.probe_microproblems
            << " divisors k2_wide=" << decision.k2_probe_divisor << " arwlf=" << decision.arwlf_probe_divisor
            << '\n';
  std::cout.flush();
  if (!exact) {
    std::cout << "# INVALID: outputs disagree; no timing recorded\n";
    return 4;
  }

  // --- matched timing -------------------------------------------------------
  rg::RootSink timed = make_sink();
  std::vector<Entity> entities;
  entities.push_back(Entity{"k2", [&]() {
                              timed.reset();
                              rg::run_k2(data, timed);
                              return timed.digest.residue_sum;
                            }, true, {}});
  entities.push_back(Entity{"k2_wide", [&]() {
                              timed.reset();
                              a64::run_k2_wide(data, index, keys, timed, nullptr, false);
                              return timed.digest.residue_sum;
                            }, true, {}});
  entities.push_back(Entity{"arwlf", [&]() {
                              timed.reset();
                              static_cast<void>(aw::run_arwlf(data, index, keys, aw::Config{}, timed, nullptr));
                              return timed.digest.residue_sum;
                            }, true, {}});
  std::vector<std::string> dispatch_choices;
  entities.push_back(Entity{"dispatch", [&]() {
                              const aw::Dispatch d = aw::dispatch_and_run(data, index, keys, &tables, timed);
                              dispatch_choices.push_back(aw::backend_label(d.ran));
                              return timed.digest.residue_sum;
                            }, true, {}});
  if (queue_applicable) {
    entities.push_back(Entity{"queue_dense", [&]() {
                                timed.reset();
                                rg::run_radix(data, index, tables, rg::RadixRoute::QueueDense,
                                              rg::RadixStop::Full, timed);
                                return timed.digest.residue_sum;
                              }, true, {}});
  }
  entities.push_back(Entity{"k2_kernel_only", [&]() { return rg::run_k2_kernel_only(data, index); },
                            false, {}});
  if (!config.entities.empty()) {
    const std::string list = "," + config.entities + ",";
    std::erase_if(entities, [&](const Entity& e) { return list.find("," + e.name + ",") == std::string::npos; });
  }
  std::ofstream samples_out(config.out_dir + "/" + id + "_samples.csv");
  samples_out << "id,entity,sample,position,ms,pin,cpu_start,cpu_end,class_start,class_end\n";
  if (config.timing) {
    const std::size_t rounds = config.warmups + config.samples;
    for (std::size_t round = 0U; round < rounds; ++round) {
      const bool record = round >= config.warmups;
      for (std::size_t position = 0U; position < entities.size(); ++position) {
        Entity& entity = entities[(round & 1U) == 0U ? position : entities.size() - 1U - position];
        const int cpu_start = current_cpu();
        const auto start = Clock::now();
        const std::uint64_t checksum = entity.run();
        const double ms = ms_since(start);
        const int cpu_end = current_cpu();
        g_sink = g_sink + checksum;
        if (entity.exact && checksum != dk.residue_sum) {
          std::cout << "# INVALID: timed " << entity.name << " answer drifted\n";
          return 4;
        }
        if (record) {
          entity.samples.push_back(ms);
          samples_out << id << ',' << entity.name << ',' << round - config.warmups << ','
                      << position << ',' << fixed(ms, 4) << ',' << config.pin << ',' << cpu_start
                      << ',' << cpu_end << ',' << class_of(cpu_start) << ',' << class_of(cpu_end) << '\n';
        }
      }
      samples_out.flush();
    }
  }
  const int ac_end = ac_line_status();
  const bool power_ok = ac_start == 1 && ac_end == 1;
  std::vector<Stats> stats;
  for (const Entity& entity : entities) {
    stats.push_back(summarize(entity.samples));
  }
  const auto median_of = [&](const std::string& name) {
    for (std::size_t e = 0U; e < entities.size(); ++e) {
      if (entities[e].name == name) {
        return stats[e].median;
      }
    }
    return 0.0;
  };
  const double k2_ms = median_of("k2");
  const double arwlf_ms = median_of("arwlf");
  const double ns_per_packet = packets > 0.0 ? arwlf_ms * 1e6 / packets : 0.0;
  const double ns_per_pair =
      at.logical_pairs == 0U ? 0.0 : arwlf_ms * 1e6 / static_cast<double>(at.logical_pairs);
  std::string choices;
  for (const std::string& c : dispatch_choices) {
    choices += (choices.empty() ? "" : "|") + c;
  }

  const std::string summary_path = config.out_dir + "/summary.csv";
  const bool fresh = !std::filesystem::exists(summary_path);
  std::ofstream summary(summary_path, std::ios::app);
  if (fresh) {
    summary << "id,n,seed,a_size,b_size,c_size,Q,W,microproblems,exactness,k2_hits25,k2_fullq32,"
               "residue,structural,"
               "residue_sum,arwlf_k32_hits,arwlf_mid_hits,k2wide_mid_hist,leaf_triples,packets,lanes,"
               "logical_pairs,rows,lane_utilization,filter_survivors,mid_bits,chunks,avx2,index_mib,"
               "build_ms,dispatch_chosen,"
               "dispatch_pred_queue_ms,dispatch_pred_arwlf_ms,dispatch_pred_k2wide_ms,dispatch_probe_ms,"
               "dispatch_ran_per_sample,entity,median_ms,p25_ms,p75_ms,samples,warmups,k2_over_entity,"
               "arwlf_ns_per_packet,arwlf_ns_per_pair,peak_ws_mib,ac_start,ac_end,power_ok,pin,pinned_cpu\n";
  }
  for (std::size_t e = 0U; e < entities.size(); ++e) {
    summary << id << ',' << workload->n << ',' << hex(workload->seed) << ',' << workload->a_size << ','
            << workload->b_size << ',' << workload->c_size << ',' << keys.full_bits << ','
            << keys.residual_bits << ',' << index.selected_count << ','
            << (exact ? "EXACT" : "DIVERGED") << ',' << dk.triples << ',' << dk.full_q << ','
            << dk.residue << ',' << dk.structural << ',' << hex(dk.residue_sum) << ','
            << k32_t.projected_hits << ',' << at.projected_hits << ',' << sweep_at_mid << ','
            << at.leaf_triples << ',' << at.packets << ',' << at.lanes << ',' << at.logical_pairs << ','
            << at.rows << ',' << fixed(lane_util, 4) << ',' << at.filter_survivors << ',' << at.mid_bits << ',' << at.chunks << ',' << at.avx2 << ','
            << fixed(static_cast<double>(at.index_bytes) / 1048576.0, 1) << ','
            << fixed(static_cast<double>(at.build_ns) * 1e-6, 2) << ',' << aw::backend_label(decision.chosen) << ',' << decision.predicted_ms[0] << ','
            << decision.predicted_ms[1] << ',' << decision.predicted_ms[2] << ','
            << fixed(decision.probe_ms, 2) << ',' << choices << ',' << entities[e].name << ','
            << fixed(stats[e].median) << ',' << fixed(stats[e].p25) << ',' << fixed(stats[e].p75)
            << ',' << stats[e].samples << ',' << config.warmups << ','
            << fixed(stats[e].median > 0.0 ? k2_ms / stats[e].median : 0.0, 4) << ','
            << fixed(ns_per_packet, 3) << ',' << fixed(ns_per_pair, 4) << ',' << fixed(peak_working_set_mib(), 1) << ',' << ac_start
            << ',' << ac_end << ',' << (power_ok ? 1 : 0) << ',' << config.pin << ',' << pinned_cpu
            << '\n';
  }
  std::cout << "#";
  for (std::size_t e = 0U; e < entities.size(); ++e) {
    std::cout << ' ' << entities[e].name << '=' << fixed(stats[e].median) << "ms";
  }
  std::cout << " | K2/ARWLF=" << fixed(arwlf_ms > 0.0 ? k2_ms / arwlf_ms : 0.0, 3)
            << "x | ARWLF ns/packet=" << fixed(ns_per_packet, 2) << " ns/pair=" << fixed(ns_per_pair, 4) << " | dispatch ran " << choices
            << " | peak_ws_MiB=" << fixed(peak_working_set_mib(), 0)
            << (power_ok ? "" : " [POWER NOT AC]") << '\n';
  return 0;
}
