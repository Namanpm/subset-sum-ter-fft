// Planted-target tests on an authentic root (positive-output experiment).
//
// Usage: ter_arwlf_planted_tests <root snapshot, n=108 or n=72>
// n=108 (Q=69) is the experiment's root and the only one that exercises the
// carry into the high key bits.  The n=72 anchor (Q <= 64, 3.7 MB) runs the
// same checks small enough for AddressSanitizer.
// Exit 77 (CTest SKIPPED) when the snapshot is absent: the n=108 roots are
// ~0.8 GB and not versioned (see results/arwlf/regime-20261001/snapshots.json).
//
// Authorities, independent of PlantedTargets' construction code:
//   - full Q-bit key identity q_a + q_b == d (mod 2^Q) by the synthetic
//     oracle's own two-word addition;
//   - low-32 quotient sum == tau;
//   - original-residue sum == root target (mod 2^n) by this file's own word
//     arithmetic;
//   - parent digits in {-1, 0, +1} with the expected counts, recounted here;
//   - every backend (frozen K2, K2_WIDE, ARWLF, DISPATCH) reports exactly the
//     planted set as final witnesses, and decoys produce none;
//   - destruction restores the workload bit for bit.

#include "ter_arwlf_campaign.hpp"
#include "ter_synthetic_root.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace a64 = ter::a64;
namespace aw = ter::arwlf;
namespace ac = ter::arwlf_campaign;
namespace mr8 = ter::microreport8;
namespace rg = ter::root_generalization;
namespace tt = ter::translated_tag;
namespace sr = ter::synthetic_root;

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

// Order-sensitive digest of everything a planting may touch.
std::uint64_t state_digest(const rg::Workload& w, const std::uint32_t u) {
  std::uint64_t h = 0x5EEDU;
  const auto mix = [&](const std::uint64_t v) { h = rg::mix64(h ^ v); };
  mix(rg::fingerprint(*w.data));
  for (std::size_t list = 0U; list < 3U; ++list) {
    mix(w.key_lo[list].size());
    mix(w.authority.quotient[list].size());
    mix(w.authority.residue[list].size());
    mix(w.authority.digits[list].size());
    mix(w.authority.list_sizes[list]);
  }
  for (const std::uint64_t k : w.key_lo[2]) {
    mix(k);
  }
  for (const std::uint8_t k : w.key_hi[2]) {
    mix(k);
  }
  for (const std::uint32_t q : w.authority.quotient[2]) {
    mix(q);
  }
  for (const std::uint32_t word : w.data->targets[u].exact) {
    mix(word);
  }
  mix(w.data->view.exact[u] == w.data->targets[u].exact.data() ? 1U : 0U);
  mix(w.data->view.coarse[u] == w.data->targets[u].coarse.data() ? 1U : 0U);
  return h;
}

// res_a + res_b + res_c (mod 2^n) == root, in 64-bit limbs of the 32-bit words.
bool residue_closes(const tt::AuthorityTables& t, const ter::EntryId a, const ter::EntryId b, const ter::EntryId c) {
  const std::uint32_t words = t.residue_words;
  std::uint64_t carry = 0U;
  for (std::uint32_t w = 0U; w < words; ++w) {
    const std::uint64_t sum = carry + t.residue[0][static_cast<std::size_t>(a) * words + w] +
                              t.residue[1][static_cast<std::size_t>(b) * words + w] +
                              t.residue[2][static_cast<std::size_t>(c) * words + w];
    carry = sum >> 32U;
    std::uint64_t word = sum & 0xFFFFFFFFULL;
    if (w + 1U == words && t.n % 32U != 0U) {
      word &= (std::uint64_t{1} << (t.n % 32U)) - 1U;
    }
    if (word != t.root_target[w]) {
      return false;
    }
  }
  return true;
}

bool parent_valid(const tt::AuthorityTables& t, const ter::EntryId a, const ter::EntryId b, const ter::EntryId c) {
  ter::DigitCounts counts;
  for (std::uint32_t i = 0U; i < t.n; ++i) {
    const int p = t.digits[0][static_cast<std::size_t>(a) * t.n + i] + t.digits[1][static_cast<std::size_t>(b) * t.n + i] +
                  t.digits[2][static_cast<std::size_t>(c) * t.n + i];
    if (p < -1 || p > 1) {
      return false;
    }
    (p > 0 ? counts.positive : p < 0 ? counts.negative : counts.zero) += 1U;
  }
  return counts.positive == t.expected_parent.positive && counts.negative == t.expected_parent.negative &&
         counts.zero == t.expected_parent.zero;
}

struct Outputs {
  std::array<std::vector<ac::Witness>, 4> witnesses;  // K2, K2_WIDE, ARWLF, DISPATCH (sorted)
  std::array<std::uint64_t, 4> structural{};
  std::array<std::uint64_t, 4> residue{};
};

Outputs run_backends(rg::Workload& w, const std::uint32_t cell) {
  mr8::Dataset& data = *w.data;
  const a64::FamilyKeys keys = a64::build_family_keys(w);
  check(keys.ok, "family keys rebuild");
  rg::RootIndex index;
  rg::build_root_index(data, index);
  index.selected.fill(0U);
  index.selected[cell] = 1U;
  index.selected_count = 1U;
  rg::RadixTables tables;
  tables.allocate();
  Outputs out;
  for (std::size_t b = 0U; b < 4U; ++b) {
    rg::RootSink sink;
    sink.data = &data;
    sink.index = &index;
    sink.authority = &w.authority;
    sink.witnesses = &out.witnesses[b];
    sink.reset();
    if (b == 0U) {
      rg::run_k2(data, sink);
    } else if (b == 1U) {
      a64::run_k2_wide(data, index, keys, sink, nullptr, false);
    } else if (b == 2U) {
      check(aw::run_arwlf(data, index, keys, aw::Config{}, sink, nullptr) == aw::Status::Completed, "ARWLF completes");
    } else {
      static_cast<void>(aw::dispatch_and_run(data, index, keys, &tables, sink));
    }
    std::sort(out.witnesses[b].begin(), out.witnesses[b].end());
    out.structural[b] = sink.digest.structural;
    out.residue[b] = sink.digest.residue;
  }
  return out;
}

std::vector<ac::Witness> planted_part(const std::vector<ac::Witness>& all, const ter::EntryId first) {
  std::vector<ac::Witness> out;
  for (const ac::Witness& w : all) {
    if (w[2] >= first) {
      out.push_back(w);
    }
  }
  return out;
}

void test_planted(rg::Workload& w) {
  rg::RootIndex probe;
  rg::build_root_index(*w.data, probe);
  rg::select_microproblems(probe, 1U);  // the campaign's anchor cell
  std::uint32_t cell = 0U;
  for (std::uint32_t c = 0U; c < ac::kCells; ++c) {
    cell = probe.selected[c] != 0U ? c : cell;
  }
  const std::uint32_t u = (cell / 64U + cell % 64U) & 63U;
  const std::uint64_t before = state_digest(w, u);
  const std::size_t y_before = w.data->y_family[u].size();
  const std::uint32_t q_bits = w.full_quotient_bits;
  std::cout << "# n=" << w.n << " Q=" << q_bits << " anchor cell " << cell << " |Y_u|=" << y_before << '\n';

  // Authentic baseline: the anchor's own final outputs (expected none).
  const Outputs authentic = run_backends(w, cell);
  for (std::size_t b = 1U; b < 4U; ++b) {
    check(authentic.witnesses[b] == authentic.witnesses[0], "authentic: every backend == frozen K2");
  }

  constexpr std::uint32_t kCount = 200U;
  std::vector<ac::Witness> first_expected;
  {
    const ac::PlantedTargets planted(w, cell, ac::Plant{ac::PlantKind::Positive, kCount, 0xB1A57001ULL});
    const ter::EntryId first = planted.first_planted();
    const std::vector<ac::Witness>& expected = planted.expected();
    first_expected = expected;
    check(expected.size() == kCount, "positive: M planted triples");
    check(w.data->y_family[u].size() == y_before + kCount, "positive: Y_u grew by M");
    std::cout << "# positive: " << expected.size() << " planted, " << planted.rejected_pairs()
              << " pairs rejected (no parent digit completion)\n";
    const tt::AuthorityTables& t = w.authority;
    bool keys_ok = true;
    bool quotients_ok = true;
    bool residues_ok = true;
    bool parents_ok = true;
    bool frozen_authority_ok = true;
    for (const ac::Witness& x : expected) {
      const sr::Value qa{w.key_lo[0][x[0]], w.key_hi[0][x[0]]};
      const sr::Value qb{w.key_lo[1][x[1]], w.key_hi[1][x[1]]};
      const sr::Value d = sr::add(qa, qb, q_bits);
      keys_ok = keys_ok && d.lo == w.key_lo[2][x[2]] && d.hi == w.key_hi[2][x[2]];
      quotients_ok = quotients_ok && static_cast<std::uint32_t>(t.quotient[0][x[0]] + t.quotient[1][x[1]] +
                                                                t.quotient[2][x[2]]) == t.tau_low32;
      residues_ok = residues_ok && residue_closes(t, x[0], x[1], x[2]);
      parents_ok = parents_ok && parent_valid(t, x[0], x[1], x[2]);
      frozen_authority_ok = frozen_authority_ok && tt::residue_sum_matches_root(t, x[0], x[1], x[2]) &&
                            tt::structural_parent_valid(t, x[0], x[1], x[2]);
    }
    check(keys_ok, "positive: q_a + q_b == d on all Q bits (independent two-word add)");
    check(quotients_ok, "positive: low-32 quotients sum to tau");
    check(residues_ok, "positive: residues sum to the root target mod 2^n (independent)");
    check(parents_ok, "positive: parent digits valid with expected counts (independent)");
    check(frozen_authority_ok, "positive: frozen authority accepts every planted triple");

    const Outputs out = run_backends(w, cell);
    const char* names[4] = {"K2", "K2_WIDE", "ARWLF", "DISPATCH"};
    for (std::size_t b = 0U; b < 4U; ++b) {
      check(planted_part(out.witnesses[b], first) == expected,
            std::string("positive: ") + names[b] + " reports exactly the planted set");
      check(out.witnesses[b].size() == expected.size() + authentic.witnesses[0].size(),
            std::string("positive: ") + names[b] + " reports nothing else new");
      check(out.structural[b] == expected.size() + authentic.structural[0],
            std::string("positive: ") + names[b] + " structural count == planted + authentic");
    }
  }
  check(state_digest(w, u) == before, "positive: workload restored bit for bit");
  {
    const ac::PlantedTargets again(w, cell, ac::Plant{ac::PlantKind::Positive, kCount, 0xB1A57001ULL});
    check(again.expected() == first_expected, "positive: deterministic for a fixed seed");
  }
  check(state_digest(w, u) == before, "positive (repeat): workload restored");
  {
    const ac::PlantedTargets decoys(w, cell, ac::Plant{ac::PlantKind::Decoy, kCount, 0xB1A57001ULL});
    check(decoys.expected().empty() && w.data->y_family[u].size() == y_before + kCount,
          "decoy: M entries, no expected outputs");
    const Outputs out = run_backends(w, cell);
    for (std::size_t b = 0U; b < 4U; ++b) {
      check(planted_part(out.witnesses[b], decoys.first_planted()).empty() &&
                out.witnesses[b] == authentic.witnesses[0],
            "decoy: no planted output, authentic outputs unchanged");
    }
  }
  check(state_digest(w, u) == before, "decoy: workload restored bit for bit");

  // Fail-closed preconditions.
  rg::Workload empty;
  bool threw = false;
  try {
    const ac::PlantedTargets bad(empty, cell, ac::Plant{ac::PlantKind::Positive, 1U, 1U});
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  check(threw, "a workload without keys or authority is refused");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: ter_arwlf_planted_tests <root snapshot, n=108 or n=72>\n";
    return 2;
  }
  if (!std::filesystem::exists(argv[1])) {
    std::cout << "SKIPPED: snapshot " << argv[1] << " not present (not versioned; see snapshots.json)\n";
    return 77;
  }
  auto workload = rg::load_snapshot_workload(argv[1], /*with_authority=*/true);
  if (workload->status != rg::WorkloadStatus::Completed || !workload->authority.ok ||
      (workload->n != 108U && workload->n != 72U)) {
    std::cerr << "FAIL: snapshot did not load as an authentic n=108 or n=72 root with authority\n";
    return 1;
  }
  test_planted(*workload);
  std::cout << "ter_arwlf_planted_tests: " << g_checks << " checks, " << g_failures << " failures\n";
  return g_failures == 0U ? 0 : 1;
}
