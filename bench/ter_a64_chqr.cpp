#include "ter_a64_chqr.hpp"

#include "ter_microreport8_kernels.hpp"

#include <algorithm>
#include <bit>

namespace ter::a64 {
namespace {

struct Entry {
  std::uint64_t perm = 0;  // digit-permuted refined residual: lowest digit in the top bits
  std::uint64_t key = 0;   // full W-bit residual
  std::uint32_t id = 0;    // EntryId in the donor list
  std::uint8_t hi = 0;     // quotient bits above 64
};

struct Shape {
  std::uint32_t levels = 0;
  std::uint32_t refined_bits = 0;
  std::uint64_t refined_mask = 0;
  std::uint32_t residual_bits = 0;  // W
  std::uint64_t residual_mask = 0;
  std::uint32_t hi_bits = 0;
  std::uint8_t hi_mask = 0;
  std::array<std::uint32_t, kMaxLevels> width{};
  std::array<std::uint32_t, kMaxLevels> offset{};  // bit offset of the digit in the key
  std::array<std::uint32_t, kMaxLevels> shift{};   // bit offset of the digit in perm
  std::array<std::uint64_t, kMaxLevels> mask{};
};

[[nodiscard]] std::uint64_t low_mask(const std::uint32_t bits) noexcept {
  return bits >= 64U ? ~std::uint64_t{0} : (std::uint64_t{1} << bits) - 1U;
}

[[nodiscard]] Shape make_shape(const DigitProfile& profile, const FamilyKeys& keys) {
  Shape shape;
  shape.levels = profile.levels;
  shape.refined_bits = profile.residual_bits;
  shape.refined_mask = low_mask(profile.residual_bits);
  shape.residual_bits = keys.residual_bits;
  shape.residual_mask = low_mask(keys.residual_bits);
  shape.hi_bits = keys.hi_bits;
  shape.hi_mask = static_cast<std::uint8_t>((1U << keys.hi_bits) - 1U);
  std::uint32_t offset = 0U;
  for (std::uint32_t level = 0U; level < profile.levels; ++level) {
    shape.width[level] = profile.widths[level];
    shape.offset[level] = offset;
    offset += profile.widths[level];
    shape.shift[level] = profile.residual_bits - offset;
    shape.mask[level] = low_mask(profile.widths[level]);
  }
  return shape;
}

[[nodiscard]] std::uint64_t permute(const Shape& shape, const std::uint64_t key) noexcept {
  std::uint64_t perm = 0U;
  for (std::uint32_t level = 0U; level < shape.levels; ++level) {
    perm |= ((key >> shape.offset[level]) & shape.mask[level]) << shape.shift[level];
  }
  return perm;
}

[[nodiscard]] std::uint32_t digit(const Shape& shape, const Entry& entry,
                                  const std::uint32_t level) noexcept {
  return static_cast<std::uint32_t>((entry.perm >> shape.shift[level]) & shape.mask[level]);
}

// Direct leaves with at most this many Y entries scan them linearly.
constexpr std::uint32_t kLinearLeaf = 16U;

struct Run {
  std::uint32_t digit;
  std::uint32_t begin;
  std::uint32_t end;
};

// Children of a node at `level`: consecutive runs of equal digit, ascending,
// because every entry of the node already agrees on the digits above it.
void split_runs(const Shape& shape, const std::vector<Entry>& entries, const std::uint32_t begin,
                const std::uint32_t end, const std::uint32_t level, std::vector<Run>& runs) {
  runs.clear();
  for (std::uint32_t i = begin; i < end;) {
    const std::uint32_t d = digit(shape, entries[i], level);
    std::uint32_t j = i + 1U;
    while (j < end && digit(shape, entries[j], level) == d) {
      ++j;
    }
    runs.push_back({d, i, j});
    i = j;
  }
}

// Carry-aware depth-first refinement of node triples (Q_node, X_node, Y_node,
// carry_in).  A child triple is entered only when Y has the one digit the
// exact incoming carry requires (spec 10.3).  Run buffers are per level, so
// the recursion stops allocating once they have grown.
struct Refiner {
  const Shape& shape;
  const std::uint64_t direct_threshold;
  const std::uint32_t kappa;
  const std::vector<Entry>& q;
  const std::vector<Entry>& x;
  const std::vector<Entry>& y;
  rg::RootSink& sink;
  Telemetry& telemetry;
  std::array<std::vector<Run>, kMaxLevels> q_runs;
  std::array<std::vector<Run>, kMaxLevels> x_runs;
  std::array<std::vector<Run>, kMaxLevels> y_runs;

  // Every (q, x) of the two ranges against Y's range: exact on the refined
  // bits (sorted search), then on all W residual bits, then on the high bits
  // with the carry out of the low-F addition.  Exact however many levels were
  // consumed above: the node invariant only narrows the Y range.
  void resolve(const std::uint32_t qb, const std::uint32_t qe, const std::uint32_t xb,
               const std::uint32_t xe, const std::uint32_t yb, const std::uint32_t ye) {
    telemetry.direct_pairs += static_cast<std::uint64_t>(qe - qb) * (xe - xb);
    if (ye - yb <= kLinearLeaf) {
      // Small Y range: compare keys directly.  Equal refined bits is exactly
      // equal perm, so the counters mean the same as on the search path.
      for (std::uint32_t qi = qb; qi < qe; ++qi) {
        for (std::uint32_t xi = xb; xi < xe; ++xi) {
          const std::uint64_t sum = q[qi].key + x[xi].key + kappa;
          const std::uint64_t value = sum & shape.residual_mask;
          const std::uint64_t carry = sum >> shape.residual_bits;
          for (std::uint32_t yi = yb; yi < ye; ++yi) {
            if (((y[yi].key ^ value) & shape.refined_mask) != 0U) {
              continue;
            }
            ++telemetry.projected_hits;
            if (y[yi].key != value) {
              continue;
            }
            ++telemetry.fast_hits;
            if (shape.hi_bits != 0U &&
                static_cast<std::uint8_t>((q[qi].hi + x[xi].hi + carry) & shape.hi_mask) != y[yi].hi) {
              continue;
            }
            ++telemetry.full_hits;
            sink.emit_triple(q[qi].id, x[xi].id, y[yi].id);
          }
        }
      }
      return;
    }
    const auto y_begin = y.begin() + yb;
    const auto y_end = y.begin() + ye;
    for (std::uint32_t qi = qb; qi < qe; ++qi) {
      for (std::uint32_t xi = xb; xi < xe; ++xi) {
        // key < 2^W and W <= 58, so the sum cannot overflow 64 bits.
        const std::uint64_t sum = q[qi].key + x[xi].key + kappa;
        const std::uint64_t value = sum & shape.residual_mask;
        const std::uint64_t carry = sum >> shape.residual_bits;
        const std::uint64_t target = permute(shape, value & shape.refined_mask);
        auto it = std::lower_bound(y_begin, y_end, target,
                                   [](const Entry& e, const std::uint64_t p) { return e.perm < p; });
        for (; it != y_end && it->perm == target; ++it) {
          ++telemetry.projected_hits;
          if (it->key != value) {
            continue;
          }
          ++telemetry.fast_hits;
          if (shape.hi_bits != 0U &&
              static_cast<std::uint8_t>((q[qi].hi + x[xi].hi + carry) & shape.hi_mask) != it->hi) {
            continue;
          }
          ++telemetry.full_hits;
          sink.emit_triple(q[qi].id, x[xi].id, it->id);
        }
      }
    }
  }

  void node(const std::uint32_t level, const std::uint32_t qb, const std::uint32_t qe,
            const std::uint32_t xb, const std::uint32_t xe, const std::uint32_t yb,
            const std::uint32_t ye, const std::uint32_t carry) {
    ++telemetry.tasks[level];
    if (level == shape.levels ||
        static_cast<std::uint64_t>(qe - qb) * (xe - xb) <= direct_threshold) {
      resolve(qb, qe, xb, xe, yb, ye);
      return;
    }
    std::vector<Run>& qr = q_runs[level];
    std::vector<Run>& xr = x_runs[level];
    std::vector<Run>& yr = y_runs[level];
    split_runs(shape, q, qb, qe, level, qr);
    split_runs(shape, x, xb, xe, level, xr);
    split_runs(shape, y, yb, ye, level, yr);
    const std::uint32_t width = shape.width[level];
    const auto mask = static_cast<std::uint32_t>(shape.mask[level]);
    for (const Run& qrun : qr) {
      // Over ascending x digits, want = (qd + xd + carry) mod 2^width ascends,
      // wraps to 0 at most once, then ascends again: a linear merge against
      // the ascending Y digits, restarted once at the wrap.
      std::size_t yp = 0U;
      bool wrapped = false;
      for (const Run& xrun : xr) {
        ++telemetry.digit_pairs;
        const std::uint32_t sum = qrun.digit + xrun.digit + carry;
        const std::uint32_t want = sum & mask;
        if (!wrapped && (sum >> width) != 0U) {
          wrapped = true;
          yp = 0U;
        }
        while (yp < yr.size() && yr[yp].digit < want) {
          ++yp;
        }
        if (yp == yr.size()) {
          if (wrapped) {
            break;
          }
          continue;
        }
        if (yr[yp].digit != want) {
          continue;
        }
        node(level + 1U, qrun.begin, qrun.end, xrun.begin, xrun.end, yr[yp].begin, yr[yp].end,
             sum >> width);
      }
    }
  }
};

// Sorted family arrays, built lazily inside the timed call and reused by every
// selected microproblem that touches the family.
struct FamilyCache {
  std::array<std::array<std::vector<Entry>, rg::kFamilies>, 3> entries;
  std::array<std::array<bool, rg::kFamilies>, 3> built{};

  const std::vector<Entry>& get(const Shape& shape, const std::size_t list,
                                const std::uint32_t family,
                                const std::vector<mr8::Posting>& postings, const FamilyKeys& keys,
                                Telemetry& telemetry) {
    std::vector<Entry>& out = entries[list][family];
    if (!built[list][family]) {
      built[list][family] = true;
      const std::vector<std::uint64_t>& key = keys.key[list][family];
      const std::vector<std::uint8_t>& hi = keys.hi[list][family];
      out.resize(postings.size());
      for (std::size_t i = 0U; i < postings.size(); ++i) {
        out[i] = Entry{permute(shape, key[i] & shape.refined_mask), key[i], postings[i].id, hi[i]};
      }
      std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
        return a.perm != b.perm ? a.perm < b.perm : a.id < b.id;
      });
      telemetry.index_bytes += out.size() * sizeof(Entry);
    }
    return out;
  }
};

}  // namespace

DigitProfile default_profile(const std::uint32_t residual_bits, const std::uint32_t digit_bits) {
  DigitProfile profile;
  profile.residual_bits = residual_bits;
  std::uint32_t left = residual_bits;
  while (left > 0U && profile.levels < kMaxLevels) {
    const std::uint32_t width = std::min(left, digit_bits);
    profile.widths[profile.levels++] = width;
    left -= width;
  }
  return profile;
}

FamilyKeys build_family_keys(const rg::Workload& workload) {
  FamilyKeys keys;
  const std::uint32_t full = workload.full_quotient_bits;
  if (full < 25U || full > 69U || workload.key_lo[0].empty() || workload.data == nullptr) {
    return keys;
  }
  keys.full_bits = full;
  keys.fast_bits = std::min<std::uint32_t>(full, 64U);
  keys.residual_bits = keys.fast_bits - mr8::kSplitBits;
  keys.hi_bits = full - keys.fast_bits;
  const std::uint64_t mask = low_mask(keys.residual_bits);
  const mr8::Dataset& data = *workload.data;
  const std::array<const std::array<std::vector<mr8::Posting>, rg::kFamilies>*, 3> lists{
      &data.q_family, &data.x_family, &data.y_family};
  for (std::size_t list = 0U; list < 3U; ++list) {
    for (std::uint32_t family = 0U; family < rg::kFamilies; ++family) {
      const auto& postings = (*lists[list])[family];
      auto& key = keys.key[list][family];
      auto& hi = keys.hi[list][family];
      key.resize(postings.size());
      hi.resize(postings.size());
      for (std::size_t i = 0U; i < postings.size(); ++i) {
        const std::uint64_t lo = workload.key_lo[list][postings[i].id];
        // The family and the 19-bit residual must be exactly the donor's.
        if ((lo & (rg::kFamilies - 1U)) != family ||
            ((lo >> mr8::kSplitBits) & mr8::kResidualMask) != postings[i].value) {
          return FamilyKeys{};
        }
        key[i] = (lo >> mr8::kSplitBits) & mask;
        hi[i] = workload.key_hi[list][postings[i].id];
      }
    }
  }
  keys.ok = true;
  return keys;
}

void run_a64(const mr8::Dataset& data, const rg::RootIndex& index, const FamilyKeys& keys,
             const Config& config, rg::RootSink& sink, Telemetry* const telemetry_out) {
  Telemetry local;
  Telemetry& telemetry = telemetry_out != nullptr ? *telemetry_out : local;
  const Shape shape = make_shape(config.profile, keys);
  FamilyCache cache;
  for (std::uint32_t s = 0U; s < rg::kFamilies; ++s) {
    for (std::uint32_t t = 0U; t < rg::kFamilies; ++t) {
      if (!index.active(s, t)) {
        continue;
      }
      const std::uint32_t u = (s + t) & (rg::kFamilies - 1U);
      const std::uint32_t kappa = (s + t) >> mr8::kSplitBits;
      const std::vector<Entry>& q = cache.get(shape, 0U, s, data.q_family[s], keys, telemetry);
      const std::vector<Entry>& x = cache.get(shape, 1U, t, data.x_family[t], keys, telemetry);
      const std::vector<Entry>& y = cache.get(shape, 2U, u, data.y_family[u], keys, telemetry);
      telemetry.logical_pairs += static_cast<std::uint64_t>(q.size()) * x.size();
      Refiner refiner{shape, config.direct_threshold, kappa, q, x, y, sink, telemetry, {}, {}, {}};
      // The low-6 carry kappa is level 0's carry-in.
      refiner.node(0U, 0U, static_cast<std::uint32_t>(q.size()), 0U,
                   static_cast<std::uint32_t>(x.size()), 0U, static_cast<std::uint32_t>(y.size()),
                   kappa);
    }
  }
}

void run_k2_wide(const mr8::Dataset& data, const rg::RootIndex& index, const FamilyKeys& keys,
                 rg::RootSink& sink, Telemetry* const telemetry_out, const bool sweep,
                 const std::uint32_t work_divisor) {
  Telemetry local;
  Telemetry& telemetry = telemetry_out != nullptr ? *telemetry_out : local;
  using TileKernelFn = std::uint64_t (*)(const mr8::Tile&, const mr8::TargetView&);
  const TileKernelFn kernel = mr8::avx2::translation_unit_is_avx2()
                                  ? &mr8::avx2::k2_coarse_gather
                                  : &mr8::k2_coarse_gather_portable;
  const std::uint32_t residual_bits = keys.residual_bits;
  const std::uint64_t residual_mask = low_mask(residual_bits);
  const auto hi_mask = static_cast<std::uint8_t>((1U << keys.hi_bits) - 1U);
  mr8::Tile tile{};
  for (std::uint32_t u = 0U; u < rg::kFamilies; ++u) {
    const std::uint32_t* const exact = data.view.exact[u];
    const std::vector<mr8::Posting>& ys = data.y_family[u];
    const std::vector<std::uint64_t>& ky = keys.key[2][u];
    const std::vector<std::uint8_t>& hy = keys.hi[2][u];
    for (std::uint32_t s = 0U; s < rg::kFamilies; ++s) {
      const mr8::MicroproblemKey key = mr8::microproblem_for(u, s);
      if (!index.active(key.s, key.t)) {
        continue;
      }
      const std::vector<mr8::Posting>& qs = data.q_family[key.s];
      const std::vector<mr8::Posting>& xs = data.x_family[key.t];
      const std::vector<std::uint64_t>& kq = keys.key[0][key.s];
      const std::vector<std::uint64_t>& kx = keys.key[1][key.t];
      const std::vector<std::uint8_t>& hq = keys.hi[0][key.s];
      const std::vector<std::uint8_t>& hx = keys.hi[1][key.t];
      telemetry.logical_pairs += static_cast<std::uint64_t>(qs.size()) * xs.size();
      const std::size_t divisor = work_divisor == 0U ? 1U : work_divisor;
      const std::size_t x_blocks = (mr8::block_count(xs.size()) + divisor - 1U) / divisor;
      const std::size_t q_blocks = mr8::block_count(qs.size());
      for (std::size_t x_block = 0U; x_block < x_blocks; ++x_block) {
        for (std::size_t q_block = 0U; q_block < q_blocks; ++q_block) {
          mr8::make_tile(data, key, x_block, q_block, tile);
          std::uint64_t candidates = kernel(tile, data.view);
          while (candidates != 0U) {
            const auto lane = static_cast<std::uint32_t>(std::countr_zero(candidates));
            candidates &= candidates - 1U;
            const std::uint32_t row = lane >> 3U;
            const std::uint32_t column = lane & 7U;
            const std::uint32_t value =
                (tile.x[row] + tile.q[column] + tile.kappa) & mr8::kResidualMask;
            if (((exact[value >> 5U] >> (value & 31U)) & 1U) == 0U) {
              continue;
            }
            const std::uint32_t y_group = index.y_lookup(u, value);
            if (y_group == rg::kNoGroup) {
              ++sink.digest.directory_misses;
              continue;
            }
            const tt::ResidualGroup& group = index.y_groups[u][y_group];
            if (group.value != value) {
              ++sink.digest.residual_mismatches;
              continue;
            }
            const std::size_t qi = q_block * 8U + column;
            const std::size_t xi = x_block * 8U + row;
            const std::uint64_t sum = kq[qi] + kx[xi] + key.kappa;
            const std::uint64_t target = sum & residual_mask;
            const std::uint64_t carry = sum >> residual_bits;
            for (std::uint32_t yi = group.begin; yi < group.begin + group.count; ++yi) {
              ++telemetry.projected_hits;
              if (sweep) {
                const std::uint64_t differ = (target ^ ky[yi]) & residual_mask;
                const std::uint32_t common =
                    differ == 0U ? residual_bits
                                 : static_cast<std::uint32_t>(std::countr_zero(differ));
                ++telemetry.sweep[common];
              }
              if (ky[yi] != target) {
                continue;
              }
              ++telemetry.fast_hits;
              if (keys.hi_bits != 0U &&
                  static_cast<std::uint8_t>((hq[qi] + hx[xi] + carry) & hi_mask) != hy[yi]) {
                continue;
              }
              ++telemetry.full_hits;
              sink.emit_triple(qs[qi].id, xs[xi].id, ys[yi].id);
            }
          }
        }
      }
    }
  }
}

}  // namespace ter::a64
