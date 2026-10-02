// TER^2 radix occupancy locator: builders, scalar reference, portable kernels.
//
// This translation unit is compiled portably.  Only ter_radix13_locator_avx2.cpp
// is compiled for AVX2, so the builders and the driver cannot silently acquire
// AVX2 instruction selection.
//
// Every kernel here is a template over the position width and is explicitly
// instantiated at the bottom for exactly the radices the benchmark sweeps.  A
// radix comparison therefore runs one kernel source, not two, so a difference
// between two radices cannot be a difference between two implementations.

#include "ter_radix13_locator.hpp"

#include <algorithm>
#include <limits>

namespace ter::radix {
namespace {

// Groups ascending residual values, keeping every posting span intact.
// Multiplicity is never collapsed: the span is what a match expands to.
template <unsigned PB, typename Class>
[[nodiscard]] bool build_class(const std::uint32_t* const values, const std::size_t count,
                               Class& out) {
  using G = Geom<PB>;
  out.groups.clear();
  out.occupancy.fill(0U);
  out.tags_dense.assign(G::kR, 0U);
  out.group_base.assign(G::kR, 0U);
  out.group_ids.clear();
  out.tags_compact.clear();
  out.occupied = 0U;
  out.distinct = 0U;
  out.postings = count;

  std::size_t index = 0U;
  while (index < count) {
    std::size_t run = index + 1U;
    while (run < count && values[run] == values[index]) {
      ++run;
    }
    const std::uint32_t value = values[index];
    if (value > kResidualMask) {
      return false;  // outside the 19-bit residual universe
    }
    out.groups.push_back(tt::ResidualGroup{value, static_cast<std::uint32_t>(index),
                                           static_cast<std::uint32_t>(run - index)});
    const std::uint32_t position = value & G::kPositionMask;
    const std::uint32_t tag = value >> G::kPositionBits;
    out.occupancy[position >> 6U] |= std::uint64_t{1} << (position & 63U);
    out.tags_dense[position] |= std::uint64_t{1} << tag;
    index = run;
  }
  out.distinct = static_cast<std::uint32_t>(out.groups.size());
  if (out.distinct > G::kHitCapacity) {
    return false;  // one query could overrun the witness buffer
  }

  // Position-major slot bases, and the compact tag array in the same order.
  std::uint32_t running = 0U;
  for (std::uint32_t position = 0U; position < G::kR; ++position) {
    out.group_base[position] = running;
    const std::uint64_t word = out.tags_dense[position];
    if (word != 0U) {
      out.tags_compact.push_back(word);
      ++out.occupied;
      running += static_cast<std::uint32_t>(std::popcount(word));
    }
  }
  if (out.occupied > G::kExtractCapacity) {
    return false;  // one query could overrun the extraction buffer
  }
  out.group_ids.assign(out.distinct, kNoGroup);

  // The directory is ascending by value, i.e. ascending by (tag, position);
  // the position-major slots are ascending by (position, tag).  This is the
  // permutation between them, written once at build time.
  for (std::uint32_t group = 0U; group < out.distinct; ++group) {
    const std::uint32_t value = out.groups[group].value;
    const std::uint32_t position = value & G::kPositionMask;
    const std::uint32_t tag = value >> G::kPositionBits;
    const std::uint32_t within = static_cast<std::uint32_t>(
        std::popcount(out.tags_dense[position] & ((std::uint64_t{1} << tag) - 1U)));
    out.group_ids[out.group_base[position] + within] = group;
  }
  for (const std::uint32_t group : out.group_ids) {
    if (group == kNoGroup) {
      return false;  // the permutation did not cover every slot
    }
  }
  return true;
}

}  // namespace

template <unsigned PB>
bool build_source_class(const std::uint32_t* const values, const std::size_t count,
                        SourceClass<PB>& out) {
  using G = Geom<PB>;
  if (!build_class<PB>(values, count, out)) {
    return false;
  }
  std::uint32_t running = 0U;
  for (std::uint32_t word = 0U; word < G::kWords; ++word) {
    out.occupied_prefix[word] = running;
    running += static_cast<std::uint32_t>(std::popcount(out.occupancy[word]));
  }
  return running == out.occupied;
}

template <unsigned PB>
bool build_target_class(const std::uint32_t* const values, const std::size_t count,
                        TargetClass<PB>& out) {
  using G = Geom<PB>;
  if (!build_class<PB>(values, count, out)) {
    return false;
  }
  // Rank of each occupied position among occupied positions.  Unoccupied
  // entries keep the rank of the next occupied position; the guard never emits
  // them, and the dense tag word there is zero in any case.
  out.bucket_rank.assign(G::kR, 0U);
  std::uint32_t running = 0U;
  for (std::uint32_t position = 0U; position < G::kR; ++position) {
    out.bucket_rank[position] = running;
    if (out.tags_dense[position] != 0U) {
      ++running;
    }
  }
  return running == out.occupied;
}

template <unsigned PB>
void build_shifted_views(const TargetClass<PB>& target, std::uint64_t* const out) noexcept {
  using G = Geom<PB>;
  const std::uint64_t* const occupancy = target.occupancy.data();
  for (std::uint32_t shift = 0U; shift < G::kShifts; ++shift) {
    std::uint64_t* const view = out + static_cast<std::size_t>(shift) * G::kViewWords;
    if (shift == 0U) {
      for (std::uint32_t word = 0U; word < G::kWords; ++word) {
        view[word] = occupancy[word];
      }
    } else {
      for (std::uint32_t word = 0U; word < G::kWords; ++word) {
        view[word] = (occupancy[word] >> shift) |
                     (occupancy[(word + 1U) & (G::kWords - 1U)] << (64U - shift));
      }
    }
    // Wrap padding so a 256-bit load starting at any word stays contiguous.
    for (std::uint32_t pad = 0U; pad < G::kSliceWords; ++pad) {
      view[G::kWords + pad] = view[pad];
    }
  }
}

template <unsigned PB>
std::vector<std::uint32_t> route_query_scalar(const SourceClass<PB>& source,
                                              const TargetClass<PB>& target,
                                              const std::uint32_t d) {
  using G = Geom<PB>;
  std::vector<std::uint32_t> hits;
  const std::uint32_t r = d & G::kPositionMask;
  const std::uint32_t alpha = d >> G::kPositionBits;
  for (std::uint32_t position = 0U; position < G::kR; ++position) {
    const std::uint64_t source_word = source.tags_dense[position];
    if (source_word == 0U) {
      continue;
    }
    const std::uint32_t sum = position + r;
    const std::uint32_t j = sum & G::kPositionMask;
    const std::uint32_t carry = sum >> G::kPositionBits;
    const std::uint64_t target_word = target.tags_dense[j];
    if (target_word == 0U) {
      continue;
    }
    for (std::uint32_t tag = 0U; tag < G::kH; ++tag) {
      if (((source_word >> tag) & 1U) == 0U) {
        continue;
      }
      const std::uint32_t b = (tag + alpha + carry) & G::kTagMask;
      if (((target_word >> b) & 1U) != 0U) {
        hits.push_back(pack_hit<PB>(position, tag, b));
      }
    }
  }
  return hits;
}

template <unsigned PB>
std::uint64_t guard_only_portable(const RouteArgs<PB>& args) noexcept {
  using G = Geom<PB>;
  std::uint64_t nonzero = 0U;
  for (std::uint32_t word = 0U; word < G::kWords; word += G::kSliceWords) {
    const std::uint32_t base = (word + args.word_offset) & (G::kWords - 1U);
    const std::uint64_t c0 = args.x_occupancy[word + 0U] & args.y_view[base + 0U];
    const std::uint64_t c1 = args.x_occupancy[word + 1U] & args.y_view[base + 1U];
    const std::uint64_t c2 = args.x_occupancy[word + 2U] & args.y_view[base + 2U];
    const std::uint64_t c3 = args.x_occupancy[word + 3U] & args.y_view[base + 3U];
    nonzero += ((c0 | c1 | c2 | c3) != 0U) ? 1U : 0U;
  }
  return nonzero;
}

template <unsigned PB>
std::uint64_t extract_only_portable(const RouteArgs<PB>& args) noexcept {
  using G = Geom<PB>;
  std::uint64_t accumulator = 0U;
  for (std::uint32_t word = 0U; word < G::kWords; word += G::kSliceWords) {
    const std::uint32_t base = (word + args.word_offset) & (G::kWords - 1U);
    std::uint64_t candidates[4];
    candidates[0] = args.x_occupancy[word + 0U] & args.y_view[base + 0U];
    candidates[1] = args.x_occupancy[word + 1U] & args.y_view[base + 1U];
    candidates[2] = args.x_occupancy[word + 2U] & args.y_view[base + 2U];
    candidates[3] = args.x_occupancy[word + 3U] & args.y_view[base + 3U];
    if ((candidates[0] | candidates[1] | candidates[2] | candidates[3]) == 0U) {
      continue;
    }
    for (std::uint32_t lane = 0U; lane < G::kSliceWords; ++lane) {
      std::uint64_t bits = candidates[lane];
      const std::uint32_t position_base = (word + lane) * 64U;
      while (bits != 0U) {
        const std::uint32_t bit = static_cast<std::uint32_t>(std::countr_zero(bits));
        bits &= bits - 1U;
        const std::uint32_t sum = position_base + bit + args.r;
        accumulator += (sum & G::kPositionMask) + (sum >> G::kPositionBits);
      }
    }
  }
  return accumulator;
}

template <unsigned PB>
std::uint32_t route_query_compact_portable(const RouteArgs<PB>& args,
                                           std::uint32_t* const out) noexcept {
  using G = Geom<PB>;
  std::uint32_t found = 0U;
  for (std::uint32_t word = 0U; word < G::kWords; word += G::kSliceWords) {
    const std::uint32_t base = (word + args.word_offset) & (G::kWords - 1U);
    std::uint64_t candidates[4];
    candidates[0] = args.x_occupancy[word + 0U] & args.y_view[base + 0U];
    candidates[1] = args.x_occupancy[word + 1U] & args.y_view[base + 1U];
    candidates[2] = args.x_occupancy[word + 2U] & args.y_view[base + 2U];
    candidates[3] = args.x_occupancy[word + 3U] & args.y_view[base + 3U];
    if ((candidates[0] | candidates[1] | candidates[2] | candidates[3]) == 0U) {
      continue;
    }
    for (std::uint32_t lane = 0U; lane < G::kSliceWords; ++lane) {
      std::uint64_t bits = candidates[lane];
      const std::uint32_t source_word = word + lane;
      const std::uint32_t position_base = source_word * 64U;
      while (bits != 0U) {
        const std::uint32_t bit = static_cast<std::uint32_t>(std::countr_zero(bits));
        bits &= bits - 1U;
        const std::uint32_t position = position_base + bit;
        const std::uint32_t sum = position + args.r;
        const std::uint32_t j = sum & G::kPositionMask;
        const std::uint32_t carry = sum >> G::kPositionBits;
        const std::uint32_t xr =
            args.x_prefix[source_word] +
            static_cast<std::uint32_t>(std::popcount(args.x_occupancy[source_word] &
                                                     ((std::uint64_t{1} << bit) - 1U)));
        const std::uint32_t yr = args.y_bucket_rank[j];
        std::uint64_t exact =
            rotate_tags<PB>(args.x_tags[xr], (args.alpha + carry) & G::kTagMask) &
            args.y_tags[yr];
        while (exact != 0U) {
          const std::uint32_t b = static_cast<std::uint32_t>(std::countr_zero(exact));
          exact &= exact - 1U;
          out[found++] =
              pack_hit<PB>(position, (b - args.alpha - carry) & G::kTagMask, b);
        }
      }
    }
  }
  return found;
}

template <unsigned PB>
std::uint32_t route_query_dense_portable(const RouteArgs<PB>& args,
                                         std::uint32_t* const out) noexcept {
  using G = Geom<PB>;
  std::uint32_t found = 0U;
  for (std::uint32_t word = 0U; word < G::kWords; word += G::kSliceWords) {
    const std::uint32_t base = (word + args.word_offset) & (G::kWords - 1U);
    std::uint64_t candidates[4];
    candidates[0] = args.x_occupancy[word + 0U] & args.y_view[base + 0U];
    candidates[1] = args.x_occupancy[word + 1U] & args.y_view[base + 1U];
    candidates[2] = args.x_occupancy[word + 2U] & args.y_view[base + 2U];
    candidates[3] = args.x_occupancy[word + 3U] & args.y_view[base + 3U];
    if ((candidates[0] | candidates[1] | candidates[2] | candidates[3]) == 0U) {
      continue;
    }
    for (std::uint32_t lane = 0U; lane < G::kSliceWords; ++lane) {
      std::uint64_t bits = candidates[lane];
      const std::uint32_t position_base = (word + lane) * 64U;
      while (bits != 0U) {
        const std::uint32_t bit = static_cast<std::uint32_t>(std::countr_zero(bits));
        bits &= bits - 1U;
        const std::uint32_t position = position_base + bit;
        const std::uint32_t sum = position + args.r;
        const std::uint32_t j = sum & G::kPositionMask;
        const std::uint32_t carry = sum >> G::kPositionBits;
        std::uint64_t exact =
            rotate_tags<PB>(args.x_tags[position], (args.alpha + carry) & G::kTagMask) &
            args.y_tags[j];
        while (exact != 0U) {
          const std::uint32_t b = static_cast<std::uint32_t>(std::countr_zero(exact));
          exact &= exact - 1U;
          out[found++] =
              pack_hit<PB>(position, (b - args.alpha - carry) & G::kTagMask, b);
        }
      }
    }
  }
  return found;
}

template <unsigned PB>
std::uint32_t route_query_dense_portable_counted(const RouteArgs<PB>& args,
                                                 std::uint32_t* const out,
                                                 Counters& counters) noexcept {
  using G = Geom<PB>;
  std::uint32_t found = 0U;
  for (std::uint32_t word = 0U; word < G::kWords; word += G::kSliceWords) {
    const std::uint32_t base = (word + args.word_offset) & (G::kWords - 1U);
    std::uint64_t candidates[4];
    candidates[0] = args.x_occupancy[word + 0U] & args.y_view[base + 0U];
    candidates[1] = args.x_occupancy[word + 1U] & args.y_view[base + 1U];
    candidates[2] = args.x_occupancy[word + 2U] & args.y_view[base + 2U];
    candidates[3] = args.x_occupancy[word + 3U] & args.y_view[base + 3U];
    ++counters.guard_vectors;
    const std::uint32_t population = static_cast<std::uint32_t>(
        std::popcount(candidates[0]) + std::popcount(candidates[1]) +
        std::popcount(candidates[2]) + std::popcount(candidates[3]));
    ++counters.guard_popcount_histogram[popcount_bucket(population)];
    if (population == 0U) {
      ++counters.guard_zero_vectors;
      continue;
    }
    counters.emitted_buckets += population;
    for (std::uint32_t lane = 0U; lane < G::kSliceWords; ++lane) {
      std::uint64_t bits = candidates[lane];
      const std::uint32_t position_base = (word + lane) * 64U;
      while (bits != 0U) {
        const std::uint32_t bit = static_cast<std::uint32_t>(std::countr_zero(bits));
        bits &= bits - 1U;
        const std::uint32_t position = position_base + bit;
        const std::uint32_t sum = position + args.r;
        const std::uint32_t j = sum & G::kPositionMask;
        const std::uint32_t carry = sum >> G::kPositionBits;
        std::uint64_t exact =
            rotate_tags<PB>(args.x_tags[position], (args.alpha + carry) & G::kTagMask) &
            args.y_tags[j];
        if (exact != 0U) {
          ++counters.nonempty_buckets;
        }
        while (exact != 0U) {
          const std::uint32_t b = static_cast<std::uint32_t>(std::countr_zero(exact));
          exact &= exact - 1U;
          ++counters.witnesses;
          out[found++] =
              pack_hit<PB>(position, (b - args.alpha - carry) & G::kTagMask, b);
        }
      }
    }
  }
  return found;
}

template <unsigned PB>
std::uint32_t route_query_batched_portable(const RouteArgs<PB>& args,
                                           std::uint32_t* const out) noexcept {
  using G = Geom<PB>;
  std::uint32_t positions[G::kExtractCapacity];
  std::uint32_t count = 0U;
  for (std::uint32_t word = 0U; word < G::kWords; word += G::kSliceWords) {
    const std::uint32_t base = (word + args.word_offset) & (G::kWords - 1U);
    std::uint64_t candidates[4];
    candidates[0] = args.x_occupancy[word + 0U] & args.y_view[base + 0U];
    candidates[1] = args.x_occupancy[word + 1U] & args.y_view[base + 1U];
    candidates[2] = args.x_occupancy[word + 2U] & args.y_view[base + 2U];
    candidates[3] = args.x_occupancy[word + 3U] & args.y_view[base + 3U];
    if ((candidates[0] | candidates[1] | candidates[2] | candidates[3]) == 0U) {
      continue;
    }
    for (std::uint32_t lane = 0U; lane < G::kSliceWords; ++lane) {
      std::uint64_t bits = candidates[lane];
      const std::uint32_t position_base = (word + lane) * 64U;
      while (bits != 0U) {
        const std::uint32_t bit = static_cast<std::uint32_t>(std::countr_zero(bits));
        bits &= bits - 1U;
        positions[count++] = position_base + bit;
      }
    }
  }
  std::uint32_t found = 0U;
  for (std::uint32_t index = 0U; index < count; ++index) {
    const std::uint32_t position = positions[index];
    const std::uint32_t sum = position + args.r;
    const std::uint32_t j = sum & G::kPositionMask;
    const std::uint32_t carry = sum >> G::kPositionBits;
    std::uint64_t exact =
        rotate_tags<PB>(args.x_tags[position], (args.alpha + carry) & G::kTagMask) &
        args.y_tags[j];
    while (exact != 0U) {
      const std::uint32_t b = static_cast<std::uint32_t>(std::countr_zero(exact));
      exact &= exact - 1U;
      out[found++] = pack_hit<PB>(position, (b - args.alpha - carry) & G::kTagMask, b);
    }
  }
  return found;
}

// ---------------------------------------------------------------------------
// Explicit instantiation.  PB = 13 is the radix the first campaign measured and
// is kept so that result stays reproducible from this same source; PB = 15 is
// the retune the calibrated cost model pointed at.  PB = 14 and 16 bracket it.
// ---------------------------------------------------------------------------
#define TER_RADIX_INSTANTIATE(PB)                                                        \
  template bool build_source_class<PB>(const std::uint32_t*, std::size_t,                \
                                       SourceClass<PB>&);                                \
  template bool build_target_class<PB>(const std::uint32_t*, std::size_t,                \
                                       TargetClass<PB>&);                                \
  template void build_shifted_views<PB>(const TargetClass<PB>&, std::uint64_t*) noexcept; \
  template std::vector<std::uint32_t> route_query_scalar<PB>(                            \
      const SourceClass<PB>&, const TargetClass<PB>&, std::uint32_t);                     \
  template std::uint64_t guard_only_portable<PB>(const RouteArgs<PB>&) noexcept;          \
  template std::uint64_t extract_only_portable<PB>(const RouteArgs<PB>&) noexcept;        \
  template std::uint32_t route_query_compact_portable<PB>(const RouteArgs<PB>&,          \
                                                          std::uint32_t*) noexcept;      \
  template std::uint32_t route_query_dense_portable<PB>(const RouteArgs<PB>&,            \
                                                        std::uint32_t*) noexcept;        \
  template std::uint32_t route_query_dense_portable_counted<PB>(                         \
      const RouteArgs<PB>&, std::uint32_t*, Counters&) noexcept;                          \
  template std::uint32_t route_query_batched_portable<PB>(const RouteArgs<PB>&,          \
                                                          std::uint32_t*) noexcept;

TER_RADIX_INSTANTIATE(13)
TER_RADIX_INSTANTIATE(14)
TER_RADIX_INSTANTIATE(15)
TER_RADIX_INSTANTIATE(16)

#undef TER_RADIX_INSTANTIATE

}  // namespace ter::radix
