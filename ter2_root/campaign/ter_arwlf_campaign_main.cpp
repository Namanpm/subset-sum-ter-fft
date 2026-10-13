// ARWLF regime campaign: one root snapshot, one phase, one fresh process.
//
// Phases (preregistered; see ARWLF_REGIME_CAMPAIGN.md):
//   census      geometry of all 4096 microproblems (no backend runs) and the
//               stratified selection, written per root;
//   stratified  the 4 x 4 stratified authentic microproblems, discovery protocol;
//   anchor      the declared microproblem (the first of the fixed 0x5EED5EED
//               shuffle, as every earlier n=108 run), final protocol;
//   thin-qx     calibration roots only: the anchor with Q_s and X_t thinned
//               toward each target lanes-per-packet L, three thinning seeds,
//               discovery protocol (AUTHENTIC_SOURCE_THINNED);
//   thin-y      calibration roots only: the anchor with Y_u thinned at
//               p_Y = 1/2, 1/4, 1/8, discovery (AUTHENTIC_TARGET_THINNED);
//   plant       calibration roots only: the anchor with M planted final outputs
//               and, per M, a decoy twin with the same Y growth and no output,
//               discovery (AUTHENTIC_PLANTED_TARGETS / AUTHENTIC_DECOY_TARGETS);
//   whole      every microproblem of the root as one case, final protocol;
//   confirm     an explicit case list, final protocol.
//
// Every case: build the (possibly thinned) state, its keys and index, and its
// census geometry, all untimed; an exactness pass in which every backend's
// final-authority witness list must equal frozen K2's and the v3 gates hold
// (any failure writes an artifact and aborts the campaign); a memory guard
// (RESOURCE_LIMIT, never a silent skip); then matched rounds whose backend
// order rotates every round and reverses on odd rounds.  Every timed run is
// re-checked against the reference.  Rows stream to CSV and JSONL as they are
// produced; a rerun skips the cases already written.

#include "ter_a64_chqr.hpp"
#include "ter_arwlf.hpp"
#include "ter_arwlf_campaign.hpp"
#include "ter_bench_host.hpp"
#include "ter_microreport8_kernels.hpp"
#include "ter_root_generalization_workload.hpp"
#include "ter_root_queue_kernels.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifndef TER_CONFIGURE_TIME_COMMIT
#define TER_CONFIGURE_TIME_COMMIT "unknown"
#endif
#ifndef TER_CONFIGURE_TIME_DIRTY
#define TER_CONFIGURE_TIME_DIRTY -1
#endif

namespace {

namespace aw = ter::arwlf;
namespace ac = ter::arwlf_campaign;
namespace a64 = ter::a64;
namespace rg = ter::root_generalization;
namespace mr8 = ter::microreport8;
namespace host = ter::bench_host;

using Clock = std::chrono::steady_clock;
using Witness = std::array<ter::EntryId, 3>;

volatile std::uint64_t g_sink = 0U;

// Preregistered sweep constants.
constexpr std::array<double, 12> kTargetLanes{1.0, 2.0, 4.0, 8.0, 12.0, 16.0, 20.0, 24.0, 32.0, 48.0, 64.0, 96.0};
constexpr std::array<std::uint64_t, 3> kThinningSeeds{0xA11CE001ULL, 0xA11CE002ULL, 0xA11CE003ULL};
constexpr std::array<double, 3> kTargetRetention{0.5, 0.25, 0.125};
// Positive-output experiment (Amendment 2): planted final outputs per case,
// each with a matched decoy control of the same Y growth.
constexpr std::array<std::uint32_t, 5> kPlantCounts{100U, 1000U, 10000U, 100000U, 1000000U};
constexpr std::uint64_t kPlantSeed = 0xB1A57001ULL;
constexpr std::size_t kSentinelEvery = 20U;

struct Protocol {
  const char* name;
  std::size_t warmups;
  std::size_t samples;
};
constexpr Protocol kDiscovery{"discovery", 1U, 3U};
constexpr Protocol kFinal{"final", 3U, 7U};
constexpr Protocol kSmoke{"smoke", 0U, 1U};  // development only; labelled in every row

[[nodiscard]] double ms_since(const Clock::time_point start) noexcept {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

[[nodiscard]] std::string fixed(const double value, const int digits) {
  std::ostringstream text;
  text << std::fixed << std::setprecision(digits) << value;
  return text.str();
}

[[nodiscard]] std::string hex(const std::uint64_t value) {
  std::ostringstream text;
  text << "0x" << std::hex << value;
  return text.str();
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

// ---------------------------------------------------------------------------
// Rows: one field list, written as a CSV line and as a JSON object.
// ---------------------------------------------------------------------------
class Row {
 public:
  Row& str(const std::string_view key, const std::string_view value) {
    fields_.push_back(Field{std::string(key), csv_text(value), json_string(value)});
    return *this;
  }
  Row& num(const std::string_view key, const double value, const int digits = 6) {
    if (std::isfinite(value)) {
      const std::string text = fixed(value, digits);
      fields_.push_back(Field{std::string(key), text, text});
    } else {
      fields_.push_back(Field{std::string(key), value > 0.0 ? "inf" : (value < 0.0 ? "-inf" : "nan"), "null"});
    }
    return *this;
  }
  Row& u64(const std::string_view key, const std::uint64_t value) {
    const std::string text = std::to_string(value);
    fields_.push_back(Field{std::string(key), text, text});
    return *this;
  }
  Row& i64(const std::string_view key, const std::int64_t value) {
    const std::string text = std::to_string(value);
    fields_.push_back(Field{std::string(key), text, text});
    return *this;
  }
  Row& flag(const std::string_view key, const bool value) {
    fields_.push_back(Field{std::string(key), value ? "1" : "0", value ? "true" : "false"});
    return *this;
  }

  [[nodiscard]] std::string header() const {
    std::string out;
    for (const Field& f : fields_) {
      out += (out.empty() ? "" : ",") + f.key;
    }
    return out;
  }
  [[nodiscard]] std::string csv() const {
    std::string out;
    for (std::size_t i = 0U; i < fields_.size(); ++i) {
      out += (i == 0U ? "" : ",") + fields_[i].csv;
    }
    return out;
  }
  [[nodiscard]] std::string json() const {
    std::string out = "{";
    for (std::size_t i = 0U; i < fields_.size(); ++i) {
      out += (i == 0U ? "" : ",") + json_string(fields_[i].key) + ":" + fields_[i].json;
    }
    return out + "}";
  }

 private:
  struct Field {
    std::string key;
    std::string csv;
    std::string json;
  };

  [[nodiscard]] static std::string csv_text(const std::string_view value) {
    if (value.find_first_of(",\"\n\r") == std::string_view::npos) {
      return std::string(value);
    }
    std::string out = "\"";
    for (const char c : value) {
      out += c == '"' ? std::string("\"\"") : std::string(1, c);
    }
    return out + "\"";
  }
  [[nodiscard]] static std::string json_string(const std::string_view value) {
    std::string out = "\"";
    for (const char c : value) {
      if (c == '"' || c == '\\') {
        out += '\\';
        out += c;
      } else if (static_cast<unsigned char>(c) < 0x20U) {
        char buffer[8];
        std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
        out += buffer;
      } else {
        out += c;
      }
    }
    return out + "\"";
  }

  std::vector<Field> fields_;
};

// Appends rows to <stem>.csv and <stem>.jsonl, flushing every row.  An existing
// CSV must carry exactly this row's header: schemas are never mixed.
class Stream {
 public:
  explicit Stream(const std::filesystem::path& stem)
      : csv_path_(stem.string() + ".csv"), json_path_(stem.string() + ".jsonl") {}

  void write(const Row& row) {
    if (!open_) {
      const std::string header = row.header();
      std::string existing;
      if (std::ifstream in(csv_path_); in) {
        std::getline(in, existing);
      }
      if (!existing.empty() && existing != header) {
        throw std::runtime_error("schema mismatch in " + csv_path_.string());
      }
      csv_.open(csv_path_, std::ios::app);
      json_.open(json_path_, std::ios::app);
      if (!csv_ || !json_) {
        throw std::runtime_error("cannot open " + csv_path_.string());
      }
      if (existing.empty()) {
        csv_ << header << '\n';
      }
      open_ = true;
    }
    csv_ << row.csv() << '\n';
    json_ << row.json() << '\n';
    csv_.flush();
    json_.flush();
  }

 private:
  std::filesystem::path csv_path_;
  std::filesystem::path json_path_;
  std::ofstream csv_;
  std::ofstream json_;
  bool open_ = false;
};

[[nodiscard]] std::vector<std::string> split_csv(const std::string& line) {
  std::vector<std::string> out(1);
  bool quoted = false;
  for (std::size_t i = 0U; i < line.size(); ++i) {
    const char c = line[i];
    if (quoted) {
      if (c == '"' && i + 1U < line.size() && line[i + 1U] == '"') {
        out.back() += '"';
        ++i;
      } else if (c == '"') {
        quoted = false;
      } else {
        out.back() += c;
      }
    } else if (c == '"') {
      quoted = true;
    } else if (c == ',') {
      out.emplace_back();
    } else {
      out.back() += c;
    }
  }
  return out;
}

// Case ids of `root` already written to cases.csv (any status).
[[nodiscard]] std::set<std::string> written_cases(const std::filesystem::path& path, const std::string& root) {
  std::set<std::string> done;
  std::ifstream in(path);
  std::string line;
  if (!std::getline(in, line)) {
    return done;
  }
  const std::vector<std::string> header = split_csv(line);
  const auto column = [&](const std::string& name) {
    return static_cast<std::size_t>(std::find(header.begin(), header.end(), name) - header.begin());
  };
  const std::size_t root_at = column("root");
  const std::size_t case_at = column("case_id");
  while (std::getline(in, line)) {
    const std::vector<std::string> cells = split_csv(line);
    if (root_at < cells.size() && case_at < cells.size() && cells[root_at] == root) {
      done.insert(cells[case_at]);
    }
  }
  return done;
}

// ---------------------------------------------------------------------------
// Configuration and cases.
// ---------------------------------------------------------------------------
struct Config {
  std::string snapshot;
  std::string out_dir;
  std::string label;
  std::string campaign = "unnamed";
  std::string role = "calibration";  // calibration | holdout
  std::string phase;
  std::string pin = "p-core";
  std::string protocol;  // override: discovery | final | smoke
  std::vector<std::string> cases;
  double memory_limit_mib = 4096.0;
  std::size_t max_cases = std::numeric_limits<std::size_t>::max();
  bool allow_debug = false;
};

[[nodiscard]] std::vector<std::string> split(const std::string& text, const char separator) {
  std::vector<std::string> out;
  std::string item;
  std::istringstream in(text);
  while (std::getline(in, item, separator)) {
    if (!item.empty()) {
      out.push_back(item);
    }
  }
  return out;
}

[[nodiscard]] bool parse(int argc, char** argv, Config& config) {
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
    if (take("--snapshot=", config.snapshot) || take("--out-dir=", config.out_dir) ||
        take("--label=", config.label) || take("--campaign=", config.campaign) ||
        take("--role=", config.role) || take("--phase=", config.phase) || take("--pin=", config.pin) ||
        take("--protocol=", config.protocol)) {
      continue;
    }
    if (take("--cases=", value)) {
      config.cases = split(value, ',');
    } else if (take("--memory-limit-mib=", value)) {
      config.memory_limit_mib = std::stod(value);
    } else if (take("--max-cases=", value)) {
      config.max_cases = std::stoul(value);
    } else if (arg == "--allow-debug") {
      config.allow_debug = true;
    } else {
      std::cerr << "unknown argument: " << arg << '\n';
      return false;
    }
  }
  static const std::set<std::string> phases{"census", "stratified", "stratified-corners", "anchor", "thin-qx",
                                            "thin-y",  "plant",      "whole",              "confirm"};
  return !config.snapshot.empty() && !config.out_dir.empty() && phases.count(config.phase) == 1U &&
         (config.role == "calibration" || config.role == "holdout") &&
         (config.pin == "p-core" || config.pin == "none") &&
         (config.protocol.empty() || config.protocol == "discovery" || config.protocol == "final" ||
          config.protocol == "smoke") &&
         (config.phase != "confirm" || !config.cases.empty());
}

struct CaseSpec {
  std::string id;           // unique within a root
  std::string source_kind;  // AUTHENTIC_FULLTER | AUTHENTIC_SOURCE_THINNED | AUTHENTIC_TARGET_THINNED
  std::uint32_t cell = ac::kCells;  // kCells: every microproblem
  ac::Thinning thinning;
  double target_lanes = 0.0;  // thin-qx only
  double base_lanes = 0.0;    // authentic L of the cell, thin-qx only
  double sqrt_rule = 1.0;     // sqrt(target / base): the uncalibrated retention, thin-qx only
  std::int64_t pair_bin = -1;
  std::int64_t lane_bin = -1;
  std::int64_t members = -1;
  Protocol protocol = kDiscovery;
  bool reachable = true;  // false: target L above the authentic L; recorded, never run
  ac::Plant plant;        // count > 0: planted targets (positive-output experiment)
};

// ---------------------------------------------------------------------------
// The campaign.
// ---------------------------------------------------------------------------
struct Backend {
  std::string name;
  std::function<void()> run;  // leaves its answer in the timed sink
};

struct Summary {
  double median = 0.0;
  double p25 = 0.0;
  double p75 = 0.0;
  double min = 0.0;
  double max = 0.0;
};

[[nodiscard]] Summary summarize(std::vector<double> samples) {
  Summary s;
  if (samples.empty()) {
    return s;
  }
  std::sort(samples.begin(), samples.end());
  s.median = ac::quantile(samples, 0.5);
  s.p25 = ac::quantile(samples, 0.25);
  s.p75 = ac::quantile(samples, 0.75);
  s.min = samples.front();
  s.max = samples.back();
  return s;
}

// Fixed CPU-bound workload (the reference packet filter on fixed data):
// machine-drift evidence, never part of any case.
[[nodiscard]] std::pair<double, std::uint64_t> sentinel() {
  aw::LeafTables tables{};
  for (std::uint32_t chunk = 0U; chunk < aw::kChunks; ++chunk) {
    for (std::uint32_t w = 0U; w < 8U; ++w) {
      tables.word[chunk][w] = static_cast<std::uint32_t>(rg::mix64(chunk * 8U + w)) |
                              static_cast<std::uint32_t>(rg::mix64(1000U + chunk * 8U + w));
    }
  }
  std::array<std::uint32_t, 16> q{};
  std::array<std::uint32_t, 16> x{};
  for (std::uint32_t i = 0U; i < 16U; ++i) {
    q[i] = static_cast<std::uint32_t>(rg::mix64(2000U + i));
    x[i] = static_cast<std::uint32_t>(rg::mix64(3000U + i));
  }
  std::uint16_t lanes[256];
  std::uint64_t checksum = 0U;
  const auto start = Clock::now();
  for (std::uint32_t round = 0U; round < (1U << 16U); ++round) {
    q[round & 15U] += round;
    checksum += aw::wide_leaf_filter_scalar(q.data(), 16U, x.data(), 16U, round & 1U, tables, 4U,
                                            0xFFFFFFFFU, lanes);
  }
  return {ms_since(start), checksum};
}

class Campaign {
 public:
  using Geometry = ac::Geometry;

  Campaign(const Config& config, std::unique_ptr<rg::Workload> workload, std::vector<int> classes, int pinned)
      : config_(config),
        workload_(std::move(workload)),
        data_(*workload_->data),
        classes_(std::move(classes)),
        pinned_(pinned),
        root_(config.label.empty() ? std::filesystem::path(config.snapshot).stem().string() : config.label),
        attempt_(hex(static_cast<std::uint64_t>(std::chrono::system_clock::now().time_since_epoch().count()))),
        cases_(std::filesystem::path(config.out_dir) / "cases"),
        samples_(std::filesystem::path(config.out_dir) / "samples"),
        sentinels_(std::filesystem::path(config.out_dir) / "sentinel") {
    tables_.allocate();
    keys_ = a64::build_family_keys(*workload_);
    rg::build_root_index(data_, index_);
    authentic_ = ac::census(keys_, index_);
    rg::select_microproblems(index_, 1U);
    for (std::uint32_t cell = 0U; cell < ac::kCells; ++cell) {
      if (index_.selected[cell] != 0U) {
        anchor_ = cell;
      }
    }
    strata_ = ac::select_stratified(authentic_);
  }

  [[nodiscard]] bool keys_ok() const noexcept { return keys_.ok; }

  // Phase dispatch.  Returns the process exit code.
  int run() {
    std::cout << "# root " << root_ << " n=" << workload_->n << " seed=" << hex(workload_->seed)
              << " Q=" << keys_.full_bits << " W=" << keys_.residual_bits << " anchor cell=" << anchor_
              << " (L=" << fixed(authentic_[anchor_].lanes_per_packet(), 2) << ") strata=" << strata_.size()
              << " phase=" << config_.phase << " role=" << config_.role << '\n';
    if (config_.phase == "census") {
      write_census();
      return 0;
    }
    if ((config_.phase == "thin-qx" || config_.phase == "thin-y" || config_.phase == "plant") &&
        config_.role != "calibration") {
      std::cout << "# refused: thinning and planting run on calibration roots only\n";
      return 2;
    }
    const std::vector<CaseSpec> plan = cases();
    const std::set<std::string> done = written_cases(std::filesystem::path(config_.out_dir) / "cases.csv", root_);
    log_sentinel("start");
    std::size_t ran = 0U;
    for (const CaseSpec& spec : plan) {
      if (done.count(spec.id) != 0U) {
        std::cout << "# skip (already written) " << spec.id << '\n';
        continue;
      }
      if (ran == config_.max_cases) {
        break;
      }
      const int code = run_case(spec);
      if (code != 0) {
        return code;
      }
      ++ran;
      if (ran % kSentinelEvery == 0U) {
        log_sentinel("periodic");
      }
    }
    log_sentinel("end");
    return 0;
  }

 private:
  [[nodiscard]] Protocol protocol_for(const Protocol phase_default) const {
    if (config_.protocol == "discovery") {
      return kDiscovery;
    }
    if (config_.protocol == "final") {
      return kFinal;
    }
    if (config_.protocol == "smoke") {
      return kSmoke;
    }
    return phase_default;
  }

  [[nodiscard]] CaseSpec authentic_case(const std::uint32_t cell, const Protocol protocol) const {
    CaseSpec spec;
    spec.id = (cell == anchor_ ? "anchor:c" : "cell:c") + std::to_string(cell);
    spec.source_kind = "AUTHENTIC_FULLTER";
    spec.cell = cell;
    spec.protocol = protocol_for(protocol);
    for (const ac::Stratum& stratum : strata_) {
      if (stratum.cell == cell) {
        spec.pair_bin = stratum.pair_bin;
        spec.lane_bin = stratum.lane_bin;
        spec.members = stratum.members;
      }
    }
    return spec;
  }

  [[nodiscard]] CaseSpec source_case(const double target, const std::uint64_t seed, const Protocol protocol) const {
    CaseSpec spec;
    std::ostringstream id;
    id << "qx:L" << target << ":s" << hex(seed);
    spec.id = id.str();
    spec.source_kind = "AUTHENTIC_SOURCE_THINNED";
    spec.cell = anchor_;
    spec.base_lanes = authentic_[anchor_].lanes_per_packet();
    spec.target_lanes = target;
    spec.reachable = target <= spec.base_lanes;
    // Census-only calibration; the plan is built before any case, on the authentic keys.
    const ac::Retention r = ac::calibrate_source_retention(keys_, data_, workload_->seed, anchor_, target, seed);
    spec.sqrt_rule = r.sqrt_rule;
    spec.thinning = ac::Thinning{r.p, 1.0, seed};
    spec.protocol = protocol_for(protocol);
    return spec;
  }

  [[nodiscard]] CaseSpec target_case(const double p, const std::uint64_t seed, const Protocol protocol) const {
    CaseSpec spec;
    std::ostringstream id;
    id << "y:p" << p << ":s" << hex(seed);
    spec.id = id.str();
    spec.source_kind = "AUTHENTIC_TARGET_THINNED";
    spec.cell = anchor_;
    spec.thinning = ac::Thinning{1.0, p, seed};
    spec.protocol = protocol_for(protocol);
    return spec;
  }

  [[nodiscard]] CaseSpec plant_case(const ac::PlantKind kind, const std::uint32_t count, const std::uint64_t seed,
                                    const Protocol protocol) const {
    CaseSpec spec;
    const bool positive = kind == ac::PlantKind::Positive;
    spec.id = std::string(positive ? "plant:pos:M" : "plant:decoy:M") + std::to_string(count) + ":s" + hex(seed);
    spec.source_kind = positive ? "AUTHENTIC_PLANTED_TARGETS" : "AUTHENTIC_DECOY_TARGETS";
    spec.cell = anchor_;
    spec.plant = ac::Plant{kind, count, seed};
    spec.protocol = protocol_for(protocol);
    return spec;
  }

  [[nodiscard]] CaseSpec whole_case(const Protocol protocol) const {
    CaseSpec spec;
    spec.id = "whole:all";
    spec.source_kind = "AUTHENTIC_FULLTER";
    spec.cell = ac::kCells;
    spec.protocol = protocol_for(protocol);
    return spec;
  }

  // "anchor" | "whole" | "cell=<id>" | "qx=<L>@<seed>" | "y=<p>@<seed>"
  [[nodiscard]] CaseSpec parse_case(const std::string& text) const {
    if (text == "anchor") {
      return authentic_case(anchor_, kFinal);
    }
    if (text == "whole") {
      return whole_case(kFinal);
    }
    const std::size_t eq = text.find('=');
    const std::string kind = text.substr(0, eq);
    const std::string rest = eq == std::string::npos ? std::string() : text.substr(eq + 1U);
    if (kind == "cell") {
      return authentic_case(static_cast<std::uint32_t>(std::stoul(rest)), kFinal);
    }
    const std::size_t at = rest.find('@');
    if ((kind == "qx" || kind == "y") && at != std::string::npos) {
      const double value = std::stod(rest.substr(0, at));
      const std::uint64_t seed = std::stoull(rest.substr(at + 1U), nullptr, 0);
      return kind == "qx" ? source_case(value, seed, kFinal) : target_case(value, seed, kFinal);
    }
    if ((kind == "pos" || kind == "decoy") && at != std::string::npos) {
      const auto count = static_cast<std::uint32_t>(std::stoul(rest.substr(0, at)));
      const std::uint64_t seed = std::stoull(rest.substr(at + 1U), nullptr, 0);
      return plant_case(kind == "pos" ? ac::PlantKind::Positive : ac::PlantKind::Decoy, count, seed, kFinal);
    }
    throw std::invalid_argument("bad case spec: " + text);
  }

  [[nodiscard]] std::vector<CaseSpec> cases() const {
    std::vector<CaseSpec> plan;
    if (config_.phase == "stratified" || config_.phase == "stratified-corners") {
      // A stratum that is the anchor is measured once, by the anchor phase,
      // under the final protocol.  The corner variant keeps the four extreme
      // grid cells (lowest / highest pair bin x lowest / highest lane bin).
      const bool corners = config_.phase == "stratified-corners";
      for (const ac::Stratum& stratum : strata_) {
        const bool corner = (stratum.pair_bin == 0U || stratum.pair_bin == 3U) &&
                            (stratum.lane_bin == 0U || stratum.lane_bin == 3U);
        if (stratum.cell != anchor_ && (!corners || corner)) {
          plan.push_back(authentic_case(stratum.cell, kDiscovery));
        }
      }
    } else if (config_.phase == "anchor") {
      plan.push_back(authentic_case(anchor_, kFinal));
    } else if (config_.phase == "thin-qx") {
      for (const double target : kTargetLanes) {
        for (const std::uint64_t seed : kThinningSeeds) {
          plan.push_back(source_case(target, seed, kDiscovery));
        }
      }
    } else if (config_.phase == "thin-y") {
      for (const double p : kTargetRetention) {
        plan.push_back(target_case(p, kThinningSeeds[0], kDiscovery));
      }
    } else if (config_.phase == "plant") {
      for (const std::uint32_t count : kPlantCounts) {
        plan.push_back(plant_case(ac::PlantKind::Positive, count, kPlantSeed, kDiscovery));
        plan.push_back(plant_case(ac::PlantKind::Decoy, count, kPlantSeed, kDiscovery));
      }
    } else if (config_.phase == "whole") {
      plan.push_back(whole_case(kFinal));
    } else if (config_.phase == "confirm") {
      for (const std::string& text : config_.cases) {
        plan.push_back(parse_case(text));
        plan.back().id += ":confirm";  // distinct from the discovery case of the same point
      }
    }
    return plan;
  }

  void write_census() {
    // Deterministic: a rerun replaces the file instead of appending to it.
    const std::filesystem::path stem = std::filesystem::path(config_.out_dir) / (root_ + "_census");
    std::filesystem::remove(stem.string() + ".csv");
    std::filesystem::remove(stem.string() + ".jsonl");
    Stream out(stem);
    std::uint64_t max_x_groups = 0U;
    std::uint64_t max_y_groups = 0U;
    for (const Geometry& g : authentic_) {
      max_x_groups = std::max(max_x_groups, g.x_groups);
      max_y_groups = std::max(max_y_groups, g.y_groups);
    }
    const bool queue_legal = max_x_groups <= rg::RadixGeom::kHitCapacity && max_y_groups <= rg::RadixGeom::kHitCapacity;
    for (const Geometry& g : authentic_) {
      const ac::Stratum* pick = nullptr;
      for (const ac::Stratum& stratum : strata_) {
        pick = stratum.cell == g.cell ? &stratum : pick;
      }
      Row row;
      provenance(row);
      row.str("source_kind", "AUTHENTIC_FULLTER");
      geometry(row, g);
      row.flag("queue_legal_root", queue_legal)
          .flag("anchor", g.cell == anchor_)
          .flag("stratified_pick", pick != nullptr)
          .i64("pair_bin", pick != nullptr ? static_cast<std::int64_t>(pick->pair_bin) : -1)
          .i64("lane_bin", pick != nullptr ? static_cast<std::int64_t>(pick->lane_bin) : -1)
          .i64("stratum_members", pick != nullptr ? static_cast<std::int64_t>(pick->members) : -1);
      out.write(row);
    }
    std::cout << "# census written: " << authentic_.size() << " microproblems, " << strata_.size()
              << " strata, queue legal=" << queue_legal << " (max groups X " << max_x_groups << ", Y "
              << max_y_groups << ")\n";
  }

  void provenance(Row& row) const {
    row.str("campaign", config_.campaign)
        .str("attempt", attempt_)
        .str("root", root_)
        .str("role", config_.role)
        .str("phase", config_.phase)
        .u64("n", workload_->n)
        .str("root_seed", hex(workload_->seed))
        .u64("Q", keys_.full_bits)
        .u64("W", keys_.residual_bits)
        .str("git_commit", TER_CONFIGURE_TIME_COMMIT)
        .i64("git_dirty_at_configure", TER_CONFIGURE_TIME_DIRTY)
        .str("compiler", compiler())
        .str("build", build_type())
        .str("pin", config_.pin)
        .i64("pinned_cpu", pinned_);
  }

  static void geometry(Row& row, const Geometry& g) {
    row.u64("cell", g.cell)
        .u64("s", g.s)
        .u64("t", g.t)
        .u64("u", g.u)
        .u64("kappa", g.kappa)
        .u64("q_count", g.q_count)
        .u64("x_count", g.x_count)
        .u64("y_count", g.y_count)
        .u64("logical_pairs", g.logical_pairs)
        .num("log2_pairs", g.logical_pairs == 0U ? 0.0 : std::log2(static_cast<double>(g.logical_pairs)), 4)
        .u64("q_leaves", g.q_leaves)
        .u64("x_leaves", g.x_leaves)
        .u64("y_leaves", g.y_leaves)
        .u64("q_leaf_max", g.q_leaf_max)
        .u64("x_leaf_max", g.x_leaf_max)
        .u64("y_leaf_max", g.y_leaf_max)
        .num("q_leaf_mean", g.q_leaves == 0U ? 0.0 : static_cast<double>(g.q_count) / g.q_leaves, 3)
        .num("x_leaf_mean", g.x_leaves == 0U ? 0.0 : static_cast<double>(g.x_count) / g.x_leaves, 3)
        .num("y_leaf_mean", g.y_leaves == 0U ? 0.0 : static_cast<double>(g.y_count) / g.y_leaves, 3)
        .u64("leaf_triples", g.leaf_triples)
        .u64("packets", g.packets)
        .u64("lanes", g.lanes)
        .u64("rows", g.rows)
        .num("lanes_per_packet", g.lanes_per_packet(), 4)
        .num("lane_utilization", g.lanes_per_packet() / 256.0, 5)
        .num("row_utilization", g.row_utilization(), 5)
        .u64("x_groups", g.x_groups)
        .u64("y_groups", g.y_groups);
  }

  void log_sentinel(const std::string& when) {
    const auto [ms, checksum] = sentinel();
    g_sink = g_sink + checksum;
    Row row;
    provenance(row);
    row.str("when", when).u64("cases_run", cases_run_).num("sentinel_ms", ms, 4).u64("checksum", checksum)
        .i64("cpu", host::current_cpu());
    sentinels_.write(row);
    std::cout << "# sentinel " << when << " " << fixed(ms, 3) << " ms\n";
  }

  // Selects the case's microproblems on the current index.
  void select(const CaseSpec& spec) {
    if (spec.cell == ac::kCells) {
      rg::select_microproblems(index_, ac::kCells);
      return;
    }
    index_.selected.fill(0U);
    index_.selected[spec.cell] = 1U;
    index_.selected_count = 1U;
  }

  void case_row(Row& row, const CaseSpec& spec, const Geometry& g) const {
    provenance(row);
    row.str("case_id", spec.id)
        .str("source_kind", spec.source_kind)
        .str("protocol", spec.protocol.name)
        .u64("warmups", spec.protocol.warmups)
        .u64("samples", spec.protocol.samples)
        .flag("anchor", spec.cell == anchor_)
        .i64("pair_bin", spec.pair_bin)
        .i64("lane_bin", spec.lane_bin)
        .i64("stratum_members", spec.members)
        .num("retain_q", spec.thinning.p_qx, 6)
        .num("retain_x", spec.thinning.p_qx, 6)
        .num("retain_y", spec.thinning.p_y, 6)
        .str("thinning_seed", hex(spec.thinning.seed))
        .num("retain_sqrt_rule", spec.sqrt_rule, 6)
        .num("target_lanes_per_packet", spec.target_lanes, 2)
        .num("base_lanes_per_packet", spec.base_lanes, 4)
        .str("plant_kind", spec.plant.count == 0U ? "" : (spec.plant.kind == ac::PlantKind::Positive ? "positive" : "decoy"))
        .u64("plant_count", spec.plant.count)
        .str("plant_seed", hex(spec.plant.seed));
    geometry(row, g);
  }

  // Exactness record of one case (defaults: the pass did not run).
  struct Exactness {
    bool ran = false;
    bool gate_status = false;
    bool gate_wide = false;
    bool gate_k32 = false;
    bool gate_mid = false;
    bool gate_answer = false;
    bool gate_witness = false;
    bool gate_clean = false;
    bool gate_census = false;
    bool gate_planted = true;            // planted part of the witnesses == the planted set (planted cases)
    std::uint64_t planted_expected = 0;  // known final outputs planted
    std::uint64_t planted_found = 0;     // ... found by frozen K2 (the others must equal K2)
    std::uint64_t authentic_witnesses = 0;
    std::uint64_t plant_rejected_pairs = 0;
    std::uint64_t k2_hits25 = 0;   // frozen K2 survivors on 25 low quotient bits
    std::uint64_t k2_hits32 = 0;   // ... passing the low-32 quotient prefilter
    std::uint64_t residue_exact = 0;
    std::uint64_t structural = 0;
    std::uint64_t residue_sum = 0;
    std::uint64_t witnesses = 0;
    std::uint64_t arwlf_k32_hits = 0;
    std::uint64_t mid_k = 0;
    std::uint64_t arwlf_mid_hits = 0;   // projected hits at k = 19 + mid bits
    std::uint64_t k2wide_mid_hits = 0;  // K2_WIDE's width histogram at the same k
    std::uint64_t full_key_triples = 0;
    std::uint64_t k2wide_triples = 0;
    std::uint64_t filter_survivors = 0;
    std::uint64_t arwlf_rows = 0;
    bool avx2 = false;
    std::string drift_backend;
    double exactness_ms = 0.0;

    [[nodiscard]] bool exact() const noexcept {
      return ran && gate_status && gate_wide && gate_k32 && gate_mid && gate_answer && gate_witness && gate_clean &&
             gate_census && gate_planted && drift_backend.empty();
    }
  };

  // Timing record of one backend of one case (defaults: not timed).
  struct Timing {
    std::string backend;
    Summary stats{std::nan(""), std::nan(""), std::nan(""), std::nan(""), std::nan("")};
    std::uint64_t timed_samples = 0;
    double k2_over_backend = std::nan("");
    bool queue_legal = false;
    std::string best_backend;
    double best_ms = std::nan("");
    double regret = std::nan("");
    aw::Dispatch decision{};
    bool dispatched = false;
    std::string ran_mode;
    std::string ran_list;
    std::uint64_t off_core = 0;
    int ac_start = -1;
    int ac_end = -1;
  };

  static void exactness_fields(Row& row, const Exactness& e) {
    row.str("exactness", !e.ran ? "NOT_RUN" : (e.exact() ? "EXACT" : "DIVERGED"))
        .flag("gate_status", e.gate_status)
        .flag("gate_wide", e.gate_wide)
        .flag("gate_k32", e.gate_k32)
        .flag("gate_mid", e.gate_mid)
        .flag("gate_answer", e.gate_answer)
        .flag("gate_witness", e.gate_witness)
        .flag("gate_clean", e.gate_clean)
        .flag("gate_census", e.gate_census)
        .flag("gate_planted", e.gate_planted)
        .u64("planted_expected", e.planted_expected)
        .u64("planted_found", e.planted_found)
        .u64("authentic_witnesses", e.authentic_witnesses)
        .u64("plant_rejected_pairs", e.plant_rejected_pairs)
        .u64("k2_hits25", e.k2_hits25)
        .u64("k2_hits32", e.k2_hits32)
        .u64("residue_exact", e.residue_exact)
        .u64("structural", e.structural)
        .str("residue_sum", hex(e.residue_sum))
        .u64("witnesses", e.witnesses)
        .u64("arwlf_k32_hits", e.arwlf_k32_hits)
        .u64("mid_k", e.mid_k)
        .u64("arwlf_mid_hits", e.arwlf_mid_hits)
        .u64("k2wide_mid_hits", e.k2wide_mid_hits)
        .u64("full_key_triples", e.full_key_triples)
        .u64("k2wide_triples", e.k2wide_triples)
        .u64("arwlf_filter_survivors", e.filter_survivors)
        .u64("arwlf_rows", e.arwlf_rows)
        .flag("arwlf_avx2", e.avx2)
        .str("drift_backend", e.drift_backend)
        .num("exactness_ms", e.exactness_ms, 1);
  }

  static void timing_fields(Row& row, const Timing& t, const Geometry& g) {
    const double pairs = static_cast<double>(g.logical_pairs);
    const double packets = static_cast<double>(g.packets);
    row.str("backend", t.backend)
        .num("median_ms", t.stats.median, 4)
        .num("p25_ms", t.stats.p25, 4)
        .num("p75_ms", t.stats.p75, 4)
        .num("min_ms", t.stats.min, 4)
        .num("max_ms", t.stats.max, 4)
        .u64("timed_samples", t.timed_samples)
        .num("k2_over_backend", t.k2_over_backend, 4)
        .num("ns_per_pair", pairs > 0.0 ? t.stats.median * 1e6 / pairs : std::nan(""), 5)
        .num("ns_per_packet", packets > 0.0 ? t.stats.median * 1e6 / packets : std::nan(""), 4)
        .flag("queue_legal", t.queue_legal)
        .str("best_backend", t.best_backend)
        .num("best_ms", t.best_ms, 4)
        .num("dispatch_regret", t.regret, 5)
        .str("dispatch_chosen", t.dispatched ? aw::backend_label(t.decision.chosen) : "")
        .str("dispatch_ran_mode", t.ran_mode)
        .str("dispatch_ran_samples", t.ran_list)
        .num("dispatch_pred_queue_ms", t.dispatched ? t.decision.predicted_ms[0] : std::nan(""), 3)
        .num("dispatch_pred_arwlf_ms", t.dispatched ? t.decision.predicted_ms[1] : std::nan(""), 3)
        .num("dispatch_pred_k2wide_ms", t.dispatched ? t.decision.predicted_ms[2] : std::nan(""), 3)
        .num("dispatch_probe_ms", t.dispatched ? t.decision.probe_ms : std::nan(""), 3)
        .num("arwlf_packets_per_pair", t.dispatched ? t.decision.arwlf_packets_per_pair : std::nan(""), 6)
        .u64("off_core_samples", t.off_core)
        .i64("ac_start", t.ac_start)
        .i64("ac_end", t.ac_end);
  }

  // One cases.csv row: the same schema whatever the status.
  void write_case(const CaseSpec& spec, const Geometry& g, const std::string& status, const std::string& detail,
                  const Exactness& e, const Timing& t) {
    const host::Memory memory = host::process_memory();
    Row row;
    case_row(row, spec, g);
    row.str("status", status).str("detail", detail);
    exactness_fields(row, e);
    timing_fields(row, t, g);
    row.num("working_set_mib", memory.working_set_mib, 1).num("peak_working_set_mib", memory.peak_working_set_mib, 1);
    cases_.write(row);
  }

  // A case that is recorded but not timed (UNREACHABLE / EMPTY / RESOURCE_LIMIT / FAILED).
  void untimed(const CaseSpec& spec, const Geometry& g, const std::string& status, const std::string& detail,
               const Exactness& e = Exactness{}) {
    write_case(spec, g, status, detail, e, Timing{});
    std::cout << "# case " << spec.id << " : " << status << " (" << detail << ")\n";
  }

  [[nodiscard]] int fail(const CaseSpec& spec, const Geometry& g, const Exactness& e) {
    const std::filesystem::path dir = std::filesystem::path(config_.out_dir) / "failures";
    std::filesystem::create_directories(dir);
    std::string name = root_ + "_" + spec.id;
    std::replace_if(name.begin(), name.end(), [](const char c) { return c == ':' || c == '@'; }, '_');
    Row row;
    case_row(row, spec, g);
    exactness_fields(row, e);
    std::ofstream(dir / (name + ".json")) << row.json() << '\n';
    std::cout << "# EXACTNESS FAILURE in " << spec.id << ": artifact " << (dir / (name + ".json")).string()
              << "; campaign aborted\n";
    return 4;
  }

  int run_case(const CaseSpec& spec) {
    ++cases_run_;
    if (!spec.reachable) {
      untimed(spec, authentic_[spec.cell], "UNREACHABLE", "target L above the authentic L of the cell");
      return 0;
    }
    std::optional<ac::ThinnedMicroproblem> thinned;
    if (spec.cell != ac::kCells) {
      thinned.emplace(data_, workload_->seed, spec.cell, spec.thinning);
    }
    std::optional<ac::PlantedTargets> planted;
    if (spec.plant.count != 0U) {
      planted.emplace(*workload_, spec.cell, spec.plant);
    }
    keys_ = a64::FamilyKeys{};
    keys_ = a64::build_family_keys(*workload_);
    rg::build_root_index(data_, index_);
    select(spec);
    const std::vector<Geometry> state = ac::census(keys_, index_);
    const Geometry g = spec.cell == ac::kCells ? ac::selected_geometry(state, index_) : state[spec.cell];
    if (!keys_.ok) {
      untimed(spec, g, "FAILED", "family keys unavailable");
      return 3;
    }
    if (g.logical_pairs == 0U) {
      untimed(spec, g, "EMPTY", "no logical pairs after thinning");
      return 0;
    }
    std::uint64_t max_x = 0U;
    std::uint64_t max_y = 0U;
    for (std::uint32_t f = 0U; f < rg::kFamilies; ++f) {
      max_x = std::max<std::uint64_t>(max_x, index_.x_groups[f].size());
      max_y = std::max<std::uint64_t>(max_y, index_.y_groups[f].size());
    }
    const bool queue_legal = max_x <= rg::RadixGeom::kHitCapacity && max_y <= rg::RadixGeom::kHitCapacity;
    const auto make_sink = [&](std::vector<Witness>* witnesses) {
      rg::RootSink sink;
      sink.data = &data_;
      sink.index = &index_;
      sink.authority = &workload_->authority;
      sink.witnesses = witnesses;
      sink.reset();
      return sink;
    };
    const auto sorted = [](std::vector<Witness> w) {
      std::sort(w.begin(), w.end());
      return w;
    };

    // --- exactness (untimed) ---------------------------------------------------
    const auto exact_start = Clock::now();
    std::vector<Witness> w_k2;
    std::vector<Witness> w_wide;
    std::vector<Witness> w_arwlf;
    std::vector<Witness> w_queue;
    std::vector<Witness> w_dispatch;
    rg::RootSink k2 = make_sink(&w_k2);
    rg::run_k2(data_, k2);
    rg::RootSink wide = make_sink(&w_wide);
    a64::Telemetry wide_t;
    a64::run_k2_wide(data_, index_, keys_, wide, &wide_t, /*sweep=*/true);
    rg::RootSink arwlf = make_sink(&w_arwlf);
    aw::Telemetry at;
    aw::Config counted;
    counted.account = true;
    const aw::Status a_status = aw::run_arwlf(data_, index_, keys_, counted, arwlf, &at);
    rg::RootSink plain = make_sink(nullptr);
    const aw::Status p_status = aw::run_arwlf(data_, index_, keys_, aw::Config{}, plain, nullptr);
    aw::Config k32_config;
    k32_config.check_bits = 13U;
    rg::RootSink k32 = make_sink(nullptr);
    aw::Telemetry k32_t;
    const aw::Status k32_status = aw::run_arwlf(data_, index_, keys_, k32_config, k32, &k32_t);
    rg::RootSink queue = make_sink(&w_queue);
    if (queue_legal) {
      rg::run_radix(data_, index_, tables_, rg::RadixRoute::QueueDense, rg::RadixStop::Full, queue);
    }
    rg::RootSink dispatched = make_sink(&w_dispatch);
    const aw::Dispatch decision = aw::dispatch_and_run(data_, index_, keys_, &tables_, dispatched);

    const rg::OutputDigest& dk = k2.digest;
    const rg::OutputDigest& dw = wide.digest;
    const rg::OutputDigest& da = arwlf.digest;
    const std::vector<Witness> reference = sorted(w_k2);
    const auto same_answer = [&](const rg::OutputDigest& d) {
      return d.residue == dk.residue && d.residue_sum == dk.residue_sum && d.structural == dk.structural;
    };
    Exactness e;
    e.ran = true;
    for (std::uint32_t c = aw::kLeafBits + at.mid_bits; c <= keys_.residual_bits; ++c) {
      e.k2wide_mid_hits += wide_t.sweep[c];
    }
    e.gate_status = a_status == aw::Status::Completed && p_status == aw::Status::Completed &&
                    k32_status == aw::Status::Completed;
    e.gate_wide = da.triples == dw.triples && da.triple_sum == dw.triple_sum && da.triple_xor == dw.triple_xor &&
                  da.triple_sum2 == dw.triple_sum2 && at.full_hits == da.triples;
    e.gate_k32 = k32_t.projected_hits == dk.full_q;
    e.gate_mid = at.projected_hits == e.k2wide_mid_hits;
    e.gate_answer = same_answer(dw) && same_answer(da) && same_answer(plain.digest) &&
                    plain.digest.triple_sum == da.triple_sum && same_answer(dispatched.digest) &&
                    (!queue_legal || (same_answer(queue.digest) && queue.digest.triple_sum == dk.triple_sum));
    e.gate_witness = reference.size() == dk.residue && sorted(w_wide) == reference && sorted(w_arwlf) == reference &&
                     sorted(w_dispatch) == reference && (!queue_legal || sorted(w_queue) == reference);
    e.gate_clean = dk.directory_misses == 0U && dk.residual_mismatches == 0U && dw.directory_misses == 0U &&
                   dw.residual_mismatches == 0U;
    // The census is an independent count of the geometry the counted run walks.
    e.gate_census = at.logical_pairs == g.logical_pairs && at.leaf_triples == g.leaf_triples &&
                    at.packets == g.packets && at.lanes == g.lanes && (!at.avx2 || at.rows == g.rows);
    e.authentic_witnesses = reference.size();
    if (planted) {
      std::vector<Witness> found;
      for (const Witness& w : reference) {
        if (w[2] >= planted->first_planted()) {
          found.push_back(w);
        }
      }
      e.planted_expected = planted->expected().size();
      e.planted_found = found.size();
      e.authentic_witnesses = reference.size() - found.size();
      e.plant_rejected_pairs = planted->rejected_pairs();
      e.gate_planted = found == planted->expected() && dk.structural >= planted->expected().size();
    }
    e.k2_hits25 = dk.triples;
    e.k2_hits32 = dk.full_q;
    e.residue_exact = dk.residue;
    e.structural = dk.structural;
    e.residue_sum = dk.residue_sum;
    e.witnesses = reference.size();
    e.arwlf_k32_hits = k32_t.projected_hits;
    e.mid_k = 19U + at.mid_bits;
    e.arwlf_mid_hits = at.projected_hits;
    e.full_key_triples = da.triples;
    e.k2wide_triples = dw.triples;
    e.filter_survivors = at.filter_survivors;
    e.arwlf_rows = at.rows;
    e.avx2 = at.avx2;
    e.exactness_ms = ms_since(exact_start);
    if (!e.exact()) {
      return fail(spec, g, e);
    }

    // --- memory guard --------------------------------------------------------
    const host::Memory memory = host::process_memory();
    if (memory.working_set_mib > config_.memory_limit_mib) {
      untimed(spec, g, "RESOURCE_LIMIT",
              "working set " + fixed(memory.working_set_mib, 1) + " MiB > " + fixed(config_.memory_limit_mib, 0) +
                  " MiB after the exactness pass",
              e);
      return 0;
    }

    // --- matched timing --------------------------------------------------------
    std::vector<Witness> w_timed;
    rg::RootSink timed = make_sink(&w_timed);
    aw::Dispatch last{};
    std::vector<Backend> backends;
    backends.push_back(Backend{"k2", [&]() {
                                 timed.reset();
                                 rg::run_k2(data_, timed);
                               }});
    backends.push_back(Backend{"k2_wide", [&]() {
                                 timed.reset();
                                 a64::run_k2_wide(data_, index_, keys_, timed, nullptr, false);
                               }});
    if (queue_legal) {
      backends.push_back(Backend{"queue_dense", [&]() {
                                   timed.reset();
                                   rg::run_radix(data_, index_, tables_, rg::RadixRoute::QueueDense,
                                                 rg::RadixStop::Full, timed);
                                 }});
    }
    backends.push_back(Backend{"arwlf", [&]() {
                                 timed.reset();
                                 static_cast<void>(aw::run_arwlf(data_, index_, keys_, aw::Config{}, timed, nullptr));
                               }});
    backends.push_back(Backend{"dispatch", [&]() { last = aw::dispatch_and_run(data_, index_, keys_, &tables_, timed); }});
    std::vector<std::vector<double>> samples(backends.size());
    std::vector<std::string> dispatch_ran;
    std::uint64_t off_core = 0U;
    const int ac_start = host::ac_line_status();
    const std::size_t rounds = spec.protocol.warmups + spec.protocol.samples;
    for (std::size_t round = 0U; round < rounds; ++round) {
      const bool record = round >= spec.protocol.warmups;
      const std::vector<std::size_t> order = ac::round_order(backends.size(), static_cast<std::uint32_t>(round));
      for (std::size_t position = 0U; position < order.size(); ++position) {
        const std::size_t b = order[position];
        const int cpu_start = host::current_cpu();
        const auto start = Clock::now();
        backends[b].run();
        const double ms = ms_since(start);
        const int cpu_end = host::current_cpu();
        g_sink = g_sink + timed.digest.residue_sum;
        if (!same_answer(timed.digest) || sorted(w_timed) != reference) {
          e.drift_backend = backends[b].name + " round " + std::to_string(round);
          return fail(spec, g, e);
        }
        if (!record) {
          continue;
        }
        const bool on_core = pinned_ < 0 || (cpu_start == pinned_ && cpu_end == pinned_);
        off_core += on_core ? 0U : 1U;
        samples[b].push_back(ms);
        const bool is_dispatch = backends[b].name == "dispatch";
        if (is_dispatch) {
          dispatch_ran.emplace_back(aw::backend_label(last.ran));
        }
        Row row;
        provenance(row);
        row.str("case_id", spec.id)
            .str("source_kind", spec.source_kind)
            .str("protocol", spec.protocol.name)
            .str("backend", backends[b].name)
            .u64("sample", round - spec.protocol.warmups)
            .u64("position", position)
            .num("ms", ms, 4)
            .i64("cpu_start", cpu_start)
            .i64("cpu_end", cpu_end)
            .i64("class_start", host::class_of(classes_, cpu_start))
            .i64("class_end", host::class_of(classes_, cpu_end))
            .flag("on_pinned_core", on_core)
            .str("dispatch_ran", is_dispatch ? aw::backend_label(last.ran) : "")
            .num("dispatch_probe_ms", is_dispatch ? last.probe_ms : std::nan(""), 3);
        samples_.write(row);
      }
    }

    // --- summary rows ------------------------------------------------------------
    Timing shared;
    shared.queue_legal = queue_legal;
    shared.decision = decision;
    shared.dispatched = true;
    shared.off_core = off_core;
    shared.ac_start = ac_start;
    shared.ac_end = host::ac_line_status();
    std::vector<Summary> stats;
    for (const std::vector<double>& s : samples) {
      stats.push_back(summarize(s));
    }
    const auto median_of = [&](const std::string& name) {
      for (std::size_t b = 0U; b < backends.size(); ++b) {
        if (backends[b].name == name) {
          return stats[b].median;
        }
      }
      return std::nan("");
    };
    shared.best_ms = std::numeric_limits<double>::infinity();
    for (std::size_t b = 0U; b < backends.size(); ++b) {
      if (backends[b].name != "dispatch" && stats[b].median < shared.best_ms) {
        shared.best_ms = stats[b].median;
        shared.best_backend = backends[b].name;
      }
    }
    shared.regret = median_of("dispatch") / shared.best_ms;
    std::size_t mode_count = 0U;
    for (const std::string& r : dispatch_ran) {
      shared.ran_list += (shared.ran_list.empty() ? "" : "|") + r;
      const auto c = static_cast<std::size_t>(std::count(dispatch_ran.begin(), dispatch_ran.end(), r));
      if (c > mode_count) {
        mode_count = c;
        shared.ran_mode = r;
      }
    }
    const std::string status = off_core == 0U ? "COMPLETED" : "PIN_VIOLATION";
    const double k2_ms = median_of("k2");
    for (std::size_t b = 0U; b < backends.size(); ++b) {
      Timing t = shared;
      t.backend = backends[b].name;
      t.stats = stats[b];
      t.timed_samples = samples[b].size();
      t.k2_over_backend = k2_ms / stats[b].median;
      write_case(spec, g, status, "", e, t);
    }
    std::cout << "# case " << spec.id << " [" << spec.source_kind << ", " << spec.protocol.name << "] L="
              << fixed(g.lanes_per_packet(), 2) << " pairs=" << g.logical_pairs << " :";
    for (std::size_t b = 0U; b < backends.size(); ++b) {
      std::cout << ' ' << backends[b].name << '=' << fixed(stats[b].median, 2);
    }
    std::cout << " ms | K2/ARWLF=" << fixed(k2_ms / median_of("arwlf"), 2) << " regret=" << fixed(shared.regret, 3)
              << " best=" << shared.best_backend << " dispatch ran " << shared.ran_mode
              << " | witnesses=" << reference.size() << " k32=" << dk.full_q << " | " << status << '\n';
    std::cout.flush();
    return 0;
  }

  const Config& config_;
  std::unique_ptr<rg::Workload> workload_;
  mr8::Dataset& data_;
  std::vector<int> classes_;
  int pinned_;
  std::string root_;
  std::string attempt_;
  Stream cases_;
  Stream samples_;
  Stream sentinels_;
  a64::FamilyKeys keys_;
  rg::RootIndex index_;
  rg::RadixTables tables_;
  std::vector<Geometry> authentic_;
  std::vector<ac::Stratum> strata_;
  std::uint32_t anchor_ = 0U;
  std::uint64_t cases_run_ = 0U;
};

}  // namespace

int main(int argc, char** argv) {
  Config config;
  if (!parse(argc, argv, config)) {
    std::cerr << "usage: ter_arwlf_campaign --snapshot=PATH --out-dir=DIR "
                 "--phase=census|stratified|stratified-corners|anchor|thin-qx|thin-y|whole|confirm"
                 " [--role=calibration|holdout]"
                 " [--label=L] [--campaign=ID] [--pin=p-core|none] [--protocol=discovery|final|smoke]"
                 " [--cases=anchor,whole,cell=ID,qx=L@SEED,y=P@SEED] [--memory-limit-mib=M]"
                 " [--max-cases=K] [--allow-debug]\n";
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
  std::vector<int> classes = host::efficiency_classes();
  int pinned = -1;
  if (config.pin == "p-core") {
    pinned = host::pin_to_p_core(classes);
    if (pinned < 0) {
      std::cout << "# refused: P-core pinning failed\n";
      return 5;
    }
  }
  std::filesystem::create_directories(config.out_dir);
  auto workload = rg::load_snapshot_workload(config.snapshot, /*with_authority=*/true);
  std::cout << "# snapshot " << config.snapshot << " : " << rg::status_label(workload->status)
            << (workload->status_detail.empty() ? "" : " (" + workload->status_detail + ")") << '\n';
  if (workload->status != rg::WorkloadStatus::Completed || !workload->authority.ok) {
    return 3;
  }
  try {
    Campaign campaign(config, std::move(workload), std::move(classes), pinned);
    if (!campaign.keys_ok()) {
      std::cout << "# NotApplicable: full keys unavailable\n";
      return 3;
    }
    return campaign.run();
  } catch (const std::exception& error) {
    std::cout << "# error: " << error.what() << '\n';
    return 6;
  }
}
