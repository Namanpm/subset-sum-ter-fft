// Group-staged root streaming and packed root snapshots: focused tests.
//
// The authority is the classic solve_full_ter() root capture.  Snapshot records
// are decoded here with an independent bit reader (memcpy + shifts), not with
// the snapshot module's accessors, and compared entry for entry.
//
// Coverage: streamed lists == classic capture (digits, residues, targets,
// parameters, sizes, order) on several n = 36 seeds and on the n = 72 anchor;
// the snapshot-built AUTHENTIC workload == the in-process capture workload on
// the anchor (families, ids, tau, authority tables); corruption, truncation and
// header tampering are rejected; a declared memory limit that cannot hold one
// group yields ResourceLimit with no file and no partial left behind; a sink
// that refuses records yields GenerationIncomplete.

#include "ter/limitations.hpp"
#include "ter_root_generalization_workload.hpp"
#include "ter_root_snapshot.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {

namespace rg = ter::root_generalization;
namespace rs = ter::root_snapshot;

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

[[nodiscard]] std::string temp_path(const std::string& name) {
  return (std::filesystem::temp_directory_path() / ("ter_root_snapshot_test_" + name)).string();
}

// Independent decode of one record straight from the file bytes.
struct Decoded {
  std::vector<int> digits;
  std::vector<std::uint32_t> residue;
};

[[nodiscard]] Decoded decode(const std::vector<std::uint8_t>& bytes, const std::size_t at,
                             const std::uint32_t n) {
  const std::uint32_t words64 = (n + 63U) / 64U;
  const std::uint32_t words32 = (n + 31U) / 32U;
  Decoded out;
  for (std::uint32_t coordinate = 0U; coordinate < n; ++coordinate) {
    std::uint64_t positive = 0U;
    std::uint64_t negative = 0U;
    std::memcpy(&positive, bytes.data() + at + (coordinate / 64U) * 8U, 8U);
    std::memcpy(&negative, bytes.data() + at + (words64 + coordinate / 64U) * 8U, 8U);
    const bool p = ((positive >> (coordinate % 64U)) & 1U) != 0U;
    const bool m = ((negative >> (coordinate % 64U)) & 1U) != 0U;
    out.digits.push_back(p ? 1 : (m ? -1 : 0));
  }
  for (std::uint32_t word = 0U; word < words32; ++word) {
    std::uint32_t value = 0U;
    std::memcpy(&value, bytes.data() + at + words64 * 16U + word * 4U, 4U);
    out.residue.push_back(value);
  }
  return out;
}

[[nodiscard]] std::vector<std::uint8_t> read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

[[nodiscard]] bool padded_equal(const std::vector<std::uint32_t>& padded,
                                const std::vector<std::uint32_t>& trimmed) {
  if (trimmed.size() > padded.size()) {
    return false;
  }
  for (std::size_t word = 0U; word < padded.size(); ++word) {
    if (padded[word] != (word < trimmed.size() ? trimmed[word] : 0U)) {
      return false;
    }
  }
  return true;
}

// Captures (n, seed) both ways and requires identity.  Returns the snapshot path.
std::string check_identity(const std::uint32_t n, const std::uint64_t seed, const std::string& tag) {
  const ter::Instance instance = rg::authentic_instance(n, seed);
  ter::FullTerConfig config = rg::authentic_config(seed);
  ter::FullTerRootCapture classic;
  config.root_capture = &classic;
  static_cast<void>(ter::solve_full_ter(instance, config));
  check(classic.status == ter::Status::Completed, tag + " classic capture completes");
  if (classic.status != ter::Status::Completed) {
    return {};
  }
  config.root_capture = nullptr;
  const auto attempt = static_cast<std::uint32_t>(classic.seed - seed);
  const std::string path = temp_path(tag + ".snap");
  const rs::CaptureOutcome outcome = rs::capture_to_file(instance, config, attempt, path, 4096U, "test");
  check(outcome.written && outcome.stream.status == ter::Status::Completed,
        tag + " streamed capture completes");
  check(!std::filesystem::exists(path + ".partial"), tag + " no partial file left");
  check(outcome.stream.parameters.n == classic.parameters.n &&
            outcome.stream.parameters.r1 == classic.parameters.r1 &&
            outcome.stream.parameters.r2 == classic.parameters.r2,
        tag + " parameters identical");
  check(outcome.stream.attempt_seed == classic.seed, tag + " same attempt seed");
  const std::vector<std::uint8_t> bytes = read_file(path);
  rs::Header header{};
  std::memcpy(&header, bytes.data(), sizeof(header));
  bool identical = true;
  for (std::size_t list = 0U; list < 3U; ++list) {
    identical = identical && header.entries[list] == classic.children[list].size() &&
                outcome.stream.level1_sizes[list] == classic.children[list].size();
    std::vector<std::uint32_t> target(header.child_target[list],
                                      header.child_target[list] + header.words32);
    identical = identical && padded_equal(target, classic.child_targets_words_le[list]);
    for (std::uint64_t index = 0U; identical && index < header.entries[list]; ++index) {
      const Decoded entry = decode(bytes, static_cast<std::size_t>(header.offset[list] + index * header.record_bytes), n);
      const auto& digits = classic.children[list][index];
      for (std::uint32_t coordinate = 0U; coordinate < n; ++coordinate) {
        identical = identical && entry.digits[coordinate] == digits[coordinate];
      }
      identical = identical && padded_equal(entry.residue, classic.residues_words_le[list][index]);
    }
  }
  std::vector<std::uint32_t> root(header.root_target, header.root_target + header.words32);
  identical = identical && padded_equal(root, classic.root_target_words_le);
  check(identical, tag + " snapshot equals the classic capture entry for entry, in order");
  rs::Snapshot loaded;
  std::string error;
  check(rs::load(path, loaded, error), tag + " snapshot loads and validates: " + error);
  return path;
}

void test_small_identity() {
  for (const std::uint64_t seed : {0x1001ULL, 0x2003ULL, 0x3007ULL}) {
    const std::string path = check_identity(36U, seed, "n36_" + std::to_string(seed));
    std::filesystem::remove(path);
  }
}

void test_anchor_workload_identity() {
  const std::uint64_t seed = ter::microreport8::kReferenceRootSeed;
  const std::string path = check_identity(72U, seed, "n72_anchor");
  if (path.empty()) {
    return;
  }
  const auto from_snapshot = rg::load_snapshot_workload(path, true);
  const auto in_process = rg::capture_authentic(72U, seed, true);
  check(from_snapshot->status == rg::WorkloadStatus::Completed, "anchor snapshot workload completes");
  check(in_process->status == rg::WorkloadStatus::Completed, "anchor in-process workload completes");
  if (from_snapshot->status == rg::WorkloadStatus::Completed &&
      in_process->status == rg::WorkloadStatus::Completed) {
    check(from_snapshot->fingerprint == in_process->fingerprint,
          "anchor families and ids identical (fingerprint)");
    check(from_snapshot->data->tau_low32 == in_process->data->tau_low32, "anchor tau identical");
    check(from_snapshot->a_size == 19109U && from_snapshot->b_size == 32496U &&
              from_snapshot->c_size == 33505U,
          "anchor list sizes are the reference geometry");
    const auto& a = from_snapshot->authority;
    const auto& b = in_process->authority;
    bool same = a.ok && b.ok && a.n == b.n && a.r1 == b.r1 && a.tau_low32 == b.tau_low32 &&
                a.residue_words == b.residue_words && a.root_target == b.root_target &&
                a.shape_available == b.shape_available &&
                a.expected_parent.positive == b.expected_parent.positive &&
                a.expected_parent.negative == b.expected_parent.negative &&
                a.expected_parent.zero == b.expected_parent.zero;
    for (std::size_t list = 0U; list < 3U; ++list) {
      same = same && a.quotient[list] == b.quotient[list] && a.residue[list] == b.residue[list] &&
             a.digits[list] == b.digits[list];
    }
    check(same, "anchor authority tables identical (quotient, residue, digits, targets, shape)");
  }
  std::filesystem::remove(path);
}

void test_integrity() {
  const std::string path = check_identity(36U, 0x4009ULL, "integrity");
  if (path.empty()) {
    return;
  }
  const std::vector<std::uint8_t> bytes = read_file(path);
  rs::Snapshot snapshot;
  std::string error;
  const auto rewrite = [&](const std::vector<std::uint8_t>& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(content.data()), static_cast<std::streamsize>(content.size()));
  };
  std::vector<std::uint8_t> flipped = bytes;
  flipped[bytes.size() - 3U] ^= 0x10U;
  rewrite(flipped);
  check(!rs::load(path, snapshot, error), "a flipped record bit is rejected");
  rewrite(std::vector<std::uint8_t>(bytes.begin(), bytes.end() - 48));
  check(!rs::load(path, snapshot, error), "a truncated snapshot is rejected");
  std::vector<std::uint8_t> tampered = bytes;
  tampered[offsetof(rs::Header, n)] ^= 0x01U;
  rewrite(tampered);
  check(!rs::load(path, snapshot, error), "a tampered header is rejected");
  std::vector<std::uint8_t> longer = bytes;
  longer.push_back(0U);
  rewrite(longer);
  check(!rs::load(path, snapshot, error), "trailing bytes are rejected");
  rewrite(bytes);
  check(rs::load(path, snapshot, error), "the original bytes load again");
  std::filesystem::remove(path);
}

void test_fail_closed() {
  const std::uint64_t seed = ter::microreport8::kReferenceRootSeed;
  const ter::Instance instance = rg::authentic_instance(72U, seed);
  ter::FullTerConfig config = rg::authentic_config(seed);
  const std::string path = temp_path("fail_closed.snap");
  std::filesystem::remove(path);
  // Measure the per-group declared peaks, then set the limit to the largest
  // peak minus one byte: every group that fits is streamed first, and the
  // capture must still fail closed when the largest group cannot fit.
  const rs::CaptureOutcome measured = rs::capture_to_file(instance, config, 0U, path, 4096U, "test");
  check(measured.written, "n=72 group-staged capture completes under the frozen limit");
  std::filesystem::remove(path);
  const auto& peaks = measured.stream.group_peak_bytes;
  const std::uint64_t largest = std::max({peaks[0], peaks[1], peaks[2]});
  const bool mid_stream = peaks[0] < largest;
  std::cout << "# n=72 declared group peaks (bytes): " << peaks[0] << ' ' << peaks[1] << ' '
            << peaks[2] << (mid_stream ? " -> failure lands after group 0 was streamed\n"
                                       : " -> failure lands in group 0\n");
  config.memory_limit_bytes = largest - 1U;
  const rs::CaptureOutcome outcome = rs::capture_to_file(instance, config, 0U, path, 4096U, "test");
  check(outcome.stream.status == ter::Status::ResourceLimit,
        "a limit one byte below the largest group peak is ResourceLimit, not a partial success");
  check(!outcome.written && !std::filesystem::exists(path) &&
            !std::filesystem::exists(path + ".partial"),
        "a failed capture leaves no snapshot and no partial file");

  struct Refuse {
    static bool begin(void*, std::uint32_t, std::uint64_t, const std::vector<std::uint32_t>&) {
      return true;
    }
    static bool write(void*, const std::uint8_t*, std::size_t) { return false; }
  };
  ter::FullTerRootStream stream;
  stream.begin_list = &Refuse::begin;
  stream.write_records = &Refuse::write;
  const ter::Instance small = rg::authentic_instance(36U, 0x1001ULL);
  const auto refused =
      ter::stream_full_ter_root_lists(small, rg::authentic_config(0x1001ULL), 0U, stream, 4096U);
  check(refused.status == ter::Status::GenerationIncomplete,
        "a sink that refuses records yields GenerationIncomplete");
}

}  // namespace

int main() {
  std::cout << "# ter_root_snapshot_tests\n";
  test_small_identity();
  test_integrity();
  test_fail_closed();
  test_anchor_workload_identity();
  std::cout << "checks=" << g_checks << " failures=" << g_failures << '\n';
  std::cout << (g_failures == 0U ? "RESULT: PASS\n" : "RESULT: FAIL\n");
  return g_failures == 0U ? EXIT_SUCCESS : EXIT_FAILURE;
}
