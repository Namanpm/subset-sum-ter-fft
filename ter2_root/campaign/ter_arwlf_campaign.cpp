#include "ter_arwlf_campaign.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace ter::arwlf_campaign {
namespace {

constexpr std::uint32_t kLeafMask = arwlf::kLeaves - 1U;

[[nodiscard]] std::uint64_t blocks(const std::uint64_t entries) noexcept {
  return (entries + arwlf::kPacket - 1U) / arwlf::kPacket;
}

}  // namespace

LeafFamily leaf_family(const std::vector<std::uint64_t>& keys) {
  LeafFamily family;
  family.count.assign(arwlf::kLeaves, 0U);
  for (const std::uint64_t key : keys) {
    ++family.count[key & kLeafMask];
  }
  family.size = keys.size();
  for (std::uint32_t leaf = 0U; leaf < arwlf::kLeaves; ++leaf) {
    const std::uint32_t c = family.count[leaf];
    (c == 0U ? family.empty : family.nonempty).push_back(leaf);
    family.blocks += blocks(c);
    family.max = std::max(family.max, c);
    if (c > arwlf::kPacket) {
      family.overflow.push_back(leaf);
      family.extra_blocks += blocks(c) - 1U;
    }
  }
  return family;
}

Geometry measure(const LeafFamily& q, const LeafFamily& x, const LeafFamily& y, const std::uint32_t s,
                 const std::uint32_t t) {
  Geometry g;
  g.cell = s * rg::kFamilies + t;
  g.s = s;
  g.t = t;
  g.u = (s + t) & (rg::kFamilies - 1U);
  g.kappa = (s + t) >> mr8::kSplitBits;
  g.q_count = q.size;
  g.x_count = x.size;
  g.y_count = y.size;
  g.logical_pairs = q.size * x.size;
  g.q_leaves = static_cast<std::uint32_t>(q.nonempty.size());
  g.x_leaves = static_cast<std::uint32_t>(x.nonempty.size());
  g.y_leaves = static_cast<std::uint32_t>(y.nonempty.size());
  g.q_leaf_max = q.max;
  g.x_leaf_max = x.max;
  g.y_leaf_max = y.max;

  // Sum over the smaller of the nonempty Y leaves and their complement.
  const bool complement = y.empty.size() < y.nonempty.size();
  const std::vector<std::uint32_t>& ks = complement ? y.empty : y.nonempty;
  std::uint64_t triples = 0U;
  std::uint64_t packets = 0U;
  std::uint64_t lanes = 0U;
  std::uint64_t extra = 0U;  // overflow rows
  for (const std::uint32_t k : ks) {
    for (const std::uint32_t i : q.nonempty) {
      const std::uint32_t nx = x.count[(k - i - g.kappa) & kLeafMask];
      if (nx != 0U) {
        const std::uint32_t nq = q.count[i];
        ++triples;
        packets += blocks(nq) * blocks(nx);
        lanes += static_cast<std::uint64_t>(nq) * nx;
      }
    }
    for (const std::uint32_t j : x.overflow) {
      extra += static_cast<std::uint64_t>(q.count[(k - g.kappa - j) & kLeafMask]) * (blocks(x.count[j]) - 1U);
    }
  }
  if (complement) {
    triples = static_cast<std::uint64_t>(q.nonempty.size()) * x.nonempty.size() - triples;
    packets = q.blocks * x.blocks - packets;
    lanes = q.size * x.size - lanes;
    extra = q.size * x.extra_blocks - extra;
  }
  g.leaf_triples = triples;
  g.packets = packets;
  g.lanes = lanes;
  g.rows = static_cast<std::uint64_t>(y.nonempty.size()) * q.size + extra;
  return g;
}

LeafCensus::LeafCensus(const a64::FamilyKeys& keys) {
  for (std::size_t list = 0U; list < 3U; ++list) {
    for (std::uint32_t f = 0U; f < rg::kFamilies; ++f) {
      family_[list][f] = leaf_family(keys.key[list][f]);
    }
  }
}

Geometry LeafCensus::measure(const std::uint32_t s, const std::uint32_t t) const {
  return arwlf_campaign::measure(family_[0][s], family_[1][t], family_[2][(s + t) & (rg::kFamilies - 1U)], s, t);
}

std::vector<Geometry> census(const a64::FamilyKeys& keys, const rg::RootIndex& index) {
  const LeafCensus leaves(keys);
  std::vector<Geometry> out;
  out.reserve(kCells);
  for (std::uint32_t s = 0U; s < rg::kFamilies; ++s) {
    for (std::uint32_t t = 0U; t < rg::kFamilies; ++t) {
      Geometry g = leaves.measure(s, t);
      g.x_groups = index.x_groups[t].size();
      g.y_groups = index.y_groups[g.u].size();
      out.push_back(g);
    }
  }
  return out;
}

Geometry selected_geometry(const std::vector<Geometry>& census, const rg::RootIndex& index) {
  Geometry sum;
  sum.cell = kCells;
  for (const Geometry& g : census) {
    if (!index.active(g.s, g.t)) {
      continue;
    }
    sum.q_count += g.q_count;
    sum.x_count += g.x_count;
    sum.y_count += g.y_count;
    sum.logical_pairs += g.logical_pairs;
    sum.q_leaves += g.q_leaves;
    sum.x_leaves += g.x_leaves;
    sum.y_leaves += g.y_leaves;
    sum.q_leaf_max = std::max(sum.q_leaf_max, g.q_leaf_max);
    sum.x_leaf_max = std::max(sum.x_leaf_max, g.x_leaf_max);
    sum.y_leaf_max = std::max(sum.y_leaf_max, g.y_leaf_max);
    sum.leaf_triples += g.leaf_triples;
    sum.packets += g.packets;
    sum.lanes += g.lanes;
    sum.rows += g.rows;
    sum.x_groups = std::max(sum.x_groups, g.x_groups);
    sum.y_groups = std::max(sum.y_groups, g.y_groups);
  }
  return sum;
}

double quantile(const std::vector<double>& sorted, const double fraction) {
  if (sorted.empty()) {
    return 0.0;
  }
  const double position = fraction * static_cast<double>(sorted.size() - 1U);
  const auto lower = static_cast<std::size_t>(position);
  const std::size_t upper = lower + 1U < sorted.size() ? lower + 1U : lower;
  const double weight = position - static_cast<double>(lower);
  return sorted[lower] * (1.0 - weight) + sorted[upper] * weight;
}

std::vector<Stratum> select_stratified(const std::vector<Geometry>& census) {
  struct Point {
    double x;
    double y;
    std::uint32_t cell;
  };
  std::vector<Point> population;
  for (const Geometry& g : census) {
    if (g.packets > 0U && g.logical_pairs > 0U) {
      population.push_back(Point{std::log2(static_cast<double>(g.logical_pairs)), g.lanes_per_packet(), g.cell});
    }
  }
  if (population.empty()) {
    return {};
  }
  const auto cuts = [&](const auto axis) {
    std::vector<double> values;
    values.reserve(population.size());
    for (const Point& p : population) {
      values.push_back(axis(p));
    }
    std::sort(values.begin(), values.end());
    return std::array<double, 3>{quantile(values, 0.25), quantile(values, 0.50), quantile(values, 0.75)};
  };
  const auto get_x = [](const Point& p) { return p.x; };
  const auto get_y = [](const Point& p) { return p.y; };
  const std::array<double, 3> cx = cuts(get_x);
  const std::array<double, 3> cy = cuts(get_y);
  const double scale_x = cx[2] > cx[0] ? cx[2] - cx[0] : 1.0;
  const double scale_y = cy[2] > cy[0] ? cy[2] - cy[0] : 1.0;
  const auto bin = [](const double v, const std::array<double, 3>& c) {
    return static_cast<std::uint32_t>((v >= c[0] ? 1 : 0) + (v >= c[1] ? 1 : 0) + (v >= c[2] ? 1 : 0));
  };
  std::array<std::vector<Point>, 16> cells;
  for (const Point& p : population) {  // cell order is kept inside every grid cell
    cells[bin(p.x, cx) * 4U + bin(p.y, cy)].push_back(p);
  }
  std::vector<Stratum> out;
  for (std::uint32_t grid = 0U; grid < 16U; ++grid) {
    const std::vector<Point>& members = cells[grid];
    if (members.empty()) {
      continue;
    }
    std::vector<double> xs;
    std::vector<double> ys;
    for (const Point& p : members) {
      xs.push_back(p.x);
      ys.push_back(p.y);
    }
    std::sort(xs.begin(), xs.end());
    std::sort(ys.begin(), ys.end());
    const double mx = quantile(xs, 0.5);
    const double my = quantile(ys, 0.5);
    const Point* best = nullptr;
    double best_distance = std::numeric_limits<double>::infinity();
    for (const Point& p : members) {
      const double dx = (p.x - mx) / scale_x;
      const double dy = (p.y - my) / scale_y;
      const double distance = dx * dx + dy * dy;
      if (distance < best_distance) {
        best_distance = distance;
        best = &p;
      }
    }
    out.push_back(Stratum{best->cell, grid / 4U, grid % 4U, static_cast<std::uint32_t>(members.size())});
  }
  std::sort(out.begin(), out.end(), [](const Stratum& a, const Stratum& b) { return a.cell < b.cell; });
  return out;
}

std::uint64_t thinning_hash(const std::uint64_t root_seed, const std::uint32_t cell, const std::uint64_t tag,
                            const std::uint64_t entry_id, const std::uint64_t thinning_seed) noexcept {
  std::uint64_t h = thinning_seed;
  h = rg::mix64(h ^ root_seed);
  h = rg::mix64(h ^ cell);
  h = rg::mix64(h ^ tag);
  return rg::mix64(h ^ entry_id);
}

std::uint64_t keep_threshold(const double p) noexcept {
  if (!(p > 0.0)) {
    return 0U;
  }
  if (p >= 1.0) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return static_cast<std::uint64_t>(std::ldexp(p, 64));
}

bool keep(const std::uint64_t hash, const double p) noexcept {
  if (p >= 1.0) {
    return true;
  }
  return hash < keep_threshold(p);
}

double source_retention(const double target, const double base) noexcept {
  if (!(base > 0.0) || !(target > 0.0)) {
    return 1.0;
  }
  return std::clamp(std::sqrt(target / base), 0.01, 1.0);
}

namespace {

// Keys of the entries of one family that survive `p` (family-aligned).
[[nodiscard]] std::vector<std::uint64_t> surviving_keys(const std::vector<std::uint64_t>& keys,
                                                        const std::vector<mr8::Posting>& postings,
                                                        const std::uint64_t root_seed, const std::uint32_t cell,
                                                        const std::uint64_t tag, const double p,
                                                        const std::uint64_t seed) {
  if (p >= 1.0) {
    return keys;
  }
  std::vector<std::uint64_t> kept;
  for (std::size_t i = 0U; i < postings.size(); ++i) {
    if (keep(thinning_hash(root_seed, cell, tag, postings[i].id, seed), p)) {
      kept.push_back(keys[i]);
    }
  }
  return kept;
}

}  // namespace

Geometry thinned_geometry(const a64::FamilyKeys& keys, const mr8::Dataset& data, const std::uint64_t root_seed,
                          const std::uint32_t cell, const Thinning& thinning) {
  const std::uint32_t s = cell / rg::kFamilies;
  const std::uint32_t t = cell % rg::kFamilies;
  const std::uint32_t u = (s + t) & (rg::kFamilies - 1U);
  return measure(
      leaf_family(surviving_keys(keys.key[0][s], data.q_family[s], root_seed, cell, kTagQ, thinning.p_qx, thinning.seed)),
      leaf_family(surviving_keys(keys.key[1][t], data.x_family[t], root_seed, cell, kTagX, thinning.p_qx, thinning.seed)),
      leaf_family(surviving_keys(keys.key[2][u], data.y_family[u], root_seed, cell, kTagY, thinning.p_y, thinning.seed)),
      s, t);
}

Retention calibrate_source_retention(const a64::FamilyKeys& keys, const mr8::Dataset& data,
                                     const std::uint64_t root_seed, const std::uint32_t cell, const double target,
                                     const std::uint64_t seed) {
  const std::uint32_t s = cell / rg::kFamilies;
  const std::uint32_t t = cell % rg::kFamilies;
  const LeafFamily y = leaf_family(keys.key[2][(s + t) & (rg::kFamilies - 1U)]);
  const auto lanes_at = [&](const double p) {
    return measure(leaf_family(surviving_keys(keys.key[0][s], data.q_family[s], root_seed, cell, kTagQ, p, seed)),
                   leaf_family(surviving_keys(keys.key[1][t], data.x_family[t], root_seed, cell, kTagX, p, seed)), y,
                   s, t)
        .lanes_per_packet();
  };
  Retention r;
  const double base = lanes_at(1.0);
  r.sqrt_rule = source_retention(target, base);
  if (base <= target) {
    r.lanes_per_packet = base;
    return r;
  }
  double lo = 1e-4;
  double hi = 1.0;
  for (int step = 0; step < 48; ++step) {
    const double mid = 0.5 * (lo + hi);
    if (lanes_at(mid) <= target) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  r.p = lo;
  r.lanes_per_packet = lanes_at(lo);
  return r;
}

namespace {

// Moves the survivors of `list` into it and the original into `saved`.
void thin_list(std::vector<mr8::Posting>& list, std::vector<mr8::Posting>& saved, const std::uint64_t root_seed,
               const std::uint32_t cell, const std::uint64_t tag, const double p, const std::uint64_t seed) {
  std::vector<mr8::Posting> kept;
  for (const mr8::Posting& entry : list) {
    if (keep(thinning_hash(root_seed, cell, tag, entry.id, seed), p)) {
      kept.push_back(entry);
    }
  }
  kept.shrink_to_fit();
  saved = std::move(list);
  list = std::move(kept);
}

}  // namespace

ThinnedMicroproblem::ThinnedMicroproblem(mr8::Dataset& data, const std::uint64_t root_seed,
                                         const std::uint32_t cell, const Thinning& thinning)
    : data_(data),
      s_(cell / rg::kFamilies),
      t_(cell % rg::kFamilies),
      u_((cell / rg::kFamilies + cell % rg::kFamilies) & (rg::kFamilies - 1U)) {
  if (thinning.p_qx < 1.0) {
    thin_list(data_.q_family[s_], q_, root_seed, cell, kTagQ, thinning.p_qx, thinning.seed);
    q_thinned_ = true;
    thin_list(data_.x_family[t_], x_, root_seed, cell, kTagX, thinning.p_qx, thinning.seed);
    x_thinned_ = true;
  }
  if (thinning.p_y < 1.0) {
    thin_list(data_.y_family[u_], y_, root_seed, cell, kTagY, thinning.p_y, thinning.seed);
    y_thinned_ = true;
    y_target_ = std::move(data_.targets[u_]);
    data_.targets[u_] = mr8::TargetSet{};
    mr8::build_target(data_.y_family[u_], data_.targets[u_]);
    point_view_at_target();
  }
}

ThinnedMicroproblem::~ThinnedMicroproblem() {
  if (q_thinned_) {
    data_.q_family[s_] = std::move(q_);
  }
  if (x_thinned_) {
    data_.x_family[t_] = std::move(x_);
  }
  if (y_thinned_) {
    data_.y_family[u_] = std::move(y_);
    data_.targets[u_] = std::move(y_target_);
    point_view_at_target();
  }
}

void ThinnedMicroproblem::point_view_at_target() noexcept {
  const mr8::TargetSet& target = data_.targets[u_];
  data_.view.exact[u_] = target.exact.data();
  data_.view.coarse[u_] = target.coarse.data();
  data_.view.sig07[u_] = target.sig07.data();
  data_.view.sig815[u_] = target.sig815.data();
}

std::vector<std::size_t> round_order(const std::size_t count, const std::uint32_t round) {
  std::vector<std::size_t> order(count);
  std::iota(order.begin(), order.end(), std::size_t{0});
  if (count == 0U) {
    return order;
  }
  std::rotate(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(round % count), order.end());
  if ((round & 1U) != 0U) {
    std::reverse(order.begin(), order.end());
  }
  return order;
}

}  // namespace ter::arwlf_campaign
