// ARWLF regime campaign: census, stratification, thinning and witness tests.
//
// Authorities: a naive geometry count that walks all 8192 target leaves and
// all 8192 Q leaves (no complement, no family totals); the counted ARWLF run
// (Telemetry leaf_triples / packets / lanes / rows); the whole-value brute
// force of ter_synthetic_root.hpp for thinned roots; brute-force nearest-to-
// median selection; exact integer thresholds for thinning.

#include "ter_arwlf_campaign.hpp"
#include "ter_synthetic_root.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <set>
#include <string>
#include <vector>

namespace {

namespace aw = ter::arwlf;
namespace ac = ter::arwlf_campaign;
namespace tt = ter::translated_tag;
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
// Geometry.
// ---------------------------------------------------------------------------
struct Naive {
  std::uint64_t triples = 0;
  std::uint64_t packets = 0;
  std::uint64_t lanes = 0;
  std::uint64_t rows = 0;
};

// Every target leaf, every Q leaf, straight from the definitions.
Naive naive_geometry(const a64::FamilyKeys& keys, const std::uint32_t s, const std::uint32_t t) {
  const std::uint32_t u = (s + t) & 63U;
  const std::uint32_t kappa = (s + t) >> 6U;
  std::array<std::vector<std::uint32_t>, 3> count;
  const std::array<std::uint32_t, 3> family{s, t, u};
  for (std::size_t list = 0U; list < 3U; ++list) {
    count[list].assign(aw::kLeaves, 0U);
    for (const std::uint64_t key : keys.key[list][family[list]]) {
      ++count[list][key % aw::kLeaves];
    }
  }
  const auto ceil16 = [](const std::uint64_t v) { return (v + 15U) / 16U; };
  Naive n;
  for (std::uint32_t k = 0U; k < aw::kLeaves; ++k) {
    if (count[2][k] == 0U) {
      continue;
    }
    n.rows += keys.key[0][s].size();
    for (std::uint32_t i = 0U; i < aw::kLeaves; ++i) {
      const std::uint64_t nq = count[0][i];
      if (nq == 0U) {
        continue;  // contributes no lane, packet, triple or row
      }
      const std::uint32_t j = (k + 2U * aw::kLeaves - i - kappa) % aw::kLeaves;
      const std::uint64_t nx = count[1][j];
      if (nx != 0U) {
        ++n.triples;
        n.packets += ceil16(nq) * ceil16(nx);
        n.lanes += nq * nx;
      }
      if (nx > 16U) {
        n.rows += nq * (ceil16(nx) - 1U);
      }
    }
  }
  return n;
}

// A root with independent list sizes (list 2 holds d directly).
Synthetic sized_root(const std::uint32_t bits, const std::uint64_t seed, const std::size_t q, const std::size_t x,
                     const std::size_t y) {
  Synthetic root;
  root.bits = bits;
  Rng rng{seed};
  const std::array<std::size_t, 3> sizes{q, x, y};
  for (std::size_t list = 0U; list < 3U; ++list) {
    for (std::size_t i = 0U; i < sizes[list]; ++i) {
      root.lists[list].push_back(mask_value(Value{rng.next(), rng.next()}, bits));
    }
  }
  finish(root);
  return root;
}

// Q only in family 5, X only in family 9, Y spread: microproblem (5, 9) is dense.
Synthetic concentrated_root(const std::uint64_t seed, const std::size_t q, const std::size_t x, const std::size_t y) {
  Synthetic root;
  root.bits = 69U;
  Rng rng{seed};
  const std::array<std::size_t, 3> sizes{q, x, y};
  for (std::size_t list = 0U; list < 3U; ++list) {
    for (std::size_t i = 0U; i < sizes[list]; ++i) {
      Value v = mask_value(Value{rng.next(), rng.next()}, 69U);
      if (list < 2U) {
        v.lo = (v.lo & ~std::uint64_t{63}) | (list == 0U ? 5U : 9U);
      }
      root.lists[list].push_back(v);
    }
  }
  finish(root);
  return root;
}

void check_geometry(const Synthetic& root, const std::vector<std::uint32_t>& cells, const std::string& label) {
  const std::vector<ac::Geometry> census = ac::census(root.keys, root.index);
  check(census.size() == ac::kCells, label + ": census covers 4096 cells");
  for (const std::uint32_t cell : cells) {
    const ac::Geometry& g = census[cell];
    const Naive n = naive_geometry(root.keys, cell / 64U, cell % 64U);
    const std::string where = label + " cell " + std::to_string(cell);
    check(g.leaf_triples == n.triples, where + ": triples == naive");
    check(g.packets == n.packets, where + ": packets == naive");
    check(g.lanes == n.lanes, where + ": lanes == naive");
    check(g.rows == n.rows, where + ": rows == naive");
    check(g.logical_pairs == root.keys.key[0][g.s].size() * root.keys.key[1][g.t].size(), where + ": pairs");
    check(g.x_groups == root.index.x_groups[g.t].size() && g.y_groups == root.index.y_groups[g.u].size(),
          where + ": group counts");

    // The counted ARWLF run walks exactly this geometry.
    rg::RootIndex index = root.index;
    index.selected.fill(0U);
    index.selected[cell] = 1U;
    index.selected_count = 1U;
    rg::RootSink sink = make_sink(root);
    sink.index = &index;
    aw::Config config;
    config.account = true;
    aw::Telemetry t;
    const aw::Status status = aw::run_arwlf(*root.data, index, root.keys, config, sink, &t);
    check(status == aw::Status::Completed, where + ": counted run completes");
    check(t.leaf_triples == g.leaf_triples && t.packets == g.packets && t.lanes == g.lanes &&
              t.logical_pairs == g.logical_pairs,
          where + ": census == counted ARWLF telemetry");
    check(!t.avx2 || t.rows == g.rows, where + ": census rows == AVX2 rows");
  }
  // selected_geometry sums the selected cells.
  rg::RootIndex index = root.index;
  index.selected.fill(0U);
  index.selected_count = 0U;
  ac::Geometry expect;
  for (const std::uint32_t cell : cells) {
    index.selected[cell] = 1U;
    ++index.selected_count;
    expect.lanes += census[cell].lanes;
    expect.packets += census[cell].packets;
    expect.rows += census[cell].rows;
    expect.logical_pairs += census[cell].logical_pairs;
  }
  const ac::Geometry sum = ac::selected_geometry(census, index);
  check(sum.cell == ac::kCells && sum.lanes == expect.lanes && sum.packets == expect.packets &&
            sum.rows == expect.rows && sum.logical_pairs == expect.logical_pairs,
        label + ": selected_geometry sums the selection");
}

void test_geometry() {
  // Sparse everything: Y leaves mostly empty (direct sum over nonempty K).
  {
    const Synthetic root = make_root(69U, 0x1234U, 2000U);
    std::vector<std::uint32_t> cells;
    for (std::uint32_t cell = 0U; cell < ac::kCells; cell += 61U) {
      cells.push_back(cell);
    }
    check_geometry(root, cells, "sparse");
  }
  // Dense leaves with overflow blocks (pooled low bits).
  {
    const Synthetic root = make_root(47U, 0x4242U, 600U, 16U);
    std::vector<std::uint32_t> cells;
    for (std::uint32_t cell = 0U; cell < ac::kCells; ++cell) {
      if (!root.data->q_family[cell / 64U].empty() && !root.data->x_family[cell % 64U].empty()) {
        cells.push_back(cell);
      }
    }
    check(!cells.empty(), "pooled root has work");
    check_geometry(root, cells, "pooled");
  }
  {
    const Synthetic root = make_root(69U, 0x3131U, 600U, 1U);  // one leaf: dozens of overflow blocks
    std::vector<std::uint32_t> cells;
    for (std::uint32_t cell = 0U; cell < ac::kCells; ++cell) {
      if (!root.data->q_family[cell / 64U].empty() && !root.data->x_family[cell % 64U].empty()) {
        cells.push_back(cell);
      }
    }
    check_geometry(root, cells, "single-leaf");
  }
  // Dense Y (> 4096 nonempty leaves per family): the complement path.
  {
    const Synthetic root = sized_root(69U, 0x7777U, 3000U, 3000U, 64U * 9000U);
    std::uint64_t dense = 0U;
    for (std::uint32_t u = 0U; u < 64U; ++u) {
      std::set<std::uint64_t> leaves;
      for (const std::uint64_t key : root.keys.key[2][u]) {
        leaves.insert(key % aw::kLeaves);
      }
      dense += leaves.size() > aw::kLeaves / 2U ? 1U : 0U;
    }
    check(dense == 64U, "dense-Y root takes the complement path in every family");
    check_geometry(root, {0U, 1U, 65U, 640U, 2047U, 4032U, 4095U}, "dense-Y");
  }
}

// ---------------------------------------------------------------------------
// Stratification.
// ---------------------------------------------------------------------------
void test_stratified() {
  std::vector<ac::Geometry> census(ac::kCells);
  Rng rng{0x5757U};
  for (std::uint32_t cell = 0U; cell < ac::kCells; ++cell) {
    ac::Geometry& g = census[cell];
    g.cell = cell;
    g.s = cell / 64U;
    g.t = cell % 64U;
    g.logical_pairs = 1000U + rng.next() % 1000000U;
    g.packets = 1U + rng.next() % 1000U;
    g.lanes = g.packets * (1U + rng.next() % 256U);
  }
  census[17].packets = 0U;  // outside the population
  census[17].lanes = 0U;
  const std::vector<ac::Stratum> picks = ac::select_stratified(census);
  check(picks.size() == 16U, "spread population fills all 16 cells");
  check(std::is_sorted(picks.begin(), picks.end(), [](const ac::Stratum& a, const ac::Stratum& b) {
          return a.cell < b.cell;
        }) && std::adjacent_find(picks.begin(), picks.end(), [](const ac::Stratum& a, const ac::Stratum& b) {
                return a.cell == b.cell;
              }) == picks.end(),
        "picks ascending and distinct");
  const std::vector<ac::Stratum> again = ac::select_stratified(census);
  check(again.size() == picks.size() &&
            std::equal(again.begin(), again.end(), picks.begin(),
                       [](const ac::Stratum& a, const ac::Stratum& b) { return a.cell == b.cell; }),
        "selection is deterministic");

  // Brute-force reference of the documented rule.
  std::vector<double> xs;
  std::vector<double> ys;
  std::vector<std::uint32_t> members;
  for (const ac::Geometry& g : census) {
    if (g.packets > 0U) {
      xs.push_back(std::log2(static_cast<double>(g.logical_pairs)));
      ys.push_back(g.lanes_per_packet());
      members.push_back(g.cell);
    }
  }
  std::vector<double> sx = xs;
  std::vector<double> sy = ys;
  std::sort(sx.begin(), sx.end());
  std::sort(sy.begin(), sy.end());
  const std::array<double, 3> cx{ac::quantile(sx, 0.25), ac::quantile(sx, 0.5), ac::quantile(sx, 0.75)};
  const std::array<double, 3> cy{ac::quantile(sy, 0.25), ac::quantile(sy, 0.5), ac::quantile(sy, 0.75)};
  const auto bin = [](const double v, const std::array<double, 3>& c) {
    std::uint32_t b = 0U;
    for (const double cut : c) {
      b += v >= cut ? 1U : 0U;
    }
    return b;
  };
  bool all_match = true;
  for (const ac::Stratum& pick : picks) {
    std::vector<std::size_t> in;
    for (std::size_t m = 0U; m < members.size(); ++m) {
      if (bin(xs[m], cx) == pick.pair_bin && bin(ys[m], cy) == pick.lane_bin) {
        in.push_back(m);
      }
    }
    std::vector<double> mx;
    std::vector<double> my;
    for (const std::size_t m : in) {
      mx.push_back(xs[m]);
      my.push_back(ys[m]);
    }
    std::sort(mx.begin(), mx.end());
    std::sort(my.begin(), my.end());
    const double cmx = ac::quantile(mx, 0.5);
    const double cmy = ac::quantile(my, 0.5);
    std::uint32_t best = 0U;
    double best_d = std::numeric_limits<double>::infinity();
    for (const std::size_t m : in) {
      const double dx = (xs[m] - cmx) / (cx[2] - cx[0]);
      const double dy = (ys[m] - cmy) / (cy[2] - cy[0]);
      if (dx * dx + dy * dy < best_d) {
        best_d = dx * dx + dy * dy;
        best = members[m];
      }
    }
    all_match = all_match && best == pick.cell && in.size() == pick.members;
  }
  check(all_match, "every pick is its cell's nearest-to-median member (brute force)");

  // Degenerate populations.
  std::vector<ac::Geometry> flat(ac::kCells);
  for (std::uint32_t cell = 0U; cell < ac::kCells; ++cell) {
    flat[cell].cell = cell;
    flat[cell].logical_pairs = 4096U;
    flat[cell].packets = 16U;
    flat[cell].lanes = 1024U;
  }
  const std::vector<ac::Stratum> one = ac::select_stratified(flat);
  check(one.size() == 1U && one[0].cell == 0U && one[0].pair_bin == 3U && one[0].lane_bin == 3U &&
            one[0].members == ac::kCells,
        "identical geometry: one stratum, lowest cell");
  std::vector<ac::Geometry> none(ac::kCells);
  check(ac::select_stratified(none).empty(), "no population: no strata");
}

// ---------------------------------------------------------------------------
// Thinning.
// ---------------------------------------------------------------------------
void test_threshold() {
  check(ac::keep_threshold(0.5) == (std::uint64_t{1} << 63U), "threshold(1/2) == 2^63");
  check(ac::keep_threshold(0.25) == (std::uint64_t{1} << 62U), "threshold(1/4) == 2^62");
  check(ac::keep_threshold(0.0) == 0U && ac::keep_threshold(-1.0) == 0U, "threshold(<=0) == 0");
  check(ac::keep_threshold(std::nan("")) == 0U, "threshold(NaN) == 0");
  const double below_one = std::nextafter(1.0, 0.0);
  check(ac::keep_threshold(below_one) == std::numeric_limits<std::uint64_t>::max() - 2047U,
        "threshold(1 - 2^-53) == 2^64 - 2^11");
  const std::uint64_t half = std::uint64_t{1} << 63U;
  check(ac::keep(half - 1U, 0.5) && !ac::keep(half, 0.5), "keep is strict at the threshold");
  check(ac::keep(std::numeric_limits<std::uint64_t>::max(), 1.0) && !ac::keep(0U, 0.0), "p = 1 keeps all, p = 0 none");
  check(!ac::keep(0U, std::nan("")), "NaN keeps none");

  // Empirical rate within 6 sigma, and nesting across p for one seed.
  constexpr std::uint64_t kDraws = 1U << 20U;
  for (const double p : {0.01, 0.1, 0.3, 0.77}) {
    std::uint64_t kept = 0U;
    for (std::uint64_t id = 0U; id < kDraws; ++id) {
      kept += ac::keep(ac::thinning_hash(0x1001U, 77U, ac::kTagQ, id, 0xA11CE001U), p) ? 1U : 0U;
    }
    const double mean = p * kDraws;
    const double sigma = std::sqrt(mean * (1.0 - p));
    check(std::fabs(static_cast<double>(kept) - mean) < 6.0 * sigma, "empirical keep rate ~ p at p=" + std::to_string(p));
  }
  bool nested = true;
  for (std::uint64_t id = 0U; id < 200000U; ++id) {
    const std::uint64_t h = ac::thinning_hash(0x2003U, 5U, ac::kTagX, id, 0xA11CE002U);
    nested = nested && (!ac::keep(h, 0.2) || ac::keep(h, 0.5)) && (!ac::keep(h, 0.5) || ac::keep(h, 0.9));
  }
  check(nested, "survivors at p are a subset of survivors at p' > p");
  check(ac::thinning_hash(1U, 2U, ac::kTagQ, 3U, 4U) != ac::thinning_hash(1U, 2U, ac::kTagX, 3U, 4U) &&
            ac::thinning_hash(1U, 2U, ac::kTagQ, 3U, 4U) != ac::thinning_hash(1U, 3U, ac::kTagQ, 3U, 4U) &&
            ac::thinning_hash(1U, 2U, ac::kTagQ, 3U, 4U) != ac::thinning_hash(1U, 2U, ac::kTagQ, 3U, 5U),
        "hash separates list, cell and seed");
  check(std::fabs(ac::source_retention(16.0, 100.0) - 0.4) < 1e-12, "retention sqrt(16/100) = 0.4");
  check(ac::source_retention(1e-9, 100.0) == 0.01 && ac::source_retention(500.0, 100.0) == 1.0,
        "retention clamps to [0.01, 1]");
  check(ac::source_retention(4.0, 0.0) == 1.0, "retention with no base keeps everything");
}

[[nodiscard]] bool same_postings(const std::vector<mr8::Posting>& a, const std::vector<mr8::Posting>& b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const mr8::Posting& p, const mr8::Posting& q) {
           return p.value == q.value && p.id == q.id;
         });
}

// `sub` is a subsequence of `all`, and holds exactly the entries keep() admits.
[[nodiscard]] bool exact_survivors(const std::vector<mr8::Posting>& sub, const std::vector<mr8::Posting>& all,
                                   const std::uint64_t root_seed, const std::uint32_t cell, const std::uint64_t tag,
                                   const double p, const std::uint64_t seed) {
  std::vector<mr8::Posting> expect;
  for (const mr8::Posting& e : all) {
    if (ac::keep(ac::thinning_hash(root_seed, cell, tag, e.id, seed), p)) {
      expect.push_back(e);
    }
  }
  return same_postings(sub, expect);
}

void test_thinned_microproblem() {
  Synthetic root = make_root(69U, 0x9876U, 3000U, 64U);
  mr8::Dataset& data = *root.data;
  const std::uint64_t before = rg::fingerprint(data);
  // A cell with work on every list.
  std::uint32_t cell = 0U;
  for (std::uint32_t c = 0U; c < ac::kCells; ++c) {
    const std::uint32_t u = (c / 64U + c % 64U) & 63U;
    if (data.q_family[c / 64U].size() > 20U && data.x_family[c % 64U].size() > 20U && data.y_family[u].size() > 20U) {
      cell = c;
      break;
    }
  }
  const std::uint32_t s = cell / 64U;
  const std::uint32_t t = cell % 64U;
  const std::uint32_t u = (s + t) & 63U;
  const std::vector<mr8::Posting> q0 = data.q_family[s];
  const std::vector<mr8::Posting> x0 = data.x_family[t];
  const std::vector<mr8::Posting> y0 = data.y_family[u];
  const std::vector<std::uint32_t> exact0 = data.targets[u].exact;
  const std::uint32_t* const view0 = data.view.exact[u];
  constexpr std::uint64_t kRootSeed = 0x9876U;

  for (const ac::Thinning thinning : {ac::Thinning{0.5, 1.0, 0xA11CE001U}, ac::Thinning{0.2, 1.0, 0xA11CE003U},
                                      ac::Thinning{1.0, 0.25, 0xA11CE001U}, ac::Thinning{0.6, 0.5, 0xA11CE002U}}) {
    const std::string label = "thinning p_qx=" + std::to_string(thinning.p_qx) + " p_y=" + std::to_string(thinning.p_y);
    // Predicted from the authentic keys, before anything is thinned.
    const ac::Geometry predicted = ac::thinned_geometry(root.keys, data, kRootSeed, cell, thinning);
    {
      const ac::ThinnedMicroproblem thinned(data, kRootSeed, cell, thinning);
      check(exact_survivors(data.q_family[s], q0, kRootSeed, cell, ac::kTagQ, thinning.p_qx, thinning.seed),
            label + ": Q holds exactly its survivors, in order");
      check(exact_survivors(data.x_family[t], x0, kRootSeed, cell, ac::kTagX, thinning.p_qx, thinning.seed),
            label + ": X holds exactly its survivors, in order");
      check(exact_survivors(data.y_family[u], y0, kRootSeed, cell, ac::kTagY, thinning.p_y, thinning.seed),
            label + ": Y holds exactly its survivors, in order");
      if (thinning.p_y >= 1.0) {
        check(data.targets[u].exact == exact0 && data.view.exact[u] == view0, label + ": Y target untouched");
      } else {
        mr8::TargetSet rebuilt;
        mr8::build_target(data.y_family[u], rebuilt);
        check(data.targets[u].exact == rebuilt.exact && data.view.exact[u] == data.targets[u].exact.data() &&
                  data.view.coarse[u] == data.targets[u].coarse.data(),
              label + ": Y target and view rebuilt for the thinned list");
      }
      // Exactness on the thinned state: ARWLF and K2_WIDE == whole-value brute force.
      rg::build_root_index(data, root.index);
      build_keys(root);
      root.index.selected.fill(0U);
      root.index.selected[cell] = 1U;
      root.index.selected_count = 1U;
      const rg::OutputDigest expect = oracle(root);
      rg::RootSink arwlf = make_sink(root);
      check(aw::run_arwlf(data, root.index, root.keys, aw::Config{}, arwlf, nullptr) == aw::Status::Completed,
            label + ": ARWLF completes");
      rg::RootSink wide = make_sink(root);
      a64::run_k2_wide(data, root.index, root.keys, wide, nullptr, false);
      for (const rg::OutputDigest* d : {&arwlf.digest, &wide.digest}) {
        check(d->triples == expect.triples && d->triple_sum == expect.triple_sum &&
                  d->triple_xor == expect.triple_xor && d->triple_sum2 == expect.triple_sum2,
              label + ": backend == brute force on the thinned root");
      }
      // The census of the thinned state matches the counted run.
      const std::vector<ac::Geometry> census = ac::census(root.keys, root.index);
      aw::Config counted;
      counted.account = true;
      aw::Telemetry tel;
      rg::RootSink sink = make_sink(root);
      static_cast<void>(aw::run_arwlf(data, root.index, root.keys, counted, sink, &tel));
      check(tel.lanes == census[cell].lanes && tel.packets == census[cell].packets &&
                (!tel.avx2 || tel.rows == census[cell].rows),
            label + ": thinned census == counted telemetry");
      const ac::Geometry& g = census[cell];
      check(predicted.lanes == g.lanes && predicted.packets == g.packets && predicted.rows == g.rows &&
                predicted.leaf_triples == g.leaf_triples && predicted.logical_pairs == g.logical_pairs &&
                predicted.y_count == g.y_count,
            label + ": thinned_geometry == census of the thinned state");
    }
    rg::build_root_index(data, root.index);
    build_keys(root);
    check(same_postings(data.q_family[s], q0) && same_postings(data.x_family[t], x0) &&
              same_postings(data.y_family[u], y0),
          label + ": families restored entry for entry");
    check(data.targets[u].exact == exact0 && data.view.exact[u] == data.targets[u].exact.data(),
          label + ": Y target and view restored");
    check(rg::fingerprint(data) == before, label + ": dataset fingerprint restored");
  }
  // Calibrated retention: L at p is within the target, and admitting the very
  // next entry (the smallest surviving-hash above the threshold) exceeds it.
  {
    Synthetic dense = concentrated_root(0xD0D0U, 60000U, 60000U, 64U * 200U);
    const ac::Geometry full = ac::LeafCensus(dense.keys).measure(5U, 9U);
    check(full.lanes_per_packet() > 4.0, "calibration fixture is dense (L > 4)");
    constexpr std::uint32_t kCell = 5U * 64U + 9U;
    for (const double target : {1.0, 1.5, 2.0, 3.0, 4.0}) {
      const ac::Retention r = ac::calibrate_source_retention(dense.keys, *dense.data, 0xD0D0U, kCell, target, 0xA11CE001U);
      // The next entry the nested thinning would admit.
      std::uint64_t next = std::numeric_limits<std::uint64_t>::max();
      const std::uint64_t threshold = ac::keep_threshold(r.p);
      for (const auto& [family, tag] : {std::pair{&dense.data->q_family[5], ac::kTagQ}, std::pair{&dense.data->x_family[9], ac::kTagX}}) {
        for (const mr8::Posting& e : *family) {
          const std::uint64_t h = ac::thinning_hash(0xD0D0U, kCell, tag, e.id, 0xA11CE001U);
          next = h >= threshold ? std::min(next, h) : next;
        }
      }
      const double p_next = std::ldexp(static_cast<double>(next) + 1e6, -64);
      const double above =
          ac::thinned_geometry(dense.keys, *dense.data, 0xD0D0U, kCell, ac::Thinning{p_next, 1.0, 0xA11CE001U})
              .lanes_per_packet();
      check(r.lanes_per_packet <= target && r.lanes_per_packet > 0.0 && r.p < 1.0,
            "calibrated L within target " + std::to_string(target));
      check(above > target, "one more entry exceeds target " + std::to_string(target));
      check(std::fabs(r.sqrt_rule - ac::source_retention(target, full.lanes_per_packet())) < 1e-15,
            "sqrt rule recorded at target " + std::to_string(target));
      const ac::Retention again =
          ac::calibrate_source_retention(dense.keys, *dense.data, 0xD0D0U, 5U * 64U + 9U, target, 0xA11CE001U);
      check(again.p == r.p, "calibration deterministic at target " + std::to_string(target));
    }
    const ac::Retention high =
        ac::calibrate_source_retention(dense.keys, *dense.data, 0xD0D0U, 5U * 64U + 9U, 1e9, 0xA11CE001U);
    check(high.p == 1.0 && high.lanes_per_packet == full.lanes_per_packet(), "unreachable target keeps everything");
  }
  // p = 1 everywhere: nothing moves.
  {
    const ac::ThinnedMicroproblem none(data, kRootSeed, cell, ac::Thinning{});
    check(same_postings(data.q_family[s], q0) && data.view.exact[u] == view0, "identity thinning is a no-op");
  }
}

// ---------------------------------------------------------------------------
// Matched order and the witness hook.
// ---------------------------------------------------------------------------
void test_round_order() {
  check(ac::round_order(5U, 0U) == std::vector<std::size_t>{0U, 1U, 2U, 3U, 4U}, "round 0 canonical");
  check(ac::round_order(5U, 1U) == std::vector<std::size_t>{0U, 4U, 3U, 2U, 1U}, "round 1 rotated 1, reversed");
  check(ac::round_order(5U, 2U) == std::vector<std::size_t>{2U, 3U, 4U, 0U, 1U}, "round 2 rotated 2");
  check(ac::round_order(4U, 7U) == std::vector<std::size_t>{2U, 1U, 0U, 3U}, "round 7 of 4 rotated 3, reversed");
  check(ac::round_order(0U, 3U).empty(), "no backends");
  for (std::uint32_t round = 0U; round < 20U; ++round) {
    std::vector<std::size_t> order = ac::round_order(5U, round);
    std::sort(order.begin(), order.end());
    check(order == std::vector<std::size_t>{0U, 1U, 2U, 3U, 4U}, "every round is a permutation");
  }
  // Over 2 x count rounds every backend leads at least once.
  std::set<std::size_t> leaders;
  for (std::uint32_t round = 0U; round < 10U; ++round) {
    leaders.insert(ac::round_order(5U, round).front());
  }
  check(leaders.size() == 5U, "every backend leads some round");
}

void test_witness_hook() {
  // A four-entry authority over n = 32: entries pass the low-32 quotient and
  // residue stages exactly when the brute-force sums say so.
  tt::AuthorityTables authority;
  authority.ok = true;
  authority.n = 32U;
  authority.residue_words = 1U;
  authority.tau_low32 = 100U;
  authority.root_target = {1000U};
  authority.shape_available = false;
  const std::array<std::vector<std::uint32_t>, 3> quotient{std::vector<std::uint32_t>{10U, 20U, 30U, 40U},
                                                           std::vector<std::uint32_t>{50U, 40U, 30U, 20U},
                                                           std::vector<std::uint32_t>{40U, 40U, 40U, 30U}};
  const std::array<std::vector<std::uint32_t>, 3> residue{std::vector<std::uint32_t>{100U, 200U, 300U, 400U},
                                                          std::vector<std::uint32_t>{500U, 400U, 300U, 900U},
                                                          std::vector<std::uint32_t>{400U, 400U, 400U, 300U}};
  for (std::size_t list = 0U; list < 3U; ++list) {
    authority.quotient[list] = quotient[list];
    authority.residue[list] = residue[list];
    authority.digits[list].assign(4U * 32U, 0);
  }
  std::vector<std::array<ter::EntryId, 3>> witnesses;
  rg::RootSink sink;
  sink.authority = &authority;
  sink.witnesses = &witnesses;
  sink.reset();
  std::vector<std::array<ter::EntryId, 3>> expect;
  for (ter::EntryId a = 0U; a < 4U; ++a) {
    for (ter::EntryId b = 0U; b < 4U; ++b) {
      for (ter::EntryId c = 0U; c < 4U; ++c) {
        sink.emit_triple(a, b, c);
        if (quotient[0][a] + quotient[1][b] + quotient[2][c] == 100U &&
            residue[0][a] + residue[1][b] + residue[2][c] == 1000U) {
          expect.push_back({a, b, c});
        }
      }
    }
  }
  check(!expect.empty(), "witness fixture has residue-exact triples");
  check(witnesses == expect && sink.digest.residue == expect.size(), "witnesses == brute-force residue-exact triples");
  sink.reset();
  check(witnesses.empty(), "reset clears the witness list");
  rg::RootSink silent;
  silent.authority = &authority;
  silent.reset();
  silent.emit_triple(expect[0][0], expect[0][1], expect[0][2]);
  check(silent.digest.residue == 1U, "no witness list: digest still counts");
}

}  // namespace

int main() {
  test_geometry();
  test_stratified();
  test_threshold();
  test_thinned_microproblem();
  test_round_order();
  test_witness_hook();
  std::cout << "ter_arwlf_campaign_tests: " << g_checks << " checks, " << g_failures << " failures\n";
  return g_failures == 0U ? 0 : 1;
}
