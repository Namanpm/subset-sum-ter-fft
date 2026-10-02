// A64-CHQR: focused exactness tests.
//
// Authority: a brute-force triple loop over every (q, x, y) of every selected
// microproblem that decides the root equation on whole Q-bit values with its
// own two-word addition (no digits, no residual split, no carry recurrence).
//
// Coverage: Q = 69 (five high bits) and Q = 47 (none); planted solutions,
// carry chains through every digit boundary (all-ones residual digits),
// duplicated values under distinct EntryIds (multiplicity kept), adversarial
// triples equal on the low 64 bits but not on the high five, several direct
// thresholds and refinement widths, K2_WIDE == A64 == oracle, the width-sweep
// identity projected_hits(k) == sum_{c >= k-6} sweep[c], and on the authentic
// n = 72 anchor: A64 == K2_WIDE, and A64's residue survivors == frozen K2's.

#include "ter/full_ter.hpp"
#include "ter_a64_chqr.hpp"
#include "ter_microreport8_kernels.hpp"
#include "ter_root_generalization_workload.hpp"
#include "ter_root_queue_kernels.hpp"
#include "ter_root_snapshot.hpp"
#include "ter_synthetic_root.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

namespace a64 = ter::a64;
namespace rg = ter::root_generalization;
namespace mr8 = ter::microreport8;
namespace rs = ter::root_snapshot;
using namespace ter::synthetic_root;

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


void test_synthetic(const std::uint32_t bits, const std::uint64_t seed) {
  const std::string tag = "Q=" + std::to_string(bits) + " seed=" + std::to_string(seed);
  Synthetic root = make_root(bits, seed, 2000U);
  check(root.keys.ok, tag + " keys built");
  const rg::OutputDigest expected = oracle(root);
  check(expected.triples >= 40U, tag + " planted solutions are found by the oracle");
  const std::uint32_t w = root.keys.residual_bits;

  rg::RootSink wide = make_sink(root);
  a64::Telemetry wide_t;
  a64::run_k2_wide(*root.data, root.index, root.keys, wide, &wide_t, true);
  check(same_triples(expected, wide.digest), tag + " K2_WIDE == brute force");
  check(wide.digest.directory_misses == 0U && wide.digest.residual_mismatches == 0U,
        tag + " K2_WIDE clean");
  check(wide_t.full_hits == expected.triples, tag + " K2_WIDE full hits count");

  for (const std::uint64_t threshold : {0ULL, 16ULL, 128ULL, 1ULL << 40U}) {
    for (const std::uint32_t digit_bits : {13U, 8U}) {
      a64::Config config;
      config.profile = a64::default_profile(w, digit_bits);
      config.direct_threshold = threshold;
      rg::RootSink sink = make_sink(root);
      a64::Telemetry telemetry;
      a64::run_a64(*root.data, root.index, root.keys, config, sink, &telemetry);
      const std::string where = tag + " threshold=" + std::to_string(threshold) +
                                " digits=" + std::to_string(digit_bits);
      check(same_triples(expected, sink.digest), where + " A64 == brute force");
      check(telemetry.full_hits == expected.triples, where + " A64 full hits count");
    }
  }

  // Width sweep: A64 refined to k bits reproduces the K2_WIDE histogram.
  for (std::uint32_t step = 25U;; step += 4U) {
    const std::uint32_t k = std::min(step, 6U + w);
    a64::Config config;
    config.profile = a64::default_profile(k - 6U);
    config.direct_threshold = 0U;
    rg::RootSink sink = make_sink(root);
    a64::Telemetry telemetry;
    a64::run_a64(*root.data, root.index, root.keys, config, sink, &telemetry);
    std::uint64_t predicted = 0U;
    for (std::uint32_t c = k - 6U; c <= w; ++c) {
      predicted += wide_t.sweep[c];
    }
    const std::string where = tag + " k=" + std::to_string(k);
    check(telemetry.projected_hits == predicted, where + " projected hits == sweep sum");
    check(same_triples(expected, sink.digest), where + " A64 output unchanged by width");
    if (k == 6U + w) {
      break;
    }
  }

  // Declared subsample: every backend restricted identically.
  rg::select_microproblems(root.index, 700U);
  const rg::OutputDigest sub = oracle(root);
  rg::RootSink sub_a64 = make_sink(root);
  a64::Config config;
  config.profile = a64::default_profile(w);
  a64::run_a64(*root.data, root.index, root.keys, config, sub_a64, nullptr);
  rg::RootSink sub_wide = make_sink(root);
  a64::run_k2_wide(*root.data, root.index, root.keys, sub_wide, nullptr, false);
  check(same_triples(sub, sub_a64.digest) && same_triples(sub, sub_wide.digest),
        tag + " subsample: A64 == K2_WIDE == brute force");
  check(sub.triples < expected.triples, tag + " subsample is a strict subset here");
}

// Mutation guard for the high-bit authority: dropping it must be detected.
void test_high_bits_matter() {
  Synthetic root = make_root(69U, 0xABCDU, 500U);
  const rg::OutputDigest expected = oracle(root);
  rg::RootSink wide = make_sink(root);
  a64::Telemetry telemetry;
  a64::run_k2_wide(*root.data, root.index, root.keys, wide, &telemetry, false);
  check(telemetry.fast_hits >= telemetry.full_hits + 20U,
        "Q=69: the planted low-64-only collisions reach the high-bit authority");
  check(wide.digest.triples == expected.triples, "Q=69: and are rejected there");
}

void test_anchor() {
  const std::uint64_t seed = mr8::kReferenceRootSeed;
  const ter::Instance instance = rg::authentic_instance(72U, seed);
  ter::FullTerConfig config = rg::authentic_config(seed);
  ter::FullTerRootCapture classic;
  config.root_capture = &classic;
  static_cast<void>(ter::solve_full_ter(instance, config));
  check(classic.status == ter::Status::Completed, "anchor classic capture completes");
  if (classic.status != ter::Status::Completed) {
    return;
  }
  config.root_capture = nullptr;
  const auto attempt = static_cast<std::uint32_t>(classic.seed - seed);
  const std::string path =
      (std::filesystem::temp_directory_path() / "ter_a64_chqr_test_anchor.snap").string();
  const rs::CaptureOutcome outcome = rs::capture_to_file(instance, config, attempt, path, 4096U, "test");
  check(outcome.written, "anchor snapshot written");
  const auto workload = rg::load_snapshot_workload(path, true);
  std::filesystem::remove(path);
  check(workload->status == rg::WorkloadStatus::Completed, "anchor workload loads");
  check(workload->full_quotient_bits == 47U, "anchor Q = 47");
  if (workload->status != rg::WorkloadStatus::Completed) {
    return;
  }
  const a64::FamilyKeys keys = a64::build_family_keys(*workload);
  check(keys.ok && keys.residual_bits == 41U && keys.hi_bits == 0U, "anchor keys: W = 41");
  const mr8::Dataset& data = *workload->data;
  rg::RootIndex index;
  rg::build_root_index(data, index);
  const auto sink_for = [&]() {
    rg::RootSink sink;
    sink.data = &data;
    sink.index = &index;
    sink.authority = &workload->authority;
    sink.reset();
    return sink;
  };
  rg::RootSink k2 = sink_for();
  rg::run_k2(data, k2);
  rg::RootSink wide = sink_for();
  a64::run_k2_wide(data, index, keys, wide, nullptr, false);
  rg::RootSink tree = sink_for();
  a64::Config a64_config;
  a64_config.profile = a64::default_profile(keys.residual_bits);
  a64::run_a64(data, index, keys, a64_config, tree, nullptr);
  check(k2.digest.full_q == 4718U, "anchor frozen K2 full-Q count is the measured 4718");
  // Authentic cross-checks against the donor quotient authority: 25-bit and
  // 32-bit projected hits from the full keys equal frozen K2's triples and
  // its low-32 prefilter survivors.
  a64::Telemetry wide_t;
  rg::RootSink wide_again = sink_for();
  a64::run_k2_wide(data, index, keys, wide_again, &wide_t, false);
  check(wide_t.projected_hits == k2.digest.triples, "anchor K2_WIDE 25-bit hits == frozen K2 triples");
  a64::Config k32;
  k32.profile = a64::default_profile(26U);
  a64::Telemetry k32_t;
  rg::RootSink k32_sink = sink_for();
  a64::run_a64(data, index, keys, k32, k32_sink, &k32_t);
  check(k32_t.projected_hits == k2.digest.full_q, "anchor A64 k=32 hits == frozen K2 low-32 survivors");
  check(same_triples(wide.digest, tree.digest), "anchor A64 == K2_WIDE");
  check(tree.digest.triples == tree.digest.full_q && tree.digest.triples == tree.digest.residue,
        "anchor: every A64 triple passes the low-32 and residue authorities");
  check(tree.digest.residue == k2.digest.residue && tree.digest.residue_sum == k2.digest.residue_sum &&
            tree.digest.structural == k2.digest.structural,
        "anchor: A64 residue/structural survivors == frozen K2's");
  std::cout << "anchor: frozen K2 triples=" << k2.digest.triples << " fullQ=" << k2.digest.full_q
            << " residue=" << k2.digest.residue << " structural=" << k2.digest.structural
            << " | A64 triples=" << tree.digest.triples << '\n';
}

}  // namespace

int main() {
  test_synthetic(69U, 0x1234U);
  test_synthetic(69U, 0x9876U);
  test_synthetic(47U, 0x5555U);
  test_high_bits_matter();
  test_anchor();
  std::cout << "ter_a64_chqr_tests: " << g_checks << " checks, " << g_failures << " failures\n";
  return g_failures == 0U ? 0 : 1;
}
