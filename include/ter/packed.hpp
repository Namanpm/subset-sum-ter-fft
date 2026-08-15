#pragma once

#include "ter/model.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ter {

struct TerEntry {
  EntryId id = 0;
  QuotientResidue q = 0;
  std::uint32_t word_offset = 0;
};

static_assert(sizeof(TerEntry) == 12U);

struct PackedTerList {
  ListRole role = ListRole::A;
  std::uint64_t target_m2 = 0;
  std::uint32_t words_per_entry = 0;
  std::uint64_t valid_mask_last = 0;
  std::vector<TerEntry> entries;
  std::vector<std::uint64_t> positive_words;
  std::vector<std::uint64_t> negative_words;
  std::vector<std::uint64_t> residues_m1;
};

struct PackedSnapshot {
  Instance instance{};
  MergeSpec spec{};
  std::array<PackedTerList, 3> lists{};
  bool has_planted_triple = false;
  std::array<EntryId, 3> planted_entry_ids{};
};

struct PackedSnapshotResult {
  Status status = Status::InvalidConfiguration;
  PackedSnapshot snapshot{};
  std::string message;
};

struct PackedSumWord {
  std::uint64_t positive = 0;
  std::uint64_t negative = 0;
  std::uint64_t invalid = 0;
};

[[nodiscard]] PackedSumWord add_packed_word(std::uint64_t a_positive,
                                            std::uint64_t a_negative,
                                            std::uint64_t b_positive,
                                            std::uint64_t b_negative,
                                            std::uint64_t c_positive,
                                            std::uint64_t c_negative) noexcept;

[[nodiscard]] PackedSnapshotResult pack_snapshot(const InputSnapshot& snapshot);

[[nodiscard]] ReferenceTernaryVector unpack_entry(const PackedTerList& list,
                                                  EntryId id,
                                                  std::uint32_t n);

[[nodiscard]] OperationResult audit_packed_snapshot(const PackedSnapshot& snapshot);

}  // namespace ter
