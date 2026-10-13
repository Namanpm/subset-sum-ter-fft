#include "ter_hash_join_root.hpp"
#include "ter_arwlf.hpp"
#include "ter_synthetic_root.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {
namespace hj = ter::hash_join_root;
namespace sr = ter::synthetic_root;
namespace rg = ter::root_generalization;
namespace aw = ter::arwlf;
using Witness = std::array<ter::EntryId, 3>;
std::uint64_t checks = 0U;
std::uint64_t failures = 0U;

void check(const bool condition, const std::string& message) {
  ++checks;
  if (!condition) {
    ++failures;
    if (failures <= 20U) { std::cerr << "FAIL: " << message << '\n'; }
  }
}

// Independent cubic oracle on ORIGINAL whole-Q two-word values: no residual
// split, hash, quotient carry recurrence or backend digest in its decision.
std::vector<Witness> identity_oracle(const sr::Synthetic& root) {
  std::vector<Witness> expected;
  for (std::size_t a = 0U; a < root.lists[0].size(); ++a) {
    for (std::size_t b = 0U; b < root.lists[1].size(); ++b) {
      const auto s = static_cast<std::uint32_t>(root.lists[0][a].lo & 63U);
      const auto t = static_cast<std::uint32_t>(root.lists[1][b].lo & 63U);
      if (!root.index.active(s, t)) { continue; }
      const sr::Value sum = sr::add(root.lists[0][a], root.lists[1][b], root.bits);
      for (std::size_t c = 0U; c < root.lists[2].size(); ++c) {
        const sr::Value y = root.lists[2][c];
        if (sum.lo == y.lo && sum.hi == y.hi) {
          expected.push_back({static_cast<ter::EntryId>(a), static_cast<ter::EntryId>(b),
                              static_cast<ter::EntryId>(c)});
        }
      }
    }
  }
  std::sort(expected.begin(), expected.end());
  return expected;
}

// Pass-through synthetic authority ONLY to collect every raw emitted identity
// through the real frozen sink. Zero residues/digits here are deliberately NOT
// claimed to be authentic TER data. The oracle, not this authority, decides Q.
ter::translated_tag::AuthorityTables collector(const sr::Synthetic& root) {
  ter::translated_tag::AuthorityTables authority;
  authority.ok = true;
  authority.n = root.bits;
  authority.quotient_bits = root.bits;
  authority.residue_words = (root.bits + 31U) / 32U;
  authority.root_target.resize(authority.residue_words, 0U);
  authority.expected_parent = ter::DigitCounts{0U, root.bits, 0U};
  authority.shape_available = true;
  for (std::size_t l = 0U; l < 3U; ++l) {
    const std::size_t n = root.lists[l].size();
    authority.list_sizes[l] = n;
    authority.quotient[l].resize(n, 0U);
    authority.residue[l].resize(n * authority.residue_words, 0U);
    authority.digits[l].resize(n * authority.n, 0);
  }
  return authority;
}

void compare(sr::Synthetic& root, const bool compare_backends) {
  const std::vector<Witness> expected = identity_oracle(root);
  auto authority = collector(root);
  std::vector<Witness> actual;
  actual.reserve(expected.size());
  rg::RootSink sink = sr::make_sink(root);
  sink.authority = &authority;
  sink.witnesses = &actual;
  hj::Telemetry telemetry;
  const auto status = hj::run_hash_join(*root.data, root.index, root.keys, hj::Config{}, sink, &telemetry);
  const std::string tag = "Q=" + std::to_string(root.bits);
  check(status == hj::Status::Completed, tag + " hash join completes");
  std::sort(actual.begin(), actual.end());
  check(actual == expected, tag + " exact identity MULTISET == cubic oracle");
  check(telemetry.full_hits == expected.size() && sink.digest.triples == expected.size(),
        tag + " full-hit count preserves multiplicity");
  std::uint64_t pairs = 0U;
  for (std::uint32_t s = 0U; s < rg::kFamilies; ++s) {
    for (std::uint32_t t = 0U; t < rg::kFamilies; ++t) {
      if (root.index.active(s, t)) {
        pairs += root.data->q_family[s].size() * root.data->x_family[t].size();
      }
    }
  }
  check(telemetry.logical_pairs == pairs, tag + " counts selected logical pair mass even for empty Y");
  if (compare_backends && status == hj::Status::Completed) {
    sink.reset();
    ter::a64::run_k2_wide(*root.data, root.index, root.keys, sink, nullptr, false);
    std::sort(actual.begin(), actual.end());
    check(actual == expected, tag + " K2-WIDE identity multiset agrees");
    if (root.bits >= 35U) {
      sink.reset();
      aw::Config config;
      config.use_avx2 = false;  // oracle gate does not require host AVX2
      check(aw::run_arwlf(*root.data, root.index, root.keys, config, sink, nullptr) == aw::Status::Completed,
            tag + " scalar ARWLF completes");
      std::sort(actual.begin(), actual.end());
      check(actual == expected, tag + " ARWLF identity multiset agrees");
    }
  }
}

void test_boundaries() {
  // Missing carries, unsigned wrap, zero-key sentinel bugs, high-bit false
  // matches and accidental duplicate deduplication all break these fixtures.
  for (const auto bits : {25U, 35U, 47U, 63U, 64U, 65U, 69U}) {
    for (const auto seed : {1ULL, 0x7201ULL, 0x1001ULL}) {
      auto root = sr::make_root(bits, seed, 4U);
      compare(root, bits == 47U || bits == 69U);
      rg::select_microproblems(root.index, 700U);
      compare(root, false);
    }
  }
  // All eight identities must be emitted, even though every VALUE is zero.
  sr::Synthetic duplicates;
  duplicates.bits = 69U;
  for (auto& list : duplicates.lists) { list.assign(2U, sr::Value{}); }
  sr::finish(duplicates);
  check(identity_oracle(duplicates).size() == 8U, "literal 2*2*2 duplicate multiplicity");
  compare(duplicates, true);

  // Concentrated keys force real linear-probing collisions and mixed duplicate
  // runs; every distinct target has a query, so dropping any bucket is visible.
  sr::Synthetic collisions;
  collisions.bits = 47U;
  collisions.lists[0].push_back(sr::Value{});
  for (std::uint64_t i = 0U; i < 128U; ++i) {
    const sr::Value value{i * 64U, 0U};
    collisions.lists[1].push_back(value);
    collisions.lists[2].push_back(value);
    if (i % 7U == 0U) { collisions.lists[2].push_back(value); }
  }
  sr::finish(collisions);
  compare(collisions, true);

  sr::Synthetic empty;
  empty.bits = 69U;
  sr::finish(empty);
  compare(empty, false);
  auto no_targets = sr::make_root(47U, 9U, 2U);
  no_targets.lists[2].clear();
  sr::finish(no_targets);
  compare(no_targets, false);
}

void test_failure_contract() {
  auto root = sr::make_root(69U, 13U, 2U);
  rg::RootSink sink = sr::make_sink(root);
  sink.digest.triples = 123U;
  hj::Telemetry telemetry;
  hj::Config limit;
  limit.memory_limit_bytes = 1U;
  check(hj::run_hash_join(*root.data, root.index, root.keys, limit, sink, &telemetry) == hj::Status::ResourceLimit,
        "preflight reports insufficient auxiliary memory");
  check(sink.digest.triples == 123U, "resource refusal leaves caller output unchanged");
  root.keys.ok = false;
  check(hj::run_hash_join(*root.data, root.index, root.keys, hj::Config{}, sink) == hj::Status::InvalidInput,
        "invalid keys do not masquerade as empty output");
  root.keys.ok = true;
  root.keys.residual_bits = 64U;
  check(hj::run_hash_join(*root.data, root.index, root.keys, hj::Config{}, sink) == hj::Status::InvalidInput,
        "inconsistent width rejected before shift");
  sr::build_keys(root);
  for (auto& family : root.keys.key[2]) {
    if (!family.empty()) { family.pop_back(); break; }
  }
  check(hj::run_hash_join(*root.data, root.index, root.keys, hj::Config{}, sink) == hj::Status::InvalidInput,
        "misaligned posting/key sizes rejected");
  sr::build_keys(root);
  for (auto& family : root.keys.key[0]) {
    if (!family.empty()) { family[0] = std::numeric_limits<std::uint64_t>::max(); break; }
  }
  check(hj::run_hash_join(*root.data, root.index, root.keys, hj::Config{}, sink) == hj::Status::InvalidInput,
        "out-of-domain residual rejected");
  check(sink.digest.triples == 123U, "all input refusals are nonmutating");
  sr::build_keys(root);
  sink.data = nullptr;
  check(hj::run_hash_join(*root.data, root.index, root.keys, hj::Config{}, sink) == hj::Status::InvalidInput,
        "miswired sink rejected");
  sink.data = root.data.get();
  rg::select_microproblems(root.index, 0U);
  limit.memory_limit_bytes = 0U;
  check(hj::run_hash_join(*root.data, root.index, root.keys, limit, sink) == hj::Status::Completed,
        "explicit empty selection needs no memory");
}
}  // namespace

int main() {
  test_boundaries();
  test_failure_contract();
  std::cout << "ter_hash_join_tests: " << checks << " checks, " << failures << " failures\n";
  return failures == 0U ? 0 : 1;
}
