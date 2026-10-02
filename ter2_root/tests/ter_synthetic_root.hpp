#pragma once

// Synthetic TER^2 roots with an independent brute-force oracle, shared by the
// A64-CHQR and ARWLF tests.
//
// The oracle decides the root equation on whole Q-bit values with its own
// two-word addition: no digits, leaves, residual split or carry recurrence.

#include "ter_a64_chqr.hpp"
#include "ter_microreport8_kernels.hpp"
#include "ter_root_generalization_workload.hpp"
#include "ter_root_queue_kernels.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace ter::synthetic_root {

namespace a64 = ter::a64;
namespace rg = ter::root_generalization;
namespace mr8 = ter::microreport8;

struct Value {  // a Q-bit value: bits [0, 64) and [64, Q)
  std::uint64_t lo = 0;
  std::uint64_t hi = 0;
};

[[nodiscard]] inline Value mask_value(Value v, const std::uint32_t bits) {
  if (bits < 64U) {
    return Value{v.lo & ((std::uint64_t{1} << bits) - 1U), 0U};
  }
  v.hi &= (std::uint64_t{1} << (bits - 64U)) - 1U;
  return v;
}

[[nodiscard]] inline Value add(const Value a, const Value b, const std::uint32_t bits) {
  const std::uint64_t lo = a.lo + b.lo;
  const std::uint64_t carry = lo < a.lo ? 1U : 0U;
  return mask_value(Value{lo, a.hi + b.hi + carry}, bits);
}

struct Rng {
  std::uint64_t state;
  std::uint64_t next() { return rg::mix64(state++); }
};

// A synthetic root: three lists of Q-bit values (list 2 already holds d), the
// donor-shaped dataset (families, Y bitmaps, view), its index and full keys.
struct Synthetic {
  std::uint32_t bits = 0;
  std::array<std::vector<Value>, 3> lists;
  std::unique_ptr<mr8::Dataset> data;
  rg::RootIndex index;
  a64::FamilyKeys keys;
};

inline void build_keys(Synthetic& root);

inline void finish(Synthetic& root) {
  root.data = std::make_unique<mr8::Dataset>();
  mr8::Dataset& data = *root.data;
  std::array<std::array<std::vector<mr8::Posting>, rg::kFamilies>*, 3> families{
      &data.q_family, &data.x_family, &data.y_family};
  for (std::size_t list = 0U; list < 3U; ++list) {
    for (std::size_t id = 0U; id < root.lists[list].size(); ++id) {
      const std::uint64_t lo = root.lists[list][id].lo;
      (*families[list])[lo & 63U].push_back(
          mr8::Posting{static_cast<std::uint32_t>((lo >> 6U) & mr8::kResidualMask),
                       static_cast<ter::EntryId>(id)});
    }
    for (auto& family : *families[list]) {
      std::stable_sort(family.begin(), family.end(),
                       [](const mr8::Posting& a, const mr8::Posting& b) { return a.value < b.value; });
    }
  }
  for (std::uint32_t u = 0U; u < rg::kFamilies; ++u) {
    mr8::build_target(data.y_family[u], data.targets[u]);
    data.view.exact[u] = data.targets[u].exact.data();
    data.view.coarse[u] = data.targets[u].coarse.data();
  }
  data.ok = true;
  rg::build_root_index(data, root.index);
  build_keys(root);
}

// Family-aligned full keys of the dataset's current families (EntryId-indexed
// values, so a thinned family keeps exactly its survivors' keys).
inline void build_keys(Synthetic& root) {
  const mr8::Dataset& data = *root.data;
  const std::array<const std::array<std::vector<mr8::Posting>, rg::kFamilies>*, 3> families{
      &data.q_family, &data.x_family, &data.y_family};
  a64::FamilyKeys& keys = root.keys;
  keys = a64::FamilyKeys{};
  keys.full_bits = root.bits;
  keys.fast_bits = std::min<std::uint32_t>(root.bits, 64U);
  keys.residual_bits = keys.fast_bits - 6U;
  keys.hi_bits = root.bits - keys.fast_bits;
  for (std::size_t list = 0U; list < 3U; ++list) {
    for (std::uint32_t f = 0U; f < rg::kFamilies; ++f) {
      for (const mr8::Posting& p : (*families[list])[f]) {
        const Value v = root.lists[list][p.id];
        const std::uint64_t residual =
            keys.residual_bits == 64U ? v.lo >> 6U
                                      : (v.lo >> 6U) & ((std::uint64_t{1} << keys.residual_bits) - 1U);
        keys.key[list][f].push_back(residual);
        keys.hi[list][f].push_back(static_cast<std::uint8_t>(v.hi));
      }
    }
  }
  keys.ok = true;
}

// Random values with small families so the brute force is cheap, plus planted
// structure.  pool > 0 draws the low 19 bits (family + 13-bit leaf) of every
// random value from `pool` fixed values, so families and leaves are dense.
inline Synthetic make_root(const std::uint32_t bits, const std::uint64_t seed, const std::size_t size,
                           const std::uint32_t pool = 0U) {
  Synthetic root;
  root.bits = bits;
  Rng rng{seed};
  std::vector<std::uint64_t> lows;
  for (std::uint32_t i = 0U; i < pool; ++i) {
    lows.push_back(rng.next() & 0x7FFFFU);
  }
  const auto random_value = [&]() {
    Value v = mask_value(Value{rng.next(), rng.next()}, bits);
    if (!lows.empty()) {
      v.lo = (v.lo & ~std::uint64_t{0x7FFFF}) | lows[rng.next() % lows.size()];
    }
    return v;
  };
  const Value all_ones = mask_value(Value{~std::uint64_t{0}, ~std::uint64_t{0}}, bits);
  for (std::size_t list = 0U; list < 3U; ++list) {
    for (std::size_t i = 0U; i < size; ++i) {
      root.lists[list].push_back(random_value());
    }
  }
  const auto plant = [&](const Value a, const Value b) {
    root.lists[0].push_back(a);
    root.lists[1].push_back(b);
    root.lists[2].push_back(add(a, b, bits));
  };
  for (int i = 0; i < 40; ++i) {
    plant(random_value(), random_value());
  }
  // Carry chains: a + b rolls over every digit (13-bit digits of the residual
  // and the 6-bit family split), and wraps the whole Q-bit value.
  plant(all_ones, Value{1U, 0U});
  plant(all_ones, all_ones);
  plant(mask_value(Value{~std::uint64_t{0}, 0U}, bits), Value{1U, 0U});  // carry into hi
  for (std::uint32_t boundary : {6U, 19U, 32U, 45U, 58U}) {
    if (boundary >= std::min<std::uint32_t>(bits, 64U)) {
      continue;
    }
    const std::uint64_t low = (std::uint64_t{1} << boundary) - 1U;
    plant(mask_value(Value{low | (rng.next() << boundary), rng.next()}, bits), Value{1U, 0U});
  }
  // Multiplicity: a duplicated a, a duplicated b and a duplicated d.
  {
    const Value a = random_value();
    const Value b = random_value();
    for (int copy = 0; copy < 3; ++copy) {
      root.lists[0].push_back(a);
      root.lists[1].push_back(b);
      root.lists[2].push_back(add(a, b, bits));
    }
  }
  // Adversarial high-bit collisions: equal on the low 64 bits only.
  if (bits > 64U) {
    for (int i = 0; i < 20; ++i) {
      const Value a = random_value();
      const Value b = random_value();
      Value d = add(a, b, bits);
      d.hi ^= 1U + static_cast<std::uint64_t>(i % 31);
      d = mask_value(d, bits);
      root.lists[0].push_back(a);
      root.lists[1].push_back(b);
      root.lists[2].push_back(d);
    }
  }
  finish(root);
  return root;
}

inline rg::RootSink make_sink(const Synthetic& root) {
  rg::RootSink sink;
  sink.data = root.data.get();
  sink.index = &root.index;
  sink.reset();
  return sink;
}

// Brute force over whole values.
inline rg::OutputDigest oracle(const Synthetic& root) {
  rg::RootSink sink = make_sink(root);
  const mr8::Dataset& data = *root.data;
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
            const Value d = root.lists[2][y.id];
            if (sum.lo == d.lo && sum.hi == d.hi) {
              sink.emit_triple(q.id, x.id, y.id);
            }
          }
        }
      }
    }
  }
  return sink.digest;
}

[[nodiscard]] inline bool same_triples(const rg::OutputDigest& a, const rg::OutputDigest& b) {
  return a.triples == b.triples && a.triple_sum == b.triple_sum && a.triple_xor == b.triple_xor &&
         a.triple_sum2 == b.triple_sum2;
}

}  // namespace ter::synthetic_root
