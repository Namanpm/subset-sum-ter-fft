// TER^2 root-queue generalization experiment: focused correctness tests.
//
// Truth here never uses the machinery under test.  Workload properties are
// checked from the raw postings; the exact output of every backend is checked
// against a brute-force triple enumeration over raw postings (no bitmaps, no
// groups, no radix tables), re-hashed with the documented digest rule, and the
// backends are checked against each other and the oracle.
//
// Coverage: generator determinism and seed sensitivity; residue range, family
// sizes and sort order; the profile emulator's size scaling and multiplicity;
// an adversarial hand-built workload with wraparound, kappa = 1 microproblems,
// duplicate residues, empty families, more than 1024 groups in one family and
// EntryIds above 65535 (the widths at which the previous harness's keys would
// alias); and oracle == K2 == QUEUE_DENSE == LANE_SKIP on several small
// uniform and profile workloads.

#include "ter_microreport8_kernels.hpp"
#include "ter_radix13_packed.hpp"
#include "ter_root_generalization_workload.hpp"
#include "ter_root_queue_kernels.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

namespace rg = ter::root_generalization;
namespace mr8 = ter::microreport8;
namespace pk = ter::radix13_packed;
using ter::EntryId;

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
// Brute force over raw postings: every (q, x, y) posting triple of every
// microproblem with (q + x + kappa) mod 2^19 == y, hashed with the documented
// triple rule.  Quadratic-times-linear; tiny workloads only.
// ---------------------------------------------------------------------------
struct Brute {
  std::uint64_t pairs = 0U;
  std::uint64_t triples = 0U;
  std::uint64_t triple_sum = 0U;
  std::uint64_t triple_xor = 0U;
  std::uint64_t triple_sum2 = 0U;
};

[[nodiscard]] Brute brute_force(const mr8::Dataset& data) {
  Brute brute;
  for (std::uint32_t s = 0U; s < rg::kFamilies; ++s) {
    for (std::uint32_t t = 0U; t < rg::kFamilies; ++t) {
      const std::uint32_t u = (s + t) & (rg::kFamilies - 1U);
      const std::uint32_t kappa = (s + t) >> mr8::kSplitBits;
      for (const mr8::Posting& q : data.q_family[s]) {
        for (const mr8::Posting& x : data.x_family[t]) {
          const std::uint64_t wide = static_cast<std::uint64_t>(q.value) + x.value + kappa;
          const auto value = static_cast<std::uint32_t>(wide % mr8::kResidualUniverse);
          bool pair = false;
          for (const mr8::Posting& y : data.y_family[u]) {
            if (y.value != value) {
              continue;
            }
            pair = true;
            ++brute.triples;
            const std::uint64_t first =
                rg::mix64(((static_cast<std::uint64_t>(q.id) << 32U) | x.id) ^ rg::mix64(y.id));
            const std::uint64_t second =
                rg::mix64(((static_cast<std::uint64_t>(y.id) << 32U) | q.id) ^
                          rg::mix64(x.id + 0x632BE59BD9B4E019ULL));
            brute.triple_sum += first;
            brute.triple_xor ^= first;
            brute.triple_sum2 += second;
          }
          brute.pairs += pair ? 1U : 0U;
        }
      }
    }
  }
  return brute;
}

struct Outputs {
  rg::OutputDigest digest;
  std::vector<std::uint64_t> keys;
};

enum class Backend { Oracle, K2, Queue, LaneSkip };

[[nodiscard]] Outputs run(const mr8::Dataset& data, const rg::RootIndex& index,
                          rg::RadixTables& tables, const Backend backend) {
  Outputs outputs;
  rg::RootSink sink;
  sink.data = &data;
  sink.index = &index;
  sink.depth = rg::SinkDepth::Full;
  sink.pair_keys = &outputs.keys;
  sink.pair_key_limit = 1U << 24U;
  sink.reset();
  switch (backend) {
    case Backend::Oracle:
      rg::run_oracle(data, index, sink);
      break;
    case Backend::K2:
      rg::run_k2(data, sink);
      break;
    case Backend::Queue:
      rg::run_radix(data, index, tables, rg::RadixRoute::QueueDense, rg::RadixStop::Full, sink);
      break;
    case Backend::LaneSkip:
      rg::run_radix(data, index, tables, rg::RadixRoute::LaneSkip, rg::RadixStop::Full, sink);
      break;
  }
  outputs.digest = sink.digest;
  std::sort(outputs.keys.begin(), outputs.keys.end());
  return outputs;
}

void check_backends(const mr8::Dataset& data, const std::string& what, const bool with_brute) {
  rg::RootIndex index;
  rg::build_root_index(data, index);
  rg::RadixTables tables;
  tables.allocate();
  const Outputs oracle = run(data, index, tables, Backend::Oracle);
  check(oracle.digest.residual_mismatches == 0U && oracle.digest.directory_misses == 0U,
        what + " [oracle clean]");
  if (with_brute) {
    const Brute brute = brute_force(data);
    check(oracle.digest.pair_hits == brute.pairs, what + " [oracle pairs == brute force]");
    check(oracle.digest.triples == brute.triples, what + " [oracle triples == brute force]");
    check(oracle.digest.triple_sum == brute.triple_sum &&
              oracle.digest.triple_xor == brute.triple_xor &&
              oracle.digest.triple_sum2 == brute.triple_sum2,
          what + " [oracle triple multiset == brute force]");
  }
  static const char* const kNames[] = {"k2", "queue_dense", "lane_skip"};
  const Backend backends[] = {Backend::K2, Backend::Queue, Backend::LaneSkip};
  for (std::size_t which = 0U; which < 3U; ++which) {
    const Outputs other = run(data, index, tables, backends[which]);
    const std::string tag = what + " [" + kNames[which] + "]";
    check(tables.built || which == 0U, tag + " radix tables built");
    check(other.digest.residual_mismatches == 0U && other.digest.directory_misses == 0U,
          tag + " clean");
    check(other.digest.pair_hits == oracle.digest.pair_hits, tag + " pair hits");
    check(other.digest.triples == oracle.digest.triples, tag + " triples");
    check(other.digest.pair_sum == oracle.digest.pair_sum, tag + " pair multiset");
    check(other.digest.triple_sum == oracle.digest.triple_sum &&
              other.digest.triple_xor == oracle.digest.triple_xor &&
              other.digest.triple_sum2 == oracle.digest.triple_sum2,
          tag + " triple multiset");
    check(other.keys == oracle.keys, tag + " sorted pair keys");
    if (backends[which] != Backend::K2) {
      check(other.digest.group_emissions == oracle.digest.group_emissions &&
                other.digest.group_sum == oracle.digest.group_sum,
            tag + " residual groups");
    }
  }
}

// ---------------------------------------------------------------------------

void test_generators() {
  const auto first = rg::generate_uniform(0.25, 0x1001U);
  const auto again = rg::generate_uniform(0.25, 0x1001U);
  const auto other = rg::generate_uniform(0.25, 0x2003U);
  const auto denser = rg::generate_uniform(0.5, 0x1001U);
  check(first->status == rg::WorkloadStatus::Completed, "uniform generation completes");
  check(first->fingerprint == again->fingerprint, "uniform generation is deterministic");
  check(first->fingerprint != other->fingerprint, "uniform generation depends on the seed");
  check(first->fingerprint != denser->fingerprint, "uniform generation depends on the scale");
  check(first->a_size == static_cast<std::uint64_t>(std::llround(0.25 * 19109.0)),
        "uniform |A| is the declared multiple of the reference");
  check(first->b_size == static_cast<std::uint64_t>(std::llround(0.25 * 32496.0)),
        "uniform |B| is the declared multiple of the reference");
  check(first->c_size == static_cast<std::uint64_t>(std::llround(0.25 * 33505.0)),
        "uniform |C| is the declared multiple of the reference");
  check(first->pair_universe == first->a_size * first->b_size, "P0 = |A||B|");
  check(!first->authority.ok, "synthetic workloads carry no authority");
  bool ranged = true;
  bool sorted = true;
  for (const auto* lists : {&first->data->q_family, &first->data->x_family, &first->data->y_family}) {
    for (const auto& family : *lists) {
      for (std::size_t at = 0U; at < family.size(); ++at) {
        ranged = ranged && family[at].value < mr8::kResidualUniverse;
        sorted = sorted && (at == 0U || family[at - 1U].value <= family[at].value);
      }
    }
  }
  check(ranged, "uniform residues lie in [0, 2^19)");
  check(sorted, "uniform families are ascending");
  check(std::string(rg::family_label(first->family)) == "SYNTHETIC_UNIFORM", "uniform label");

  // Profile: measured from a workload with planted multiplicity.
  mr8::Dataset source;
  for (std::uint32_t family = 0U; family < rg::kFamilies; ++family) {
    for (std::uint32_t value = 0U; value < 40U + family; ++value) {
      const std::uint32_t residue = (value * 7919U + family * 131U) & mr8::kResidualMask;
      source.q_family[family].push_back(mr8::Posting{residue, 0U});
      source.x_family[family].push_back(mr8::Posting{residue, 0U});
      source.x_family[family].push_back(mr8::Posting{residue, 0U});  // multiplicity 2
      source.y_family[family].push_back(mr8::Posting{residue ^ 0x40000U, 0U});
    }
    for (auto* list : {&source.q_family[family], &source.x_family[family], &source.y_family[family]}) {
      std::sort(list->begin(), list->end(),
                [](const mr8::Posting& l, const mr8::Posting& r) { return l.value < r.value; });
    }
  }
  const rg::ResidualProfile profile = rg::measure_profile(source, 0U, 0U);
  check(profile.ok, "profile measured");
  check(profile.family_sizes[1][5] == 2U * (40U + 5U), "profile records per-family sizes");
  check(profile.multiplicity[1][2] > 0U && profile.multiplicity[1][1] == 0U,
        "profile records multiplicity");
  const auto emulated = rg::generate_profile_emulation(profile, 2.0, 0x1001U);
  const auto emulated_again = rg::generate_profile_emulation(profile, 2.0, 0x1001U);
  check(emulated->status == rg::WorkloadStatus::Completed, "profile emulation completes");
  check(emulated->fingerprint == emulated_again->fingerprint, "profile emulation is deterministic");
  check(emulated->data->x_family[5].size() == 4U * (40U + 5U), "profile emulation scales sizes");
  bool paired = true;
  for (const auto& family : emulated->data->x_family) {
    for (std::size_t at = 0U; at + 1U < family.size(); at += 2U) {
      paired = paired && family[at].value == family[at + 1U].value;
    }
  }
  check(paired, "profile emulation reproduces a multiplicity-2 profile");
  check(std::string(rg::family_label(emulated->family)) == "RESIDUAL_PROFILE_EMULATION",
        "profile label is never authentic");
}

void test_adversarial() {
  // Hand-built: wraparound residues, kappa = 1 microproblems, duplicates, empty
  // families, one family with > 1024 distinct values, EntryIds above 65535.
  auto workload = rg::generate_uniform(0.0, 0x77U);  // empty families, finished view
  mr8::Dataset& data = *workload->data;
  EntryId next_q = 70000U;
  EntryId next_x = 140000U;
  EntryId next_y = 210000U;
  const auto push = [](std::vector<mr8::Posting>& list, const std::uint32_t value, EntryId& id) {
    list.push_back(mr8::Posting{value & mr8::kResidualMask, id++});
  };
  // Microproblem (s=0, t=0, u=0, kappa=0): a wide X family and many witnesses.
  for (std::uint32_t index = 0U; index < 1500U; ++index) {
    push(data.x_family[0], index * 349U + 11U, next_x);
  }
  for (std::uint32_t index = 0U; index < 40U; ++index) {
    push(data.q_family[0], index * 104729U + 3U, next_q);
  }
  push(data.q_family[0], 3U, next_q);  // duplicate residue
  for (std::uint32_t index = 0U; index < 1200U; ++index) {
    // y = x + q for a spread of (x, q), including wraparound past 2^19.
    const std::uint32_t x = (index * 7U % 1500U) * 349U + 11U;
    const std::uint32_t q = (index % 40U) * 104729U + 3U;
    push(data.y_family[0], x + q, next_y);
  }
  push(data.y_family[0], 11U + 3U, next_y);  // duplicate Y posting
  // Microproblem (s=40, t=30): kappa = 1, u = 6.
  for (std::uint32_t index = 0U; index < 30U; ++index) {
    push(data.q_family[40], mr8::kResidualMask - index, next_q);  // near the top
    push(data.x_family[30], index * 3U, next_x);
    push(data.y_family[6], (mr8::kResidualMask - index) + index * 3U + 1U, next_y);  // wraps
  }
  for (auto* lists : {&data.q_family, &data.x_family, &data.y_family}) {
    for (auto& family : *lists) {
      std::sort(family.begin(), family.end(), [](const mr8::Posting& l, const mr8::Posting& r) {
        return l.value != r.value ? l.value < r.value : l.id < r.id;
      });
    }
  }
  for (std::uint32_t family = 0U; family < rg::kFamilies; ++family) {
    mr8::build_target(data.y_family[family], data.targets[family]);
    data.view.exact[family] = data.targets[family].exact.data();
    data.view.coarse[family] = data.targets[family].coarse.data();
    data.view.sig07[family] = data.targets[family].sig07.data();
    data.view.sig815[family] = data.targets[family].sig815.data();
  }
  rg::RootIndex index;
  rg::build_root_index(data, index);
  check(index.x_groups[0].size() > 1024U, "adversarial: a family with more than 1024 groups");
  check_backends(data, "adversarial hand-built", /*with_brute=*/true);
  const Brute brute = brute_force(data);
  check(brute.pairs > 1000U && brute.triples > brute.pairs,
        "adversarial workload has many witnesses and Y multiplicity");
}

void test_small_workloads() {
  // Brute force is posting-cubic, so only the smallest scale runs it; every
  // workload is checked backend-against-oracle on sorted pair keys.
  check_backends(*rg::generate_uniform(0.02, 0x1001U)->data, "uniform x0.02", true);
  check_backends(*rg::generate_uniform(0.10, 0x2003U)->data, "uniform x0.10", false);
  check_backends(*rg::generate_uniform(0.25, 0x3007U)->data, "uniform x0.25", false);
  mr8::Dataset profile_source = *rg::generate_uniform(0.25, 0x4009U)->data;
  const rg::ResidualProfile profile = rg::measure_profile(profile_source, 0U, 0U);
  check_backends(*rg::generate_profile_emulation(profile, 0.5, 0x500BU)->data, "profile x0.50",
                 false);
}

}  // namespace

int main() {
  const mr8::IsaReport isa = mr8::detect_isa();
  const bool executable = isa.usable && isa.bmi2 && mr8::avx2::translation_unit_is_avx2() &&
                          ter::radix::avx2::translation_unit_is_avx2() &&
                          pk::bmi2::translation_unit_is_bmi2();
  std::cout << "# ter_root_queue_generalization_tests -- backends executable: "
            << (executable ? "yes" : "NO") << '\n';
  if (!executable) {
    std::cout << "RESULT: FAIL (AVX2/BMI2 backends not executable; nothing was verified)\n";
    return EXIT_FAILURE;
  }
  test_generators();
  test_adversarial();
  test_small_workloads();
  std::cout << "checks=" << g_checks << " failures=" << g_failures << '\n';
  if (g_failures != 0U) {
    std::cout << "RESULT: FAIL\n";
    return EXIT_FAILURE;
  }
  std::cout << "RESULT: PASS\n";
  return EXIT_SUCCESS;
}
