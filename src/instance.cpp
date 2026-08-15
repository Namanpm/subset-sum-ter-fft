#include "ter/instance.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <utility>

namespace ter {
namespace {

class SplitMix64 {
 public:
  explicit SplitMix64(std::uint64_t seed) noexcept : state_(seed) {}

  [[nodiscard]] std::uint64_t next() noexcept {
    std::uint64_t value = (state_ += 0x9e3779b97f4a7c15ULL);
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
  }

  [[nodiscard]] std::uint64_t bounded(std::uint64_t bound) noexcept {
    if (bound <= 1U) {
      return 0U;
    }
    const std::uint64_t threshold = (std::uint64_t{0} - bound) % bound;
    for (;;) {
      const std::uint64_t value = next();
      if (value >= threshold) {
        return value % bound;
      }
    }
  }

 private:
  std::uint64_t state_;
};

void add_value_to_target(Instance& instance, std::uint32_t value_index) noexcept {
  const std::size_t width = instance.words_per_value;
  const std::size_t base = static_cast<std::size_t>(value_index) * width;
  std::uint64_t carry = 0;
  for (std::size_t word = 0; word < width; ++word) {
    const std::uint64_t original = instance.root_target_le[word];
    const std::uint64_t addend = instance.values_le[base + word];
    const std::uint64_t first = original + addend;
    const std::uint64_t carry_from_first = first < original ? 1U : 0U;
    const std::uint64_t second = first + carry;
    const std::uint64_t carry_from_second = second < first ? 1U : 0U;
    instance.root_target_le[word] = second;
    carry = carry_from_first | carry_from_second;
  }
}

}  // namespace

InstanceResult build_instance(std::uint32_t n, std::uint8_t b1, std::uint64_t seed) {
  InstanceResult result;
  if (n == 0U || n % 2U != 0U || b1 == 0U || b1 >= 64U ||
      static_cast<std::uint32_t>(b1) >= n) {
    result.message = "instance requires positive even n and 0 < b1 < min(n,64)";
    return result;
  }

  const std::uint64_t words_per_value =
      (static_cast<std::uint64_t>(n) + 63U) / 64U;
  if (words_per_value > std::numeric_limits<std::uint32_t>::max() ||
      words_per_value > std::numeric_limits<std::size_t>::max() / n) {
    result.status = Status::ResourceLimit;
    result.message = "instance allocation size exceeds addressable memory";
    return result;
  }

  auto& instance = result.value;
  instance.n = n;
  instance.b1 = b1;
  instance.seed = seed;
  instance.words_per_value = static_cast<std::uint32_t>(words_per_value);
  const std::size_t total_words = static_cast<std::size_t>(n) * instance.words_per_value;
  instance.values_le.resize(total_words);
  instance.residues_m1.resize(n);
  instance.planted_root.assign(n, 0U);
  instance.root_target_le.assign(instance.words_per_value, 0U);

  SplitMix64 random(seed);
  const std::uint32_t high_bits = n % 64U;
  const std::uint64_t high_mask =
      high_bits == 0U ? std::numeric_limits<std::uint64_t>::max()
                      : (std::uint64_t{1} << high_bits) - 1U;
  const std::uint64_t m1_mask = (std::uint64_t{1} << b1) - 1U;

  for (std::uint32_t index = 0; index < n; ++index) {
    const std::size_t base = static_cast<std::size_t>(index) * instance.words_per_value;
    for (std::size_t word = 0; word < instance.words_per_value; ++word) {
      instance.values_le[base + word] = random.next();
    }
    instance.values_le[base + instance.words_per_value - 1U] &= high_mask;
    instance.residues_m1[index] = instance.values_le[base] & m1_mask;
  }

  std::vector<std::uint32_t> permutation(n);
  std::iota(permutation.begin(), permutation.end(), 0U);
  for (std::uint32_t remaining = n; remaining > 1U; --remaining) {
    const std::uint32_t selected =
        static_cast<std::uint32_t>(random.bounded(remaining));
    std::swap(permutation[remaining - 1U], permutation[selected]);
  }
  for (std::uint32_t position = 0; position < n / 2U; ++position) {
    const std::uint32_t selected = permutation[position];
    instance.planted_root[selected] = 1U;
    add_value_to_target(instance, selected);
  }
  instance.root_target_le.back() &= high_mask;

  result.status = Status::Completed;
  return result;
}

}  // namespace ter
