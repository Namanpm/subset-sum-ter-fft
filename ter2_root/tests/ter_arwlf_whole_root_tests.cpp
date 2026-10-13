// Whole-root runner tests on the authentic n=72 anchor root (3.7 MB, every
// backend runs the whole root in seconds).
//
// Usage: ter_arwlf_whole_root_tests <n=72 root snapshot>
// Exit 77 (CTest SKIPPED) when the snapshot is absent.
//
// Checks: the 64 family selections partition the 4096 microproblems; with one
// planted output, frozen K2 over the whole root reports the authentic outputs
// plus exactly the planted one; K2 and K2_WIDE chunked per family report
// exactly what one whole-root call reports (witness lists and counts); ARWLF
// and DISPATCH over the whole root equal K2; the workload is restored.

#include "ter_arwlf_campaign.hpp"
#include "ter_arwlf_whole_root.hpp"
#include "ter_root_generalization_workload.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

namespace ac = ter::arwlf_campaign;
namespace wr = ter::arwlf_whole_root;
namespace a64 = ter::a64;
namespace aw = ter::arwlf;
namespace rg = ter::root_generalization;

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

struct Union {
  std::vector<wr::Witness> witnesses;
  std::uint64_t residue = 0U;
  std::uint64_t structural = 0U;
  std::uint64_t pair_hits = 0U;  // nonzero in every family: a skipped chunk changes it
  std::uint64_t full_q = 0U;
};

// The runner's chunk loop: one call per family u, ascending.
Union chunked(const wr::Backend backend, const rg::Workload& w, rg::RootIndex& index, const a64::FamilyKeys& keys,
              rg::RadixTables& tables) {
  Union out;
  for (std::uint32_t u = 0U; u < rg::kFamilies; ++u) {
    wr::select_family(index, u);
    const wr::Pass pass = wr::run_selection(backend, *w.data, index, keys, tables, w.authority);
    out.witnesses.insert(out.witnesses.end(), pass.witnesses.begin(), pass.witnesses.end());
    out.residue += pass.digest.residue;
    out.structural += pass.digest.structural;
    out.pair_hits += pass.digest.pair_hits;
    out.full_q += pass.digest.full_q;
  }
  std::sort(out.witnesses.begin(), out.witnesses.end());
  return out;
}

void test_partition() {
  rg::RootIndex index;
  std::vector<std::uint32_t> hits(ac::kCells, 0U);
  bool family_ok = true;
  for (std::uint32_t u = 0U; u < rg::kFamilies; ++u) {
    wr::select_family(index, u);
    std::uint32_t selected = 0U;
    for (std::uint32_t cell = 0U; cell < ac::kCells; ++cell) {
      if (index.selected[cell] != 0U) {
        ++selected;
        ++hits[cell];
        family_ok = family_ok && ((cell / rg::kFamilies + cell % rg::kFamilies) & (rg::kFamilies - 1U)) == u;
      }
    }
    family_ok = family_ok && selected == rg::kFamilies && index.selected_count == rg::kFamilies;
  }
  check(family_ok, "select_family(u) selects exactly the 64 cells with (s + t) mod 64 == u");
  check(std::all_of(hits.begin(), hits.end(), [](const std::uint32_t h) { return h == 1U; }),
        "the 64 family selections partition the 4096 microproblems");
  wr::select_all(index);
  check(index.selected_count == ac::kCells &&
            std::all_of(index.selected.begin(), index.selected.end(), [](const auto v) { return v != 0U; }),
        "select_all selects every microproblem");
  for (const wr::Backend b : {wr::Backend::K2, wr::Backend::K2Wide, wr::Backend::Arwlf, wr::Backend::Dispatch}) {
    check(wr::parse_backend(wr::label(b)) == b, std::string("backend label round trip: ") + wr::label(b));
  }
  check(!wr::parse_backend("queue_dense").has_value() && !wr::parse_backend("").has_value(),
        "unknown backends are refused");
  check(wr::family_chunked(wr::Backend::K2) && wr::family_chunked(wr::Backend::K2Wide) &&
            !wr::family_chunked(wr::Backend::Arwlf) && !wr::family_chunked(wr::Backend::Dispatch),
        "only K2 and K2_WIDE are chunked");
}

void test_whole_root(rg::Workload& w) {
  rg::RootIndex index;
  rg::build_root_index(*w.data, index);
  rg::RadixTables tables;
  tables.allocate();
  std::uint32_t anchor = ac::kCells;
  rg::select_microproblems(index, 1U);
  for (std::uint32_t cell = 0U; cell < ac::kCells; ++cell) {
    anchor = index.selected[cell] != 0U ? cell : anchor;
  }

  // Authentic whole-root outputs, before planting.
  a64::FamilyKeys keys = a64::build_family_keys(w);
  check(keys.ok, "authentic family keys");
  wr::select_all(index);
  const wr::Pass authentic = wr::run_selection(wr::Backend::K2, *w.data, index, keys, tables, w.authority);
  std::cout << "# n=" << w.n << " anchor cell " << anchor << ", authentic whole-root outputs "
            << authentic.witnesses.size() << '\n';

  {
    const ac::PlantedTargets planted(w, anchor, ac::Plant{ac::PlantKind::Positive, 1U, 0xB1A57001ULL});
    check(planted.expected().size() == 1U, "one planted output");
    keys = a64::build_family_keys(w);
    rg::build_root_index(*w.data, index);
    wr::select_all(index);
    const wr::Pass k2 = wr::run_selection(wr::Backend::K2, *w.data, index, keys, tables, w.authority);
    std::vector<wr::Witness> expected = authentic.witnesses;
    expected.push_back(planted.expected()[0]);
    std::sort(expected.begin(), expected.end());
    check(k2.witnesses == expected, "K2 whole root == authentic outputs + the planted one");
    check(k2.digest.residue == expected.size() && k2.digest.structural == authentic.digest.structural + 1U,
          "K2 whole-root residue and structural counts");
    check(k2.ms > 0.0, "the pass is timed");

    for (const wr::Backend b : {wr::Backend::K2, wr::Backend::K2Wide}) {
      const Union u = chunked(b, w, index, keys, tables);
      check(u.witnesses == k2.witnesses, std::string(wr::label(b)) + " per-family chunks == one whole-root K2 call");
      check(u.residue == k2.digest.residue && u.structural == k2.digest.structural,
            std::string(wr::label(b)) + " chunk counts sum to the whole-root counts");
      if (b == wr::Backend::K2) {
        check(k2.digest.pair_hits > 0U && u.pair_hits == k2.digest.pair_hits && u.full_q == k2.digest.full_q,
              "K2 per-family chunk pair-hit and full-q counts sum to the whole-root call's");
      }
    }
    // The campaign's gate_wide: K2_WIDE and ARWLF agree on every full-key triple
    // (K2 itself keeps no full-key triples, so a mislabelled call fails here).
    wr::select_all(index);
    const wr::Pass wide = wr::run_selection(wr::Backend::K2Wide, *w.data, index, keys, tables, w.authority);
    const wr::Pass arwlf = wr::run_selection(wr::Backend::Arwlf, *w.data, index, keys, tables, w.authority);
    check(wide.digest.triples > 0U && wide.digest.triples == arwlf.digest.triples &&
              wide.digest.triple_sum == arwlf.digest.triple_sum && wide.digest.triple_xor == arwlf.digest.triple_xor,
          "K2_WIDE and ARWLF agree on every full-key triple (gate_wide)");
    for (const wr::Backend b : {wr::Backend::K2Wide, wr::Backend::Arwlf, wr::Backend::Dispatch}) {
      wr::select_all(index);
      const wr::Pass pass = wr::run_selection(b, *w.data, index, keys, tables, w.authority);
      check(pass.witnesses == k2.witnesses && pass.digest.structural == k2.digest.structural,
            std::string(wr::label(b)) + " whole root == K2");
      if (b == wr::Backend::Arwlf) {
        check(pass.arwlf_status == aw::Status::Completed, "ARWLF completes on the whole root");
      }
      if (b == wr::Backend::Dispatch) {
        std::cout << "# DISPATCH ran " << aw::backend_label(pass.dispatch.ran) << '\n';
        check(index.selected_count == ac::kCells, "DISPATCH restores the whole-root selection");
      }
    }
  }
  keys = a64::build_family_keys(w);
  rg::build_root_index(*w.data, index);
  wr::select_all(index);
  const wr::Pass after = wr::run_selection(wr::Backend::K2, *w.data, index, keys, tables, w.authority);
  check(after.witnesses == authentic.witnesses, "workload restored: authentic outputs again");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: ter_arwlf_whole_root_tests <n=72 root snapshot>\n";
    return 2;
  }
  test_partition();
  if (!std::filesystem::exists(argv[1])) {
    std::cout << "SKIPPED: snapshot " << argv[1] << " not present\n";
    return 77;
  }
  auto workload = rg::load_snapshot_workload(argv[1], /*with_authority=*/true);
  if (workload->status != rg::WorkloadStatus::Completed || !workload->authority.ok) {
    std::cerr << "FAIL: snapshot did not load as an authentic root with authority\n";
    return 1;
  }
  test_whole_root(*workload);
  std::cout << "ter_arwlf_whole_root_tests: " << g_checks << " checks, " << g_failures << " failures\n";
  return g_failures == 0U ? 0 : 1;
}
