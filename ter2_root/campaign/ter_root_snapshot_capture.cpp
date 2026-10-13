// Phase A of the n=108 root experiment: capture one Full TER attempt's three
// genuine L1 root lists into a packed snapshot, then exit.
//
// Group-staged and capture-only (ter::stream_full_ter_root_lists): no L1 -> L0
// merge and no solver verdict.  Benchmarking the snapshot happens later, in a
// fresh process (ter_root_queue_generalization_bench --snapshot=...).
//
// --verify-against-solve runs the classic solve_full_ter() root capture for the
// same (n, seed) first, takes its attempt, and requires the snapshot to equal
// that capture entry for entry.  It is only feasible where the classic capture
// fits the declared limit (e.g. n = 36, 72); that is the identity gate proving
// the staged pipeline computes exactly the solver's lists.

#include "ter/limitations.hpp"
#include "ter_root_generalization_workload.hpp"
#include "ter_root_snapshot.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

namespace {

namespace rg = ter::root_generalization;
namespace rs = ter::root_snapshot;

[[nodiscard]] const char* status_name(const ter::Status status) {
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

[[nodiscard]] double mib(const std::uint64_t bytes) { return static_cast<double>(bytes) / 1048576.0; }

// Entry-for-entry comparison of the snapshot with a classic root capture.
[[nodiscard]] std::string compare_with_capture(const rs::Snapshot& snapshot,
                                               const ter::FullTerRootCapture& capture) {
  const rs::Header& header = snapshot.header;
  if (capture.parameters.n != header.n || capture.parameters.r1 != header.r1 ||
      capture.parameters.r2 != header.r2) {
    return "parameters differ";
  }
  const auto same_words = [](std::span<const std::uint32_t> padded,
                             const std::vector<std::uint32_t>& trimmed) {
    for (std::size_t word = 0U; word < padded.size(); ++word) {
      if (padded[word] != (word < trimmed.size() ? trimmed[word] : 0U)) {
        return false;
      }
    }
    return trimmed.size() <= padded.size();
  };
  if (!same_words(snapshot.root_target(), capture.root_target_words_le)) {
    return "root target differs";
  }
  for (std::size_t list = 0U; list < 3U; ++list) {
    if (!same_words(snapshot.child_target(list), capture.child_targets_words_le[list])) {
      return "child target " + std::to_string(list) + " differs";
    }
    if (capture.children[list].size() != header.entries[list] ||
        capture.residues_words_le[list].size() != header.entries[list]) {
      return "list " + std::to_string(list) + " size differs";
    }
    for (std::uint64_t index = 0U; index < header.entries[list]; ++index) {
      const auto& digits = capture.children[list][index];
      for (std::uint32_t coordinate = 0U; coordinate < header.n; ++coordinate) {
        if (snapshot.digit(list, index, coordinate) != digits[coordinate]) {
          return "list " + std::to_string(list) + " entry " + std::to_string(index) +
                 " digits differ";
        }
      }
      if (!same_words(snapshot.residue(list, index), capture.residues_words_le[list][index])) {
        return "list " + std::to_string(list) + " entry " + std::to_string(index) +
               " residue differs";
      }
    }
  }
  return {};
}

}  // namespace

int main(const int argc, char** const argv) {
  std::uint32_t n = 0U;
  std::uint64_t seed = 0U;
  std::string attempt_text = "0";
  std::string out;
  std::uint64_t buffer_mib = 4U;
  bool verify = false;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument{argv[index]};
    const auto value = [&](const std::string_view prefix) {
      return std::string(argument.substr(prefix.size()));
    };
    if (argument.rfind("--n=", 0U) == 0U) {
      n = static_cast<std::uint32_t>(std::strtoul(value("--n=").c_str(), nullptr, 10));
    } else if (argument.rfind("--seed=", 0U) == 0U) {
      seed = std::strtoull(value("--seed=").c_str(), nullptr, 0);
    } else if (argument.rfind("--attempt=", 0U) == 0U) {
      attempt_text = value("--attempt=");
    } else if (argument.rfind("--out=", 0U) == 0U) {
      out = value("--out=");
    } else if (argument.rfind("--buffer-mib=", 0U) == 0U) {
      buffer_mib = std::strtoull(value("--buffer-mib=").c_str(), nullptr, 10);
    } else if (argument == "--verify-against-solve") {
      verify = true;
    } else {
      std::cerr << "unknown argument: " << argument << '\n';
      return EXIT_FAILURE;
    }
  }
  if (n == 0U || out.empty() || buffer_mib == 0U || (attempt_text == "auto" && !verify)) {
    std::cerr << "usage: ter_root_snapshot_capture --n=N --seed=S --out=FILE "
                 "[--attempt=K] [--buffer-mib=M] [--verify-against-solve [--attempt=auto]]\n";
    return EXIT_FAILURE;
  }

  const ter::Instance instance = rg::authentic_instance(n, seed);
  ter::FullTerConfig config = rg::authentic_config(seed);
  std::cout << "# ter_root_snapshot_capture n=" << n << " seed=0x" << std::hex << seed << std::dec
            << " memory_limit_mib=" << config.memory_limit_bytes / 1048576U
            << " buffer_mib=" << buffer_mib << " commit=" << TER_CONFIGURE_TIME_COMMIT << '\n';
  if (instance.n == 0U) {
    std::cerr << "FATAL: instance construction failed\n";
    return EXIT_FAILURE;
  }

  ter::FullTerRootCapture classic;
  std::uint32_t attempt = 0U;
  if (verify) {
    ter::FullTerConfig classic_config = config;
    classic_config.root_capture = &classic;
    const auto solved = ter::solve_full_ter(instance, classic_config);
    std::cout << "# classic solve: status=" << status_name(solved.status)
              << " capture=" << status_name(classic.status)
              << " declared_peak_mib=" << mib(solved.telemetry.peak_working_bytes) << '\n';
    if (classic.status != ter::Status::Completed) {
      std::cerr << "FATAL: the classic capture is not available for verification: "
                << solved.message << '\n';
      return EXIT_FAILURE;
    }
    const std::uint64_t classic_attempt = classic.seed - seed;
    if (attempt_text == "auto") {
      attempt = static_cast<std::uint32_t>(classic_attempt);
    } else {
      attempt = static_cast<std::uint32_t>(std::strtoul(attempt_text.c_str(), nullptr, 10));
      if (attempt != classic_attempt) {
        std::cerr << "FATAL: the classic capture holds attempt " << classic_attempt
                  << "; use --attempt=auto\n";
        return EXIT_FAILURE;
      }
    }
  } else {
    attempt = static_cast<std::uint32_t>(std::strtoul(attempt_text.c_str(), nullptr, 10));
  }
  // The stream admits attempt k only inside a budget of more than k attempts;
  // the budget gates nothing else, so attempt k's lists are solve_full_ter's.
  if (attempt == std::numeric_limits<std::uint32_t>::max()) {
    std::cerr << "FATAL: attempt " << attempt << " is outside every attempt budget\n";
    return EXIT_FAILURE;
  }
  config.attempt_limit = std::max(config.attempt_limit, attempt + 1U);

  const rs::CaptureOutcome outcome = rs::capture_to_file(
      instance, config, attempt, out, buffer_mib * 1048576U, TER_CONFIGURE_TIME_COMMIT);
  const ter::FullTerRootStreamResult& stream = outcome.stream;
  std::cout << "# stream status=" << status_name(stream.status)
            << (stream.message.empty() ? "" : " (" + stream.message + ")") << '\n';
  std::cout << "# n=" << stream.parameters.n << " r1=" << stream.parameters.r1
            << " r2=" << stream.parameters.r2 << " Q=" << stream.parameters.n - stream.parameters.r1
            << " attempt=" << attempt << " attempt_seed=0x" << std::hex << stream.attempt_seed
            << std::dec << " record_bytes=" << stream.record_bytes << '\n';
  std::cout << "# level2 sizes:";
  for (const auto size : stream.level2_sizes) {
    std::cout << ' ' << size;
  }
  std::cout << "\n# level1 sizes: A=" << stream.level1_sizes[0] << " B=" << stream.level1_sizes[1]
            << " C=" << stream.level1_sizes[2] << '\n';
  std::cout << "# declared accounting (MiB): base=" << mib(stream.base_resident_bytes)
            << " group peaks=" << mib(stream.group_peak_bytes[0]) << '/'
            << mib(stream.group_peak_bytes[1]) << '/' << mib(stream.group_peak_bytes[2])
            << " peak=" << mib(stream.peak_working_bytes) << " (limit "
            << mib(config.memory_limit_bytes) << ")\n";
  std::cout << "# elapsed_s=" << static_cast<double>(stream.elapsed_ns) / 1e9 << '\n';
  if (!outcome.written) {
    std::cerr << "FATAL: no snapshot written: " << outcome.io_error << '\n';
    return EXIT_FAILURE;
  }
  rs::Snapshot snapshot;
  std::string error;
  if (!rs::load(out, snapshot, error)) {
    std::cerr << "FATAL: written snapshot does not load: " << error << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "# snapshot " << out << " bytes=" << outcome.file_bytes
            << " process_peak_working_set_mib=" << mib(snapshot.header.process_peak_working_set)
            << " (measured by the OS in this capture process)\n";
  if (verify) {
    const std::string difference = compare_with_capture(snapshot, classic);
    std::cout << "# identity vs classic solve_full_ter capture (attempt " << attempt
              << "): " << (difference.empty() ? "IDENTICAL" : "DIFFERENT: " + difference)
              << '\n';
    if (!difference.empty()) {
      return EXIT_FAILURE;
    }
  }
  std::ofstream report(out + ".json");
  report << "{\"n\":" << n << ",\"seed\":" << seed << ",\"attempt\":" << attempt
         << ",\"attempt_seed\":" << stream.attempt_seed << ",\"r1\":" << stream.parameters.r1
         << ",\"r2\":" << stream.parameters.r2 << ",\"record_bytes\":" << stream.record_bytes
         << ",\"level1_sizes\":[" << stream.level1_sizes[0] << ',' << stream.level1_sizes[1] << ','
         << stream.level1_sizes[2] << "],\"level2_sizes\":[";
  for (std::size_t index = 0U; index < 9U; ++index) {
    report << (index == 0U ? "" : ",") << stream.level2_sizes[index];
  }
  report << "],\"declared_base_bytes\":" << stream.base_resident_bytes
         << ",\"declared_group_peak_bytes\":[" << stream.group_peak_bytes[0] << ','
         << stream.group_peak_bytes[1] << ',' << stream.group_peak_bytes[2]
         << "],\"declared_peak_bytes\":" << stream.peak_working_bytes
         << ",\"memory_limit_bytes\":" << config.memory_limit_bytes
         << ",\"process_peak_working_set_bytes\":" << snapshot.header.process_peak_working_set
         << ",\"stream_buffer_bytes\":" << stream.stream_buffer_bytes
         << ",\"file_bytes\":" << outcome.file_bytes << ",\"elapsed_ns\":" << stream.elapsed_ns
         << ",\"verified_against_classic_capture\":" << (verify ? "true" : "false")
         << ",\"commit\":\"" << TER_CONFIGURE_TIME_COMMIT << "\"}\n";
  return EXIT_SUCCESS;
}
