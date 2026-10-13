// ARWLF whole-root run (ARWLF_WHOLE_ROOT.md): one frozen backend over every
// microproblem of one authentic root, in one process.
//
//   ter_arwlf_whole_root --snapshot=PATH --out-dir=DIR --run=ID --backend=k2|k2_wide|arwlf|dispatch
//                        [--pin=p-core|none] [--memory-limit-mib=M] [--allow-debug]
//
// The root carries exactly one planted final output (the positive-output
// construction, M = 1, seed 0xB1A57001, on the anchor microproblem), so every
// run must report it.  Setup (load, plant, keys, index, census) is untimed and
// recorded; the timed region of every chunk is the regime campaign's.  K2 and
// K2_WIDE run one chunk per target family (see ter_arwlf_whole_root.hpp), each
// appended to runs.jsonl as it finishes; a rerun skips finished chunks.
// Exit codes: 0 done (or RESOURCE_LIMIT recorded), 2 refused, 3 load failure,
// 4 exactness failure (row written), 5 pinning failure, 6 error.

#include "ter_arwlf.hpp"
#include "ter_arwlf_campaign.hpp"
#include "ter_arwlf_whole_root.hpp"
#include "ter_bench_host.hpp"
#include "ter_microreport8_kernels.hpp"
#include "ter_root_generalization_workload.hpp"
#include "ter_root_queue_kernels.hpp"

#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace aw = ter::arwlf;
namespace ac = ter::arwlf_campaign;
namespace wr = ter::arwlf_whole_root;
namespace a64 = ter::a64;
namespace rg = ter::root_generalization;
namespace mr8 = ter::microreport8;
namespace host = ter::bench_host;

using Clock = std::chrono::steady_clock;

constexpr std::uint64_t kPlantSeed = 0xB1A57001ULL;  // ARWLF_WHOLE_ROOT.md

[[nodiscard]] double ms_since(const Clock::time_point start) noexcept {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

[[nodiscard]] std::string hex(const std::uint64_t value) {
  std::ostringstream out;
  out << "0x" << std::hex << value;
  return out.str();
}

[[nodiscard]] std::string fixed(const double value, const int digits) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(digits) << value;
  return out.str();
}

[[nodiscard]] std::string compiler() {
#if defined(_MSC_FULL_VER)
  return "MSVC " + std::to_string(_MSC_FULL_VER);
#elif defined(__clang__)
  return std::string("Clang ") + __clang_version__;
#elif defined(__GNUC__)
  return std::string("GCC ") + __VERSION__;
#else
  return "unknown";
#endif
}

[[nodiscard]] const char* build_type() noexcept {
#if defined(NDEBUG)
  return "Release";
#else
  return "Debug";
#endif
}

[[nodiscard]] std::string utc_now() {
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &now);
#else
  gmtime_r(&now, &tm);
#endif
  std::ostringstream out;
  out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  return out.str();
}

// One JSON object, fields in insertion order.  Keys and string values here are
// plain identifiers, paths and labels; quotes and backslashes are escaped.
class Json {
 public:
  Json& str(const std::string& key, const std::string& value) {
    std::string escaped;
    for (const char c : value) {
      if (c == '"' || c == '\\') {
        escaped.push_back('\\');
      }
      escaped.push_back(c);
    }
    return raw(key, "\"" + escaped + "\"");
  }
  Json& u64(const std::string& key, const std::uint64_t value) { return raw(key, std::to_string(value)); }
  Json& i64(const std::string& key, const std::int64_t value) { return raw(key, std::to_string(value)); }
  Json& num(const std::string& key, const double value, const int digits) { return raw(key, fixed(value, digits)); }
  Json& flag(const std::string& key, const bool value) { return raw(key, value ? "true" : "false"); }
  Json& raw(const std::string& key, const std::string& value) {
    text_ += (text_.empty() ? "{" : ",") + ("\"" + key + "\":") + value;
    return *this;
  }
  [[nodiscard]] std::string done() const { return text_ + "}"; }

 private:
  std::string text_;
};

[[nodiscard]] std::string witness_list(const std::vector<wr::Witness>& witnesses) {
  std::string out = "[";
  for (std::size_t i = 0U; i < witnesses.size(); ++i) {
    const wr::Witness& w = witnesses[i];
    out += (i == 0U ? "[" : ",[") + std::to_string(w[0]) + "," + std::to_string(w[1]) + "," + std::to_string(w[2]) +
           "]";
  }
  return out + "]";
}

// Value of a top-level string field of one runs.jsonl line ("" if absent).
[[nodiscard]] std::string field(const std::string& line, const std::string& key) {
  const std::string pattern = "\"" + key + "\":\"";
  const std::size_t at = line.find(pattern);
  if (at == std::string::npos) {
    return "";
  }
  const std::size_t begin = at + pattern.size();
  const std::size_t end = line.find('"', begin);
  return end == std::string::npos ? "" : line.substr(begin, end - begin);
}

struct Config {
  std::string snapshot;
  std::string out_dir;
  std::string run;
  std::string backend;
  std::string pin = "p-core";
  double memory_limit_mib = 4096.0;
  bool allow_debug = false;
};

[[nodiscard]] bool parse(const int argc, char** argv, Config& config) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const auto take = [&](const std::string_view prefix, std::string& out) {
      if (arg.rfind(prefix, 0) != 0) {
        return false;
      }
      out = std::string(arg.substr(prefix.size()));
      return true;
    };
    std::string value;
    if (take("--snapshot=", config.snapshot) || take("--out-dir=", config.out_dir) || take("--run=", config.run) ||
        take("--backend=", config.backend) || take("--pin=", config.pin)) {
      continue;
    }
    if (take("--memory-limit-mib=", value)) {
      config.memory_limit_mib = std::stod(value);
    } else if (arg == "--allow-debug") {
      config.allow_debug = true;
    } else {
      std::cerr << "unknown argument: " << arg << '\n';
      return false;
    }
  }
  return !config.snapshot.empty() && !config.out_dir.empty() && !config.run.empty() &&
         config.run.find('"') == std::string::npos && wr::parse_backend(config.backend).has_value() &&
         (config.pin == "p-core" || config.pin == "none");
}

int run(const Config& config, const wr::Backend backend, const int pinned) {
  const std::filesystem::path out_dir(config.out_dir);
  std::filesystem::create_directories(out_dir);
  const std::filesystem::path runs_path = out_dir / "runs.jsonl";
  const std::string root = std::filesystem::path(config.snapshot).stem().string();

  // Finished chunks of this run; a run id never changes backend or root.
  std::set<std::string> done;
  {
    std::ifstream in(runs_path);
    std::string line;
    while (std::getline(in, line)) {
      if (field(line, "run_id") != config.run) {
        continue;
      }
      if (field(line, "backend") != wr::label(backend) || field(line, "root") != root) {
        std::cout << "# refused: run " << config.run << " already recorded with another backend or root\n";
        return 2;
      }
      if (field(line, "status") == "COMPLETED") {
        done.insert(field(line, "chunk"));
      }
    }
  }
  std::vector<std::string> chunks;
  if (wr::family_chunked(backend)) {
    for (std::uint32_t u = 0U; u < rg::kFamilies; ++u) {
      chunks.push_back((u < 10U ? "u0" : "u") + std::to_string(u));
    }
  } else {
    chunks.emplace_back("all");
  }
  std::size_t remaining = 0U;
  for (const std::string& chunk : chunks) {
    remaining += done.count(chunk) == 0U ? 1U : 0U;
  }
  if (remaining == 0U) {
    std::cout << "# run " << config.run << " already complete\n";
    return 0;
  }

  // --- untimed setup ---------------------------------------------------------
  auto start = Clock::now();
  auto workload = rg::load_snapshot_workload(config.snapshot, /*with_authority=*/true);
  const double load_ms = ms_since(start);
  std::cout << "# snapshot " << config.snapshot << " : " << rg::status_label(workload->status) << " ("
            << fixed(load_ms / 1000.0, 1) << " s)\n";
  if (workload->status != rg::WorkloadStatus::Completed || !workload->authority.ok) {
    return 3;
  }
  mr8::Dataset& data = *workload->data;
  std::uint32_t anchor = ac::kCells;
  {
    rg::RootIndex probe;
    rg::build_root_index(data, probe);
    rg::select_microproblems(probe, 1U);  // the campaign's anchor microproblem
    for (std::uint32_t cell = 0U; cell < ac::kCells; ++cell) {
      anchor = probe.selected[cell] != 0U ? cell : anchor;
    }
  }
  start = Clock::now();
  const ac::PlantedTargets planted(*workload, anchor, ac::Plant{ac::PlantKind::Positive, 1U, kPlantSeed});
  const double plant_ms = ms_since(start);
  const wr::Witness expected = planted.expected().at(0);
  const std::uint32_t anchor_u = (anchor / rg::kFamilies + anchor % rg::kFamilies) & (rg::kFamilies - 1U);
  start = Clock::now();
  const a64::FamilyKeys keys = a64::build_family_keys(*workload);
  const double keys_ms = ms_since(start);
  if (!keys.ok) {
    std::cout << "# NotApplicable: full keys unavailable\n";
    return 3;
  }
  start = Clock::now();
  rg::RootIndex index;
  rg::build_root_index(data, index);
  rg::RadixTables tables;
  tables.allocate();
  const double index_ms = ms_since(start);
  start = Clock::now();
  const std::vector<ac::Geometry> census = ac::census(keys, index);
  const double census_ms = ms_since(start);
  std::cout << "# root " << root << " n=" << workload->n << " Q=" << keys.full_bits << " anchor cell " << anchor
            << " (u=" << anchor_u << ") planted (" << expected[0] << ", " << expected[1] << ", " << expected[2]
            << ") run " << config.run << " backend " << wr::label(backend) << ", " << remaining << " of "
            << chunks.size() << " chunks to run\n";

  std::ofstream out(runs_path, std::ios::app);
  const auto row = [&](const std::string& chunk, const std::string& status) {
    Json j;
    j.str("run_id", config.run)
        .str("chunk", chunk)
        .str("backend", wr::label(backend))
        .str("status", status)
        .str("root", root)
        .u64("n", workload->n)
        .str("root_seed", hex(workload->seed))
        .u64("Q", keys.full_bits)
        .u64("W", keys.residual_bits)
        .str("git_commit", TER_CONFIGURE_TIME_COMMIT)
        .i64("git_dirty_at_configure", TER_CONFIGURE_TIME_DIRTY)
        .str("compiler", compiler())
        .str("build", build_type())
        .str("pin", config.pin)
        .i64("pinned_cpu", pinned)
        .u64("anchor_cell", anchor)
        .str("plant_seed", hex(kPlantSeed))
        .u64("first_planted", planted.first_planted())
        .raw("planted", witness_list({expected}))
        .num("setup_load_ms", load_ms, 1)
        .num("setup_plant_ms", plant_ms, 1)
        .num("setup_keys_ms", keys_ms, 1)
        .num("setup_index_ms", index_ms, 1)
        .num("setup_census_ms", census_ms, 1)
        .str("utc", utc_now());
    return j;
  };
  const auto write = [&](const Json& j) {
    out << j.done() << '\n';
    out.flush();
    if (!out) {
      throw std::runtime_error("cannot append to " + runs_path.string());
    }
  };

  const host::Memory setup_memory = host::process_memory();
  if (setup_memory.working_set_mib > config.memory_limit_mib) {
    Json j = row("all", "RESOURCE_LIMIT");
    j.num("working_set_mib", setup_memory.working_set_mib, 1)
        .str("detail", "working set above " + fixed(config.memory_limit_mib, 0) + " MiB after setup");
    write(j);
    std::cout << "# RESOURCE_LIMIT: working set " << fixed(setup_memory.working_set_mib, 1) << " MiB\n";
    return 0;
  }

  // --- timed chunks ------------------------------------------------------------
  for (std::size_t c = 0U; c < chunks.size(); ++c) {
    const std::string& chunk = chunks[c];
    if (done.count(chunk) != 0U) {
      continue;
    }
    const int ac_start = host::ac_line_status();
    if (ac_start == 0) {
      std::cout << "# refused: on battery (rerun to resume)\n";
      return 2;
    }
    if (wr::family_chunked(backend)) {
      wr::select_family(index, static_cast<std::uint32_t>(c));
    } else {
      wr::select_all(index);
    }
    std::uint64_t cells = 0U;
    std::uint64_t pairs = 0U;
    bool has_anchor = false;
    for (std::uint32_t cell = 0U; cell < ac::kCells; ++cell) {
      if (index.selected[cell] != 0U) {
        ++cells;
        pairs += census[cell].logical_pairs;
        has_anchor = has_anchor || cell == anchor;
      }
    }
    const int cpu_start = host::current_cpu();
    const wr::Pass pass = wr::run_selection(backend, data, index, keys, tables, workload->authority);
    const int cpu_end = host::current_cpu();
    const int ac_end = host::ac_line_status();
    const host::Memory memory = host::process_memory();

    // The planted output must be reported by exactly the chunk holding the anchor.
    std::vector<wr::Witness> planted_found;
    for (const wr::Witness& w : pass.witnesses) {
      if (w[2] >= planted.first_planted()) {
        planted_found.push_back(w);
      }
    }
    const bool planted_ok = has_anchor ? planted_found == std::vector<wr::Witness>{expected} : planted_found.empty();
    const bool status_ok = backend != wr::Backend::Arwlf || pass.arwlf_status == aw::Status::Completed;
    const bool clean = pass.digest.directory_misses == 0U && pass.digest.residual_mismatches == 0U &&
                       pass.witnesses.size() == pass.digest.residue;
    const bool exact = planted_ok && status_ok && clean;
    const bool on_core = pinned < 0 || (cpu_start == pinned && cpu_end == pinned);
    Json j = row(chunk, exact ? "COMPLETED" : "EXACTNESS_FAILURE");
    j.u64("cells", cells)
        .u64("logical_pairs", pairs)
        .num("ms", pass.ms, 4)
        .i64("cpu_start", cpu_start)
        .i64("cpu_end", cpu_end)
        .flag("on_pinned_core", on_core)
        .i64("ac_start", ac_start)
        .i64("ac_end", ac_end)
        .u64("residue_exact", pass.digest.residue)
        .u64("structural", pass.digest.structural)
        .u64("full_q", pass.digest.full_q)
        .u64("pair_hits", pass.digest.pair_hits)
        .str("residue_sum", hex(pass.digest.residue_sum))
        .u64("directory_misses", pass.digest.directory_misses)
        .u64("residual_mismatches", pass.digest.residual_mismatches)
        .flag("planted_ok", planted_ok)
        .raw("witnesses", witness_list(pass.witnesses))
        .str("arwlf_status", backend == wr::Backend::Arwlf ? (status_ok ? "Completed" : "NOT_COMPLETED") : "")
        .str("dispatch_ran", backend == wr::Backend::Dispatch ? aw::backend_label(pass.dispatch.ran) : "")
        .num("dispatch_probe_ms", backend == wr::Backend::Dispatch ? pass.dispatch.probe_ms : 0.0, 3)
        .num("working_set_mib", memory.working_set_mib, 1)
        .num("peak_working_set_mib", memory.peak_working_set_mib, 1);
    write(j);
    std::cout << "# " << config.run << " " << chunk << " : " << fixed(pass.ms / 1000.0, 2) << " s, " << cells
              << " cells, " << pass.witnesses.size() << " outputs"
              << (backend == wr::Backend::Dispatch ? std::string(" (ran ") + aw::backend_label(pass.dispatch.ran) + ")"
                                                   : std::string())
              << (exact ? "" : "  EXACTNESS FAILURE") << '\n';
    if (!exact) {
      return 4;
    }
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Config config;
  if (!parse(argc, argv, config)) {
    std::cerr << "usage: ter_arwlf_whole_root --snapshot=PATH --out-dir=DIR --run=ID "
                 "--backend=k2|k2_wide|arwlf|dispatch [--pin=p-core|none] [--memory-limit-mib=M] [--allow-debug]\n";
    return 2;
  }
  if (std::string(build_type()) != "Release" && !config.allow_debug) {
    std::cout << "# refused: not a Release build (timings would be meaningless)\n";
    return 2;
  }
  if (host::ac_line_status() == 0) {
    std::cout << "# refused: on battery\n";
    return 2;
  }
  if (!aw::avx2::translation_unit_is_avx2() || !mr8::detect_isa().usable) {
    std::cout << "# refused: the AVX2 ARWLF path is unavailable on this build or CPU\n";
    return 2;
  }
  int pinned = -1;
  if (config.pin == "p-core") {
    pinned = host::pin_to_p_core(host::efficiency_classes());
    if (pinned < 0) {
      std::cout << "# refused: P-core pinning failed\n";
      return 5;
    }
  }
  try {
    return run(config, *wr::parse_backend(config.backend), pinned);
  } catch (const std::exception& error) {
    std::cout << "# error: " << error.what() << '\n';
    return 6;
  }
}
