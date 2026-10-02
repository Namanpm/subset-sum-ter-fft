#include "ter_translated_tag_locator.hpp"

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <new>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

namespace ter::translated_tag {

namespace {

// Groups a value-ascending family into distinct residual groups, keeping the
// complete posting span of every group.  Duplicates are adjacent because the
// donor stores every family sorted by value.
void group_values(const std::uint32_t* const values, const std::size_t count,
                  std::vector<ResidualGroup>& groups) {
  groups.clear();
  std::size_t index = 0U;
  while (index < count) {
    std::size_t run = index + 1U;
    while (run < count && values[run] == values[index]) {
      ++run;
    }
    groups.push_back(ResidualGroup{values[index], static_cast<std::uint32_t>(index),
                                   static_cast<std::uint32_t>(run - index)});
    index = run;
  }
}

}  // namespace

void AlignedWords::reset(const std::size_t words) {
  release();
  if (words == 0U) {
    return;
  }
  const std::size_t bytes = words * sizeof(std::uint64_t);
#if defined(_MSC_VER)
  void* memory = _aligned_malloc(bytes, 64U);
#else
  void* memory = nullptr;
  if (posix_memalign(&memory, 64U, bytes) != 0) {
    memory = nullptr;
  }
#endif
  if (memory == nullptr) {
    throw std::bad_alloc();
  }
  data_ = static_cast<std::uint64_t*>(memory);
  words_ = words;
  std::fill(data_, data_ + words_, std::uint64_t{0});
}

void AlignedWords::release() noexcept {
  if (data_ != nullptr) {
#if defined(_MSC_VER)
    _aligned_free(data_);
#else
    std::free(data_);
#endif
    data_ = nullptr;
  }
  words_ = 0U;
}

void build_source_class(const std::uint32_t* const values, const std::size_t count,
                        SourceClass& out) {
  group_values(values, count, out.groups);
  for (auto& layer : out.group_at) {
    layer.fill(kNoGroup);
  }
  out.occupancy.fill(0U);
  out.tag.fill(0U);
  out.excess.clear();
  out.primary_values = 0U;
  out.layer_values.fill(0U);
  out.postings = count;

  // Deterministic placement: within a position the groups arrive in ascending
  // value order, so layer 0 holds the smallest value there and layer 1 the next.
  // No hash, no seed, no data-dependent ranking.
  std::array<std::uint32_t, kR> depth{};
  depth.fill(0U);
  for (std::uint32_t group = 0U; group < out.groups.size(); ++group) {
    const std::uint32_t value = out.groups[group].value;
    const std::uint32_t position = value & kPositionMask;
    const std::uint32_t layer = depth[position];
    if (layer >= kLayers) {
      out.excess.push_back(group);
      ++depth[position];
      continue;
    }
    out.group_at[layer][position] = group;
    ++depth[position];
    ++out.primary_values;
    ++out.layer_values[layer];
    const std::uint32_t slice = position / kVectorBits;
    const std::uint32_t within = position % kVectorBits;
    const std::uint32_t word = within / 64U;
    const std::uint64_t bit = std::uint64_t{1} << (within % 64U);
    out.occupancy[occupancy_index(layer, slice) + word] |= bit;
    const std::uint32_t tag = value >> kPositionBits;
    for (std::uint32_t index = 0U; index < kTagBits; ++index) {
      if (((tag >> index) & 1U) != 0U) {
        out.tag[source_tag_index(layer, slice, index) + word] |= bit;
      }
    }
  }
}

void build_target_class(const std::uint32_t* const values, const std::size_t count,
                        TargetClass& out) {
  group_values(values, count, out.groups);
  for (auto& layer : out.group_at) {
    layer.fill(kNoGroup);
  }
  out.plane.fill(0U);
  out.excess.clear();
  out.primary_values = 0U;
  out.layer_values.fill(0U);
  out.postings = count;

  std::array<std::uint32_t, kR> depth{};
  depth.fill(0U);
  for (std::uint32_t group = 0U; group < out.groups.size(); ++group) {
    const std::uint32_t value = out.groups[group].value;
    const std::uint32_t position = value & kPositionMask;
    const std::uint32_t layer = depth[position];
    if (layer >= kLayers) {
      out.excess.push_back(group);
      ++depth[position];
      continue;
    }
    out.group_at[layer][position] = group;
    ++depth[position];
    ++out.primary_values;
    ++out.layer_values[layer];
    const std::uint32_t word = position / 64U;
    const std::uint64_t bit = std::uint64_t{1} << (position % 64U);
    out.plane[target_plane_index(layer, 0U) + word] |= bit;
    const std::uint32_t tag = value >> kPositionBits;
    for (std::uint32_t index = 0U; index < kTagBits; ++index) {
      if (((tag >> index) & 1U) != 0U) {
        out.plane[target_plane_index(layer, 1U + index) + word] |= bit;
      }
    }
  }
}

void build_translation_table(const SourceClass& source, std::uint64_t* const out) noexcept {
  // alpha = 0 is the class's own tags.
  for (std::uint32_t layer = 0U; layer < kLayers; ++layer) {
    for (std::uint32_t slice = 0U; slice < kSlices; ++slice) {
      for (std::uint32_t bit = 0U; bit < kTagBits; ++bit) {
        const std::size_t source_at = source_tag_index(layer, slice, bit);
        const std::size_t out_at = translation_index(0U, layer, slice, bit);
        for (std::uint32_t word = 0U; word < kSliceWords; ++word) {
          out[out_at + word] = source.tag[source_at + word];
        }
      }
    }
  }
  // Bit-sliced increment: adding one to every occupied position's 8-bit tag,
  // modulo 256 because the carry out of bit 7 is dropped.  Positions are
  // independent, so the carry chain never crosses a word.
  for (std::uint32_t alpha = 0U; alpha + 1U < kH; ++alpha) {
    for (std::uint32_t layer = 0U; layer < kLayers; ++layer) {
      for (std::uint32_t slice = 0U; slice < kSlices; ++slice) {
        const std::uint64_t* const occupancy =
            &source.occupancy[occupancy_index(layer, slice)];
        const std::uint64_t* const current =
            out + translation_index(alpha, layer, slice, 0U);
        std::uint64_t* const next = out + translation_index(alpha + 1U, layer, slice, 0U);
        for (std::uint32_t word = 0U; word < kSliceWords; ++word) {
          std::uint64_t carry = occupancy[word];
          for (std::uint32_t bit = 0U; bit < kTagBits; ++bit) {
            const std::uint64_t previous = current[bit * kSliceWords + word];
            next[bit * kSliceWords + word] = previous ^ carry;
            carry &= previous;
          }
        }
      }
    }
  }
}

void build_shifted_views(const TargetClass& target, std::uint64_t* const out) noexcept {
  for (std::uint32_t delta = 0U; delta < 64U; ++delta) {
    for (std::uint32_t layer = 0U; layer < kLayers; ++layer) {
      for (std::uint32_t plane = 0U; plane < kPlanes; ++plane) {
        const std::uint64_t* const source = &target.plane[target_plane_index(layer, plane)];
        std::uint64_t* const destination = out + shifted_view_index(delta, layer, plane);
        for (std::uint32_t word = 0U; word < kWords; ++word) {
          // Never a shift by 64: delta == 0 is the identity copy.
          const std::uint64_t value =
              delta == 0U ? source[word]
                          : ((source[word] >> delta) |
                             (source[(word + 1U) & (kWords - 1U)] << (64U - delta)));
          destination[word] = value;
          destination[word + kWords] = value;
        }
      }
    }
  }
}

std::vector<std::uint32_t> route_query_scalar(const SourceClass& source,
                                              const TargetClass& target,
                                              const std::uint32_t d) {
  std::vector<std::uint32_t> hits;
  for (std::uint32_t y_layer = 0U; y_layer < kLayers; ++y_layer) {
    for (std::uint32_t x_layer = 0U; x_layer < kLayers; ++x_layer) {
      for (std::uint32_t position = 0U; position < kR; ++position) {
        const std::uint32_t x_group = source.group_at[x_layer][position];
        if (x_group == kNoGroup) {
          continue;
        }
        const std::uint32_t x_value = source.groups[x_group].value;
        const std::uint32_t y_value = (x_value + d) & kResidualMask;
        const std::uint32_t y_group = target.group_at[y_layer][y_value & kPositionMask];
        if (y_group != kNoGroup && target.groups[y_group].value == y_value) {
          hits.push_back(pack_hit(position, x_layer, y_layer));
        }
      }
    }
  }
  std::sort(hits.begin(), hits.end());
  return hits;
}

namespace {

template <bool kCount>
[[nodiscard]] std::uint32_t route_portable_impl(const std::uint64_t* const occupancy,
                                                const std::uint64_t* const translation,
                                                const std::uint64_t* const views,
                                                const QueryPlan& plan,
                                                std::uint32_t* const out,
                                                Counters* const counters) noexcept {
  std::uint32_t hits = 0U;
  for (std::uint32_t y_layer = 0U; y_layer < kLayers; ++y_layer) {
    const std::uint64_t* const y_occupancy =
        views + shifted_view_index(plan.delta, y_layer, 0U) + plan.word_offset;
    for (std::uint32_t x_layer = 0U; x_layer < kLayers; ++x_layer) {
      for (std::uint32_t slice = 0U; slice < kSlices; ++slice) {
        const std::uint64_t* const x_occupancy =
            &occupancy[occupancy_index(x_layer, slice)];
        std::uint64_t mask[kSliceWords];
        std::uint64_t any = 0U;
        for (std::uint32_t word = 0U; word < kSliceWords; ++word) {
          mask[word] = x_occupancy[word] & y_occupancy[slice * kSliceWords + word];
          any |= mask[word];
        }
        if constexpr (kCount) {
          ++counters->occupancy_vectors;
        }
        if (any == 0U) {
          if constexpr (kCount) {
            ++counters->occupancy_zero_exits;
          }
          continue;
        }
        const std::uint8_t mode = plan.slice_mode[slice];
        if constexpr (kCount) {
          if (mode == 2U) {
            ++counters->mixed_carry_slices;
          } else {
            ++counters->uniform_carry_slices;
          }
        }
        const std::uint64_t* const low_tags =
            translation + translation_index(plan.alpha_low, x_layer, slice, 0U);
        const std::uint64_t* const high_tags =
            translation + translation_index(plan.alpha_high, x_layer, slice, 0U);
        for (std::uint32_t bit = 0U; bit < kTagBits; ++bit) {
          const std::uint64_t* const y_tag =
              views + shifted_view_index(plan.delta, y_layer, 1U + bit) + plan.word_offset +
              slice * kSliceWords;
          if constexpr (kCount) {
            ++counters->tag_plane_comparisons;
          }
          any = 0U;
          for (std::uint32_t word = 0U; word < kSliceWords; ++word) {
            const std::size_t at = bit * kSliceWords + word;
            std::uint64_t source_tag;
            if (mode == 0U) {
              source_tag = low_tags[at];
            } else if (mode == 1U) {
              source_tag = high_tags[at];
            } else {
              source_tag = (low_tags[at] & plan.low_mask[word]) |
                           (high_tags[at] & ~plan.low_mask[word]);
            }
            mask[word] &= ~(source_tag ^ y_tag[word]);
            any |= mask[word];
          }
          if (any == 0U) {
            if constexpr (kCount) {
              ++counters->tag_early_zero_exits;
            }
            break;
          }
        }
        if (any == 0U) {
          continue;
        }
        if constexpr (kCount) {
          ++counters->final_masks;
        }
        for (std::uint32_t word = 0U; word < kSliceWords; ++word) {
          std::uint64_t bits = mask[word];
          while (bits != 0U) {
            const std::uint32_t offset = static_cast<std::uint32_t>(std::countr_zero(bits));
            bits &= bits - 1U;
            const std::uint32_t position = slice * kVectorBits + word * 64U + offset;
            out[hits] = pack_hit(position, x_layer, y_layer);
            ++hits;
            if constexpr (kCount) {
              ++counters->primary_group_matches;
            }
          }
        }
      }
    }
  }
  return hits;
}

}  // namespace

std::uint32_t route_query_portable(const std::uint64_t* const occupancy,
                                   const std::uint64_t* const translation,
                                   const std::uint64_t* const views,
                                   const QueryPlan& plan,
                                   std::uint32_t* const out) noexcept {
  return route_portable_impl<false>(occupancy, translation, views, plan, out, nullptr);
}

std::uint32_t route_query_portable_counted(const std::uint64_t* const occupancy,
                                           const std::uint64_t* const translation,
                                           const std::uint64_t* const views,
                                           const QueryPlan& plan,
                                           std::uint32_t* const out,
                                           Counters& counters) noexcept {
  return route_portable_impl<true>(occupancy, translation, views, plan, out, &counters);
}

}  // namespace ter::translated_tag
