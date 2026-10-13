// One Full TER attempt's root stage on a frozen root backend, verified
// (ARWLF_FULL_SOLVE.md).  Appends one JSON line per run to --out.
//
//   ter_arwlf_solve_attempt --snapshot=PATH --out=FILE.jsonl
//                           [--backend=dispatch|arwlf|k2_wide|k2] [--pin=p-core|none] [--allow-debug]
//
// Exit codes: 0 the outcome was recorded (Found, NoWitnessThisAttempt,
// NotApplicable or ResourceLimit), 2 refused, 3 invalid snapshot, 4 invariant
// violation (recorded), 5 pinning failure, 6 error.

#include "ter_arwlf.hpp"
#include "ter_arwlf_solve.hpp"
#include "ter_bench_host.hpp"
#include "ter_microreport8_kernels.hpp"

#include <cstdint>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

namespace as = ter::arwlf_solve;
namespace aw = ter::arwlf;
namespace mr8 = ter::microreport8;
namespace wr = ter::arwlf_whole_root;
namespace host = ter::bench_host;

[[nodiscard]] const char* status_name(const ter::Status status) noexcept {
  switch (status) {
    case ter::Status::Completed: return "Completed";
    case ter::Status::Found: return "Found";
    case ter::Status::NoWitnessThisAttempt: return "NoWitnessThisAttempt";
    case ter::Status::AttemptLimitReached: return "AttemptLimitReached";
    case ter::Status::InvalidConfiguration: return "InvalidConfiguration";
    case ter::Status::InvalidShape: return "InvalidShape";
    case ter::Status::GenerationIncomplete: return "GenerationIncomplete";
    case ter::Status::ResourceLimit: return "ResourceLimit";
    case ter::Status::Cancelled: return "Cancelled";
    case ter::Status::InvariantViolation: return "InvariantViolation";
    case ter::Status::NotApplicable: return "NotApplicable";
    case ter::Status::UnsupportedWidth: return "UnsupportedWidth";
  }
  return "Unknown";
}

[[nodiscard]] const char* build_type() noexcept {
#if defined(NDEBUG)
  return "Release";
#else
  return "Debug";
#endif
}

[[nodiscard]] std::string fixed(const double value, const int digits) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(digits) << value;
  return out.str();
}

[[nodiscard]] std::string quoted(const std::string& value) {
  std::string out = "\"";
  for (const char c : value) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
    }
    out.push_back(static_cast<unsigned char>(c) < 0x20U ? ' ' : c);
  }
  return out + "\"";
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

// A verified parent is binary: one character per coordinate.
[[nodiscard]] std::string digits(const ter::ReferenceTernaryVector& parent) {
  std::string out;
  out.reserve(parent.size());
  for (const std::int8_t d : parent) {
    out.push_back(d > 0 ? '1' : d < 0 ? '-' : '0');
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  std::string snapshot;
  std::string out_path;
  std::string backend_text = "dispatch";
  std::string pin = "p-core";
  bool allow_debug = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const auto take = [&](const std::string_view prefix, std::string& value) {
      if (arg.rfind(prefix, 0) != 0) {
        return false;
      }
      value = std::string(arg.substr(prefix.size()));
      return true;
    };
    if (take("--snapshot=", snapshot) || take("--out=", out_path) || take("--backend=", backend_text) ||
        take("--pin=", pin)) {
      continue;
    }
    if (arg == "--allow-debug") {
      allow_debug = true;
      continue;
    }
    std::cerr << "unknown argument: " << arg << '\n';
    return 2;
  }
  const auto backend = wr::parse_backend(backend_text);
  if (snapshot.empty() || out_path.empty() || !backend || (pin != "p-core" && pin != "none")) {
    std::cerr << "usage: ter_arwlf_solve_attempt --snapshot=PATH --out=FILE.jsonl "
                 "[--backend=dispatch|arwlf|k2_wide|k2] [--pin=p-core|none] [--allow-debug]\n";
    return 2;
  }
  if (std::string(build_type()) != "Release" && !allow_debug) {
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
  if (pin == "p-core") {
    pinned = host::pin_to_p_core(host::efficiency_classes());
    if (pinned < 0) {
      std::cout << "# refused: P-core pinning failed\n";
      return 5;
    }
  }
  try {
    const int cpu_start = host::current_cpu();
    const as::AttemptOutcome o = as::solve_attempt(snapshot, *backend);
    const int cpu_end = host::current_cpu();
    const host::Memory memory = host::process_memory();
    std::string parents = "[";
    for (std::size_t i = 0U; i < o.parents.size(); ++i) {
      parents += (i == 0U ? "" : ",") + quoted(digits(o.parents[i]));
    }
    parents += "]";
    std::ostringstream line;
    line << "{\"snapshot\":" << quoted(snapshot) << ",\"status\":" << quoted(status_name(o.status))
         << ",\"message\":" << quoted(o.message) << ",\"n\":" << o.n << ",\"seed\":" << o.seed
         << ",\"attempt\":" << o.attempt << ",\"attempt_seed\":" << o.attempt_seed
         << ",\"backend\":" << quoted(o.backend) << ",\"dispatch_ran\":" << quoted(o.dispatch_ran)
         << ",\"load_ms\":" << fixed(o.load_ms, 1) << ",\"keys_ms\":" << fixed(o.keys_ms, 1)
         << ",\"root_ms\":" << fixed(o.root_ms, 1) << ",\"rebuild_ms\":" << fixed(o.rebuild_ms, 1)
         << ",\"verify_ms\":" << fixed(o.verify_ms, 3) << ",\"residue_exact\":" << o.residue_exact
         << ",\"structural_triples\":" << o.structural_triples << ",\"unique_parents\":" << o.parents.size()
         << ",\"planted_found\":" << (o.planted_found ? "true" : "false") << ",\"parents\":" << parents
         << ",\"pin\":" << quoted(pin) << ",\"pinned_cpu\":" << pinned << ",\"cpu_start\":" << cpu_start
         << ",\"cpu_end\":" << cpu_end << ",\"working_set_mib\":" << fixed(memory.working_set_mib, 1)
         << ",\"peak_working_set_mib\":" << fixed(memory.peak_working_set_mib, 1)
         << ",\"build\":" << quoted(build_type()) << ",\"git_commit\":" << quoted(TER_CONFIGURE_TIME_COMMIT)
         << ",\"git_dirty_at_configure\":" << TER_CONFIGURE_TIME_DIRTY << ",\"utc\":" << quoted(utc_now()) << "}";
    std::ofstream out(out_path, std::ios::app);
    out << line.str() << '\n';
    out.flush();
    if (!out) {
      std::cout << "# error: cannot append to " << out_path << '\n';
      return 6;
    }
    std::cout << "# n=" << o.n << " seed=0x" << std::hex << o.seed << std::dec << " attempt=" << o.attempt
              << " backend=" << o.backend << (o.dispatch_ran.empty() ? "" : " (ran " + o.dispatch_ran + ")")
              << " : " << status_name(o.status) << ", root " << fixed(o.root_ms / 1000.0, 2) << " s, "
              << o.structural_triples << " accepted triples, " << o.parents.size() << " verified parents"
              << (o.planted_found ? " (includes the planted solution)" : "")
              << (o.message.empty() ? "" : " -- " + o.message) << std::endl;
    if (o.status == ter::Status::InvariantViolation) {
      return 4;
    }
    if (o.status == ter::Status::InvalidConfiguration) {
      return 3;
    }
    return 0;
  } catch (const std::exception& error) {
    std::cout << "# error: " << error.what() << '\n';
    return 6;
  }
}
