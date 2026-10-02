// ARWLF: focused exactness tests.
//
// Authorities: the whole-value brute-force oracle of ter_synthetic_root.hpp
// (no leaves, chunks or carries), an independent brute-force count of
// projected hits on low quotient bits, the scalar packet sweep for the AVX2
// row sweep, and on the authentic n = 72 anchor the frozen K2 + RootSink
// authority chain.
//
// Coverage: scalar filter keeps planted lanes; backend == oracle at Q = 69
// and Q = 47 with sparse leaves, dense leaves (several 16-lane blocks per X
// leaf, partial last blocks) and single-leaf families (dozens of overflow
// blocks), both low-13 carries, scalar and AVX2, every check width; AVX2 and
// scalar pass exactly the same number of lanes through the chunk tables;
// projected-hit counts == brute force; declared subsample; probes; both guards
// abort; dispatcher output == oracle; on the anchor: k=32 hits == frozen K2's
// low-32 survivors, output == K2_WIDE, and residue/structural survivors (with
// their multiset hash) == frozen K2's, also through the dispatcher with
// QUEUE_DENSE available.

#include "ter/full_ter.hpp"
#include "ter_arwlf.hpp"
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

namespace aw = ter::arwlf;
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

// ---------------------------------------------------------------------------
// Reference filter.
// ---------------------------------------------------------------------------
void test_kernel() {
  Rng rng{0xC0FFEEU};
  for (int round = 0; round < 20000; ++round) {
    const std::uint32_t nq = 1U + static_cast<std::uint32_t>(rng.next() % 16U);
    const std::uint32_t nx = 1U + static_cast<std::uint32_t>(rng.next() % 16U);
    const std::uint32_t carry = static_cast<std::uint32_t>(rng.next() & 1U);
    const std::uint32_t mid_mask = (round & 1) != 0 ? ~0U : (1U << 28U) - 1U;
    const std::uint32_t chunks = 1U + static_cast<std::uint32_t>(rng.next() % 4U);
    std::array<std::uint32_t, 16> q{};
    std::array<std::uint32_t, 16> x{};
    for (std::uint32_t i = 0U; i < 16U; ++i) {
      q[i] = static_cast<std::uint32_t>(rng.next()) & mid_mask;
      x[i] = static_cast<std::uint32_t>(rng.next()) & mid_mask;
    }
    aw::LeafTables tables{};
    const auto add_y = [&](const std::uint32_t y) {
      for (std::uint32_t c = 0U; c < aw::kChunks; ++c) {
        const std::uint32_t byte = (y >> (8U * c)) & 0xFFU;
        tables.word[c][byte >> 5U] |= 1U << (byte & 31U);
      }
    };
    const std::uint32_t ny = static_cast<std::uint32_t>(rng.next() % 24U);
    for (std::uint32_t i = 0U; i < ny; ++i) {
      add_y(static_cast<std::uint32_t>(rng.next()) & mid_mask);
    }
    const std::uint32_t pq = static_cast<std::uint32_t>(rng.next() % nq);
    const std::uint32_t px = static_cast<std::uint32_t>(rng.next() % nx);
    add_y((q[pq] + x[px] + carry) & mid_mask);
    std::array<std::uint16_t, 256> lanes{};
    const std::uint32_t n =
        aw::wide_leaf_filter_scalar(q.data(), nq, x.data(), nx, carry, tables, chunks, mid_mask, lanes.data());
    const auto code = static_cast<std::uint16_t>(pq * 16U + px);
    check(std::find(lanes.begin(), lanes.begin() + n, code) != lanes.begin() + n,
          "reference filter: planted lane survives, round " + std::to_string(round));
  }
  check(aw::avx2::translation_unit_is_avx2(), "the AVX2 translation unit is AVX2");
}

// ---------------------------------------------------------------------------
// Backend.
// ---------------------------------------------------------------------------
// (q, x, y) equal on quotient bits [0, bits), counted on whole values.
std::uint64_t brute_projected(const Synthetic& root, const std::uint32_t bits) {
  const std::uint64_t mask = bits >= 64U ? ~std::uint64_t{0} : (std::uint64_t{1} << bits) - 1U;
  const mr8::Dataset& data = *root.data;
  std::uint64_t count = 0U;
  for (std::uint32_t s = 0U; s < rg::kFamilies; ++s) {
    for (std::uint32_t t = 0U; t < rg::kFamilies; ++t) {
      if (!root.index.active(s, t)) {
        continue;
      }
      const std::uint32_t u = (s + t) & 63U;
      for (const mr8::Posting& q : data.q_family[s]) {
        for (const mr8::Posting& x : data.x_family[t]) {
          const Value sum = add(root.lists[0][q.id], root.lists[1][x.id], root.bits);
          for (const mr8::Posting& y : data.y_family[u]) {
            count += ((sum.lo ^ root.lists[2][y.id].lo) & mask) == 0U ? 1U : 0U;
          }
        }
      }
    }
  }
  return count;
}

void test_backend(const std::uint32_t bits, const std::uint64_t seed, const std::size_t size,
                  const std::uint32_t pool) {
  const std::string tag = "Q=" + std::to_string(bits) + " seed=" + std::to_string(seed) +
                          " pool=" + std::to_string(pool);
  Synthetic root = make_root(bits, seed, size, pool);
  const rg::OutputDigest expected = oracle(root);
  check(expected.triples >= 40U, tag + " planted solutions exist");
  const std::uint32_t mid_bits = std::min<std::uint32_t>(32U, root.keys.residual_bits - aw::kLeafBits);

  std::array<std::uint64_t, 4> vector_survivors{};
  for (const bool vector : {true, false}) {
    for (std::size_t c = 0U; c < 4U; ++c) {
      const std::uint32_t check_bits = std::array<std::uint32_t, 4>{0U, 13U, 16U, 24U}[c];
      aw::Config config;
      config.use_avx2 = vector;
      config.account = true;
      config.check_bits = check_bits;
      rg::RootSink sink = make_sink(root);
      aw::Telemetry telemetry;
      const aw::Status status = aw::run_arwlf(*root.data, root.index, root.keys, config, sink, &telemetry);
      const std::string where = tag + (vector ? " avx2" : " scalar") + " check=" + std::to_string(check_bits);
      check(status == aw::Status::Completed, where + " completes");
      check(same_triples(expected, sink.digest), where + " ARWLF == brute force");
      check(telemetry.full_hits == expected.triples, where + " full hits count");
      const std::uint32_t k = 19U + (check_bits == 0U ? mid_bits : check_bits);
      check(telemetry.projected_hits == brute_projected(root, k),
            where + " projected hits at k=" + std::to_string(k) + " == brute force");
      check(telemetry.lanes <= telemetry.logical_pairs && telemetry.filter_survivors <= telemetry.lanes,
            where + " lanes <= logical pairs, survivors <= lanes");
      if (pool != 0U) {
        check(telemetry.packets > telemetry.leaf_triples, where + " over-full leaves split into packets");
      }
      if (vector) {
        vector_survivors[c] = telemetry.filter_survivors;
      } else {
        check(telemetry.filter_survivors == vector_survivors[c],
              where + " AVX2 row sweep and scalar packets pass the same lanes");
      }
    }
  }

  // Declared subsample.
  rg::select_microproblems(root.index, 700U);
  const rg::OutputDigest sub = oracle(root);
  rg::RootSink sub_sink = make_sink(root);
  check(aw::run_arwlf(*root.data, root.index, root.keys, aw::Config{}, sub_sink, nullptr) ==
                aw::Status::Completed &&
            same_triples(sub, sub_sink.digest),
        tag + " subsample: ARWLF == brute force");
  rg::select_microproblems(root.index, 4096U);

  // Probe run and guards.
  aw::Config probe;
  probe.work_divisor = 4U;
  rg::RootSink probe_sink = make_sink(root);
  check(aw::run_arwlf(*root.data, root.index, root.keys, probe, probe_sink, nullptr) ==
            aw::Status::Completed,
        tag + " probe completes");
  aw::Config slow;
  slow.guard_ns_per_pair = 1e-9;
  rg::RootSink guard_sink = make_sink(root);
  check(aw::run_arwlf(*root.data, root.index, root.keys, slow, guard_sink, nullptr) ==
            aw::Status::Aborted,
        tag + " ns/pair guard aborts");
  if (pool != 0U) {
    // Dense leaves: the first judged prefix certainly has two-chunk survivors.
    aw::Config leaky;
    leaky.guard_survivor_rate = 1e-12;
    leaky.check_bits = 16U;
    rg::RootSink leak_sink = make_sink(root);
    check(aw::run_arwlf(*root.data, root.index, root.keys, leaky, leak_sink, nullptr) ==
              aw::Status::Aborted,
          tag + " survivor-rate guard aborts");
  }

  // Dispatcher without QUEUE_DENSE: whatever it picks, the output is exact.
  rg::RootSink dispatched = make_sink(root);
  const aw::Dispatch d = aw::dispatch_and_run(*root.data, root.index, root.keys, nullptr, dispatched);
  check(same_triples(expected, dispatched.digest),
        tag + " dispatcher (" + std::string(aw::backend_label(d.ran)) + ") == brute force");
  check(!d.applicable[static_cast<std::size_t>(aw::Backend::QueueDense)],
        tag + " dispatcher: QUEUE_DENSE inapplicable without tables");
}

void test_not_applicable() {
  Synthetic root = make_root(33U, 0x77U, 200U);  // W = 27 < 13 + 16
  rg::RootSink sink = make_sink(root);
  check(aw::run_arwlf(*root.data, root.index, root.keys, aw::Config{}, sink, nullptr) ==
            aw::Status::NotApplicable,
        "W < 29: NotApplicable");
}

// ---------------------------------------------------------------------------
// Authentic anchor.
// ---------------------------------------------------------------------------
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
      (std::filesystem::temp_directory_path() / "ter_arwlf_test_anchor.snap").string();
  const rs::CaptureOutcome outcome = rs::capture_to_file(instance, config, attempt, path, 4096U, "test");
  check(outcome.written, "anchor snapshot written");
  const auto workload = rg::load_snapshot_workload(path, true);
  std::filesystem::remove(path);
  if (workload->status != rg::WorkloadStatus::Completed) {
    check(false, "anchor workload loads");
    return;
  }
  const a64::FamilyKeys keys = a64::build_family_keys(*workload);
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

  aw::Config k32;
  k32.check_bits = 13U;  // quotient bits [0, 32)
  aw::Telemetry k32_t;
  rg::RootSink k32_sink = sink_for();
  check(aw::run_arwlf(data, index, keys, k32, k32_sink, &k32_t) == aw::Status::Completed,
        "anchor ARWLF k=32 completes");
  check(k32_t.projected_hits == k2.digest.full_q && k2.digest.full_q == 4718U,
        "anchor ARWLF k=32 hits == frozen K2 low-32 survivors (4718)");

  aw::Telemetry t;
  aw::Config counted;
  counted.account = true;
  rg::RootSink arwlf = sink_for();
  check(aw::run_arwlf(data, index, keys, counted, arwlf, &t) == aw::Status::Completed,
        "anchor ARWLF completes");
  check(t.avx2 && t.chunks == 4U && t.mid_bits == 28U, "anchor ARWLF: AVX2, 4 chunks, 28-bit mid");
  check(same_triples(wide.digest, arwlf.digest), "anchor ARWLF == K2_WIDE");
  check(arwlf.digest.residue == k2.digest.residue && arwlf.digest.residue_sum == k2.digest.residue_sum &&
            arwlf.digest.structural == k2.digest.structural,
        "anchor ARWLF residue/structural survivors == frozen K2's");

  rg::RadixTables tables;
  tables.allocate();
  rg::RootSink dispatched = sink_for();
  const aw::Dispatch d = aw::dispatch_and_run(data, index, keys, &tables, dispatched);
  check(d.applicable[0] && !d.applicable[1] && d.applicable[2] && d.arwlf_packets_per_pair > 0.5,
        "anchor: QUEUE_DENSE and K2_WIDE are candidates; ARWLF is not (sparse leaves)");
  check(d.ran != aw::Backend::Arwlf, "anchor: dispatcher never runs a non-candidate");
  check(dispatched.digest.residue == k2.digest.residue &&
            dispatched.digest.residue_sum == k2.digest.residue_sum &&
            dispatched.digest.structural == k2.digest.structural,
        "anchor dispatcher (" + std::string(aw::backend_label(d.ran)) +
            ") residue/structural survivors == frozen K2's");
  std::cout << "anchor: frozen K2 fullQ=" << k2.digest.full_q << " residue=" << k2.digest.residue
            << " | ARWLF packets=" << t.packets << " survivors=" << t.filter_survivors
            << " projected(k=" << 19U + t.check_bits << ")=" << t.projected_hits
            << " | packets/pair " << d.arwlf_packets_per_pair
            << " | dispatcher chose " << aw::backend_label(d.chosen) << " (predicted ms: queue "
            << d.predicted_ms[0] << ", arwlf " << d.predicted_ms[1] << ", k2_wide " << d.predicted_ms[2]
            << ")\n";
}

}  // namespace

int main() {
  test_kernel();
  test_backend(69U, 0x1234U, 2000U, 0U);
  test_backend(47U, 0x5555U, 2000U, 0U);
  test_backend(69U, 0x9876U, 600U, 16U);
  test_backend(47U, 0x4242U, 600U, 16U);
  test_backend(69U, 0x3131U, 600U, 1U);
  test_backend(47U, 0x2727U, 600U, 1U);
  test_not_applicable();
  test_anchor();
  std::cout << "ter_arwlf_tests: " << g_checks << " checks, " << g_failures << " failures\n";
  return g_failures == 0U ? 0 : 1;
}
