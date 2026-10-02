#include "ter_root_queue_kernels.hpp"

#include "ter_microreport8_kernels.hpp"
#include "ter_radix13_packed.hpp"

#include <algorithm>
#include <bit>

namespace ter::root_generalization {
namespace {

namespace pk = ter::radix13_packed;

constexpr std::uint32_t kMask19 = mr8::kResidualMask;

// Transplanted from the translated-tag harness (group_family): one group per
// run of equal values, postings kept contiguous so multiplicity is the span.
void group_family(const std::vector<mr8::Posting>& postings,
                  std::vector<tt::ResidualGroup>& groups,
                  std::vector<std::uint32_t>* const posting_to_group) {
  groups.clear();
  if (posting_to_group != nullptr) {
    posting_to_group->assign(postings.size(), 0U);
  }
  std::size_t index = 0U;
  while (index < postings.size()) {
    std::size_t run = index + 1U;
    while (run < postings.size() && postings[run].value == postings[index].value) {
      ++run;
    }
    const auto group = static_cast<std::uint32_t>(groups.size());
    groups.push_back(tt::ResidualGroup{postings[index].value,
                                       static_cast<std::uint32_t>(index),
                                       static_cast<std::uint32_t>(run - index)});
    if (posting_to_group != nullptr) {
      for (std::size_t at = index; at < run; ++at) {
        (*posting_to_group)[at] = group;
      }
    }
    index = run;
  }
}

[[nodiscard]] std::uint64_t group_hash(const std::uint32_t s, const std::uint32_t t,
                                       const std::uint32_t q_group,
                                       const std::uint32_t x_group) noexcept {
  return mix64((static_cast<std::uint64_t>(s * kFamilies + t) << 42U) |
               (static_cast<std::uint64_t>(q_group) << 21U) | x_group);
}

// Radix class construction for every family, exactly as the matched sweeps do
// it, inside the caller's timed region.
[[nodiscard]] bool build_radix_tables(const mr8::Dataset& data, RadixTables& tables) {
  bool ok = true;
  std::vector<std::uint32_t> values;
  for (std::uint32_t u = 0U; u < kFamilies; ++u) {
    const std::vector<mr8::Posting>& ys = data.y_family[u];
    values.resize(ys.size());
    for (std::size_t index = 0U; index < ys.size(); ++index) {
      values[index] = ys[index].value;
    }
    ok = rx::build_target_class<kPB>(values.data(), values.size(), tables.targets[u]) && ok;
  }
  for (std::uint32_t u = 0U; u < kFamilies; ++u) {
    rx::build_shifted_views<kPB>(
        tables.targets[u],
        tables.views.data() + static_cast<std::size_t>(u) * RadixGeom::kViewWordsPerClass);
  }
  for (std::uint32_t t = 0U; t < kFamilies; ++t) {
    const std::vector<mr8::Posting>& xs = data.x_family[t];
    values.resize(xs.size());
    for (std::size_t index = 0U; index < xs.size(); ++index) {
      values[index] = xs[index].value;
    }
    ok = rx::build_source_class<kPB>(values.data(), values.size(), tables.sources[t]) && ok;
  }
  tables.built = ok;
  return ok;
}

using TileKernelFn = std::uint64_t (*)(const mr8::Tile&, const mr8::TargetView&);

[[nodiscard]] TileKernelFn k2_kernel() noexcept {
  return mr8::avx2::translation_unit_is_avx2() ? &mr8::avx2::k2_coarse_gather
                                               : &mr8::k2_coarse_gather_portable;
}

}  // namespace

// ---------------------------------------------------------------------------
// Shared index.
// ---------------------------------------------------------------------------
std::uint32_t RootIndex::y_lookup(const std::uint32_t u,
                                  const std::uint32_t value) const noexcept {
  const std::uint32_t* const exact = view->exact[u];
  const std::uint32_t word = value >> 5U;
  if (((exact[word] >> (value & 31U)) & 1U) == 0U) {
    return kNoGroup;
  }
  const std::uint32_t block = value >> 9U;  // 512 bits == 16 words
  std::uint32_t rank = y_block_rank[u][block];
  for (std::uint32_t index = block * 16U; index < word; ++index) {
    rank += static_cast<std::uint32_t>(std::popcount(exact[index]));
  }
  const std::uint32_t low = value & 31U;
  rank += static_cast<std::uint32_t>(
      std::popcount(exact[word] & ((std::uint32_t{1} << low) - 1U)));
  return rank;
}

void select_microproblems(RootIndex& index, const std::uint32_t count) {
  constexpr std::uint32_t kAll = kFamilies * kFamilies;
  if (count >= kAll) {
    index.selected.fill(1U);
    index.selected_count = kAll;
    return;
  }
  std::array<std::uint32_t, kAll> order{};
  for (std::uint32_t at = 0U; at < kAll; ++at) {
    order[at] = at;
  }
  std::uint64_t state = 0x5EED5EEDULL;
  for (std::uint32_t at = kAll - 1U; at > 0U; --at) {  // Fisher-Yates
    const auto pick = static_cast<std::uint32_t>(mix64(state++) % (at + 1U));
    std::swap(order[at], order[pick]);
  }
  index.selected.fill(0U);
  for (std::uint32_t at = 0U; at < count; ++at) {
    index.selected[order[at]] = 1U;
  }
  index.selected_count = count;
}

void build_root_index(const mr8::Dataset& data, RootIndex& index) {
  index.view = &data.view;
  select_microproblems(index, kFamilies * kFamilies);
  index.query_groups = 0U;
  std::uint32_t q_running = 0U;
  std::uint32_t x_running = 0U;
  for (std::uint32_t family = 0U; family < kFamilies; ++family) {
    group_family(data.q_family[family], index.q_groups[family], &index.q_group_of[family]);
    group_family(data.x_family[family], index.x_groups[family], &index.x_group_of[family]);
    group_family(data.y_family[family], index.y_groups[family], nullptr);
    index.q_offset[family] = q_running;
    index.x_offset[family] = x_running;
    q_running += static_cast<std::uint32_t>(data.q_family[family].size());
    x_running += static_cast<std::uint32_t>(data.x_family[family].size());
    index.query_groups += index.q_groups[family].size() * kFamilies;

    index.y_block_rank[family].assign(mr8::kExactWords / 16U, 0U);
    const std::uint32_t* const exact = data.view.exact[family];
    std::uint32_t running = 0U;
    for (std::size_t block = 0U; block < index.y_block_rank[family].size(); ++block) {
      index.y_block_rank[family][block] = running;
      for (std::size_t word = block * 16U; word < block * 16U + 16U; ++word) {
        running += static_cast<std::uint32_t>(std::popcount(exact[word]));
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Sink.
// ---------------------------------------------------------------------------
void RootSink::emit_triple(const EntryId a, const EntryId b, const EntryId c) noexcept {
  ++digest.triples;
  const std::uint64_t first = mix64(((static_cast<std::uint64_t>(a) << 32U) | b) ^ mix64(c));
  const std::uint64_t second =
      mix64(((static_cast<std::uint64_t>(c) << 32U) | a) ^ mix64(b + 0x632BE59BD9B4E019ULL));
  digest.triple_sum += first;
  digest.triple_xor ^= first;
  digest.triple_sum2 += second;
  if (authority == nullptr) {
    return;
  }
  // Exact, no-false-negative prefilter, then the untruncated authority and the
  // structural rule: the same three stages as the matched n=72 campaign.
  const std::uint32_t sum =
      authority->quotient[0][a] + authority->quotient[1][b] + authority->quotient[2][c];
  if (sum != authority->tau_low32) {
    return;
  }
  ++digest.full_q;
  if (!tt::residue_sum_matches_root(*authority, a, b, c)) {
    return;
  }
  ++digest.residue;
  digest.residue_sum += first;
  if (witnesses != nullptr) {
    witnesses->push_back({a, b, c});
  }
  if (tt::structural_parent_valid(*authority, a, b, c)) {
    ++digest.structural;
  }
}

void RootSink::emit_pair(const std::uint32_t s, const std::uint32_t t,
                         const std::uint32_t q_posting, const std::uint32_t x_posting,
                         const std::uint32_t u, const tt::ResidualGroup& y) {
  const std::uint64_t key =
      (static_cast<std::uint64_t>(index->q_offset[s] + q_posting) << 32U) |
      (index->x_offset[t] + x_posting);
  digest.pair_sum += mix64(key);
  if (pair_keys != nullptr && !pair_keys_overflowed) {
    if (pair_keys->size() < pair_key_limit) {
      pair_keys->push_back(key);
    } else {
      pair_keys_overflowed = true;
      pair_keys->clear();
    }
  }
  const EntryId a = data->q_family[s][q_posting].id;
  const EntryId b = data->x_family[t][x_posting].id;
  const std::vector<mr8::Posting>& ys = data->y_family[u];
  for (std::uint32_t yi = 0U; yi < y.count; ++yi) {
    emit_triple(a, b, ys[y.begin + yi].id);
  }
}

void RootSink::accept_pair(const std::uint32_t s, const std::uint32_t t, const std::uint32_t u,
                           const std::uint32_t q_posting, const std::uint32_t x_posting,
                           const std::uint32_t value) {
  ++digest.pair_hits;
  const std::uint32_t y_group = index->y_lookup(u, value);
  if (y_group == kNoGroup) {
    ++digest.directory_misses;
    return;
  }
  const tt::ResidualGroup& y = index->y_groups[u][y_group];
  if (y.value != value) {
    ++digest.residual_mismatches;
    return;
  }
  if (depth == SinkDepth::GroupOnly) {
    return;
  }
  emit_pair(s, t, q_posting, x_posting, u, y);
}

void RootSink::accept_group(const std::uint32_t s, const std::uint32_t t, const std::uint32_t u,
                            const std::uint32_t kappa, const std::uint32_t q_group,
                            const std::uint32_t x_group, const std::uint32_t y_group) {
  const tt::ResidualGroup& q = index->q_groups[s][q_group];
  const tt::ResidualGroup& x = index->x_groups[t][x_group];
  const tt::ResidualGroup& y = index->y_groups[u][y_group];
  if (((q.value + x.value + kappa) & kMask19) != y.value) {
    ++digest.residual_mismatches;
    return;
  }
  ++digest.group_emissions;
  digest.group_sum += group_hash(s, t, q_group, x_group);
  digest.pair_hits += static_cast<std::uint64_t>(q.count) * x.count;
  if (depth == SinkDepth::GroupOnly) {
    return;
  }
  for (std::uint32_t qi = 0U; qi < q.count; ++qi) {
    for (std::uint32_t xi = 0U; xi < x.count; ++xi) {
      emit_pair(s, t, q.begin + qi, x.begin + xi, u, y);
    }
  }
}

// ---------------------------------------------------------------------------
// K2.  The frozen kernel through a function pointer, the same tile enumeration
// and the same exact survivor drain as the matched n=72 campaign.
// ---------------------------------------------------------------------------
void run_k2(const mr8::Dataset& data, RootSink& sink) {
  const TileKernelFn kernel = k2_kernel();
  mr8::Tile tile{};
  for (std::uint32_t u = 0U; u < kFamilies; ++u) {
    const std::uint32_t* const exact = data.view.exact[u];
    for (std::uint32_t s = 0U; s < kFamilies; ++s) {
      const mr8::MicroproblemKey key = mr8::microproblem_for(u, s);
      if (!sink.index->active(key.s, key.t)) {
        continue;
      }
      const std::size_t x_blocks = mr8::block_count(data.x_family[key.t].size());
      const std::size_t q_blocks = mr8::block_count(data.q_family[key.s].size());
      for (std::size_t x_block = 0U; x_block < x_blocks; ++x_block) {
        for (std::size_t q_block = 0U; q_block < q_blocks; ++q_block) {
          mr8::make_tile(data, key, x_block, q_block, tile);
          std::uint64_t candidates = kernel(tile, data.view);
          while (candidates != 0U) {
            const auto lane = static_cast<std::uint32_t>(std::countr_zero(candidates));
            candidates &= candidates - 1U;
            const std::uint32_t row = lane >> 3U;
            const std::uint32_t column = lane & 7U;
            const std::uint32_t value = (tile.x[row] + tile.q[column] + tile.kappa) & kMask19;
            if (((exact[value >> 5U] >> (value & 31U)) & 1U) != 0U) {
              sink.accept_pair(key.s, key.t, u,
                               static_cast<std::uint32_t>(q_block * 8U) + column,
                               static_cast<std::uint32_t>(x_block * 8U) + row, value);
            }
          }
        }
      }
    }
  }
}

std::uint64_t run_k2_kernel_only(const mr8::Dataset& data, const RootIndex& index) {
  const TileKernelFn kernel = k2_kernel();
  mr8::Tile tile{};
  std::uint64_t candidates_total = 0U;
  for (std::uint32_t u = 0U; u < kFamilies; ++u) {
    for (std::uint32_t s = 0U; s < kFamilies; ++s) {
      const mr8::MicroproblemKey key = mr8::microproblem_for(u, s);
      if (!index.active(key.s, key.t)) {
        continue;
      }
      const std::size_t x_blocks = mr8::block_count(data.x_family[key.t].size());
      const std::size_t q_blocks = mr8::block_count(data.q_family[key.s].size());
      for (std::size_t x_block = 0U; x_block < x_blocks; ++x_block) {
        for (std::size_t q_block = 0U; q_block < q_blocks; ++q_block) {
          mr8::make_tile(data, key, x_block, q_block, tile);
          candidates_total += static_cast<std::uint64_t>(std::popcount(kernel(tile, data.view)));
        }
      }
    }
  }
  return candidates_total;
}

std::uint64_t run_k2_survivor_only(const mr8::Dataset& data, const RootIndex& index) {
  const TileKernelFn kernel = k2_kernel();
  mr8::Tile tile{};
  std::uint64_t survivors = 0U;
  for (std::uint32_t u = 0U; u < kFamilies; ++u) {
    const std::uint32_t* const exact = data.view.exact[u];
    for (std::uint32_t s = 0U; s < kFamilies; ++s) {
      const mr8::MicroproblemKey key = mr8::microproblem_for(u, s);
      if (!index.active(key.s, key.t)) {
        continue;
      }
      const std::size_t x_blocks = mr8::block_count(data.x_family[key.t].size());
      const std::size_t q_blocks = mr8::block_count(data.q_family[key.s].size());
      for (std::size_t x_block = 0U; x_block < x_blocks; ++x_block) {
        for (std::size_t q_block = 0U; q_block < q_blocks; ++q_block) {
          mr8::make_tile(data, key, x_block, q_block, tile);
          std::uint64_t candidates = kernel(tile, data.view);
          while (candidates != 0U) {
            const auto lane = static_cast<std::uint32_t>(std::countr_zero(candidates));
            candidates &= candidates - 1U;
            const std::uint32_t row = lane >> 3U;
            const std::uint32_t column = lane & 7U;
            const std::uint32_t value = (tile.x[row] + tile.q[column] + tile.kappa) & kMask19;
            survivors += (exact[value >> 5U] >> (value & 31U)) & 1U;
          }
        }
      }
    }
  }
  return survivors;
}

// ---------------------------------------------------------------------------
// Radix backends.
// ---------------------------------------------------------------------------
void RadixTables::allocate() {
  sources.resize(kFamilies);
  targets.resize(kFamilies);
  views.reset(RadixGeom::kViewWordsPerClass * kFamilies);
  hits.assign(RadixGeom::kHitCapacity, 0U);
}

std::uint64_t run_radix(const mr8::Dataset& data, const RootIndex& index, RadixTables& tables,
                        const RadixRoute route, const RadixStop stop, RootSink& sink) {
  if (!build_radix_tables(data, tables)) {
    return 0U;  // fail-closed: the caller checks tables.built
  }
  const std::uint64_t* const views = tables.views.data();
  if (stop == RadixStop::BuildOnly) {
    return views[kFamilies * RadixGeom::kViewWordsPerClass - 1U] ^
           tables.sources[0].occupancy[0];
  }
  using QueueFn = std::uint32_t (*)(const pk::PackedArgs&, std::uint32_t*);
  using SkipFn = std::uint32_t (*)(const rx::RouteArgs<kPB>&, std::uint32_t*);
  const QueueFn queue_fn = &pk::bmi2::route_query_queued_dense_control;
  const SkipFn skip_fn = &rx::avx2::route_query_skip<kPB>;
  std::uint32_t* const hits = tables.hits.data();
  std::uint64_t checksum = 0U;

  for (std::uint32_t t = 0U; t < kFamilies; ++t) {
    const rx::SourceClass<kPB>& source = tables.sources[t];
    for (std::uint32_t s = 0U; s < kFamilies; ++s) {
      if (!index.active(s, t)) {
        continue;
      }
      const std::uint32_t u = (s + t) & (kFamilies - 1U);
      const std::uint32_t kappa = (s + t) >> mr8::kSplitBits;
      const rx::TargetClass<kPB>& target = tables.targets[u];
      const std::uint64_t* const class_views =
          views + static_cast<std::size_t>(u) * RadixGeom::kViewWordsPerClass;
      const std::vector<tt::ResidualGroup>& queries = index.q_groups[s];
      for (std::uint32_t query = 0U; query < queries.size(); ++query) {
        const std::uint32_t d = (queries[query].value + kappa) & kMask19;
        const rx::QueryPlan<kPB> plan = rx::make_query_plan<kPB>(d);
        const std::uint64_t* const view =
            class_views + static_cast<std::size_t>(plan.shift) * RadixGeom::kViewWords;

        std::uint32_t found = 0U;
        if (route == RadixRoute::QueueDense) {
          // The dense-control route reads occupancy, views, dense tags and the
          // plan only; the payload-deck pointers are never dereferenced on it.
          pk::PackedArgs args;
          args.x_occupancy = source.occupancy.data();
          args.x_tags = source.tags_dense.data();
          args.y_view = view;
          args.y_tags = target.tags_dense.data();
          args.r = plan.r;
          args.alpha = plan.alpha;
          args.word_offset = plan.word_offset;
          if (stop == RadixStop::GuardQueueOnly) {
            checksum += pk::bmi2::queued_guard_only(args);
            continue;
          }
          found = queue_fn(args, hits);
        } else {
          rx::RouteArgs<kPB> args;
          args.x_occupancy = source.occupancy.data();
          args.x_tags = source.tags_dense.data();
          args.y_view = view;
          args.y_tags = target.tags_dense.data();
          args.r = plan.r;
          args.alpha = plan.alpha;
          args.word_offset = plan.word_offset;
          if (stop == RadixStop::GuardQueueOnly) {
            checksum += rx::avx2::guard_only<kPB>(args);
            continue;
          }
          found = skip_fn(args, hits);
        }
        for (std::uint32_t hit = 0U; hit < found; ++hit) {
          const std::uint32_t packed = hits[hit];
          const std::uint32_t position = rx::hit_position<kPB>(packed);
          const std::uint32_t x_group =
              rx::source_group<kPB>(source, position, rx::hit_source_tag<kPB>(packed));
          const std::uint32_t y_position = (position + plan.r) & RadixGeom::kPositionMask;
          const std::uint32_t y_group =
              rx::target_group<kPB>(target, y_position, rx::hit_target_tag<kPB>(packed));
          if (x_group == kNoGroup || y_group == kNoGroup) {
            ++sink.digest.directory_misses;
            continue;
          }
          sink.accept_group(s, t, u, kappa, query, x_group, y_group);
        }
      }
    }
  }
  return checksum;
}

// ---------------------------------------------------------------------------
// Telemetry.
// ---------------------------------------------------------------------------
QueueTelemetry count_telemetry(const mr8::Dataset& data, const RootIndex& index,
                               RadixTables& tables) {
  QueueTelemetry telemetry;
  if (!build_radix_tables(data, tables)) {
    return telemetry;
  }
  const std::uint64_t* const views = tables.views.data();
  for (std::uint32_t t = 0U; t < kFamilies; ++t) {
    const rx::SourceClass<kPB>& source = tables.sources[t];
    for (std::uint32_t s = 0U; s < kFamilies; ++s) {
      if (!index.active(s, t)) {
        continue;
      }
      const std::uint32_t u = (s + t) & (kFamilies - 1U);
      const std::uint32_t kappa = (s + t) >> mr8::kSplitBits;
      const std::uint64_t* const class_views =
          views + static_cast<std::size_t>(u) * RadixGeom::kViewWordsPerClass;
      for (const tt::ResidualGroup& query : index.q_groups[s]) {
        const rx::QueryPlan<kPB> plan = rx::make_query_plan<kPB>((query.value + kappa) & kMask19);
        const std::uint64_t* const view =
            class_views + static_cast<std::size_t>(plan.shift) * RadixGeom::kViewWords;
        ++telemetry.queries;
        telemetry.guard_word_ops += RadixGeom::kWords;
        for (std::uint32_t word = 0U; word < RadixGeom::kWords; ++word) {
          const std::uint64_t m =
              source.occupancy[word] & view[(word + plan.word_offset) & (RadixGeom::kWords - 1U)];
          if (m == 0U) {
            continue;
          }
          const auto bits = static_cast<std::uint32_t>(std::popcount(m));
          ++telemetry.nonzero_guard_words;
          telemetry.candidate_bits += bits;
          ++telemetry.popcount_histogram[bits];
        }
      }
    }
  }
  const TileKernelFn kernel = k2_kernel();
  mr8::for_each_tile(data, [&](const mr8::Tile& tile, const mr8::MicroproblemKey& key) {
    if (!index.active(key.s, key.t)) {
      return;
    }
    ++telemetry.k2_tiles;
    telemetry.k2_coarse_candidates +=
        static_cast<std::uint64_t>(std::popcount(kernel(tile, data.view)));
  });
  telemetry.ok = true;
  return telemetry;
}

// ---------------------------------------------------------------------------
// Oracle.
// ---------------------------------------------------------------------------
void run_oracle(const mr8::Dataset& data, const RootIndex& index, RootSink& sink) {
  // Own value -> group table from the raw Y postings: group g is the g-th
  // distinct value in ascending order, the numbering every backend uses.
  std::vector<std::int32_t> value_to_group(mr8::kResidualUniverse, -1);
  for (std::uint32_t u = 0U; u < kFamilies; ++u) {
    const std::vector<mr8::Posting>& ys = data.y_family[u];
    std::int32_t groups = 0;
    for (std::size_t at = 0U; at < ys.size(); ++at) {
      if (at == 0U || ys[at].value != ys[at - 1U].value) {
        value_to_group[ys[at].value] = groups++;
      }
    }
    for (std::uint32_t s = 0U; s < kFamilies; ++s) {
      const mr8::MicroproblemKey key = mr8::microproblem_for(u, s);
      if (!index.active(key.s, key.t)) {
        continue;
      }
      const std::vector<tt::ResidualGroup>& qs = index.q_groups[key.s];
      const std::vector<tt::ResidualGroup>& xs = index.x_groups[key.t];
      for (std::uint32_t qg = 0U; qg < qs.size(); ++qg) {
        const std::uint32_t base = qs[qg].value + key.kappa;
        for (std::uint32_t xg = 0U; xg < xs.size(); ++xg) {
          const std::int32_t group = value_to_group[(base + xs[xg].value) & kMask19];
          if (group >= 0) {
            sink.accept_group(key.s, key.t, u, key.kappa, qg, xg,
                              static_cast<std::uint32_t>(group));
          }
        }
      }
    }
    for (const mr8::Posting& posting : ys) {
      value_to_group[posting.value] = -1;
    }
  }
}

}  // namespace ter::root_generalization
