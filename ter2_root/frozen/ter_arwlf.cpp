#include "ter_arwlf.hpp"

#include "ter_microreport8_kernels.hpp"

#include <algorithm>
#include <chrono>
#include <limits>

namespace ter::arwlf {
namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] double ms_since(const Clock::time_point start) noexcept {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

[[nodiscard]] std::uint64_t ns_since(const Clock::time_point start) noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
}

[[nodiscard]] std::uint64_t low_mask64(const std::uint32_t bits) noexcept {
  return bits >= 64U ? ~std::uint64_t{0} : (std::uint64_t{1} << bits) - 1U;
}

[[nodiscard]] std::uint32_t low_mask32(const std::uint32_t bits) noexcept {
  return bits >= 32U ? ~std::uint32_t{0} : (std::uint32_t{1} << bits) - 1U;
}

// One donor family partitioned into terminal leaves by residual bits [0, 13).
// Postings keep their donor order inside a leaf, so the layout is deterministic.
struct LeafFamily {
  std::vector<std::uint32_t> offset;    // kLeaves + 1
  std::vector<std::uint32_t> mid;       // residual bits [13, 13 + mid_bits), + kPacket zeros
  std::vector<std::uint32_t> posting;   // index into the donor family
  std::vector<std::uint32_t> nonempty;  // ascending leaf digits with >= 1 entry
};

// Q side of the AVX2 sweep.
struct RowFamily {
  std::vector<std::uint16_t> leaf;
  std::array<std::vector<std::uint32_t>, 2> dup;
};

// X side of the AVX2 sweep.
struct LaneFamily {
  std::vector<std::uint16_t> lo;  // + kPacket zeros: every 16-lane load stays in bounds
  std::vector<LaneSlot> slot;
  std::vector<std::uint32_t> overflow;
};

void build_leaves(const std::vector<std::uint64_t>& keys, const std::uint32_t mid_bits,
                  LeafFamily& out) {
  const std::uint32_t mask = low_mask32(mid_bits);
  out.offset.assign(kLeaves + 1U, 0U);
  for (const std::uint64_t key : keys) {
    ++out.offset[(key & (kLeaves - 1U)) + 1U];
  }
  for (std::uint32_t leaf = 0U; leaf < kLeaves; ++leaf) {
    out.offset[leaf + 1U] += out.offset[leaf];
  }
  out.mid.assign(keys.size() + kPacket, 0U);
  out.posting.assign(keys.size(), 0U);
  std::vector<std::uint32_t> cursor(out.offset.begin(), out.offset.end() - 1);
  for (std::size_t p = 0U; p < keys.size(); ++p) {
    const auto leaf = static_cast<std::uint32_t>(keys[p] & (kLeaves - 1U));
    const std::uint32_t at = cursor[leaf]++;
    out.mid[at] = static_cast<std::uint32_t>(keys[p] >> kLeafBits) & mask;
    out.posting[at] = static_cast<std::uint32_t>(p);
  }
  out.nonempty.clear();
  for (std::uint32_t leaf = 0U; leaf < kLeaves; ++leaf) {
    if (out.offset[leaf + 1U] > out.offset[leaf]) {
      out.nonempty.push_back(leaf);
    }
  }
}

void build_rows(const LeafFamily& f, RowFamily& out) {
  const std::size_t n = f.posting.size();
  out.leaf.resize(n);
  out.dup[0].resize(n);
  out.dup[1].resize(n);
  for (const std::uint32_t leaf : f.nonempty) {
    for (std::uint32_t e = f.offset[leaf]; e < f.offset[leaf + 1U]; ++e) {
      out.leaf[e] = static_cast<std::uint16_t>(leaf);
      out.dup[0][e] = (f.mid[e] & 0xFFFFU) * 0x00010001U;
      out.dup[1][e] = ((f.mid[e] + 1U) & 0xFFFFU) * 0x00010001U;
    }
  }
}

void build_lanes(const LeafFamily& f, LaneFamily& out) {
  const std::size_t n = f.posting.size();
  out.lo.assign(n + kPacket, 0U);
  for (std::size_t e = 0U; e < n; ++e) {
    out.lo[e] = static_cast<std::uint16_t>(f.mid[e]);
  }
  out.slot.resize(kLeaves);
  out.overflow.clear();
  for (std::uint32_t leaf = 0U; leaf < kLeaves; ++leaf) {
    const std::uint32_t count = f.offset[leaf + 1U] - f.offset[leaf];
    const std::uint32_t live = std::min(count, kPacket);
    out.slot[leaf] = LaneSlot{f.offset[leaf], live >= kPacket ? 0x55555555U
                                                             : ((1U << (2U * live)) - 1U) & 0x55555555U};
    if (count > kPacket) {
      out.overflow.push_back(leaf);
    }
  }
}

void build_tables(const LeafFamily& f, std::vector<LeafTables>& tables) {
  tables.assign(kLeaves, LeafTables{});
  for (const std::uint32_t leaf : f.nonempty) {
    LeafTables& t = tables[leaf];
    for (std::uint32_t e = f.offset[leaf]; e < f.offset[leaf + 1U]; ++e) {
      for (std::uint32_t chunk = 0U; chunk < kChunks; ++chunk) {
        const std::uint32_t byte = (f.mid[e] >> (8U * chunk)) & 0xFFU;
        t.word[chunk][byte >> 5U] |= 1U << (byte & 31U);
      }
    }
  }
}

// Per-list family structures, built lazily inside the call (and charged to
// it), then reused by every selected microproblem that touches the family.
struct Cache {
  std::array<std::array<LeafFamily, rg::kFamilies>, 3> leaves;
  std::array<std::array<bool, rg::kFamilies>, 3> built{};
  std::array<RowFamily, rg::kFamilies> rows;          // list 0, AVX2 only
  std::array<LaneFamily, rg::kFamilies> lanes;        // list 1, AVX2 only
  std::array<std::vector<LeafTables>, rg::kFamilies> tables;  // list 2
  std::uint64_t bytes = 0;
  std::uint64_t build_ns = 0;

  void ensure(const a64::FamilyKeys& keys, const std::size_t list, const std::uint32_t f,
              const std::uint32_t mid_bits, const bool vector) {
    if (built[list][f]) {
      return;
    }
    const auto start = Clock::now();
    built[list][f] = true;
    LeafFamily& leaf = leaves[list][f];
    build_leaves(keys.key[list][f], mid_bits, leaf);
    bytes += (leaf.offset.size() + leaf.mid.size() + leaf.posting.size() + leaf.nonempty.size()) *
             sizeof(std::uint32_t);
    if (list == 0U && vector) {
      build_rows(leaf, rows[f]);
      bytes += rows[f].leaf.size() * sizeof(std::uint16_t) + 2U * rows[f].dup[0].size() * sizeof(std::uint32_t);
    } else if (list == 1U && vector) {
      build_lanes(leaf, lanes[f]);
      bytes += lanes[f].lo.size() * sizeof(std::uint16_t) + lanes[f].slot.size() * sizeof(LaneSlot) +
               lanes[f].overflow.size() * sizeof(std::uint32_t);
    } else if (list == 2U) {
      build_tables(leaf, tables[f]);
      bytes += tables[f].size() * sizeof(LeafTables);
    }
    build_ns += ns_since(start);
  }
};

// Reference sweep: every leaf pair reaching target leaf k, in <= 16 x 16
// packets through the scalar filter.
void sweep_packets(const LeafFamily& qf, const LeafFamily& xf, const std::uint32_t k,
                   const std::uint32_t kappa, const LeafTables& tables, const std::uint32_t chunks,
                   const std::uint32_t mid_mask, std::vector<Survivor>& queue) {
  std::uint16_t lanes[kPacket * kPacket];
  for (const std::uint32_t i : qf.nonempty) {
    const std::uint32_t j = (k - i - kappa) & (kLeaves - 1U);
    const std::uint32_t xb = xf.offset[j];
    const std::uint32_t xe = xf.offset[j + 1U];
    const std::uint32_t carry = (i + j + kappa) >> kLeafBits;
    const std::uint32_t qe = qf.offset[i + 1U];
    for (std::uint32_t q0 = qf.offset[i]; q0 < qe; q0 += kPacket) {
      for (std::uint32_t x0 = xb; x0 < xe; x0 += kPacket) {
        const std::uint32_t found =
            wide_leaf_filter_scalar(&qf.mid[q0], std::min(kPacket, qe - q0), &xf.mid[x0],
                                    std::min(kPacket, xe - x0), carry, tables, chunks, mid_mask, lanes);
        for (std::uint32_t n = 0U; n < found; ++n) {
          queue.push_back(Survivor{q0 + (lanes[n] >> 4U), x0 + (lanes[n] & 15U), carry});
        }
      }
    }
  }
}

// Config::account: the leaf-pair geometry of target leaf k.
void account(const LeafFamily& qf, const LeafFamily& xf, const std::uint32_t k,
             const std::uint32_t kappa, Telemetry& t) {
  for (const std::uint32_t i : qf.nonempty) {
    const std::uint32_t j = (k - i - kappa) & (kLeaves - 1U);
    const std::uint32_t nx = xf.offset[j + 1U] - xf.offset[j];
    if (nx != 0U) {
      const std::uint32_t nq = qf.offset[i + 1U] - qf.offset[i];
      ++t.leaf_triples;
      t.packets += static_cast<std::uint64_t>((nq + kPacket - 1U) / kPacket) * ((nx + kPacket - 1U) / kPacket);
      t.lanes += static_cast<std::uint64_t>(nq) * nx;
    }
  }
}

constexpr double kGuardMinPairs = 65536.0;

[[nodiscard]] bool avx2_usable() noexcept {
  return avx2::translation_unit_is_avx2() && mr8::detect_isa().usable;
}

}  // namespace

const char* status_label(const Status status) noexcept {
  switch (status) {
    case Status::Completed:
      return "Completed";
    case Status::Aborted:
      return "Aborted";
    case Status::NotApplicable:
      return "NotApplicable";
  }
  return "?";
}

const char* backend_label(const Backend backend) noexcept {
  switch (backend) {
    case Backend::QueueDense:
      return "QUEUE_DENSE";
    case Backend::Arwlf:
      return "ARWLF";
    case Backend::K2Wide:
      return "K2_WIDE";
  }
  return "?";
}

std::uint32_t wide_leaf_filter_scalar(const std::uint32_t* const q, const std::uint32_t nq,
                                      const std::uint32_t* const x, const std::uint32_t nx,
                                      const std::uint32_t carry, const LeafTables& tables,
                                      const std::uint32_t chunks, const std::uint32_t mid_mask,
                                      std::uint16_t* const survivors) noexcept {
  std::uint32_t count = 0U;
  for (std::uint32_t qi = 0U; qi < nq; ++qi) {
    for (std::uint32_t xi = 0U; xi < nx; ++xi) {
      const std::uint32_t s = (q[qi] + x[xi] + carry) & mid_mask;
      bool pass = true;
      for (std::uint32_t chunk = 0U; chunk < chunks; ++chunk) {
        const std::uint32_t byte = (s >> (8U * chunk)) & 0xFFU;
        pass = pass && ((tables.word[chunk][byte >> 5U] >> (byte & 31U)) & 1U) != 0U;
      }
      survivors[count] = static_cast<std::uint16_t>(qi * 16U + xi);
      count += pass ? 1U : 0U;
    }
  }
  return count;
}

Status run_arwlf(const mr8::Dataset& data, const rg::RootIndex& index, const a64::FamilyKeys& keys,
                 const Config& config, rg::RootSink& sink, Telemetry* const telemetry_out) {
  Telemetry local;
  Telemetry& telemetry = telemetry_out != nullptr ? *telemetry_out : local;
  const std::uint32_t w = keys.residual_bits;
  // Chunks 0 and 1 (mid bits 0..15) must be real quotient bits.
  if (!keys.ok || w < kLeafBits + 16U) {
    return Status::NotApplicable;
  }
  const std::uint32_t mid_bits = std::min<std::uint32_t>(32U, w - kLeafBits);
  const std::uint32_t mid_mask = low_mask32(mid_bits);
  const std::uint32_t check_bits =
      config.check_bits == 0U ? mid_bits : std::min(config.check_bits, mid_bits);
  const std::uint32_t check_mask = low_mask32(check_bits);
  // A chunk may be used only if all its bits are inside the checked range; the
  // top chunk of a full-width mid is exact too, since bits above mid_bits are
  // zero on both sides.
  const std::uint32_t chunks = check_bits == mid_bits ? (mid_bits + 7U) / 8U : check_bits / 8U;
  const bool vector = config.use_avx2 && chunks >= 2U && avx2_usable();
  telemetry.mid_bits = mid_bits;
  telemetry.check_bits = check_bits;
  telemetry.chunks = chunks;
  telemetry.avx2 = vector;

  const std::uint64_t w_mask = low_mask64(w);
  const auto hi_mask = static_cast<std::uint8_t>((1U << keys.hi_bits) - 1U);
  const std::uint32_t divisor = config.work_divisor == 0U ? 1U : config.work_divisor;
  bool guard_open = config.guard_ns_per_pair > 0.0 || config.guard_survivor_rate > 0.0;
  double pairs_before = 0.0;  // logical pairs covered by finished microproblems

  Cache cache;
  std::vector<Survivor> queue;
  queue.reserve(4096U);
  std::vector<std::uint64_t> pending;  // AVX2 row scratch, grown to the largest Q family
  const auto start = Clock::now();
  const auto finish = [&](const Status status) {
    telemetry.build_ns = cache.build_ns;
    telemetry.index_bytes = cache.bytes;
    return status;
  };
  for (std::uint32_t s = 0U; s < rg::kFamilies; ++s) {
    for (std::uint32_t t = 0U; t < rg::kFamilies; ++t) {
      if (!index.active(s, t)) {
        continue;
      }
      const std::uint32_t u = (s + t) & (rg::kFamilies - 1U);
      const std::uint32_t kappa = (s + t) >> mr8::kSplitBits;
      cache.ensure(keys, 0U, s, mid_bits, vector);
      cache.ensure(keys, 1U, t, mid_bits, vector);
      cache.ensure(keys, 2U, u, mid_bits, vector);
      const LeafFamily& qf = cache.leaves[0][s];
      const LeafFamily& xf = cache.leaves[1][t];
      const LeafFamily& yf = cache.leaves[2][u];
      const std::vector<LeafTables>& ytables = cache.tables[u];
      const std::vector<std::uint64_t>& kq = keys.key[0][s];
      const std::vector<std::uint64_t>& kx = keys.key[1][t];
      const std::vector<std::uint64_t>& ky = keys.key[2][u];
      const std::vector<std::uint8_t>& hq = keys.hi[0][s];
      const std::vector<std::uint8_t>& hx = keys.hi[1][t];
      const std::vector<std::uint8_t>& hy = keys.hi[2][u];
      const double pairs = static_cast<double>(kq.size()) * static_cast<double>(kx.size());
      telemetry.logical_pairs += kq.size() * kx.size();

      RowSweep rs;
      if (vector) {
        const RowFamily& rq = cache.rows[s];
        const LaneFamily& lx = cache.lanes[t];
        rs.q_leaf = rq.leaf.data();
        rs.q_dup[0] = rq.dup[0].data();
        rs.q_dup[1] = rq.dup[1].data();
        rs.q_offset = qf.offset.data();
        rs.q_mid = qf.mid.data();
        rs.q_count = static_cast<std::uint32_t>(kq.size());
        rs.x_lo = lx.lo.data();
        rs.x_slot = lx.slot.data();
        rs.x_offset = xf.offset.data();
        rs.x_mid = xf.mid.data();
        rs.x_overflow = lx.overflow.data();
        rs.x_overflow_count = static_cast<std::uint32_t>(lx.overflow.size());
        if (pending.size() < kq.size() + 1U) {
          pending.resize(kq.size() + 1U);
        }
        rs.pending = pending.data();
        rs.kappa = kappa;
        rs.chunks = chunks;
        rs.mid_mask = mid_mask;
      }
      const std::size_t y_leaves = yf.nonempty.size();
      const std::size_t k_count = (y_leaves + divisor - 1U) / divisor;
      telemetry.main_rows += k_count * kq.size();
      // Guard prefix: the first 1/64 of this microproblem's target leaves.
      const std::size_t guard_at = std::max<std::size_t>(1U, k_count / 64U);
      // Diagonal schedule: everything reaching one Y leaf runs back to back,
      // so that leaf's tables and entries stay hot.
      for (std::size_t kk = 0U; kk < k_count; ++kk) {
        const std::uint32_t k = yf.nonempty[kk];
        const std::uint32_t yb = yf.offset[k];
        const std::uint32_t ye = yf.offset[k + 1U];
        queue.clear();
        if (vector) {
          rs.k = k;
          rs.tables = &ytables[k];
          telemetry.rows += avx2::sweep_rows(rs, queue);
        } else {
          sweep_packets(qf, xf, k, kappa, ytables[k], chunks, mid_mask, queue);
        }
        if (config.account) {
          account(qf, xf, k, kappa, telemetry);
        }
        telemetry.filter_survivors += queue.size();
        // Exact resolver over the (rare) queued lanes.
        for (const Survivor& lane : queue) {
          // Compared under check_mask, a subset of mid_mask: no mask needed.
          const std::uint32_t mid = qf.mid[lane.qa] + xf.mid[lane.xa] + lane.carry;
          const std::uint32_t qp = qf.posting[lane.qa];
          const std::uint32_t xp = xf.posting[lane.xa];
          // key < 2^W, W <= 58: the sum cannot overflow 64 bits.
          const std::uint64_t sum = kq[qp] + kx[xp] + kappa;
          const std::uint64_t value = sum & w_mask;
          const std::uint64_t high_carry = w >= 64U ? 0U : sum >> w;
          for (std::uint32_t ya = yb; ya < ye; ++ya) {
            if (((yf.mid[ya] ^ mid) & check_mask) != 0U) {
              continue;
            }
            ++telemetry.projected_hits;
            const std::uint32_t yp = yf.posting[ya];
            if (ky[yp] != value) {
              continue;
            }
            ++telemetry.fast_hits;
            if (keys.hi_bits != 0U &&
                static_cast<std::uint8_t>((hq[qp] + hx[xp] + high_carry) & hi_mask) != hy[yp]) {
              continue;
            }
            ++telemetry.full_hits;
            sink.emit_triple(data.q_family[s][qp].id, data.x_family[t][xp].id,
                             data.y_family[u][yp].id);
          }
        }
        // Guard checkpoint: once, at the end of the prefix, when enough work
        // has been done to judge it.  Sweep time only (builds excluded).
        if (guard_open && kk + 1U >= guard_at) {
          const double covered =
              pairs_before + pairs * static_cast<double>(kk + 1U) / static_cast<double>(y_leaves);
          if (covered >= kGuardMinPairs) {
            guard_open = false;
            const double sweep_ns =
                static_cast<double>(ns_since(start)) - static_cast<double>(cache.build_ns);
            telemetry.guard_prefix_ns_per_pair = sweep_ns / covered;
            const double rate = static_cast<double>(telemetry.filter_survivors) / covered;
            if ((config.guard_ns_per_pair > 0.0 &&
                 telemetry.guard_prefix_ns_per_pair > config.guard_ns_per_pair) ||
                (config.guard_survivor_rate > 0.0 && rate > config.guard_survivor_rate)) {
              return finish(Status::Aborted);
            }
          }
        }
      }
      pairs_before += y_leaves == 0U ? pairs : pairs * static_cast<double>(k_count) / static_cast<double>(y_leaves);
    }
  }
  return finish(Status::Completed);
}

// ---------------------------------------------------------------------------
// Geometry.
// ---------------------------------------------------------------------------
double selected_packets(const a64::FamilyKeys& keys, const rg::RootIndex& index) {
  std::array<std::array<bool, rg::kFamilies>, 2> used{};
  for (std::uint32_t s = 0U; s < rg::kFamilies; ++s) {
    for (std::uint32_t t = 0U; t < rg::kFamilies; ++t) {
      if (index.active(s, t)) {
        used[0][s] = true;
        used[1][t] = true;
      }
    }
  }
  std::array<std::array<double, rg::kFamilies>, 2> per{};
  std::vector<std::uint32_t> counts(kLeaves, 0U);
  for (std::size_t list = 0U; list < 2U; ++list) {
    for (std::uint32_t f = 0U; f < rg::kFamilies; ++f) {
      if (!used[list][f]) {
        continue;
      }
      std::fill(counts.begin(), counts.end(), 0U);
      for (const std::uint64_t key : keys.key[list][f]) {
        ++counts[key & (kLeaves - 1U)];
      }
      double p = 0.0;
      for (const std::uint32_t c : counts) {
        p += static_cast<double>((c + kPacket - 1U) / kPacket);
      }
      per[list][f] = p;
    }
  }
  double total = 0.0;
  for (std::uint32_t s = 0U; s < rg::kFamilies; ++s) {
    for (std::uint32_t t = 0U; t < rg::kFamilies; ++t) {
      if (index.active(s, t)) {
        total += per[0][s] * per[1][t];
      }
    }
  }
  return total;
}

// ---------------------------------------------------------------------------
// Dispatcher.
// ---------------------------------------------------------------------------
namespace {

// Probe budgets in logical pairs, sized to each candidate's cost per pair.
constexpr double kK2WideProbePairs = 2097152.0;  // 2^21
constexpr double kArwlfProbePairs = 67108864.0;  // 2^26

using Selection = std::array<std::uint8_t, rg::kFamilies * rg::kFamilies>;

// Restores an index's selection when the probes are done.
struct SelectionGuard {
  rg::RootIndex& index;
  Selection selected;
  std::uint32_t count;
  explicit SelectionGuard(rg::RootIndex& i) : index(i), selected(i.selected), count(i.selected_count) {}
  SelectionGuard(const SelectionGuard&) = delete;
  SelectionGuard& operator=(const SelectionGuard&) = delete;
  ~SelectionGuard() {
    index.selected = selected;
    index.selected_count = count;
  }
};

[[nodiscard]] std::uint32_t divisor_for(const double pairs, const double budget) noexcept {
  std::uint32_t d = 1U;
  while (pairs / d > budget) {
    d *= 2U;
  }
  return d;
}

[[nodiscard]] bool has_work(const mr8::Dataset& data, const std::uint32_t cell) noexcept {
  return !data.q_family[cell / rg::kFamilies].empty() && !data.x_family[cell % rg::kFamilies].empty();
}

// Probe block: the selected microproblems with work whose s is among the
// first 8 such s values and whose t is among the first 8 such t values (cell
// order).  A square block reuses each family several times, as a full run
// does; a single selected microproblem is its own block.  Narrows `index`.
void select_probe_block(const mr8::Dataset& data, rg::RootIndex& index) {
  std::array<bool, rg::kFamilies> s_in{};
  std::array<bool, rg::kFamilies> t_in{};
  std::uint32_t s_count = 0U;
  std::uint32_t t_count = 0U;
  for (std::uint32_t cell = 0U; cell < rg::kFamilies * rg::kFamilies; ++cell) {
    if (index.selected[cell] == 0U || !has_work(data, cell)) {
      continue;
    }
    const std::uint32_t s = cell / rg::kFamilies;
    const std::uint32_t t = cell % rg::kFamilies;
    if (!s_in[s] && s_count < 8U) {
      s_in[s] = true;
      ++s_count;
    }
    if (!t_in[t] && t_count < 8U) {
      t_in[t] = true;
      ++t_count;
    }
  }
  index.selected_count = 0U;
  for (std::uint32_t cell = 0U; cell < rg::kFamilies * rg::kFamilies; ++cell) {
    const bool keep = index.selected[cell] != 0U && has_work(data, cell) && s_in[cell / rg::kFamilies] &&
                      t_in[cell % rg::kFamilies];
    index.selected[cell] = keep ? 1U : 0U;
    index.selected_count += keep ? 1U : 0U;
  }
}

[[nodiscard]] double logical_pairs(const mr8::Dataset& data, const rg::RootIndex& index) {
  double total = 0.0;
  for (std::uint32_t s = 0U; s < rg::kFamilies; ++s) {
    for (std::uint32_t t = 0U; t < rg::kFamilies; ++t) {
      if (index.active(s, t)) {
        total += static_cast<double>(data.q_family[s].size()) *
                 static_cast<double>(data.x_family[t].size());
      }
    }
  }
  return total;
}

// Pairs K2_WIDE's X-block cut actually runs with divisor d.
[[nodiscard]] double k2_wide_pairs(const mr8::Dataset& data, const rg::RootIndex& index,
                                   const std::uint32_t d) {
  double total = 0.0;
  for (std::uint32_t s = 0U; s < rg::kFamilies; ++s) {
    for (std::uint32_t t = 0U; t < rg::kFamilies; ++t) {
      if (index.active(s, t)) {
        const std::size_t x = data.x_family[t].size();
        const std::size_t blocks = (mr8::block_count(x) + d - 1U) / d;
        total += static_cast<double>(data.q_family[s].size()) *
                 static_cast<double>(std::min(x, blocks * mr8::kTileDim));
      }
    }
  }
  return total;
}

// ARWLF's main rows (target leaves run x |Q_s|) and the entries its builds
// touch, for the selected microproblems at divisor d.
struct ArwlfWork {
  double rows = 0.0;
  double entries = 0.0;
};

[[nodiscard]] ArwlfWork arwlf_work(const a64::FamilyKeys& keys, const rg::RootIndex& index,
                                   const std::uint32_t d) {
  std::array<std::array<bool, rg::kFamilies>, 3> used{};
  for (std::uint32_t s = 0U; s < rg::kFamilies; ++s) {
    for (std::uint32_t t = 0U; t < rg::kFamilies; ++t) {
      if (index.active(s, t)) {
        used[0][s] = used[1][t] = used[2][(s + t) & (rg::kFamilies - 1U)] = true;
      }
    }
  }
  std::array<double, rg::kFamilies> y_leaves{};
  std::vector<std::uint8_t> seen(kLeaves);
  ArwlfWork work;
  for (std::size_t list = 0U; list < 3U; ++list) {
    for (std::uint32_t f = 0U; f < rg::kFamilies; ++f) {
      if (!used[list][f]) {
        continue;
      }
      work.entries += static_cast<double>(keys.key[list][f].size());
      if (list == 2U) {
        std::fill(seen.begin(), seen.end(), std::uint8_t{0});
        std::uint32_t distinct = 0U;
        for (const std::uint64_t key : keys.key[2][f]) {
          std::uint8_t& mark = seen[key & (kLeaves - 1U)];
          distinct += mark == 0U ? 1U : 0U;
          mark = 1U;
        }
        y_leaves[f] = static_cast<double>((distinct + d - 1U) / d);
      }
    }
  }
  for (std::uint32_t s = 0U; s < rg::kFamilies; ++s) {
    for (std::uint32_t t = 0U; t < rg::kFamilies; ++t) {
      if (index.active(s, t)) {
        work.rows += y_leaves[(s + t) & (rg::kFamilies - 1U)] * static_cast<double>(keys.key[0][s].size());
      }
    }
  }
  return work;
}

}  // namespace

Dispatch dispatch_and_run(const mr8::Dataset& data, rg::RootIndex& index,
                          const a64::FamilyKeys& keys, rg::RadixTables* const tables,
                          rg::RootSink& sink) {
  Dispatch out;
  const auto probe_start = Clock::now();
  constexpr double kInf = std::numeric_limits<double>::infinity();
  const auto slot = [](const Backend b) { return static_cast<std::size_t>(b); };
  // Applicability.
  std::uint64_t max_x = 0U;
  std::uint64_t max_y = 0U;
  for (std::uint32_t f = 0U; f < rg::kFamilies; ++f) {
    max_x = std::max<std::uint64_t>(max_x, index.x_groups[f].size());
    max_y = std::max<std::uint64_t>(max_y, index.y_groups[f].size());
  }
  out.applicable[slot(Backend::QueueDense)] =
      tables != nullptr && max_x <= rg::RadixGeom::kHitCapacity && max_y <= rg::RadixGeom::kHitCapacity;
  // ARWLF only when leaves are dense enough for a packet to amortize: at most
  // one packet per 16 logical pairs (pre-registered).
  const double total_pairs = logical_pairs(data, index);
  out.arwlf_packets_per_pair = total_pairs > 0.0 ? selected_packets(keys, index) / total_pairs : 0.0;
  out.applicable[slot(Backend::Arwlf)] =
      keys.ok && keys.residual_bits >= kLeafBits + 16U && out.arwlf_packets_per_pair <= 1.0 / 16.0;
  out.applicable[slot(Backend::K2Wide)] = keys.ok;
  const ArwlfWork full_work =
      out.applicable[slot(Backend::Arwlf)] ? arwlf_work(keys, index, 1U) : ArwlfWork{};
  out.predicted_ms.fill(kInf);
  {
    const SelectionGuard restore(index);
    select_probe_block(data, index);
    out.probe_microproblems = index.selected_count;
    const double probe_pairs = logical_pairs(data, index);
    if (probe_pairs <= 0.0) {
      // No selected microproblem has work: every backend's answer is empty.
      sink.reset();
      out.probe_ms = ms_since(probe_start);
      return out;
    }
    out.k2_probe_divisor = divisor_for(probe_pairs, kK2WideProbePairs);
    out.arwlf_probe_divisor = divisor_for(probe_pairs, kArwlfProbePairs);
    rg::RootSink scratch = sink;
    scratch.pair_keys = nullptr;

    if (out.applicable[slot(Backend::K2Wide)]) {
      scratch.reset();
      const auto t0 = Clock::now();
      a64::run_k2_wide(data, index, keys, scratch, nullptr, false, out.k2_probe_divisor);
      out.predicted_ms[slot(Backend::K2Wide)] =
          ms_since(t0) * total_pairs / k2_wide_pairs(data, index, out.k2_probe_divisor);
    }
    if (out.applicable[slot(Backend::Arwlf)]) {
      // Builds scale with the entries they touch, the sweep with its rows.
      scratch.reset();
      Config probe_config;
      probe_config.work_divisor = out.arwlf_probe_divisor;
      Telemetry t;
      const auto t0 = Clock::now();
      static_cast<void>(run_arwlf(data, index, keys, probe_config, scratch, &t));
      const double total_ms = ms_since(t0);
      const double build_ms = static_cast<double>(t.build_ns) * 1e-6;
      const ArwlfWork probe_work = arwlf_work(keys, index, out.arwlf_probe_divisor);
      out.predicted_ms[slot(Backend::Arwlf)] =
          build_ms * full_work.entries / probe_work.entries +
          std::max(0.0, total_ms - build_ms) * full_work.rows / std::max(1.0, probe_work.rows);
    }
    if (out.applicable[slot(Backend::QueueDense)]) {
      // The class build covers every family whatever is selected; routing
      // scales with pairs.  Both are short where QUEUE_DENSE applies, so each
      // is the minimum of two runs.
      double build = kInf;
      double full = kInf;
      for (int repeat = 0; repeat < 2; ++repeat) {
        scratch.reset();
        auto t0 = Clock::now();
        static_cast<void>(rg::run_radix(data, index, *tables, rg::RadixRoute::QueueDense,
                                        rg::RadixStop::BuildOnly, scratch));
        build = std::min(build, ms_since(t0));
        scratch.reset();
        t0 = Clock::now();
        static_cast<void>(rg::run_radix(data, index, *tables, rg::RadixRoute::QueueDense,
                                        rg::RadixStop::Full, scratch));
        full = std::min(full, ms_since(t0));
      }
      if (tables->built) {
        out.predicted_ms[slot(Backend::QueueDense)] =
            build + std::max(0.0, full - build) * total_pairs / probe_pairs;
      } else {
        out.applicable[slot(Backend::QueueDense)] = false;
      }
    }
  }
  out.probe_ms = ms_since(probe_start);

  // An inapplicable or non-finite prediction ranks last; K2_WIDE (always
  // applicable) is last among equals, so it is never displaced by a tie.
  const auto rank = [&](const Backend b) {
    const double p = out.predicted_ms[slot(b)];
    return out.applicable[slot(b)] && p >= 0.0 && p < kInf ? p : kInf;
  };
  std::array<Backend, 3> order{Backend::QueueDense, Backend::Arwlf, Backend::K2Wide};
  std::stable_sort(order.begin(), order.end(),
                   [&](const Backend a, const Backend b) { return rank(a) < rank(b); });
  if (rank(order[0]) == kInf) {
    order = {Backend::K2Wide, Backend::K2Wide, Backend::K2Wide};
  }
  out.chosen = order[0];
  const auto run = [&](const Backend backend, const double guard_ns) {
    sink.reset();
    switch (backend) {
      case Backend::QueueDense:
        static_cast<void>(rg::run_radix(data, index, *tables, rg::RadixRoute::QueueDense,
                                        rg::RadixStop::Full, sink));
        return true;
      case Backend::K2Wide:
        a64::run_k2_wide(data, index, keys, sink, nullptr, false);
        return true;
      case Backend::Arwlf: {
        Config config;
        config.guard_ns_per_pair = guard_ns;
        return run_arwlf(data, index, keys, config, sink, nullptr) == Status::Completed;
      }
    }
    return false;
  };
  // Break-even against the runner-up: abort if slower per logical pair.
  const double fallback_ms = rank(order[1]);
  const double guard_ns = out.chosen == Backend::Arwlf && fallback_ms < kInf
                              ? fallback_ms * 1e6 / total_pairs
                              : 0.0;
  out.ran = out.chosen;
  if (!run(out.chosen, guard_ns)) {
    out.guard_aborted = true;
    out.ran = order[1];
    static_cast<void>(run(order[1], 0.0));
  }
  return out;
}

}  // namespace ter::arwlf
