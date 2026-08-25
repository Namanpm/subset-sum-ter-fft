#include "ter_root_capture_adapter.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace ter::root_partition_bench {
namespace {

[[nodiscard]] std::size_t source_word_count(const std::uint32_t n) noexcept {
  return (static_cast<std::size_t>(n) + 31U) / 32U;
}

[[nodiscard]] bool words_fit_width(const std::span<const std::uint32_t> words,
                                   const std::uint32_t n) noexcept {
  const std::size_t expected_words = source_word_count(n);
  if (words.size() > expected_words) {
    return false;
  }
  if (words.empty() || (n % 32U) == 0U || words.size() < expected_words) {
    return true;
  }
  const std::uint32_t top_mask = (std::uint32_t{1} << (n % 32U)) - 1U;
  return (words.back() & ~top_mask) == 0U;
}

}  // namespace

std::uint32_t quotient_low32_from_words(
    const std::span<const std::uint32_t> residue_words_le,
    const std::span<const std::uint32_t> child_target_words_le,
    const std::uint32_t n,
    const std::uint32_t r1) noexcept {
  if (r1 >= n) {
    return 0U;
  }

  const std::uint32_t quotient_bits = std::min<std::uint32_t>(32U, n - r1);
  const std::size_t first_word = static_cast<std::size_t>(r1 / 32U);
  const std::uint32_t bit_offset = r1 % 32U;
  const std::size_t last_source_word = source_word_count(n) - 1U;
  const std::size_t need_through =
      std::min(last_source_word, first_word + (bit_offset == 0U ? 0U : 1U));
  std::uint64_t borrow = 0U;
  std::uint32_t word0 = 0U;
  std::uint32_t word1 = 0U;
  for (std::size_t word = 0U; word <= need_through; ++word) {
    std::uint32_t residue_word =
        word < residue_words_le.size() ? residue_words_le[word] : 0U;
    std::uint32_t target_word =
        word < child_target_words_le.size() ? child_target_words_le[word] : 0U;
    const bool is_partial_top = word == last_source_word && (n % 32U) != 0U;
    const std::uint32_t top_mask = is_partial_top
                                       ? (std::uint32_t{1} << (n % 32U)) - 1U
                                       : std::numeric_limits<std::uint32_t>::max();
    residue_word &= top_mask;
    target_word &= top_mask;
    const std::uint64_t subtrahend = static_cast<std::uint64_t>(target_word) + borrow;
    std::uint32_t difference = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(residue_word) - subtrahend);
    borrow = static_cast<std::uint64_t>(residue_word) < subtrahend ? 1U : 0U;
    difference &= top_mask;
    if (word == first_word) {
      word0 = difference;
    } else if (word == first_word + 1U) {
      word1 = difference;
    }
  }

  std::uint32_t quotient = bit_offset == 0U
                               ? word0
                               : static_cast<std::uint32_t>(
                                     (word0 >> bit_offset) |
                                     (word1 << (32U - bit_offset)));
  if (quotient_bits < 32U) {
    quotient &= (std::uint32_t{1} << quotient_bits) - 1U;
  }
  return quotient;
}

bool capture_list_size_fits_entry_id(const std::uint64_t size) noexcept {
  return size <= std::numeric_limits<EntryId>::max();
}

CaptureRecordsResult capture_list_to_records(const FullTerRootCapture& capture,
                                             const std::size_t list_index) {
  CaptureRecordsResult result;
  try {
    if (capture.status != Status::Completed) {
      result.message = "root capture status is not Completed";
      return result;
    }
    if (list_index >= capture.children.size()) {
      result.message = "captured root list index must identify A, B, or C";
      return result;
    }
    if (capture.parameters.n == 0U || capture.parameters.r1 > capture.parameters.n) {
      result.message = "captured root n/r1 dimensions are invalid";
      return result;
    }
    const auto& children = capture.children[list_index];
    const auto& residues = capture.residues_words_le[list_index];
    const auto& target = capture.child_targets_words_le[list_index];
    if (children.size() != residues.size()) {
      result.message = "captured root child/residue list sizes disagree";
      return result;
    }
    if (!capture_list_size_fits_entry_id(static_cast<std::uint64_t>(children.size()))) {
      result.status = Status::UnsupportedWidth;
      result.message = "captured root list cannot fit the EntryId domain";
      return result;
    }
    if (!words_fit_width(target, capture.parameters.n)) {
      result.message = "captured child target exceeds the n-bit modulus";
      return result;
    }
    result.records.reserve(children.size());
    for (std::size_t index = 0U; index < children.size(); ++index) {
      if (children[index].size() != capture.parameters.n ||
          !words_fit_width(residues[index], capture.parameters.n)) {
        result.records.clear();
        result.message = "captured child entry has malformed dimensions";
        return result;
      }
      result.records.push_back({
          quotient_low32_from_words(residues[index], target, capture.parameters.n,
                                    capture.parameters.r1),
          static_cast<EntryId>(index),
      });
    }
    result.status = Status::Completed;
    return result;
  } catch (const std::bad_alloc&) {
    result.records.clear();
    result.status = Status::ResourceLimit;
    result.message = "captured root record conversion allocation failed";
    return result;
  } catch (const std::length_error& error) {
    result.records.clear();
    result.status = Status::ResourceLimit;
    result.message = error.what();
    return result;
  }
}

}  // namespace ter::root_partition_bench
