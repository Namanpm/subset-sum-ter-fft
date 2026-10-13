#include "ter_hash_join_root.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>

namespace ter::hash_join_root {

namespace {

struct Slot {
  std::uint64_t key = 0;
  std::uint32_t begin = 0;
  std::uint32_t count = 0;  // zero is empty; key zero itself is valid
};

struct TargetPosting {
  EntryId id = 0;
  std::uint8_t hi = 0;
};

// Integer avalanche followed by linear probing. This is ordinary key hashing,
// not a K2/ARWLF filter; target equality is always the complete W-bit key.
std::size_t bucket(std::uint64_t key, const std::size_t mask) noexcept {
  key ^= key >> 30U;
  key *= 0xBF58476D1CE4E5B9ULL;
  key ^= key >> 27U;
  key *= 0x94D049BB133111EBULL;
  return static_cast<std::size_t>(key ^ (key >> 31U)) & mask;
}

bool table_size(const std::size_t entries, std::size_t& slots) noexcept {
  slots = 0U;
  if (entries == 0U) { return true; }
  if (entries > std::numeric_limits<std::size_t>::max() / 2U) { return false; }
  slots = 2U;
  const std::size_t required = entries * 2U;
  while (slots < required) {
    if (slots > std::numeric_limits<std::size_t>::max() / 2U) { return false; }
    slots *= 2U;
  }
  return true;
}

bool authority_layout(const ter::translated_tag::AuthorityTables& authority) noexcept {
  if (!authority.ok || authority.n == 0U || authority.n > 128U ||
      authority.residue_words != (authority.n + 31U) / 32U ||
      authority.root_target.size() != authority.residue_words) { return false; }
  for (std::size_t l = 0U; l < 3U; ++l) {
    const std::uint64_t count = authority.list_sizes[l];
    if (count > std::numeric_limits<std::size_t>::max() / authority.n ||
        authority.quotient[l].size() != count ||
        authority.residue[l].size() != count * authority.residue_words ||
        authority.digits[l].size() != count * authority.n) { return false; }
  }
  return true;
}

}  // namespace

const char* status_label(const Status status) noexcept {
  switch (status) {
    case Status::Completed: return "Completed";
    case Status::InvalidInput: return "InvalidInput";
    case Status::ResourceLimit: return "ResourceLimit";
  }
  return "Unknown";
}

Status run_hash_join(const mr8::Dataset& data, const rg::RootIndex& index,
                     const a64::FamilyKeys& keys, const Config& config,
                     rg::RootSink& sink, Telemetry* const telemetry) {
  if (telemetry != nullptr) { *telemetry = Telemetry{}; }
  if (!data.ok || !keys.ok || keys.full_bits < 25U || keys.full_bits > 69U ||
      keys.fast_bits != std::min(keys.full_bits, 64U) ||
      keys.residual_bits != keys.fast_bits - 6U || keys.hi_bits != keys.full_bits - keys.fast_bits ||
      sink.data != &data || sink.index != &index || sink.depth != rg::SinkDepth::Full ||
      (sink.authority != nullptr && !authority_layout(*sink.authority))) {
    return Status::InvalidInput;
  }
  const std::uint64_t residual_mask = (std::uint64_t{1} << keys.residual_bits) - 1U;
  const std::uint32_t hi_mask = (1U << keys.hi_bits) - 1U;
  const std::array<const std::array<std::vector<mr8::Posting>, rg::kFamilies>*, 3> families{
      &data.q_family, &data.x_family, &data.y_family};
  // Validate before mutation/allocation, never per pair. Public FamilyKeys can
  // be manually constructed; ok=true alone is not a memory-safety contract.
  for (std::size_t l = 0U; l < 3U; ++l) {
    for (std::uint32_t f = 0U; f < rg::kFamilies; ++f) {
      const auto& postings = (*families[l])[f];
      if (postings.size() != keys.key[l][f].size() || postings.size() != keys.hi[l][f].size()) {
        return Status::InvalidInput;
      }
      for (std::size_t i = 0U; i < postings.size(); ++i) {
        if (keys.key[l][f][i] > residual_mask || keys.hi[l][f][i] > hi_mask ||
            (keys.key[l][f][i] & mr8::kResidualMask) != postings[i].value ||
            (sink.authority != nullptr && postings[i].id >= sink.authority->list_sizes[l])) {
          return Status::InvalidInput;
        }
      }
    }
  }

  std::size_t max_targets = 0U;
  std::uint64_t logical_pairs = 0U;
  std::uint64_t triple_bound = 0U;
  constexpr auto max_count = std::numeric_limits<std::uint64_t>::max();
  for (std::uint32_t s = 0U; s < rg::kFamilies; ++s) {
    for (std::uint32_t t = 0U; t < rg::kFamilies; ++t) {
      if (!index.active(s, t)) { continue; }
      const std::uint64_t nq = data.q_family[s].size();
      const std::uint64_t nx = data.x_family[t].size();
      if (nx != 0U && nq > max_count / nx) { return Status::ResourceLimit; }
      const std::uint64_t pairs = nq * nx;
      if (pairs > max_count - logical_pairs) { return Status::ResourceLimit; }
      logical_pairs += pairs;
      const std::size_t ny = data.y_family[(s + t) & 63U].size();
      if (ny != 0U && pairs > (max_count - triple_bound) / ny) { return Status::ResourceLimit; }
      triple_bound += pairs * ny;  // conservative bound prevents counter wrap
      if (pairs != 0U) { max_targets = std::max(max_targets, ny); }
    }
  }
  if (max_targets > std::numeric_limits<std::uint32_t>::max()) { return Status::ResourceLimit; }
  std::size_t max_slots = 0U;
  if (!table_size(max_targets, max_slots) ||
      max_slots > config.memory_limit_bytes / sizeof(Slot)) { return Status::ResourceLimit; }
  const std::size_t slot_bytes = max_slots * sizeof(Slot);
  if (max_targets > (config.memory_limit_bytes - slot_bytes) / sizeof(TargetPosting)) {
    return Status::ResourceLimit;
  }
  const std::size_t auxiliary_bytes = slot_bytes + max_targets * sizeof(TargetPosting);
  std::vector<Slot> table;
  std::vector<TargetPosting> postings;
  try {
    table.resize(max_slots);
    postings.resize(max_targets);
  } catch (const std::bad_alloc&) {
    return Status::ResourceLimit;
  } catch (const std::length_error&) {
    return Status::ResourceLimit;
  }

  Telemetry out;
  out.logical_pairs = logical_pairs;
  out.auxiliary_bytes = auxiliary_bytes;
  for (std::uint32_t s = 0U; s < rg::kFamilies; ++s) {
    for (std::uint32_t t = 0U; t < rg::kFamilies; ++t) {
      if (!index.active(s, t) || data.q_family[s].empty() || data.x_family[t].empty()) { continue; }
      const std::uint32_t u = (s + t) & 63U;
      const std::uint64_t kappa = (s + t) >> 6U;
      const auto& y = data.y_family[u];
      if (y.empty()) { continue; }
      std::size_t capacity = 0U;
      static_cast<void>(table_size(y.size(), capacity));  // bounded by validated maximum
      const std::size_t mask = capacity - 1U;
      std::fill_n(table.begin(), capacity, Slot{});

      // Counting-sort duplicate layout: count per hash key, prefix offsets,
      // then fill contiguous original-ID/high-bit postings. No deduplication.
      for (const std::uint64_t key : keys.key[2][u]) {
        std::size_t b = bucket(key, mask);
        while (table[b].count != 0U && table[b].key != key) { b = (b + 1U) & mask; }
        table[b].key = key;
        ++table[b].count;
      }
      std::uint32_t offset = 0U;
      for (std::size_t b = 0U; b < capacity; ++b) {
        table[b].begin = offset;
        offset += table[b].count;
      }
      // Keep counts intact as the occupancy marker; use begin as a fill cursor
      // backwards, so no extra per-key cursor allocation is needed.
      for (std::size_t b = 0U; b < capacity; ++b) { table[b].begin += table[b].count; }
      for (std::size_t i = y.size(); i-- > 0U;) {
        const std::uint64_t key = keys.key[2][u][i];
        std::size_t b = bucket(key, mask);
        while (table[b].key != key || table[b].count == 0U) { b = (b + 1U) & mask; }
        postings[--table[b].begin] = TargetPosting{y[i].id, keys.hi[2][u][i]};
      }
      ++out.tables_built;

      const auto& q = data.q_family[s];
      const auto& x = data.x_family[t];
      const auto& q_key = keys.key[0][s];
      const auto& x_key = keys.key[1][t];
      const auto& q_hi = keys.hi[0][s];
      const auto& x_hi = keys.hi[1][t];
      for (std::size_t qi = 0U; qi < q.size(); ++qi) {
        const std::uint64_t addend = q_key[qi] + kappa;
        for (std::size_t xi = 0U; xi < x.size(); ++xi) {
          // W<=58: the sum fits in 59 bits, including kappa.
          const std::uint64_t sum = addend + x_key[xi];
          const std::uint64_t key = sum & residual_mask;
          std::size_t b = bucket(key, mask);
          while (table[b].count != 0U && table[b].key != key) { b = (b + 1U) & mask; }
          const Slot& slot = table[b];
          if (slot.count == 0U) { continue; }
          out.low_hits += slot.count;
          const auto high = static_cast<std::uint32_t>(
              (q_hi[qi] + x_hi[xi] + (sum >> keys.residual_bits)) & hi_mask);
          const std::size_t end = static_cast<std::size_t>(slot.begin) + slot.count;
          for (std::size_t p = slot.begin; p < end; ++p) {
            if (keys.hi_bits != 0U && postings[p].hi != high) { continue; }
            ++out.full_hits;
            sink.emit_triple(q[qi].id, x[xi].id, postings[p].id);
          }
        }
      }
    }
  }
  if (telemetry != nullptr) { *telemetry = out; }
  return Status::Completed;
}

}  // namespace ter::hash_join_root
