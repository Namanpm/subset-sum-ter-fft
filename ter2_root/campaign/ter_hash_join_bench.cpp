// Matched prepared-root comparison. No snapshot capture, thinning or solver dispatch.
#include "ter_hash_join_root.hpp"
#include "ter_arwlf_campaign.hpp"
#include "ter_bench_host.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace hj = ter::hash_join_root;
namespace ac = ter::arwlf_campaign;
namespace aw = ter::arwlf;
namespace a64 = ter::a64;
namespace rg = ter::root_generalization;
namespace host = ter::bench_host;
using Clock = std::chrono::steady_clock;
using Witness = ac::Witness;

struct Config {
  std::string snapshot;
  std::string out;
  std::string pin = "p-core";
  std::uint32_t microproblems = 4096U;
  std::uint32_t warmups = 3U;
  std::uint32_t samples = 7U;
  std::size_t max_witnesses = 1000000U;
  std::size_t memory_limit_mib = 2048U;
  bool validate_only = false;
  bool plant = false;
};

template <class T>
bool number(const std::string_view text, T& result) {
  if (text.empty()) {
    return false;
  }
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
  return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

bool parse(const int argc, char** argv, Config& c) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const auto after = [&](const std::string_view prefix) { return arg.substr(prefix.size()); };
    if (arg.starts_with("--snapshot=")) {
      c.snapshot = after("--snapshot=");
    } else if (arg.starts_with("--out=")) {
      c.out = after("--out=");
    } else if (arg.starts_with("--pin=")) {
      c.pin = after("--pin=");
    } else if (arg.starts_with("--microproblems=")) {
      if (!number(after("--microproblems="), c.microproblems)) return false;
    } else if (arg.starts_with("--warmups=")) {
      if (!number(after("--warmups="), c.warmups)) return false;
    } else if (arg.starts_with("--samples=")) {
      if (!number(after("--samples="), c.samples)) return false;
    } else if (arg.starts_with("--max-witnesses=")) {
      if (!number(after("--max-witnesses="), c.max_witnesses)) return false;
    } else if (arg.starts_with("--memory-limit-mib=")) {
      if (!number(after("--memory-limit-mib="), c.memory_limit_mib)) return false;
    } else if (arg == "--validate-only") {
      c.validate_only = true;
    } else if (arg == "--plant=1") {
      c.plant = true;
    } else {
      return false;
    }
  }
  return !c.snapshot.empty() && (c.microproblems == 1U || c.microproblems == 4096U) &&
         (c.pin == "p-core" || c.pin == "none") && (c.validate_only || c.samples != 0U) &&
         c.samples <= std::numeric_limits<std::uint32_t>::max() - c.warmups &&
         c.memory_limit_mib <= std::numeric_limits<std::size_t>::max() / 1048576U;
}

double elapsed(const Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

std::string csv(const std::string& value) {
  std::string result = "\"";
  for (const char ch : value) {
    if (ch == '"') result += '"';
    result += ch;
  }
  return result + '"';
}

bool same_answer(const rg::OutputDigest& a, const rg::OutputDigest& b) {
  return a.residue == b.residue && a.structural == b.structural && a.residue_sum == b.residue_sum;
}

bool same_raw(const rg::OutputDigest& a, const rg::OutputDigest& b) {
  return a.triples == b.triples && a.triple_sum == b.triple_sum &&
         a.triple_xor == b.triple_xor && a.triple_sum2 == b.triple_sum2 && a.full_q == b.full_q;
}

bool clean(const rg::OutputDigest& d) {
  return d.directory_misses == 0U && d.residual_mismatches == 0U;
}

struct Pass {
  rg::OutputDigest digest;
  hj::Telemetry telemetry{};
  std::vector<Witness> witnesses;
};

int run(const Config& c) {
  // Reject lexical aliases, symlinks and hard links BEFORE loading or opening
  // anything: --out must never truncate the captured input snapshot.
  if (!c.out.empty()) {
    const auto input = std::filesystem::weakly_canonical(std::filesystem::absolute(c.snapshot));
    const auto output = std::filesystem::weakly_canonical(std::filesystem::absolute(c.out));
    std::error_code error;
    if (input == output || std::filesystem::equivalent(input, output, error)) {
      std::cerr << "OutputInvalid: output CSV aliases the input snapshot\n";
      return 2;
    }
  }
  const std::vector<int> classes = host::efficiency_classes();
  int pinned = -1;
  if (!c.validate_only) {
#ifndef NDEBUG
    std::cerr << "ReleaseRequired: timed runs require a Release/NDEBUG build\n";
    return 5;
#endif
#ifndef _WIN32
    std::cerr << "HostNotApplicable: controlled timing requires Windows AC/CPU checks\n";
    return 5;
#endif
    if (host::ac_line_status() != 1) {
      std::cerr << "PowerInvalid: timing requires verified AC power\n";
      return 5;
    }
    if (c.pin == "p-core") {
      pinned = host::pin_to_p_core(classes);
      const int best = classes.empty() ? -1 : *std::max_element(classes.begin(), classes.end());
      if (pinned < 0 || host::class_of(classes, pinned) != best || host::current_cpu() != pinned) {
        std::cerr << "PinInvalid: could not enforce highest-efficiency-core affinity\n";
        return 5;
      }
    }
  }

  const auto setup_start = Clock::now();
  auto workload = rg::load_snapshot_workload(c.snapshot, true);
  if (workload->status != rg::WorkloadStatus::Completed || !workload->authority.ok || !workload->data) {
    std::cerr << "SnapshotNotApplicable: " << rg::status_label(workload->status) << ' '
              << workload->status_detail << '\n';
    return 3;
  }
  if (workload->family != rg::WorkloadFamily::AuthenticFullTer) {
    std::cerr << "SnapshotNotApplicable: authentic snapshot required\n";
    return 3;
  }
  auto& data = *workload->data;
  rg::RootIndex index;
  rg::build_root_index(data, index);
  rg::select_microproblems(index, 1U);
  std::uint32_t anchor = 0U;
  while (anchor < ac::kCells && index.selected[anchor] == 0U) ++anchor;
  if (anchor != 2730U) {
    std::cerr << "SelectionInvalid: expected deterministic one-cell anchor c2730\n";
    return 4;
  }
  std::optional<ac::PlantedTargets> planted;
  if (c.plant) {
    planted.emplace(*workload, anchor, ac::Plant{ac::PlantKind::Positive, 1U, 0x484153484A4F494EULL});
    rg::build_root_index(data, index);
  }
  rg::select_microproblems(index, c.microproblems);
  const a64::FamilyKeys keys = a64::build_family_keys(*workload);
  if (!keys.ok) {
    std::cerr << "NotApplicable: full quotient keys unavailable\n";
    return 3;
  }
  std::uint64_t logical_pairs = 0U;
  for (std::uint32_t s = 0U; s < rg::kFamilies; ++s) {
    for (std::uint32_t t = 0U; t < rg::kFamilies; ++t) {
      if (!index.active(s, t)) continue;
      const auto q = static_cast<std::uint64_t>(data.q_family[s].size());
      const auto x = static_cast<std::uint64_t>(data.x_family[t].size());
      if (x != 0U && q > std::numeric_limits<std::uint64_t>::max() / x) {
        std::cerr << "ResourceLimit: logical-pair count overflow\n";
        return 6;
      }
      const std::uint64_t pairs = q * x;
      if (pairs > std::numeric_limits<std::uint64_t>::max() - logical_pairs) {
        std::cerr << "ResourceLimit: logical-pair sum overflow\n";
        return 6;
      }
      logical_pairs += pairs;
    }
  }
  const double setup_ms = elapsed(setup_start);
  const hj::Config hash_config{c.memory_limit_mib * 1048576U};
  const auto make_sink = [&](std::vector<Witness>* witnesses) {
    rg::RootSink sink;
    sink.data = &data;
    sink.index = &index;
    sink.authority = &workload->authority;
    sink.witnesses = witnesses;
    sink.reset();
    return sink;
  };

  // RootSink::emit_triple is noexcept. Count without collection first, then
  // reserve the FULL raw triple bound (not merely residue survivors). This
  // also leaves room for erroneous residue acceptance to reach the audit.
  const auto exact_start = Clock::now();
  rg::RootSink bound = make_sink(nullptr);
  a64::run_k2_wide(data, index, keys, bound, nullptr, false);
  if (!clean(bound.digest)) {
    std::cerr << "Diverged: K2_WIDE count-only authority pass is not clean\n";
    return 4;
  }
  // Gate the NEW backend without witness collection before trusting this bound.
  // A bad duplicate/residue implementation must fail before noexcept push_back.
  rg::RootSink hash_count = make_sink(nullptr);
  hj::Telemetry hash_count_t;
  const hj::Status hash_count_status =
      hj::run_hash_join(data, index, keys, hash_config, hash_count, &hash_count_t);
  if (hash_count_status != hj::Status::Completed) {
    std::cerr << "hash_join " << hj::status_label(hash_count_status) << ": count-only gate failed\n";
    return 3;
  }
  if (!clean(hash_count.digest) || !same_raw(hash_count.digest, bound.digest) ||
      !same_answer(hash_count.digest, bound.digest) || hash_count_t.logical_pairs != logical_pairs ||
      hash_count_t.full_hits != bound.digest.triples) {
    std::cerr << "Diverged: hash_join count-only gate disagrees with K2_WIDE; no collection/timing\n";
    return 4;
  }
  if (bound.digest.triples > c.max_witnesses ||
      bound.digest.triples > std::numeric_limits<std::size_t>::max() / sizeof(Witness)) {
    std::cerr << "ResourceLimit: full-key raw triple bound " << bound.digest.triples
              << " exceeds --max-witnesses/storage bound; no collection or timing\n";
    return 6;
  }
  const auto reserve_count = static_cast<std::size_t>(bound.digest.triples);
  const std::array<const char*, 4> names{"k2", "k2_wide", "arwlf", "hash_join"};
  std::array<Pass, 4> audit;
  for (auto& p : audit) p.witnesses.reserve(reserve_count);
  const auto call = [&](const std::size_t backend, rg::RootSink& sink, hj::Telemetry& telemetry) {
    sink.reset();
    telemetry = hj::Telemetry{};
    if (backend == 0U) {
      rg::run_k2(data, sink);
    } else if (backend == 1U) {
      a64::run_k2_wide(data, index, keys, sink, nullptr, false);
    } else if (backend == 2U) {
      const aw::Status status = aw::run_arwlf(data, index, keys, aw::Config{}, sink, nullptr);
      if (status != aw::Status::Completed) {
        std::cerr << "ARWLF " << aw::status_label(status) << ": no accepted timing\n";
        return false;
      }
    } else {
      const hj::Status status = hj::run_hash_join(data, index, keys, hash_config, sink, &telemetry);
      if (status != hj::Status::Completed) {
        std::cerr << "hash_join " << hj::status_label(status) << ": no accepted timing\n";
        return false;
      }
    }
    return true;
  };
  for (std::size_t b = 0U; b < audit.size(); ++b) {
    auto sink = make_sink(&audit[b].witnesses);
    if (!call(b, sink, audit[b].telemetry)) return 3;
    audit[b].digest = sink.digest;
    std::sort(audit[b].witnesses.begin(), audit[b].witnesses.end());
  }
  const auto& reference = audit[1].witnesses;
  const auto& wide = audit[1].digest;
  bool exact = same_raw(wide, bound.digest) && same_answer(wide, bound.digest) &&
               reference.size() == bound.digest.residue;
  for (std::size_t b = 0U; b < audit.size(); ++b) {
    exact = exact && clean(audit[b].digest) && same_answer(audit[b].digest, wide) &&
            audit[b].witnesses == reference;
    // K2 emits 25-bit candidates; its raw digest is intentionally NOT compared.
    if (b != 0U) exact = exact && same_raw(audit[b].digest, wide);
  }
  exact = exact && audit[3].telemetry.logical_pairs == logical_pairs &&
          audit[3].telemetry.full_hits == wide.triples;
  if (planted) {
    std::vector<Witness> found;
    for (const auto& w : reference) if (w[2] >= planted->first_planted()) found.push_back(w);
    exact = exact && found == planted->expected() && wide.structural >= planted->expected().size();
  }
  const double exactness_ms = elapsed(exact_start);
  if (!exact) {
    std::cerr << "Diverged: exact witness/count/full-key digest gate failed; no timing\n";
    return 4;
  }
  std::cerr << "EXACT: " << (c.plant ? "AUTHENTIC_PLANTED_TARGETS" : "AUTHENTIC_FULLTER")
            << " selected=" << index.selected_count << " logical_pairs=" << logical_pairs
            << " full_key_hits=" << wide.triples << " residue=" << wide.residue
            << " structural=" << wide.structural << " untimed_exactness_ms=" << exactness_ms
            << "; this is not a global UNSAT claim\n";

  std::ofstream file;
  if (!c.out.empty()) {
    const auto parent = std::filesystem::path(c.out).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);
    file.open(c.out, std::ios::out | std::ios::trunc);
    if (!file) throw std::runtime_error("cannot open output CSV");
  }
  const auto emit = [&](const std::string& row) {
    std::cout << row << '\n';
    if (file.is_open()) {
      file << row << '\n';
      file.flush();
      if (!file) throw std::runtime_error("output CSV write failed");
    }
  };
  emit("phase,source_kind,snapshot,n,seed,Q,microproblems,backend,sample,position,ms,ns_per_pair,logical_pairs,raw_triples,full_key_hits,full_q32,residue,structural,residue_sum,triple_sum,triple_xor,triple_sum2,low_hits,tables_built,auxiliary_bytes,witness_bound,setup_ms,exactness_ms,pin,pinned_cpu,cpu_start,cpu_end,class_start,class_end,ac_start,ac_end,host_valid,exact,peak_working_set_mib");
  const auto row = [&](const std::string& phase, const std::size_t b, const std::uint32_t sample,
                       const std::size_t position, const std::optional<double> ms,
                       const rg::OutputDigest& d, const hj::Telemetry& ht,
                       const int cpu_start, const int cpu_end, const int ac_start, const int ac_end) {
    std::ostringstream text;
    text << std::setprecision(12) << phase << ','
         << (c.plant ? "AUTHENTIC_PLANTED_TARGETS" : "AUTHENTIC_FULLTER") << ','
         << csv(c.snapshot) << ',' << workload->n << ',' << workload->seed << ',' << keys.full_bits
         << ',' << index.selected_count << ',' << names[b] << ',' << sample << ',' << position << ',';
    if (ms) text << *ms;
    text << ',';
    if (ms && logical_pairs != 0U) text << *ms * 1e6 / static_cast<double>(logical_pairs);
    text << ',' << logical_pairs << ',' << d.triples << ',' << wide.triples << ',' << d.full_q
         << ',' << d.residue << ',' << d.structural << ',' << d.residue_sum << ',' << d.triple_sum
         << ',' << d.triple_xor << ',' << d.triple_sum2 << ',';
    if (b == 3U) text << ht.low_hits;
    text << ',';
    if (b == 3U) text << ht.tables_built;
    text << ',';
    if (b == 3U) text << ht.auxiliary_bytes;
    text << ',' << reserve_count << ',' << setup_ms << ',' << exactness_ms << ','
         << (c.validate_only ? "not_requested" : c.pin) << ',' << pinned << ',' << cpu_start << ','
         << cpu_end << ',' << host::class_of(classes, cpu_start) << ',' << host::class_of(classes, cpu_end)
         << ',' << ac_start << ',' << ac_end << ',';
    if (!c.validate_only) text << 1;
    text << ",1," << host::peak_working_set_mib();
    emit(text.str());
  };
  if (c.validate_only) {
    for (std::size_t b = 0U; b < audit.size(); ++b) {
      row("validation", b, 0U, b, std::nullopt, audit[b].digest, audit[b].telemetry,
          host::current_cpu(), host::current_cpu(), host::ac_line_status(), host::ac_line_status());
    }
    return 0;
  }

  std::vector<Witness> timed_witnesses;
  timed_witnesses.reserve(reserve_count);
  auto timed = make_sink(&timed_witnesses);
  const std::uint32_t rounds = c.warmups + c.samples;
  for (std::uint32_t round = 0U; round < rounds; ++round) {
    const auto order = ac::round_order(names.size(), round);
    for (std::size_t position = 0U; position < order.size(); ++position) {
      const auto b = order[position];
      const int power_start = host::ac_line_status();
      const int cpu_start = host::current_cpu();
      if (power_start != 1 || (pinned >= 0 && cpu_start != pinned)) {
        std::cerr << "HostInvalid: AC/affinity check failed before shot\n";
        return 5;
      }
      hj::Telemetry ht;
      const auto start = Clock::now();
      const bool completed = call(b, timed, ht);  // reset and all backend table builds charged
      const double ms = elapsed(start);
      const int cpu_end = host::current_cpu();
      const int power_end = host::ac_line_status();
      if (!completed) return 3;
      if (power_end != 1 || (pinned >= 0 && cpu_end != pinned)) {
        std::cerr << "HostInvalid: AC/affinity check failed after shot\n";
        return 5;
      }
      // Sorting, exact multiset audit and CSV reporting are outside timing.
      std::sort(timed_witnesses.begin(), timed_witnesses.end());
      if (!clean(timed.digest) || !same_answer(timed.digest, wide) || timed_witnesses != reference ||
          (b != 0U && !same_raw(timed.digest, wide)) ||
          (b == 0U && !same_raw(timed.digest, audit[0].digest)) ||
          (b == 3U && (ht.logical_pairs != logical_pairs || ht.full_hits != wide.triples))) {
        std::cerr << "Diverged: " << names[b] << " output drifted in round " << round << '\n';
        return 4;
      }
      if (round >= c.warmups) {
        row("sample", b, round - c.warmups, position, ms, timed.digest, ht,
            cpu_start, cpu_end, power_start, power_end);
      }
    }
  }
  return 0;
}
}  // namespace

int main(const int argc, char** argv) {
  Config c;
  if (!parse(argc, argv, c)) {
    std::cerr << "usage: ter_hash_join_bench --snapshot=PATH [--microproblems=1|4096]"
                 " [--warmups=3] [--samples=7] [--pin=p-core|none] [--out=CSV]"
                 " [--validate-only] [--plant=1] [--max-witnesses=1000000]"
                 " [--memory-limit-mib=2048]\n";
    return 2;
  }
  try {
    return run(c);
  } catch (const std::bad_alloc&) {
    std::cerr << "ResourceLimit: allocation failed; no complete-result claim\n";
    return 6;
  } catch (const std::exception& e) {
    std::cerr << "Failed: " << e.what() << '\n';
    return 3;
  }
}
