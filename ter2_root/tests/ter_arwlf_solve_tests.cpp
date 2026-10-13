// Differential tests of the frozen-backend root stage against the classic
// solver (ARWLF_FULL_SOLVE.md).
//
// Usage: ter_arwlf_solve_tests <scratch directory for temporary snapshots>
//
// Authority: solve_full_ter (design B root) and report_full_ter_root on the
// classic capture of the same attempt.  For every (seed, attempt) scanned at
// n = 72, with DISPATCH and with ARWLF forced:
//   - the outcome agrees (Found <=> Found, NoWitnessThisAttempt <=> same);
//   - the accepted root triples (before dedup) equal the classic report's
//     valid_root_count;
//   - a classic witness is among the verified parents.
// A pinned attempt the classic solver Finds (seed 0x7201, attempt 21) keeps the
// positive path exercised.  n = 36 (root Q = 24) must be NotApplicable; a missing or damaged
// snapshot must fail closed.

#include "ter_arwlf_solve.hpp"
#include "ter_root_generalization_workload.hpp"
#include "ter_root_snapshot.hpp"

#include "ter/full_ter.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

namespace as = ter::arwlf_solve;
namespace rg = ter::root_generalization;
namespace rs = ter::root_snapshot;
namespace wr = ter::arwlf_whole_root;

std::uint64_t g_checks = 0U;
std::uint64_t g_failures = 0U;

void check(const bool condition, const std::string& what) {
  ++g_checks;
  if (!condition) {
    ++g_failures;
    if (g_failures <= 40U) {
      std::cerr << "FAIL: " << what << '\n';
    }
  }
}

constexpr std::uint64_t kBufferBytes = 4ULL * 1048576ULL;

// Streams attempt `attempt` of (n, seed) to `path`; false if no file was written.
bool capture(const std::uint32_t n, const std::uint64_t seed, const std::uint32_t attempt, const std::string& path) {
  const ter::Instance instance = rg::authentic_instance(n, seed);
  ter::FullTerConfig config = rg::authentic_config(seed);
  config.attempt_limit = attempt + 1U;  // the stream admits attempt k only inside a budget > k
  const rs::CaptureOutcome outcome = rs::capture_to_file(instance, config, attempt, path, kBufferBytes, "test");
  return outcome.written && outcome.stream.status == ter::Status::Completed;
}

std::string label(const std::uint64_t seed, const std::uint32_t attempt) {
  return "seed " + std::to_string(seed) + " attempt " + std::to_string(attempt);
}

void test_n36_not_applicable(const std::filesystem::path& dir) {
  const std::string path = (dir / "solve_n36.snap").string();
  check(capture(36U, 0x3601U, 0U, path), "n=36 attempt 0 captures");
  const as::AttemptOutcome o = as::solve_attempt(path, wr::Backend::Dispatch);
  check(o.status == ter::Status::NotApplicable, "n=36 (root Q = 24 < 25) is NotApplicable, never a result");
  check(o.parents.empty(), "n=36: no parents reported");
  std::filesystem::remove(path);
}

void test_fail_closed(const std::filesystem::path& dir) {
  const as::AttemptOutcome missing = as::solve_attempt((dir / "does_not_exist.snap").string(), wr::Backend::Dispatch);
  check(missing.status == ter::Status::InvalidConfiguration, "a missing snapshot is InvalidConfiguration");

  const std::string path = (dir / "solve_damaged.snap").string();
  check(capture(72U, 0x7201U, 0U, path), "n=72 capture for the damage test");
  {
    std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
    rs::Header header{};
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    check(file.good() && header.entries[0] > 0U, "damage test: header read");
    file.seekg(static_cast<std::streamoff>(header.offset[0] + 17U));  // a byte of list 0's first record
    char byte = 0;
    file.read(&byte, 1);
    byte = static_cast<char>(byte ^ 0x5A);
    file.seekp(static_cast<std::streamoff>(header.offset[0] + 17U));
    file.write(&byte, 1);
    check(file.good(), "damage test: byte flipped");
  }
  const as::AttemptOutcome damaged = as::solve_attempt(path, wr::Backend::Dispatch);
  check(damaged.status == ter::Status::InvalidConfiguration && damaged.parents.empty(),
        "a snapshot whose list checksum fails is refused (InvalidConfiguration), never solved");
  std::filesystem::remove(path);
}

// One n=72 attempt, classic against the staged path with DISPATCH and forced
// ARWLF.  Returns whether the classic attempt was Found.
bool differential_attempt(const std::filesystem::path& dir, const ter::Instance& instance,
                          const std::uint64_t seed, const std::uint32_t attempt) {
  // Classic: attempt `attempt` is attempt 0 of the config seeded seed + attempt.
  ter::FullTerConfig config = rg::authentic_config(seed + attempt);
  config.attempt_limit = 1U;
  ter::FullTerRootCapture classic_capture;
  config.root_capture = &classic_capture;
  const ter::FullTerResult classic = ter::solve_full_ter(instance, config);
  check(classic_capture.status == ter::Status::Completed, "classic capture completes: " + label(seed, attempt));
  check(classic.status == ter::Status::Found || classic.status == ter::Status::AttemptLimitReached,
        "classic attempt ends Found or AttemptLimitReached: " + label(seed, attempt));
  config.root_capture = nullptr;
  const ter::FullTerRootReportResult report =
      ter::report_full_ter_root(classic_capture, config, ter::RootOutputPolicy::FullCanonicalOutput);
  check(report.status == ter::Status::Completed, "classic root report completes: " + label(seed, attempt));
  const bool classic_found = classic.status == ter::Status::Found;

  const std::string path = (dir / "solve_n72.snap").string();
  check(capture(72U, seed, attempt, path), "staged capture: " + label(seed, attempt));
  for (const wr::Backend backend : {wr::Backend::Dispatch, wr::Backend::Arwlf}) {
    const std::string what = std::string(wr::label(backend)) + ", " + label(seed, attempt);
    const as::AttemptOutcome o = as::solve_attempt(path, backend);
    check(o.attempt == attempt && o.attempt_seed == seed + attempt, "attempt identity: " + what);
    check(o.structural_triples == report.valid_root_count,
          "accepted triples == classic valid_root_count (" + std::to_string(o.structural_triples) + " vs " +
              std::to_string(report.valid_root_count) + "): " + what);
    check(o.status == (classic_found ? ter::Status::Found : ter::Status::NoWitnessThisAttempt),
          "outcome agrees with the classic attempt: " + what);
    if (classic_found) {
      check(std::find(o.parents.begin(), o.parents.end(), classic.witness) != o.parents.end(),
            "the classic witness is among the verified parents: " + what);
    }
  }
  std::filesystem::remove(path);
  if (classic_found) {
    std::cout << "# Found at " << label(seed, attempt) << " (" << report.valid_root_count << " accepted triples)\n";
  }
  return classic_found;
}

void test_n72_differential(const std::filesystem::path& dir) {
  const std::vector<std::uint64_t> seeds{0x7201U, 0x7203U, 0x7207U, 19797034556212562ULL};
  constexpr std::uint32_t kAttempts = 8U;
  std::uint32_t scanned = 0U;
  std::uint32_t found = 0U;
  for (const std::uint64_t seed : seeds) {
    const ter::Instance instance = rg::authentic_instance(72U, seed);
    for (std::uint32_t attempt = 0U; attempt < kAttempts; ++attempt) {
      ++scanned;
      found += differential_attempt(dir, instance, seed, attempt) ? 1U : 0U;
    }
  }
  std::cout << "# n=72 scan: " << scanned << " attempts, " << found << " Found\n";
  // The scan's attempts are mostly NoWitnessThisAttempt, so the positive path is
  // pinned: seed 0x7201's first Found attempt (21, measured with ter_full_bench
  // --mode=time-to-found) must be Found by the classic solver and by both backends.
  check(differential_attempt(dir, rg::authentic_instance(72U, 0x7201U), 0x7201U, 21U),
        "seed 0x7201 attempt 21 is Found, so the positive path is exercised");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: ter_arwlf_solve_tests <scratch directory>\n";
    return 2;
  }
  const std::filesystem::path dir(argv[1]);
  std::filesystem::create_directories(dir);
  test_n36_not_applicable(dir);
  test_fail_closed(dir);
  test_n72_differential(dir);
  std::cout << "ter_arwlf_solve_tests: " << g_checks << " checks, " << g_failures << " failures\n";
  return g_failures == 0U ? 0 : 1;
}
