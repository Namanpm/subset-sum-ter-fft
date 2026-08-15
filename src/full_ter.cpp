#include "ter/full_ter.hpp"

#include "ter/packed.hpp"
#include "ter/reference_vector.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <numeric>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace ter {
namespace {

using Clock = std::chrono::steady_clock;

class BigInt {
 public:
  BigInt() = default;
  BigInt(std::uint64_t value) {
    if (value != 0U) {
      words_.push_back(static_cast<std::uint32_t>(value));
      const auto high = static_cast<std::uint32_t>(value >> 32U);
      if (high != 0U) {
        words_.push_back(high);
      }
    }
  }

  [[nodiscard]] bool is_zero() const noexcept { return words_.empty(); }

  [[nodiscard]] std::uint32_t bit_length() const noexcept {
    if (words_.empty()) {
      return 0U;
    }
    return static_cast<std::uint32_t>((words_.size() - 1U) * 32U +
                                      (32U - static_cast<std::uint32_t>(
                                                   std::countl_zero(words_.back()))));
  }

  void multiply_small(std::uint32_t factor) {
    if (factor == 0U || is_zero()) {
      words_.clear();
      return;
    }
    std::uint64_t carry = 0;
    for (auto& word : words_) {
      const std::uint64_t product =
          static_cast<std::uint64_t>(word) * factor + carry;
      word = static_cast<std::uint32_t>(product);
      carry = product >> 32U;
    }
    if (carry != 0U) {
      words_.push_back(static_cast<std::uint32_t>(carry));
    }
  }

  void mask_to(std::uint32_t bits) {
    const std::size_t count = static_cast<std::size_t>(
        (static_cast<std::uint64_t>(bits) + 31U) / 32U);
    if (words_.size() > count) {
      words_.resize(count);
    }
    if (bits != 0U && bits % 32U != 0U && words_.size() == count) {
      words_.back() &= (std::uint32_t{1} << (bits % 32U)) - 1U;
    }
    trim();
  }

  [[nodiscard]] std::string decimal() const {
    if (is_zero()) {
      return "0";
    }
    std::string digits{"0"};
    for (std::uint32_t reverse = bit_length(); reverse > 0U; --reverse) {
      std::uint32_t carry = bit(reverse - 1U) ? 1U : 0U;
      for (std::size_t index = digits.size(); index > 0U; --index) {
        const std::uint32_t value =
            static_cast<std::uint32_t>(digits[index - 1U] - '0') * 2U + carry;
        digits[index - 1U] = static_cast<char>('0' + value % 10U);
        carry = value / 10U;
      }
      if (carry != 0U) {
        digits.insert(digits.begin(), static_cast<char>('0' + carry));
      }
    }
    return digits;
  }

  [[nodiscard]] std::uint64_t to_uint64() const {
    std::uint64_t value = words_.empty() ? 0U : words_[0];
    if (words_.size() > 1U) {
      value |= static_cast<std::uint64_t>(words_[1]) << 32U;
    }
    return value;
  }

  [[nodiscard]] const std::vector<std::uint32_t>& words() const noexcept {
    return words_;
  }

  static BigInt from_words(std::vector<std::uint32_t> words) {
    BigInt value;
    value.words_ = std::move(words);
    value.trim();
    return value;
  }

  friend bool operator==(const BigInt&, const BigInt&) = default;

  friend bool operator<(const BigInt& left, const BigInt& right) noexcept {
    if (left.words_.size() != right.words_.size()) {
      return left.words_.size() < right.words_.size();
    }
    for (std::size_t reverse = left.words_.size(); reverse > 0U; --reverse) {
      if (left.words_[reverse - 1U] != right.words_[reverse - 1U]) {
        return left.words_[reverse - 1U] < right.words_[reverse - 1U];
      }
    }
    return false;
  }

  friend bool operator>(const BigInt& left, const BigInt& right) noexcept {
    return right < left;
  }

 private:
  [[nodiscard]] bool bit(std::uint32_t index) const noexcept {
    const std::size_t word = index / 32U;
    return word < words_.size() &&
           (words_[word] & (std::uint32_t{1} << (index % 32U))) != 0U;
  }

  void trim() {
    while (!words_.empty() && words_.back() == 0U) {
      words_.pop_back();
    }
  }

  std::vector<std::uint32_t> words_;
};

[[nodiscard]] BigInt add_mod(const BigInt& left,
                             const BigInt& right,
                             std::uint32_t bits) {
  const std::size_t count = static_cast<std::size_t>(
      (static_cast<std::uint64_t>(bits) + 31U) / 32U);
  std::vector<std::uint32_t> words(count, 0U);
  std::uint64_t carry = 0;
  for (std::size_t index = 0; index < count; ++index) {
    const std::uint64_t a =
        index < left.words().size() ? left.words()[index] : 0U;
    const std::uint64_t b =
        index < right.words().size() ? right.words()[index] : 0U;
    const std::uint64_t sum = a + b + carry;
    words[index] = static_cast<std::uint32_t>(sum);
    carry = sum >> 32U;
  }
  auto result = BigInt::from_words(std::move(words));
  result.mask_to(bits);
  return result;
}

[[nodiscard]] BigInt subtract_mod(const BigInt& left,
                                  const BigInt& right,
                                  std::uint32_t bits) {
  const std::size_t count = static_cast<std::size_t>(
      (static_cast<std::uint64_t>(bits) + 31U) / 32U);
  std::vector<std::uint32_t> words(count, 0U);
  std::uint64_t borrow = 0;
  for (std::size_t index = 0; index < count; ++index) {
    const std::uint64_t a =
        index < left.words().size() ? left.words()[index] : 0U;
    const std::uint64_t b =
        (index < right.words().size() ? right.words()[index] : 0U) + borrow;
    words[index] = static_cast<std::uint32_t>(a - b);
    borrow = a < b ? 1U : 0U;
  }
  auto result = BigInt::from_words(std::move(words));
  result.mask_to(bits);
  return result;
}

[[nodiscard]] BigInt masked(BigInt value, std::uint32_t bits) {
  value.mask_to(bits);
  return value;
}

class SplitMix64 {
 public:
  explicit SplitMix64(std::uint64_t seed) noexcept : state_(seed) {}

  [[nodiscard]] std::uint64_t next() noexcept {
    std::uint64_t value = (state_ += 0x9e3779b97f4a7c15ULL);
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
  }

 private:
  std::uint64_t state_;
};

struct BigEntry {
  struct PackedDigits {
    std::uint32_t n = 0;
    // Positive words followed by negative words.  The two masks must not
    // overlap; unused high bits in the final word are always zero.
    std::vector<std::uint64_t> words;

    friend bool operator==(const PackedDigits&, const PackedDigits&) = default;
  } digits;
  BigInt residue;
};

using PackedDigits = BigEntry::PackedDigits;

[[nodiscard]] std::uint32_t packed_word_count(const PackedDigits& digits) noexcept {
  return static_cast<std::uint32_t>(digits.words.size() / 2U);
}

[[nodiscard]] PackedDigits pack_digits(std::span<const std::int8_t> source) {
  if (source.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw std::length_error("ternary vector exceeds the packed index range");
  }
  PackedDigits packed;
  packed.n = static_cast<std::uint32_t>(source.size());
  const std::size_t word_count = (source.size() + 63U) / 64U;
  packed.words.assign(word_count * 2U, 0U);
  for (std::size_t coordinate = 0; coordinate < source.size(); ++coordinate) {
    const std::size_t word = coordinate / 64U;
    const std::uint64_t bit = std::uint64_t{1} << (coordinate % 64U);
    if (source[coordinate] == 1) {
      packed.words[word] |= bit;
    } else if (source[coordinate] == -1) {
      packed.words[word_count + word] |= bit;
    } else if (source[coordinate] != 0) {
      throw std::invalid_argument("ternary vector contains a non-ternary digit");
    }
  }
  return packed;
}

[[nodiscard]] ReferenceTernaryVector unpack_digits(const PackedDigits& packed) {
  const std::uint32_t word_count = packed_word_count(packed);
  if (packed.words.size() != static_cast<std::size_t>(word_count) * 2U ||
      word_count != (static_cast<std::uint64_t>(packed.n) + 63U) / 64U) {
    throw std::invalid_argument("packed ternary vector dimensions are invalid");
  }
  ReferenceTernaryVector result(packed.n, 0);
  for (std::uint32_t coordinate = 0; coordinate < packed.n; ++coordinate) {
    const std::uint32_t word = coordinate / 64U;
    const std::uint64_t bit = std::uint64_t{1} << (coordinate % 64U);
    const bool positive = (packed.words[word] & bit) != 0U;
    const bool negative = (packed.words[word_count + word] & bit) != 0U;
    if (positive && negative) {
      throw std::invalid_argument("packed ternary masks overlap");
    }
    result[coordinate] = positive ? 1 : (negative ? -1 : 0);
  }
  return result;
}

[[nodiscard]] bool packed_less(const PackedDigits& left,
                               const PackedDigits& right) noexcept {
  if (left.n != right.n) {
    return left.n < right.n;
  }
  return left.words < right.words;
}

struct PairRecord {
  BigInt residue;
  std::uint32_t first = 0;
  std::uint32_t second = 0;
};

struct PairRecord64 {
  std::uint64_t residue = 0;
  std::uint32_t first = 0;
  std::uint32_t second = 0;
};

struct ProvenanceRecord {
  PackedDigits parent;
  std::uint32_t first = 0;
  std::uint32_t second = 0;
  std::uint32_t third = std::numeric_limits<std::uint32_t>::max();

  friend bool operator==(const ProvenanceRecord&, const ProvenanceRecord&) = default;
};

void canonicalize_provenance(std::vector<ProvenanceRecord>& records) {
  std::sort(records.begin(), records.end(), [](const auto& left, const auto& right) {
    if (left.parent != right.parent) {
      return packed_less(left.parent, right.parent);
    }
    return std::tuple{left.first, left.second, left.third} <
           std::tuple{right.first, right.second, right.third};
  });
}

struct Targets {
  std::array<BigInt, 3> level1;
  std::array<BigInt, 9> level2;
};

struct GuidedTree {
  std::array<ReferenceTernaryVector, 3> level1;
  std::array<ReferenceTernaryVector, 9> level2;
};

[[nodiscard]] std::uint64_t elapsed_ns(Clock::time_point start) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start)
          .count());
}

[[nodiscard]] bool checked_add(std::uint64_t left,
                               std::uint64_t right,
                               std::uint64_t& result) noexcept {
  if (left > std::numeric_limits<std::uint64_t>::max() - right) {
    return false;
  }
  result = left + right;
  return true;
}

[[nodiscard]] bool checked_multiply(std::uint64_t left,
                                    std::uint64_t right,
                                    std::uint64_t& result) noexcept {
  if (left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left) {
    return false;
  }
  result = left * right;
  return true;
}

[[nodiscard]] std::vector<std::uint32_t> primes_through(std::uint32_t limit) {
  std::vector<bool> composite(static_cast<std::size_t>(limit) + 1U, false);
  std::vector<std::uint32_t> primes;
  for (std::uint32_t candidate = 2U; candidate <= limit; ++candidate) {
    if (composite[candidate]) {
      continue;
    }
    primes.push_back(candidate);
    if (candidate <= limit / candidate) {
      for (std::uint32_t multiple = candidate * candidate; multiple <= limit;
           multiple += candidate) {
        composite[multiple] = true;
      }
    }
  }
  return primes;
}

[[nodiscard]] std::uint32_t factorial_prime_exponent(std::uint32_t n,
                                                     std::uint32_t prime) {
  std::uint32_t exponent = 0;
  while (n != 0U) {
    n /= prime;
    exponent += n;
  }
  return exponent;
}

template <std::size_t Size>
void add_multinomial_exponents(std::uint32_t total,
                               const std::array<std::uint32_t, Size>& counts,
                               const std::vector<std::uint32_t>& primes,
                               std::vector<std::uint32_t>& exponents) {
  std::uint32_t used = 0;
  for (const auto count : counts) {
    used += count;
  }
  if (used > total) {
    throw std::invalid_argument("multinomial counts exceed their population");
  }
  for (std::size_t index = 0; index < primes.size(); ++index) {
    std::uint32_t exponent = factorial_prime_exponent(total, primes[index]);
    for (const auto count : counts) {
      exponent -= factorial_prime_exponent(count, primes[index]);
    }
    exponent -= factorial_prime_exponent(total - used, primes[index]);
    exponents[index] += exponent;
  }
}

[[nodiscard]] BigInt from_prime_exponents(
    const std::vector<std::uint32_t>& primes,
    const std::vector<std::uint32_t>& exponents) {
  BigInt result{1U};
  for (std::size_t index = 0; index < primes.size(); ++index) {
    for (std::uint32_t copy = 0; copy < exponents[index]; ++copy) {
      result.multiply_small(primes[index]);
    }
  }
  return result;
}

template <std::size_t Size>
[[nodiscard]] BigInt multinomial(std::uint32_t total,
                                 const std::array<std::uint32_t, Size>& counts) {
  const auto primes = primes_through(total);
  std::vector<std::uint32_t> exponents(primes.size(), 0U);
  add_multinomial_exponents(total, counts, primes, exponents);
  return from_prime_exponents(primes, exponents);
}

[[nodiscard]] std::uint32_t floor_log2(const BigInt& value) {
  if (value.is_zero()) {
    return 0;
  }
  return value.bit_length() - 1U;
}

[[nodiscard]] ExactRepresentationCount exact_count(const BigInt& value) {
  return ExactRepresentationCount{value.decimal(), floor_log2(value)};
}

[[nodiscard]] std::uint32_t rounded_ratio(std::uint32_t n,
                                          std::uint32_t numerator,
                                          std::uint32_t denominator) {
  const std::uint64_t scaled = static_cast<std::uint64_t>(n) * numerator;
  return static_cast<std::uint32_t>((scaled + denominator / 2U) / denominator);
}

[[nodiscard]] BigInt pow2_mask(std::uint32_t bits) {
  if (bits == 0U) {
    return BigInt{};
  }
  const std::size_t count = static_cast<std::size_t>(
      (static_cast<std::uint64_t>(bits) + 31U) / 32U);
  std::vector<std::uint32_t> words(count, std::numeric_limits<std::uint32_t>::max());
  auto result = BigInt::from_words(std::move(words));
  result.mask_to(bits);
  return result;
}

[[nodiscard]] BigInt from_limbs(std::span<const std::uint64_t> limbs) {
  std::vector<std::uint32_t> words;
  words.reserve(limbs.size() * 2U);
  for (const auto limb : limbs) {
    words.push_back(static_cast<std::uint32_t>(limb));
    words.push_back(static_cast<std::uint32_t>(limb >> 32U));
  }
  return BigInt::from_words(std::move(words));
}

[[nodiscard]] BigInt instance_target(const Instance& instance) {
  auto value = from_limbs(instance.root_target_le);
  value.mask_to(instance.n);
  return value;
}

[[nodiscard]] std::vector<BigInt> instance_coefficients(const Instance& instance) {
  std::vector<BigInt> coefficients;
  coefficients.reserve(instance.n);
  for (std::uint32_t index = 0; index < instance.n; ++index) {
    const std::size_t offset =
        static_cast<std::size_t>(index) * instance.words_per_value;
    coefficients.push_back(from_limbs(std::span<const std::uint64_t>(
        instance.values_le.data() + offset, instance.words_per_value)));
  }
  return coefficients;
}

[[nodiscard]] BigInt inner_product_full(const std::vector<BigInt>& coefficients,
                                        std::span<const std::int8_t> digits,
    const BigInt& mask) {
  BigInt result;
  const auto bits = mask.bit_length();
  for (std::size_t index = 0; index < digits.size(); ++index) {
    if (digits[index] == 1) {
      result = add_mod(result, coefficients[index], bits);
    } else if (digits[index] == -1) {
      result = subtract_mod(result, coefficients[index], bits);
    }
  }
  return result;
}

[[nodiscard]] BigInt sample_residue(SplitMix64& random, std::uint32_t bits) {
  const std::uint32_t count = static_cast<std::uint32_t>(
      (static_cast<std::uint64_t>(bits) + 31U) / 32U);
  std::vector<std::uint32_t> words;
  words.reserve(count);
  for (std::uint32_t word = 0; word < count; ++word) {
    const auto random_word = random.next();
    words.push_back(static_cast<std::uint32_t>(
        word % 2U == 0U ? random_word : random_word >> 32U));
  }
  auto value = BigInt::from_words(std::move(words));
  value.mask_to(bits);
  return value;
}

[[nodiscard]] bool counts_equal(const DigitCounts& left,
                                const DigitCounts& right) noexcept {
  return left.negative == right.negative && left.zero == right.zero &&
         left.positive == right.positive;
}

[[nodiscard]] bool next_combination(std::vector<std::uint32_t>& combination,
                                    std::uint32_t n) {
  if (combination.empty()) {
    return false;
  }
  for (std::size_t reverse = combination.size(); reverse > 0U; --reverse) {
    const std::size_t index = reverse - 1U;
    const std::uint32_t maximum =
        n - static_cast<std::uint32_t>(combination.size() - index);
    if (combination[index] < maximum) {
      ++combination[index];
      for (std::size_t tail = index + 1U; tail < combination.size(); ++tail) {
        combination[tail] = combination[tail - 1U] + 1U;
      }
      return true;
    }
  }
  return false;
}

template <typename Callback>
bool for_each_combination(std::uint32_t n,
                          std::uint32_t k,
                          Callback&& callback) {
  if (k == 0U) {
    return callback(std::span<const std::uint32_t>{});
  }
  if (k > n) {
    return true;
  }
  std::vector<std::uint32_t> combination(k);
  std::iota(combination.begin(), combination.end(), 0U);
  do {
    if (!callback(combination)) {
      return false;
    }
  } while (next_combination(combination, n));
  return true;
}

[[nodiscard]] std::vector<ReferenceTernaryVector> enumerate_active_half(
    std::uint32_t n,
    const DigitCounts& counts,
    bool second_half,
    std::uint64_t limit) {
  const std::uint32_t half = n / 2U;
  std::vector<ReferenceTernaryVector> result;
  result.reserve(static_cast<std::size_t>(limit));
  for_each_combination(half, counts.negative, [&](std::span<const std::uint32_t> negative) {
    std::vector<std::uint32_t> available;
    available.reserve(half - counts.negative);
    std::size_t negative_cursor = 0;
    for (std::uint32_t coordinate = 0; coordinate < half; ++coordinate) {
      if (negative_cursor < negative.size() &&
          negative[negative_cursor] == coordinate) {
        ++negative_cursor;
      } else {
        available.push_back(coordinate);
      }
    }
    return for_each_combination(
        static_cast<std::uint32_t>(available.size()),
        counts.positive,
        [&](std::span<const std::uint32_t> positive_indexes) {
          ReferenceTernaryVector digits(n, 0);
          const std::uint32_t offset = second_half ? half : 0U;
          for (const std::uint32_t coordinate : negative) {
            digits[offset + coordinate] = -1;
          }
          for (const std::uint32_t index : positive_indexes) {
            digits[offset + available[index]] = 1;
          }
          result.push_back(std::move(digits));
          return result.size() < limit;
        });
  });
  return result;
}

void canonicalize(std::vector<ReferenceTernaryVector>& vectors) {
  std::sort(vectors.begin(), vectors.end());
  vectors.erase(std::unique(vectors.begin(), vectors.end()), vectors.end());
}

void canonicalize(std::vector<BigEntry>& entries) {
  std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
    return packed_less(left.digits, right.digits);
  });
  entries.erase(std::unique(entries.begin(), entries.end(), [](const auto& left,
                                                               const auto& right) {
                  return left.digits == right.digits;
                }),
                entries.end());
}

[[nodiscard]] std::vector<BigEntry> materialize_entries(
    std::vector<ReferenceTernaryVector> vectors,
    const std::vector<BigInt>& coefficients,
    const BigInt& full_mask) {
  std::vector<BigEntry> result;
  result.reserve(vectors.size());
  for (auto& digits : vectors) {
    const BigInt residue = inner_product_full(coefficients, digits, full_mask);
    result.push_back(BigEntry{pack_digits(digits), residue});
  }
  return result;
}

[[nodiscard]] std::uint64_t estimated_entries_bytes(
    const std::vector<BigEntry>& entries) {
  std::uint64_t bytes = 0;
  for (const auto& entry : entries) {
    std::uint64_t entry_bytes = sizeof(BigEntry);
    std::uint64_t residue_bytes = 0;
    if (!checked_multiply(entry.residue.words().size(),
                          sizeof(std::uint32_t),
                          residue_bytes)) {
      return std::numeric_limits<std::uint64_t>::max();
    }
    std::uint64_t packed_bytes = 0;
    if (!checked_multiply(entry.digits.words.capacity(),
                          sizeof(std::uint64_t),
                          packed_bytes) ||
        !checked_add(entry_bytes, packed_bytes, entry_bytes) ||
        !checked_add(entry_bytes, residue_bytes, entry_bytes) ||
        !checked_add(bytes, entry_bytes, bytes)) {
      return std::numeric_limits<std::uint64_t>::max();
    }
  }
  return bytes;
}

[[nodiscard]] std::uint64_t estimated_bigints_bytes(
    const std::vector<BigInt>& values) {
  std::uint64_t bytes = 0;
  if (!checked_multiply(values.capacity(), sizeof(BigInt), bytes)) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  for (const auto& value : values) {
    std::uint64_t words_bytes = 0;
    if (!checked_multiply(value.words().capacity(),
                          sizeof(std::uint32_t),
                          words_bytes) ||
        !checked_add(bytes, words_bytes, bytes)) {
      return std::numeric_limits<std::uint64_t>::max();
    }
  }
  return bytes;
}

[[nodiscard]] std::uint64_t estimated_targets_bytes(const Targets& targets) {
  std::uint64_t bytes = sizeof(Targets);
  const auto add_value = [&bytes](const BigInt& value) {
    std::uint64_t words_bytes = 0;
    return checked_multiply(value.words().capacity(),
                            sizeof(std::uint32_t),
                            words_bytes) &&
           checked_add(bytes, words_bytes, bytes);
  };
  for (const auto& value : targets.level1) {
    if (!add_value(value)) {
      return std::numeric_limits<std::uint64_t>::max();
    }
  }
  for (const auto& value : targets.level2) {
    if (!add_value(value)) {
      return std::numeric_limits<std::uint64_t>::max();
    }
  }
  return bytes;
}

[[nodiscard]] std::optional<PackedDigits> add_three(
    const PackedDigits& first,
    const PackedDigits& second,
    const PackedDigits& third,
    const DigitCounts& target_counts,
    FullTerStageTelemetry& telemetry) {
  if (first.n != second.n || first.n != third.n ||
      first.words.size() != second.words.size() ||
      first.words.size() != third.words.size()) {
    ++telemetry.structural_rejects;
    return std::nullopt;
  }
  const std::uint32_t word_count = packed_word_count(first);
  DigitCounts counts;
  for (std::uint32_t word = 0; word < word_count; ++word) {
    const auto sum = add_packed_word(first.words[word],
                                     first.words[word_count + word],
                                     second.words[word],
                                     second.words[word_count + word],
                                     third.words[word],
                                     third.words[word_count + word]);
    std::uint64_t valid_mask = std::numeric_limits<std::uint64_t>::max();
    if (word + 1U == word_count && first.n % 64U != 0U) {
      valid_mask = (std::uint64_t{1} << (first.n % 64U)) - 1U;
    }
    if ((sum.invalid & valid_mask) != 0U ||
        ((sum.positive | sum.negative) & ~valid_mask) != 0U) {
      ++telemetry.structural_rejects;
      return std::nullopt;
    }
    counts.positive += static_cast<std::uint32_t>(
        std::popcount(sum.positive & valid_mask));
    counts.negative += static_cast<std::uint32_t>(
        std::popcount(sum.negative & valid_mask));
  }
  counts.zero = first.n - counts.positive - counts.negative;
  if (!counts_equal(counts, target_counts)) {
    ++telemetry.distribution_rejects;
    return std::nullopt;
  }
  PackedDigits parent;
  parent.n = first.n;
  parent.words.assign(first.words.size(), 0U);
  for (std::uint32_t word = 0; word < word_count; ++word) {
    const auto sum = add_packed_word(first.words[word],
                                     first.words[word_count + word],
                                     second.words[word],
                                     second.words[word_count + word],
                                     third.words[word],
                                     third.words[word_count + word]);
    std::uint64_t valid_mask = std::numeric_limits<std::uint64_t>::max();
    if (word + 1U == word_count && first.n % 64U != 0U) {
      valid_mask = (std::uint64_t{1} << (first.n % 64U)) - 1U;
    }
    parent.words[word] = sum.positive & valid_mask;
    parent.words[word_count + word] = sum.negative & valid_mask;
  }
  return parent;
}

[[nodiscard]] std::uint64_t estimated_entry_bytes(std::uint32_t n) noexcept {
  const std::uint64_t packed_words =
      2U * ((static_cast<std::uint64_t>(n) + 63U) / 64U);
  const std::uint64_t residue_words =
      (static_cast<std::uint64_t>(n) + 31U) / 32U;
  return sizeof(BigEntry) + packed_words * sizeof(std::uint64_t) +
         residue_words * sizeof(std::uint32_t);
}

[[nodiscard]] std::vector<BigEntry> binary_mitm(
    const std::vector<BigEntry>& left,
    const std::vector<BigEntry>& right,
    const BigInt& target,
    std::uint32_t modulus_bits,
    const DigitCounts& expected,
    std::uint64_t memory_limit_bytes,
    FullTerStageTelemetry& telemetry,
    std::vector<ProvenanceRecord>* provenance,
    Status& status,
    std::string& message) {
  if (left.empty() || right.empty()) {
    status = Status::Completed;
    return {};
  }
  std::uint64_t pair_candidates = 0;
  if (!checked_multiply(left.size(), right.size(), pair_candidates) ||
      !checked_add(telemetry.pair_candidates,
                   pair_candidates,
                   telemetry.pair_candidates)) {
    status = Status::ResourceLimit;
    message = "L3-to-L2 pair count overflow";
    return {};
  }
  std::vector<std::pair<BigInt, std::uint32_t>> right_index;
  std::uint64_t index_bytes = 0;
  const std::uint64_t index_record_bytes =
      64U + ((static_cast<std::uint64_t>(modulus_bits) + 31U) / 32U) * 4U;
  if (!checked_multiply(right.size(), index_record_bytes, index_bytes) ||
      index_bytes > memory_limit_bytes ||
      left.size() > std::numeric_limits<std::uint32_t>::max() ||
      right.size() > std::numeric_limits<std::uint32_t>::max()) {
    status = Status::ResourceLimit;
    message = "L3-to-L2 residue index exceeds the declared resource limit";
    return {};
  }
  right_index.reserve(right.size());
  const std::uint64_t entry_bytes = estimated_entry_bytes(left.front().digits.n);
  const std::uint64_t maximum_outputs =
      entry_bytes == 0U || index_bytes >= memory_limit_bytes
          ? 0U
          : (memory_limit_bytes - index_bytes) / entry_bytes;
  for (std::uint32_t id = 0; id < right.size(); ++id) {
    right_index.emplace_back(masked(right[id].residue, modulus_bits), id);
  }
  std::sort(right_index.begin(), right_index.end(), [](const auto& a, const auto& b) {
    return a.first < b.first;
  });

  std::vector<BigEntry> result;
  PackedDigits zero;
  zero.n = left.front().digits.n;
  zero.words.assign(left.front().digits.words.size(), 0U);
  for (std::uint32_t first_id = 0; first_id < left.size(); ++first_id) {
    const auto& first = left[first_id];
    const BigInt required = subtract_mod(target, first.residue, modulus_bits);
    const auto lower = std::lower_bound(
        right_index.begin(), right_index.end(), required,
        [](const auto& record, const BigInt& value) { return record.first < value; });
    const auto upper = std::upper_bound(
        lower, right_index.end(), required,
        [](const BigInt& value, const auto& record) { return value < record.first; });
    for (auto current = lower; current != upper; ++current) {
      ++telemetry.modular_candidates;
      auto digits = add_three(first.digits,
                              right[current->second].digits,
                              zero,
                              expected,
                              telemetry);
      if (!digits.has_value()) {
        continue;
      }
      if (result.size() >= maximum_outputs) {
        status = Status::ResourceLimit;
        message = "L3-to-L2 output exceeds the declared memory limit";
        return {};
      }
      result.push_back(BigEntry{
          std::move(*digits),
          add_mod(first.residue,
                  right[current->second].residue,
                  first.digits.n)});
      if (provenance != nullptr) {
        provenance->push_back(ProvenanceRecord{
            result.back().digits, first_id, current->second});
      }
    }
  }
  telemetry.survivors_before_dedup += result.size();
  if (provenance != nullptr) {
    canonicalize_provenance(*provenance);
  }
  canonicalize(result);
  telemetry.survivors_after_dedup += result.size();
  telemetry.allocated_bytes = std::max(telemetry.allocated_bytes, index_bytes);
  status = Status::Completed;
  return result;
}

[[nodiscard]] std::uint64_t native_mask(std::uint32_t bits) noexcept {
  return bits == 64U ? std::numeric_limits<std::uint64_t>::max()
                     : (std::uint64_t{1} << bits) - 1U;
}

[[nodiscard]] std::vector<BigEntry> ternary_mitm_native(
    const std::vector<BigEntry>& first,
    const std::vector<BigEntry>& second,
    const std::vector<BigEntry>& third,
    const BigInt& target,
    std::uint32_t modulus_bits,
    const DigitCounts& expected,
    std::uint64_t memory_limit_bytes,
    FullTerStageTelemetry& telemetry,
    std::vector<ProvenanceRecord>* provenance,
    Status& status,
    std::string& message) {
  std::uint64_t pair_count = 0;
  std::uint64_t pair_bytes = 0;
  if (!checked_multiply(first.size(), second.size(), pair_count) ||
      pair_count > std::numeric_limits<std::size_t>::max() ||
      !checked_multiply(pair_count, sizeof(PairRecord64), pair_bytes) ||
      pair_bytes > memory_limit_bytes ||
      first.size() > std::numeric_limits<std::uint32_t>::max() ||
      second.size() > std::numeric_limits<std::uint32_t>::max()) {
    status = Status::ResourceLimit;
    message = "native ternary pair index exceeds the declared resource limit";
    return {};
  }
  if (!checked_add(telemetry.pair_candidates,
                   pair_count,
                   telemetry.pair_candidates)) {
    status = Status::ResourceLimit;
    message = "native ternary pair count overflow";
    return {};
  }
  const std::uint64_t mask = native_mask(modulus_bits);
  std::vector<PairRecord64> pairs;
  pairs.reserve(static_cast<std::size_t>(pair_count));
  for (std::uint32_t a = 0; a < first.size(); ++a) {
    for (std::uint32_t b = 0; b < second.size(); ++b) {
      pairs.push_back(PairRecord64{
          (first[a].residue.to_uint64() + second[b].residue.to_uint64()) & mask,
          a,
          b});
    }
  }
  std::sort(pairs.begin(), pairs.end(), [](const auto& left, const auto& right) {
    return left.residue < right.residue;
  });

  std::vector<BigEntry> result;
  const std::uint64_t entry_bytes = estimated_entry_bytes(third.front().digits.n);
  const std::uint64_t maximum_outputs =
      entry_bytes == 0U || pair_bytes >= memory_limit_bytes
          ? 0U
          : (memory_limit_bytes - pair_bytes) / entry_bytes;
  if (third.size() > std::numeric_limits<std::uint32_t>::max()) {
    status = Status::ResourceLimit;
    message = "native ternary third-list index exceeds the supported range";
    return {};
  }
  for (std::uint32_t third_id = 0; third_id < third.size(); ++third_id) {
    const auto& c = third[third_id];
    const std::uint64_t required =
        (target.to_uint64() - c.residue.to_uint64()) & mask;
    const auto lower = std::lower_bound(
        pairs.begin(), pairs.end(), required,
        [](const auto& pair, std::uint64_t value) { return pair.residue < value; });
    const auto upper = std::upper_bound(
        lower, pairs.end(), required,
        [](std::uint64_t value, const auto& pair) { return value < pair.residue; });
    for (auto current = lower; current != upper; ++current) {
      ++telemetry.modular_candidates;
      auto parent = add_three(first[current->first].digits,
                              second[current->second].digits,
                              c.digits,
                              expected,
                              telemetry);
      if (!parent.has_value()) {
        continue;
      }
      if (result.size() >= maximum_outputs) {
        status = Status::ResourceLimit;
        message = "native ternary merge output exceeds the declared memory limit";
        return {};
      }
      result.push_back(BigEntry{
          std::move(*parent),
          add_mod(add_mod(first[current->first].residue,
                          second[current->second].residue,
                          c.digits.n),
                  c.residue,
                  c.digits.n)});
      if (provenance != nullptr) {
        provenance->push_back(ProvenanceRecord{result.back().digits,
                                               current->first,
                                               current->second,
                                               third_id});
      }
    }
  }
  telemetry.survivors_before_dedup += result.size();
  if (provenance != nullptr) {
    canonicalize_provenance(*provenance);
  }
  canonicalize(result);
  telemetry.survivors_after_dedup += result.size();
  telemetry.allocated_bytes = std::max(telemetry.allocated_bytes, pair_bytes);
  status = Status::Completed;
  return result;
}

[[nodiscard]] std::vector<BigEntry> ternary_mitm(
    const std::vector<BigEntry>& first,
    const std::vector<BigEntry>& second,
    const std::vector<BigEntry>& third,
    const BigInt& target,
    std::uint32_t modulus_bits,
    const DigitCounts& expected,
    std::uint64_t memory_limit_bytes,
    FullTerStageTelemetry& telemetry,
    std::vector<ProvenanceRecord>* provenance,
    Status& status,
    std::string& message) {
  if (first.empty() || second.empty() || third.empty()) {
    status = Status::Completed;
    return {};
  }
  if (modulus_bits <= 64U) {
    return ternary_mitm_native(first,
                               second,
                               third,
                               target,
                               modulus_bits,
                               expected,
                               memory_limit_bytes,
                               telemetry,
                               provenance,
                               status,
                               message);
  }
  std::uint64_t pair_count = 0;
  std::uint64_t pair_bytes = 0;
  if (!checked_multiply(first.size(), second.size(), pair_count) ||
      pair_count > std::numeric_limits<std::size_t>::max() ||
      !checked_multiply(pair_count,
                        64U + ((static_cast<std::uint64_t>(modulus_bits) + 31U) /
                               32U) *
                                  4U,
                        pair_bytes) ||
      pair_bytes > memory_limit_bytes ||
      first.size() > std::numeric_limits<std::uint32_t>::max() ||
      second.size() > std::numeric_limits<std::uint32_t>::max()) {
    status = Status::ResourceLimit;
    message = "ternary pair index exceeds the declared resource limit";
    return {};
  }
  if (!checked_add(telemetry.pair_candidates,
                   pair_count,
                   telemetry.pair_candidates)) {
    status = Status::ResourceLimit;
    message = "ternary pair count overflow";
    return {};
  }
  std::vector<PairRecord> pairs;
  pairs.reserve(static_cast<std::size_t>(pair_count));
  for (std::uint32_t a = 0; a < first.size(); ++a) {
    for (std::uint32_t b = 0; b < second.size(); ++b) {
      pairs.push_back(PairRecord{
          add_mod(first[a].residue, second[b].residue, modulus_bits), a, b});
    }
  }
  std::sort(pairs.begin(), pairs.end(), [](const auto& left, const auto& right) {
    return left.residue < right.residue;
  });

  std::vector<BigEntry> result;
  const std::uint64_t entry_bytes = estimated_entry_bytes(third.front().digits.n);
  const std::uint64_t maximum_outputs =
      entry_bytes == 0U || pair_bytes >= memory_limit_bytes
          ? 0U
          : (memory_limit_bytes - pair_bytes) / entry_bytes;
  if (third.size() > std::numeric_limits<std::uint32_t>::max()) {
    status = Status::ResourceLimit;
    message = "ternary third-list index exceeds the supported range";
    return {};
  }
  for (std::uint32_t third_id = 0; third_id < third.size(); ++third_id) {
    const auto& c = third[third_id];
    const BigInt required = subtract_mod(target, c.residue, modulus_bits);
    const auto lower = std::lower_bound(
        pairs.begin(), pairs.end(), required,
        [](const auto& pair, const BigInt& value) { return pair.residue < value; });
    const auto upper = std::upper_bound(
        lower, pairs.end(), required,
        [](const BigInt& value, const auto& pair) { return value < pair.residue; });
    for (auto current = lower; current != upper; ++current) {
      ++telemetry.modular_candidates;
      auto parent = add_three(first[current->first].digits,
                              second[current->second].digits,
                              c.digits,
                              expected,
                              telemetry);
      if (!parent.has_value()) {
        continue;
      }
      if (result.size() >= maximum_outputs) {
        status = Status::ResourceLimit;
        message = "ternary merge output exceeds the declared memory limit";
        return {};
      }
      result.push_back(BigEntry{
          std::move(*parent),
          add_mod(add_mod(first[current->first].residue,
                          second[current->second].residue,
                          c.digits.n),
                  c.residue,
                  c.digits.n)});
      if (provenance != nullptr) {
        provenance->push_back(ProvenanceRecord{result.back().digits,
                                               current->first,
                                               current->second,
                                               third_id});
      }
    }
  }
  telemetry.survivors_before_dedup += result.size();
  if (provenance != nullptr) {
    canonicalize_provenance(*provenance);
  }
  canonicalize(result);
  telemetry.survivors_after_dedup += result.size();
  telemetry.allocated_bytes = std::max(telemetry.allocated_bytes, pair_bytes);
  status = Status::Completed;
  return result;
}

[[nodiscard]] std::vector<BigEntry> binary_oracle(
    const std::vector<BigEntry>& left,
    const std::vector<BigEntry>& right,
    const BigInt& target,
    std::uint32_t modulus_bits,
    const DigitCounts& expected,
    std::vector<ProvenanceRecord>* provenance) {
  std::vector<BigEntry> result;
  for (std::uint32_t first_id = 0; first_id < left.size(); ++first_id) {
    const auto& first = left[first_id];
    const auto first_digits = unpack_digits(first.digits);
    for (std::uint32_t second_id = 0; second_id < right.size(); ++second_id) {
      const auto& second = right[second_id];
      if (add_mod(first.residue, second.residue, modulus_bits) != target) {
        continue;
      }
      const auto second_digits = unpack_digits(second.digits);
      ReferenceTernaryVector digits(first_digits.size());
      for (std::size_t coordinate = 0; coordinate < digits.size(); ++coordinate) {
        digits[coordinate] = static_cast<std::int8_t>(
            first_digits[coordinate] + second_digits[coordinate]);
      }
      const auto counts = count_digits(digits);
      if (counts.has_value() && counts_equal(*counts, expected)) {
        result.push_back(BigEntry{
            pack_digits(digits),
            add_mod(first.residue,
                    second.residue,
                    first.digits.n)});
        if (provenance != nullptr) {
          provenance->push_back(
              ProvenanceRecord{result.back().digits, first_id, second_id});
        }
      }
    }
  }
  if (provenance != nullptr) {
    canonicalize_provenance(*provenance);
  }
  canonicalize(result);
  return result;
}

[[nodiscard]] std::vector<BigEntry> ternary_oracle(
    const std::vector<BigEntry>& first,
    const std::vector<BigEntry>& second,
    const std::vector<BigEntry>& third,
    const BigInt& target,
    std::uint32_t modulus_bits,
    const DigitCounts& expected,
    std::vector<ProvenanceRecord>* provenance) {
  std::vector<BigEntry> result;
  for (std::uint32_t first_id = 0; first_id < first.size(); ++first_id) {
    const auto& a = first[first_id];
    const auto a_digits = unpack_digits(a.digits);
    for (std::uint32_t second_id = 0; second_id < second.size(); ++second_id) {
      const auto& b = second[second_id];
      const auto b_digits = unpack_digits(b.digits);
      for (std::uint32_t third_id = 0; third_id < third.size(); ++third_id) {
        const auto& c = third[third_id];
        if (add_mod(add_mod(a.residue, b.residue, modulus_bits),
                    c.residue,
                    modulus_bits) != target) {
          continue;
        }
        const auto c_digits = unpack_digits(c.digits);
        ReferenceTernaryVector parent(a_digits.size(), 0);
        bool valid = true;
        for (std::size_t coordinate = 0; coordinate < parent.size(); ++coordinate) {
          const int sum = static_cast<int>(a_digits[coordinate]) +
                          b_digits[coordinate] + c_digits[coordinate];
          if (sum < -1 || sum > 1) {
            valid = false;
            break;
          }
          parent[coordinate] = static_cast<std::int8_t>(sum);
        }
        const auto counts = valid ? count_digits(parent) : std::nullopt;
        if (counts.has_value() && counts_equal(*counts, expected)) {
          result.push_back(BigEntry{
              pack_digits(parent),
              add_mod(add_mod(a.residue,
                              b.residue,
                              a.digits.n),
                      c.residue,
                      a.digits.n)});
          if (provenance != nullptr) {
            provenance->push_back(ProvenanceRecord{
                result.back().digits, first_id, second_id, third_id});
          }
        }
      }
    }
  }
  if (provenance != nullptr) {
    canonicalize_provenance(*provenance);
  }
  canonicalize(result);
  return result;
}

[[nodiscard]] bool entries_equal(const std::vector<BigEntry>& left,
                                 const std::vector<BigEntry>& right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (left[index].digits != right[index].digits ||
        left[index].residue != right[index].residue) {
      return false;
    }
  }
  return true;
}

void assign_patterns_on_half(
    const ReferenceTernaryVector& parent,
    std::size_t begin,
    std::size_t end,
    std::int8_t parent_digit,
    const std::vector<std::pair<std::array<std::int8_t, 3>, std::uint32_t>>& patterns,
    std::array<ReferenceTernaryVector, 3>& children) {
  std::vector<std::size_t> coordinates;
  for (std::size_t coordinate = begin; coordinate < end; ++coordinate) {
    if (parent[coordinate] == parent_digit) {
      coordinates.push_back(coordinate);
    }
  }
  std::size_t cursor = 0;
  for (const auto& [digits, count] : patterns) {
    for (std::uint32_t copy = 0; copy < count; ++copy) {
      if (cursor >= coordinates.size()) {
        throw std::logic_error("finite representation pattern overfilled its parent class");
      }
      for (std::size_t child = 0; child < 3U; ++child) {
        children[child][coordinates[cursor]] = digits[child];
      }
      ++cursor;
    }
  }
  if (cursor != coordinates.size()) {
    throw std::logic_error("finite representation pattern underfilled its parent class");
  }
}

[[nodiscard]] std::array<ReferenceTernaryVector, 3> decompose_root_half_balanced(
    const ReferenceTernaryVector& root,
    const FullTerParameters& parameters) {
  std::array<ReferenceTernaryVector, 3> children{
      ReferenceTernaryVector(parameters.n, 0),
      ReferenceTernaryVector(parameters.n, 0),
      ReferenceTernaryVector(parameters.n, 0)};
  const std::uint32_t simple = parameters.n / 6U - parameters.epsilon1_level1;
  const std::uint32_t e1 = parameters.epsilon1_level1;
  const std::uint32_t e0 = parameters.epsilon0_level1;
  const std::uint32_t zero = parameters.n / 2U - 6U * e0;
  const std::vector<std::pair<std::array<std::int8_t, 3>, std::uint32_t>> ones{
      {{{1, 0, 0}}, simple / 2U}, {{{0, 1, 0}}, simple / 2U},
      {{{0, 0, 1}}, simple / 2U}, {{{1, 1, -1}}, e1 / 2U},
      {{{1, -1, 1}}, e1 / 2U}, {{{-1, 1, 1}}, e1 / 2U}};
  const std::vector<std::pair<std::array<std::int8_t, 3>, std::uint32_t>> zeros{
      {{{0, 1, -1}}, e0 / 2U}, {{{0, -1, 1}}, e0 / 2U},
      {{{1, 0, -1}}, e0 / 2U}, {{{-1, 0, 1}}, e0 / 2U},
      {{{1, -1, 0}}, e0 / 2U}, {{{-1, 1, 0}}, e0 / 2U},
      {{{0, 0, 0}}, zero / 2U}};
  for (std::size_t half = 0; half < 2U; ++half) {
    const std::size_t begin = half * (parameters.n / 2U);
    const std::size_t end = begin + parameters.n / 2U;
    assign_patterns_on_half(root, begin, end, 1, ones, children);
    assign_patterns_on_half(root, begin, end, 0, zeros, children);
  }
  return children;
}

[[nodiscard]] std::array<ReferenceTernaryVector, 3> decompose_level1_half_balanced(
    const ReferenceTernaryVector& parent,
    const FullTerParameters& parameters) {
  std::array<ReferenceTernaryVector, 3> children{
      ReferenceTernaryVector(parameters.n, 0),
      ReferenceTernaryVector(parameters.n, 0),
      ReferenceTernaryVector(parameters.n, 0)};
  const std::uint32_t w1 = parameters.level1.negative;
  const std::uint32_t simple_positive = parameters.n / 18U + w1 / 3U -
                                        parameters.epsilon1_level2;
  const std::uint32_t simple_negative =
      w1 / 3U - parameters.epsilon_negative1_level2;
  const std::uint32_t zero_count = parameters.level1.zero -
                                   6U * parameters.epsilon0_level2;
  const auto e1 = parameters.epsilon1_level2;
  const auto e0 = parameters.epsilon0_level2;
  const auto en = parameters.epsilon_negative1_level2;
  const std::vector<std::pair<std::array<std::int8_t, 3>, std::uint32_t>> ones{
      {{{1, 0, 0}}, simple_positive / 2U},
      {{{0, 1, 0}}, simple_positive / 2U},
      {{{0, 0, 1}}, simple_positive / 2U},
      {{{1, 1, -1}}, e1 / 2U},
      {{{1, -1, 1}}, e1 / 2U},
      {{{-1, 1, 1}}, e1 / 2U}};
  const std::vector<std::pair<std::array<std::int8_t, 3>, std::uint32_t>> zeros{
      {{{0, 1, -1}}, e0 / 2U}, {{{0, -1, 1}}, e0 / 2U},
      {{{1, 0, -1}}, e0 / 2U}, {{{-1, 0, 1}}, e0 / 2U},
      {{{1, -1, 0}}, e0 / 2U}, {{{-1, 1, 0}}, e0 / 2U},
      {{{0, 0, 0}}, zero_count / 2U}};
  const std::vector<std::pair<std::array<std::int8_t, 3>, std::uint32_t>> negatives{
      {{{-1, 0, 0}}, simple_negative / 2U},
      {{{0, -1, 0}}, simple_negative / 2U},
      {{{0, 0, -1}}, simple_negative / 2U},
      {{{1, -1, -1}}, en / 2U},
      {{{-1, 1, -1}}, en / 2U},
      {{{-1, -1, 1}}, en / 2U}};
  for (std::size_t half = 0; half < 2U; ++half) {
    const std::size_t begin = half * (parameters.n / 2U);
    const std::size_t end = begin + parameters.n / 2U;
    assign_patterns_on_half(parent, begin, end, 1, ones, children);
    assign_patterns_on_half(parent, begin, end, 0, zeros, children);
    assign_patterns_on_half(parent, begin, end, -1, negatives, children);
  }
  return children;
}

[[nodiscard]] std::optional<GuidedTree> build_guided_tree(
    const Instance& instance,
    const FullTerParameters& parameters) {
  if (instance.planted_root.size() != parameters.n) {
    return std::nullopt;
  }
  ReferenceTernaryVector root(parameters.n, 0);
  std::uint32_t first_half_weight = 0;
  std::uint32_t second_half_weight = 0;
  for (std::uint32_t coordinate = 0; coordinate < parameters.n; ++coordinate) {
    if (instance.planted_root[coordinate] > 1U) {
      return std::nullopt;
    }
    root[coordinate] = static_cast<std::int8_t>(instance.planted_root[coordinate]);
    if (root[coordinate] == 1) {
      if (coordinate < parameters.n / 2U) {
        ++first_half_weight;
      } else {
        ++second_half_weight;
      }
    }
  }
  if (first_half_weight != parameters.n / 4U ||
      second_half_weight != parameters.n / 4U) {
    return std::nullopt;
  }
  GuidedTree tree;
  tree.level1 = decompose_root_half_balanced(root, parameters);
  for (std::size_t group = 0; group < 3U; ++group) {
    const auto children =
        decompose_level1_half_balanced(tree.level1[group], parameters);
    for (std::size_t child = 0; child < 3U; ++child) {
      tree.level2[group * 3U + child] = children[child];
    }
  }
  return tree;
}

void include_guided_halves(Level3Generation& fixture,
                           const GuidedTree& tree,
                           std::uint64_t fixture_limit) {
  const std::size_t half = tree.level2[0].size() / 2U;
  std::vector<ReferenceTernaryVector> required_first;
  std::vector<ReferenceTernaryVector> required_second;
  for (const auto& leaf : tree.level2) {
    ReferenceTernaryVector first = leaf;
    ReferenceTernaryVector second = leaf;
    for (std::size_t coordinate = 0; coordinate < leaf.size(); ++coordinate) {
      if (coordinate < half) {
        second[coordinate] = 0;
      } else {
        first[coordinate] = 0;
      }
    }
    required_first.push_back(std::move(first));
    required_second.push_back(std::move(second));
  }
  canonicalize(required_first);
  canonicalize(required_second);
  if (required_first.size() > fixture_limit ||
      required_second.size() > fixture_limit) {
    throw std::length_error("fixture limit is too small for guided level-3 halves");
  }
  const auto retain_required = [fixture_limit](
                                   std::vector<ReferenceTernaryVector> original,
                                   std::vector<ReferenceTernaryVector> required) {
    for (auto& candidate : original) {
      if (required.size() >= fixture_limit) {
        break;
      }
      if (!std::binary_search(required.begin(), required.end(), candidate)) {
        required.push_back(std::move(candidate));
        canonicalize(required);
      }
    }
    return required;
  };
  fixture.first_half =
      retain_required(std::move(fixture.first_half), std::move(required_first));
  fixture.second_half =
      retain_required(std::move(fixture.second_half), std::move(required_second));
}

[[nodiscard]] Targets random_targets(const Instance& instance,
                                     const FullTerParameters& parameters,
                                     std::uint64_t seed) {
  SplitMix64 random(seed);
  Targets targets;
  targets.level1[0] = sample_residue(random, parameters.r1);
  targets.level1[1] = sample_residue(random, parameters.r1);
  targets.level1[2] = subtract_mod(
      subtract_mod(instance_target(instance), targets.level1[0], parameters.r1),
      targets.level1[1],
      parameters.r1);
  for (std::size_t group = 0; group < 3U; ++group) {
    targets.level2[group * 3U] = sample_residue(random, parameters.r2);
    targets.level2[group * 3U + 1U] = sample_residue(random, parameters.r2);
    targets.level2[group * 3U + 2U] = subtract_mod(
        subtract_mod(targets.level1[group],
                     targets.level2[group * 3U],
                     parameters.r2),
        targets.level2[group * 3U + 1U],
        parameters.r2);
  }
  return targets;
}

[[nodiscard]] bool targets_are_consistent(const Targets& targets,
                                          const Instance& instance,
                                          const FullTerParameters& parameters) {
  BigInt level1_sum;
  for (const auto& target : targets.level1) {
    level1_sum = add_mod(level1_sum, target, parameters.r1);
  }
  if (level1_sum != masked(instance_target(instance), parameters.r1)) {
    return false;
  }
  for (std::size_t group = 0; group < 3U; ++group) {
    BigInt level2_sum;
    for (std::size_t child = 0; child < 3U; ++child) {
      level2_sum = add_mod(level2_sum,
                           targets.level2[group * 3U + child],
                           parameters.r2);
    }
    if (level2_sum != masked(targets.level1[group], parameters.r2)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] Targets guided_targets(const GuidedTree& tree,
                                     const std::vector<BigInt>& coefficients,
                                     const FullTerParameters& parameters) {
  Targets targets;
  const BigInt full_mask = pow2_mask(parameters.n);
  for (std::size_t index = 0; index < 3U; ++index) {
    targets.level1[index] = masked(
        inner_product_full(coefficients, tree.level1[index], full_mask),
        parameters.r1);
  }
  for (std::size_t index = 0; index < 9U; ++index) {
    targets.level2[index] = masked(
        inner_product_full(coefficients, tree.level2[index], full_mask),
        parameters.r2);
  }
  return targets;
}

template <std::size_t Size>
[[nodiscard]] std::uint64_t stage_fingerprint(
    const std::array<std::vector<BigEntry>, Size>& lists) {
  std::uint64_t hash = 1469598103934665603ULL;
  const auto mix = [&hash](std::uint64_t value) {
    hash ^= value;
    hash *= 1099511628211ULL;
  };
  for (const auto& list : lists) {
    mix(list.size());
    for (const auto& entry : list) {
      mix(entry.digits.n);
      for (const auto word : entry.digits.words) {
        mix(word);
      }
      for (const auto word : entry.residue.words()) {
        mix(word);
      }
    }
  }
  return hash;
}

[[nodiscard]] std::uint64_t stage_fingerprint(
    const std::vector<BigEntry>& list) {
  std::uint64_t hash = 1469598103934665603ULL;
  const auto mix = [&hash](std::uint64_t value) {
    hash ^= value;
    hash *= 1099511628211ULL;
  };
  mix(list.size());
  for (const auto& entry : list) {
    mix(entry.digits.n);
    for (const auto word : entry.digits.words) {
      mix(word);
    }
    for (const auto word : entry.residue.words()) {
      mix(word);
    }
  }
  return hash;
}

[[nodiscard]] std::uint64_t fingerprint(std::span<const std::int8_t> witness,
                                        const FullTerTelemetry& telemetry) {
  std::uint64_t hash = 1469598103934665603ULL;
  const auto mix = [&hash](std::uint64_t value) {
    hash ^= value;
    hash *= 1099511628211ULL;
  };
  for (const std::int8_t digit : witness) {
    mix(static_cast<std::uint64_t>(digit + 1));
  }
  mix(telemetry.seed);
  mix(telemetry.n);
  mix(telemetry.modulus_bits);
  mix(static_cast<std::uint64_t>(telemetry.execution_mode));
  mix(static_cast<std::uint64_t>(telemetry.family));
  mix(telemetry.parameters.r1);
  mix(telemetry.parameters.r2);
  for (const auto size : telemetry.physical_level3_sizes) {
    mix(size);
  }
  mix(telemetry.attempts.size());
  for (const auto& attempt : telemetry.attempts) {
    mix(attempt.seed);
    mix(attempt.attempt);
    mix(static_cast<std::uint64_t>(attempt.status));
    for (const auto size : attempt.level2_sizes) {
      mix(size);
    }
    for (const auto size : attempt.level1_sizes) {
      mix(size);
    }
    mix(attempt.level0_size);
    mix(attempt.level3_to_level2.modular_candidates);
    mix(attempt.level2_to_level1.modular_candidates);
    mix(attempt.level1_to_level0.modular_candidates);
    mix(attempt.level3_to_level2.output_fingerprint);
    mix(attempt.level2_to_level1.output_fingerprint);
    mix(attempt.level1_to_level0.output_fingerprint);
  }
  return hash;
}

}  // namespace

FullTerParameterResult derive_full_ter_parameters_impl(ShapeFamily family,
                                                       std::uint32_t n,
                                                       std::uint32_t modulus_bits) {
  FullTerParameterResult result;
  if ((family != ShapeFamily::FiniteSanity &&
       family != ShapeFamily::PaperRounded) ||
      n == 0U || n % 36U != 0U || modulus_bits == 0U || modulus_bits > n) {
    result.message = "full TER requires n divisible by 36 and 0 < modulus_bits <= n";
    return result;
  }

  auto& value = result.value;
  value.family = family;
  value.n = n;
  if (family == ShapeFamily::PaperRounded) {
    value.epsilon1_level1 = rounded_ratio(n, 24U, 10'000U);
    value.epsilon0_level1 = rounded_ratio(n, 66U, 10'000U);
    value.epsilon1_level2 = rounded_ratio(n, 1U, 10'000U);
    value.epsilon0_level2 = rounded_ratio(n, 4U, 10'000U);
    value.epsilon_negative1_level2 = 0U;

    // Constraint-first deterministic correction.  Only congruence and parity
    // corrections are needed, so search a bounded neighbourhood rather than a
    // quadratic finite-n domain.
    const std::uint32_t target_e1 = value.epsilon1_level1;
    const std::uint32_t target_e0 = value.epsilon0_level1;
    std::uint32_t best_distance = std::numeric_limits<std::uint32_t>::max();
    bool found = false;
    for (std::int32_t e1_delta = -12; e1_delta <= 12; ++e1_delta) {
      const std::int64_t e1_signed =
          static_cast<std::int64_t>(target_e1) + e1_delta;
      if (e1_signed < 0 || e1_signed > n / 6U) {
        continue;
      }
      const auto e1 = static_cast<std::uint32_t>(e1_signed);
      for (std::int32_t e0_delta = -12; e0_delta <= 12; ++e0_delta) {
        const std::int64_t e0_signed =
            static_cast<std::int64_t>(target_e0) + e0_delta;
        if (e0_signed < 0 || e0_signed > n / 12U) {
          continue;
        }
        const auto e0 = static_cast<std::uint32_t>(e0_signed);
        const std::uint32_t w1 = e1 + 2U * e0;
        if (w1 % 3U != 0U || e1 % 2U != 0U || e0 % 2U != 0U ||
            (n / 6U - e1) % 2U != 0U || 6U * e0 > n / 2U) {
          continue;
        }
        const std::uint32_t distance =
            (e1 > target_e1 ? e1 - target_e1 : target_e1 - e1) +
            (e0 > target_e0 ? e0 - target_e0 : target_e0 - e0);
        if (!found || distance < best_distance ||
            (distance == best_distance &&
             std::pair{e1, e0} <
                 std::pair{value.epsilon1_level1,
                           value.epsilon0_level1})) {
          found = true;
          best_distance = distance;
          value.epsilon1_level1 = e1;
          value.epsilon0_level1 = e0;
        }
      }
    }
    if (!found) {
      result.status = Status::InvalidShape;
      result.message = "paper-rounded level-1 counts have no valid finite correction";
      return result;
    }
  }

  const std::uint32_t w1 =
      value.epsilon1_level1 + 2U * value.epsilon0_level1;
  if (w1 % 3U != 0U || value.epsilon1_level1 > n / 6U ||
      6U * value.epsilon0_level1 > n / 2U ||
      value.epsilon1_level1 % 2U != 0U ||
      value.epsilon0_level1 % 2U != 0U ||
      (n / 6U - value.epsilon1_level1) % 2U != 0U) {
    result.status = Status::InvalidShape;
    result.message = "finite level-1 Table-3 counts are not integral or non-negative";
    return result;
  }
  const std::uint32_t positive1 = n / 6U + w1;
  const std::uint32_t zero1 = n - w1 - positive1;
  value.level1 = DigitCounts{w1, zero1, positive1};

  if (family == ShapeFamily::PaperRounded) {
    // The optimized decimal rates do not generally land on half-integral
    // counts.  Choose the deterministic lower even neighbour, clamped to the
    // exact Table-4 class capacity.  This changes only the finite policy; the
    // unrounded paper point remains available in asymptotic telemetry.
    const auto even_floor = [](std::uint32_t number) { return number & ~1U; };
    value.epsilon1_level2 = even_floor(std::min(
        value.epsilon1_level2, n / 18U + w1 / 3U));
    value.epsilon0_level2 =
        even_floor(std::min(value.epsilon0_level2, zero1 / 6U));
    value.epsilon_negative1_level2 = even_floor(std::min(
        value.epsilon_negative1_level2, w1 / 3U));
  }

  const std::uint32_t w2 = w1 / 3U + value.epsilon_negative1_level2 +
                           value.epsilon1_level2 +
                           2U * value.epsilon0_level2;
  const std::uint32_t positive2 = n / 18U + w2;
  if (value.epsilon1_level2 > n / 18U + w1 / 3U ||
      6U * value.epsilon0_level2 > zero1 ||
      value.epsilon_negative1_level2 > w1 / 3U || w2 > n ||
      positive2 > n - w2 || w2 % 2U != 0U || positive2 % 2U != 0U ||
      value.epsilon1_level2 % 2U != 0U ||
      value.epsilon0_level2 % 2U != 0U ||
      value.epsilon_negative1_level2 % 2U != 0U) {
    result.status = Status::InvalidShape;
    result.message = "finite level-2 Table-4 or half-support counts are invalid";
    return result;
  }
  value.level2 = DigitCounts{w2, n - w2 - positive2, positive2};
  value.level3_active_half =
      DigitCounts{w2 / 2U, n / 2U - w2 / 2U - positive2 / 2U, positive2 / 2U};

  const auto primes = primes_through(n);
  std::vector<std::uint32_t> r1_exponents(primes.size(), 0U);
  add_multinomial_exponents(
      n / 2U,
      std::array<std::uint32_t, 5>{value.epsilon1_level1,
                                   value.epsilon1_level1,
                                   value.epsilon1_level1,
                                   n / 6U - value.epsilon1_level1,
                                   n / 6U - value.epsilon1_level1},
      primes,
      r1_exponents);
  add_multinomial_exponents(
      n / 2U,
      std::array<std::uint32_t, 6>{value.epsilon0_level1,
                                   value.epsilon0_level1,
                                   value.epsilon0_level1,
                                   value.epsilon0_level1,
                                   value.epsilon0_level1,
                                   value.epsilon0_level1},
      primes,
      r1_exponents);
  const BigInt representation1 = from_prime_exponents(primes, r1_exponents);

  const std::uint32_t simple_positive2 =
      n / 18U + w1 / 3U - value.epsilon1_level2;
  const std::uint32_t simple_negative2 =
      w1 / 3U - value.epsilon_negative1_level2;
  std::vector<std::uint32_t> r2_exponents(primes.size(), 0U);
  add_multinomial_exponents(
      positive1,
      std::array<std::uint32_t, 5>{value.epsilon1_level2,
                                   value.epsilon1_level2,
                                   value.epsilon1_level2,
                                   simple_positive2,
                                   simple_positive2},
      primes,
      r2_exponents);
  add_multinomial_exponents(
      zero1,
      std::array<std::uint32_t, 6>{value.epsilon0_level2,
                                   value.epsilon0_level2,
                                   value.epsilon0_level2,
                                   value.epsilon0_level2,
                                   value.epsilon0_level2,
                                   value.epsilon0_level2},
      primes,
      r2_exponents);
  add_multinomial_exponents(
      w1,
      std::array<std::uint32_t, 5>{value.epsilon_negative1_level2,
                                   value.epsilon_negative1_level2,
                                   value.epsilon_negative1_level2,
                                   simple_negative2,
                                   simple_negative2},
      primes,
      r2_exponents);
  const BigInt representation2 = from_prime_exponents(primes, r2_exponents);
  value.representation1 = exact_count(representation1);
  value.representation2 = exact_count(representation2);
  value.r1 = value.representation1.floor_log2 / 2U;
  value.r2 = value.representation2.floor_log2 / 2U;
  if (value.r2 == 0U || value.r2 >= value.r1 || value.r1 >= modulus_bits) {
    result.status = Status::InvalidConfiguration;
    result.message = "finite representation counts do not yield 0 < r2 < r1 < modulus_bits";
    return result;
  }
  result.status = Status::Completed;
  return result;
}

FullTerParameterResult derive_full_ter_parameters(ShapeFamily family,
                                                  std::uint32_t n,
                                                  std::uint32_t modulus_bits) {
  try {
    return derive_full_ter_parameters_impl(family, n, modulus_bits);
  } catch (const std::bad_alloc&) {
    FullTerParameterResult result;
    result.status = Status::ResourceLimit;
    result.message = "finite TER parameter allocation failed";
    return result;
  } catch (const std::invalid_argument& error) {
    FullTerParameterResult result;
    result.status = Status::InvalidShape;
    result.message = error.what();
    return result;
  }
}

Level3GenerationResult generate_level3_lists_impl(
    const FullTerParameters& parameters,
    TerExecutionMode mode,
    std::uint64_t fixture_limit,
    std::uint64_t memory_limit_bytes) {
  Level3GenerationResult result;
  if ((mode != TerExecutionMode::FullTer &&
       mode != TerExecutionMode::FixtureTer) ||
      parameters.n == 0U || parameters.n % 2U != 0U ||
      parameters.level3_active_half.total() != parameters.n / 2U ||
      memory_limit_bytes == 0U ||
      (mode == TerExecutionMode::FixtureTer && fixture_limit == 0U)) {
    result.message = "level-3 generation configuration is invalid";
    return result;
  }
  const BigInt cardinality_big = multinomial(
      parameters.n / 2U,
      std::array<std::uint32_t, 2>{parameters.level3_active_half.negative,
                                   parameters.level3_active_half.positive});
  const BigInt maximum = std::numeric_limits<std::uint64_t>::max();
  const bool cardinality_overflow = cardinality_big > maximum;
  if (cardinality_overflow && mode == TerExecutionMode::FullTer) {
    result.status = Status::ResourceLimit;
    result.message = "complete level-3 cardinality exceeds the implementation index range";
    return result;
  }
  const std::uint64_t cardinality = cardinality_overflow
                                        ? std::numeric_limits<std::uint64_t>::max()
                                        : cardinality_big.to_uint64();
  const std::uint64_t generated =
      mode == TerExecutionMode::FullTer
          ? cardinality
          : (cardinality_overflow ? fixture_limit
                                  : std::min(cardinality, fixture_limit));
  std::uint64_t vector_bytes = 0;
  std::uint64_t total_bytes = 0;
  if (!checked_add(parameters.n, sizeof(ReferenceTernaryVector), vector_bytes) ||
      !checked_multiply(generated, vector_bytes, total_bytes) ||
      !checked_multiply(total_bytes, 2U, total_bytes) ||
      total_bytes > memory_limit_bytes || generated > std::numeric_limits<std::size_t>::max()) {
    result.status = Status::ResourceLimit;
    result.message = "level-3 materialization exceeds the declared memory limit";
    return result;
  }
  result.value.complete_cardinality = cardinality;
  result.value.complete_cardinality_overflow = cardinality_overflow;
  result.value.complete = !cardinality_overflow && generated == cardinality;
  result.value.first_half = enumerate_active_half(parameters.n,
                                                  parameters.level3_active_half,
                                                  false,
                                                  generated);
  result.value.second_half = enumerate_active_half(parameters.n,
                                                   parameters.level3_active_half,
                                                   true,
                                                   generated);
  if (result.value.first_half.size() != generated ||
      result.value.second_half.size() != generated) {
    result.status = Status::InvariantViolation;
    result.message = "level-3 combinatorial generator emitted the wrong cardinality";
    return result;
  }
  result.status = Status::Completed;
  return result;
}

Level3GenerationResult generate_level3_lists(const FullTerParameters& parameters,
                                             TerExecutionMode mode,
                                             std::uint64_t fixture_limit,
                                             std::uint64_t memory_limit_bytes) {
  try {
    return generate_level3_lists_impl(
        parameters, mode, fixture_limit, memory_limit_bytes);
  } catch (const std::bad_alloc&) {
    Level3GenerationResult result;
    result.status = Status::ResourceLimit;
    result.message = "level-3 generation allocation failed";
    return result;
  } catch (const std::length_error& error) {
    Level3GenerationResult result;
    result.status = Status::ResourceLimit;
    result.message = error.what();
    return result;
  } catch (const std::invalid_argument& error) {
    Level3GenerationResult result;
    result.status = Status::InvalidShape;
    result.message = error.what();
    return result;
  }
}

OperationResult verify_full_ter_witness_with_coefficients(
    const Instance& instance,
    const ReferenceTernaryVector& witness,
    const std::vector<BigInt>& coefficients) {
  if (instance.n == 0U || instance.n % 2U != 0U || witness.size() != instance.n ||
      instance.words_per_value !=
          (static_cast<std::uint64_t>(instance.n) + 63U) / 64U ||
      instance.values_le.size() !=
          static_cast<std::size_t>(instance.n) * instance.words_per_value ||
      instance.root_target_le.size() != instance.words_per_value ||
      coefficients.size() != instance.n) {
    return {Status::InvariantViolation, "root witness or instance dimensions are invalid"};
  }
  std::uint32_t weight = 0;
  for (const std::int8_t digit : witness) {
    if (digit != 0 && digit != 1) {
      return {Status::InvariantViolation, "root witness is not binary"};
    }
    weight += static_cast<std::uint32_t>(digit);
  }
  if (weight != instance.n / 2U) {
    return {Status::InvariantViolation, "root witness has the wrong Hamming weight"};
  }
  const BigInt recomputed =
      inner_product_full(coefficients, witness, pow2_mask(instance.n));
  if (recomputed != instance_target(instance)) {
    return {Status::InvariantViolation, "root witness misses the full subset-sum target"};
  }
  return {Status::Completed, {}};
}

OperationResult verify_full_ter_witness_impl(const Instance& instance,
                                             const ReferenceTernaryVector& witness) {
  if (instance.n == 0U || instance.words_per_value !=
                               (static_cast<std::uint64_t>(instance.n) + 63U) / 64U ||
      instance.values_le.size() !=
          static_cast<std::size_t>(instance.n) * instance.words_per_value) {
    return {Status::InvariantViolation, "root witness or instance dimensions are invalid"};
  }
  const auto coefficients = instance_coefficients(instance);
  return verify_full_ter_witness_with_coefficients(instance, witness, coefficients);
}

OperationResult verify_full_ter_witness(const Instance& instance,
                                        const ReferenceTernaryVector& witness) {
  try {
    return verify_full_ter_witness_impl(instance, witness);
  } catch (const std::bad_alloc&) {
    return {Status::ResourceLimit, "root witness verification allocation failed"};
  } catch (const std::invalid_argument& error) {
    return {Status::InvariantViolation, error.what()};
  }
}

FullTerResult solve_full_ter_impl(const Instance& instance, const FullTerConfig& config) {
  FullTerResult result;
  const auto solver_start = Clock::now();
  result.telemetry.seed = config.seed;
  result.telemetry.n = instance.n;
  result.telemetry.modulus_bits = instance.n;
  result.telemetry.execution_mode = config.execution_mode;
  result.telemetry.family = config.family;
  result.telemetry.hot_entry_bytes = sizeof(BigEntry);
  result.telemetry.packed_vector_bytes =
      2U * ((static_cast<std::uint64_t>(instance.n) + 63U) / 64U) *
      sizeof(std::uint64_t);
  if ((config.execution_mode != TerExecutionMode::FullTer &&
       config.execution_mode != TerExecutionMode::FixtureTer) ||
      (config.target_mode != TerTargetMode::Random &&
       config.target_mode != TerTargetMode::GuidedPlanted) ||
      config.attempt_limit == 0U || config.memory_limit_bytes == 0U ||
      instance.n == 0U ||
      instance.words_per_value !=
          (static_cast<std::uint64_t>(instance.n) + 63U) / 64U ||
      instance.values_le.size() !=
          static_cast<std::size_t>(instance.n) * instance.words_per_value ||
      instance.root_target_le.size() != instance.words_per_value) {
    result.message = "full TER solver configuration or instance is invalid";
    return result;
  }
  const auto parameters =
      derive_full_ter_parameters(config.family, instance.n, instance.n);
  if (parameters.status != Status::Completed) {
    result.status = parameters.status;
    result.message = parameters.message;
    return result;
  }
  result.telemetry.parameters = parameters.value;

  if (config.cancellation != nullptr &&
      config.cancellation->load(std::memory_order_relaxed)) {
    result.status = Status::Cancelled;
    result.message = "full TER execution was cancelled";
    result.telemetry.total_solver_ns = elapsed_ns(solver_start);
    return result;
  }

  const auto generation_start = Clock::now();
  auto generated = generate_level3_lists(parameters.value,
                                         config.execution_mode,
                                         config.fixture_level3_limit,
                                         config.memory_limit_bytes);
  if (generated.status != Status::Completed) {
    result.status = generated.status;
    result.message = generated.message;
    return result;
  }
  std::optional<GuidedTree> guided;
  if (config.target_mode == TerTargetMode::GuidedPlanted) {
    std::uint64_t generated_count = 0;
    std::uint64_t generated_bytes = 0;
    std::uint64_t guided_bytes = sizeof(GuidedTree);
    const std::uint64_t scalar_bytes =
        sizeof(ReferenceTernaryVector) + static_cast<std::uint64_t>(instance.n);
    if (!checked_add(generated.value.first_half.size(),
                     generated.value.second_half.size(),
                     generated_count) ||
        !checked_multiply(generated_count, scalar_bytes, generated_bytes) ||
        !checked_add(guided_bytes,
                     12U * scalar_bytes,
                     guided_bytes) ||
        !checked_add(generated_bytes, guided_bytes, generated_bytes) ||
        generated_bytes > config.memory_limit_bytes) {
      result.status = Status::ResourceLimit;
      result.message = "guided level-3 preparation exceeds the memory limit";
      return result;
    }
    guided = build_guided_tree(instance, parameters.value);
    if (!guided.has_value()) {
      result.status = Status::InvalidConfiguration;
      result.message =
          "guided TER requires a planted binary root balanced across both halves";
      return result;
    }
    if (config.execution_mode == TerExecutionMode::FixtureTer) {
      try {
        include_guided_halves(
            generated.value, *guided, config.fixture_level3_limit);
      } catch (const std::length_error& error) {
        result.status = Status::ResourceLimit;
        result.message = error.what();
        return result;
      }
    }
  }
  result.telemetry.physical_level3_sizes = {
      generated.value.first_half.size(), generated.value.second_half.size()};

  std::uint64_t scalar_level3_bytes = 0;
  std::uint64_t level3_vector_count = 0;
  std::uint64_t coefficient_estimate = 0;
  std::uint64_t entry_estimate = 0;
  std::uint64_t guided_estimate = 0;
  const std::uint64_t scalar_vector_bytes =
      sizeof(ReferenceTernaryVector) + static_cast<std::uint64_t>(instance.n);
  const std::uint64_t coefficient_bytes =
      sizeof(BigInt) +
      ((static_cast<std::uint64_t>(instance.n) + 31U) / 32U) *
          sizeof(std::uint32_t);
  if (!checked_add(generated.value.first_half.size(),
                   generated.value.second_half.size(),
                   level3_vector_count) ||
      !checked_multiply(level3_vector_count,
                        scalar_vector_bytes,
                        scalar_level3_bytes) ||
      !checked_multiply(instance.n, coefficient_bytes, coefficient_estimate) ||
      !checked_multiply(level3_vector_count,
                        estimated_entry_bytes(instance.n),
                        entry_estimate) ||
      (config.target_mode == TerTargetMode::GuidedPlanted &&
       (!checked_multiply(12U, scalar_vector_bytes, guided_estimate) ||
        !checked_add(guided_estimate, sizeof(GuidedTree), guided_estimate)))) {
    result.status = Status::ResourceLimit;
    result.message = "level-3 preparation memory accounting overflow";
    return result;
  }
  std::uint64_t preparation_peak = scalar_level3_bytes;
  if (!checked_add(preparation_peak, coefficient_estimate, preparation_peak) ||
      !checked_add(preparation_peak, entry_estimate, preparation_peak) ||
      preparation_peak > config.memory_limit_bytes) {
    result.status = Status::ResourceLimit;
    result.message = "level-3 preparation exceeds the declared memory limit";
    return result;
  }
  std::uint64_t guided_preparation_peak = scalar_level3_bytes;
  if (!checked_add(guided_preparation_peak,
                   coefficient_estimate,
                   guided_preparation_peak) ||
      !checked_add(guided_preparation_peak,
                   guided_estimate,
                   guided_preparation_peak) ||
      guided_preparation_peak > config.memory_limit_bytes) {
    result.status = Status::ResourceLimit;
    result.message = "guided arithmetic preparation exceeds the memory limit";
    return result;
  }
  preparation_peak = std::max(preparation_peak, guided_preparation_peak);

  const auto coefficients = instance_coefficients(instance);
  std::optional<Targets> fixed_guided_targets;
  if (guided.has_value()) {
    fixed_guided_targets =
        guided_targets(*guided, coefficients, parameters.value);
    guided.reset();
  }
  const BigInt full_mask = pow2_mask(instance.n);
  const auto left_base = materialize_entries(
      std::move(generated.value.first_half), coefficients, full_mask);
  const auto right_base = materialize_entries(
      std::move(generated.value.second_half), coefficients, full_mask);
  std::uint64_t level3_entries_bytes = 0;
  std::uint64_t base_resident = 0;
  const std::uint64_t arithmetic_bytes = estimated_bigints_bytes(coefficients);
  if (!checked_add(estimated_entries_bytes(left_base),
                   estimated_entries_bytes(right_base),
                   level3_entries_bytes) ||
      !checked_add(level3_entries_bytes, arithmetic_bytes, base_resident) ||
      (fixed_guided_targets.has_value() &&
       !checked_add(base_resident,
                    estimated_targets_bytes(*fixed_guided_targets),
                    base_resident)) ||
      base_resident > config.memory_limit_bytes) {
    result.status = Status::ResourceLimit;
    result.message = "materialized level-3 entries exceed the declared memory limit";
    return result;
  }
  result.telemetry.peak_working_bytes = std::max(base_resident, preparation_peak);
  result.telemetry.level3_allocated_bytes = level3_entries_bytes;
  result.telemetry.arithmetic_allocated_bytes = arithmetic_bytes;
  result.telemetry.level3_generation_ns = elapsed_ns(generation_start);

  for (std::uint32_t attempt = 0; attempt < config.attempt_limit; ++attempt) {
    if (config.cancellation != nullptr &&
        config.cancellation->load(std::memory_order_relaxed)) {
      result.status = Status::Cancelled;
      result.message = "full TER execution was cancelled";
      result.telemetry.total_solver_ns = elapsed_ns(solver_start);
      return result;
    }
    FullTerAttemptTelemetry attempt_telemetry;
    attempt_telemetry.seed = config.seed + attempt;
    attempt_telemetry.attempt = attempt;
    const auto attempt_start = Clock::now();
    Targets sampled_targets;
    const Targets& targets = fixed_guided_targets.has_value()
                                 ? *fixed_guided_targets
                                 : (sampled_targets = random_targets(
                                        instance,
                                        parameters.value,
                                        config.seed + attempt));
    if (!targets_are_consistent(targets, instance, parameters.value)) {
      result.status = Status::InvariantViolation;
      result.message = "sampled TER targets violate the paper's sum relationships";
      return result;
    }

    std::uint64_t attempt_base_resident = base_resident;
    if (!fixed_guided_targets.has_value() &&
        (!checked_add(attempt_base_resident,
                      estimated_targets_bytes(targets),
                      attempt_base_resident) ||
         attempt_base_resident > config.memory_limit_bytes)) {
      result.status = Status::ResourceLimit;
      result.message = "sampled target storage exceeds the memory limit";
      return result;
    }

    std::array<std::vector<BigEntry>, 9> level2;
    std::uint64_t level2_resident = attempt_base_resident;
    auto stage_start = Clock::now();
    for (std::size_t index = 0; index < level2.size(); ++index) {
      if (config.cancellation != nullptr &&
          config.cancellation->load(std::memory_order_relaxed)) {
        result.status = Status::Cancelled;
        result.message = "full TER execution was cancelled";
        return result;
      }
      if (level2_resident >= config.memory_limit_bytes) {
        result.status = Status::ResourceLimit;
        result.message = "resident level-3/level-2 lists leave no merge workspace";
        return result;
      }
      std::uint64_t oracle_work = 0;
      const bool audit_stage =
          config.audit &&
          checked_multiply(left_base.size(), right_base.size(), oracle_work) &&
          oracle_work <= 1'000'000U;
      std::vector<ProvenanceRecord> optimized_provenance;
      Status stage_status = Status::InvariantViolation;
      level2[index] = binary_mitm(left_base,
                                  right_base,
                                  targets.level2[index],
                                  parameters.value.r2,
                                  parameters.value.level2,
                                  config.memory_limit_bytes - level2_resident,
                                  attempt_telemetry.level3_to_level2,
                                  audit_stage ? &optimized_provenance : nullptr,
                                  stage_status,
                                  result.message);
      if (stage_status != Status::Completed) {
        result.status = stage_status;
        return result;
      }
      if (audit_stage) {
        std::vector<ProvenanceRecord> reference_provenance;
        const auto reference = binary_oracle(left_base,
                                             right_base,
                                             targets.level2[index],
                                             parameters.value.r2,
                                             parameters.value.level2,
                                             &reference_provenance);
        if (!entries_equal(level2[index], reference) ||
            optimized_provenance != reference_provenance) {
          result.status = Status::InvariantViolation;
          result.message =
              "optimized L3-to-L2 set or provenance disagrees with scalar oracle";
          return result;
        }
      }
      attempt_telemetry.level2_sizes[index] = level2[index].size();
      if (!checked_add(level2_resident,
                       estimated_entries_bytes(level2[index]),
                       level2_resident) ||
          level2_resident > config.memory_limit_bytes) {
        result.status = Status::ResourceLimit;
        result.message = "level-2 resident memory accounting overflow";
        return result;
      }
    }
    attempt_telemetry.level3_to_level2.output_fingerprint =
        stage_fingerprint(level2);
    attempt_telemetry.level3_to_level2.elapsed_ns = elapsed_ns(stage_start);
    attempt_telemetry.level2_allocated_bytes =
        level2_resident - attempt_base_resident;
    std::uint64_t level2_peak = 0;
    if (!checked_add(level2_resident,
                     attempt_telemetry.level3_to_level2.allocated_bytes,
                     level2_peak) ||
        level2_peak > config.memory_limit_bytes) {
      result.status = Status::ResourceLimit;
      result.message = "level-2 construction peak exceeds the declared memory limit";
      return result;
    }
    result.telemetry.peak_working_bytes =
        std::max(result.telemetry.peak_working_bytes, level2_peak);

    std::array<std::vector<BigEntry>, 3> level1;
    std::uint64_t level1_resident = level2_resident;
    stage_start = Clock::now();
    for (std::size_t group = 0; group < level1.size(); ++group) {
      if (level1_resident >= config.memory_limit_bytes) {
        result.status = Status::ResourceLimit;
        result.message = "resident level-2/level-1 lists leave no merge workspace";
        return result;
      }
      std::uint64_t oracle_pairs = 0;
      std::uint64_t oracle_work = 0;
      const bool audit_stage =
          config.audit &&
          checked_multiply(level2[group * 3U].size(),
                           level2[group * 3U + 1U].size(),
                           oracle_pairs) &&
          checked_multiply(oracle_pairs,
                           level2[group * 3U + 2U].size(),
                           oracle_work) &&
          oracle_work <= 1'000'000U;
      std::vector<ProvenanceRecord> optimized_provenance;
      Status stage_status = Status::InvariantViolation;
      level1[group] = ternary_mitm(level2[group * 3U],
                                   level2[group * 3U + 1U],
                                   level2[group * 3U + 2U],
                                   targets.level1[group],
                                   parameters.value.r1,
                                   parameters.value.level1,
                                   config.memory_limit_bytes - level1_resident,
                                   attempt_telemetry.level2_to_level1,
                                   audit_stage ? &optimized_provenance : nullptr,
                                   stage_status,
                                   result.message);
      if (stage_status != Status::Completed) {
        result.status = stage_status;
        return result;
      }
      if (audit_stage) {
        std::vector<ProvenanceRecord> reference_provenance;
        const auto reference = ternary_oracle(level2[group * 3U],
                                              level2[group * 3U + 1U],
                                              level2[group * 3U + 2U],
                                              targets.level1[group],
                                              parameters.value.r1,
                                              parameters.value.level1,
                                              &reference_provenance);
        if (!entries_equal(level1[group], reference) ||
            optimized_provenance != reference_provenance) {
          result.status = Status::InvariantViolation;
          result.message =
              "optimized L2-to-L1 set or provenance disagrees with scalar oracle";
          return result;
        }
      }
      attempt_telemetry.level1_sizes[group] = level1[group].size();
      if (!checked_add(level1_resident,
                       estimated_entries_bytes(level1[group]),
                       level1_resident) ||
          level1_resident > config.memory_limit_bytes) {
        result.status = Status::ResourceLimit;
        result.message = "level-1 resident memory accounting overflow";
        return result;
      }
    }
    attempt_telemetry.level2_to_level1.output_fingerprint =
        stage_fingerprint(level1);
    attempt_telemetry.level2_to_level1.elapsed_ns = elapsed_ns(stage_start);
    attempt_telemetry.level1_allocated_bytes = level1_resident - level2_resident;
    std::uint64_t level1_peak = 0;
    if (!checked_add(level1_resident,
                     attempt_telemetry.level2_to_level1.allocated_bytes,
                     level1_peak) ||
        level1_peak > config.memory_limit_bytes) {
      result.status = Status::ResourceLimit;
      result.message = "level-1 construction peak exceeds the declared memory limit";
      return result;
    }
    result.telemetry.peak_working_bytes =
        std::max(result.telemetry.peak_working_bytes, level1_peak);

    stage_start = Clock::now();
    Status root_status = Status::InvariantViolation;
    const DigitCounts root_counts{0U, instance.n / 2U, instance.n / 2U};
    std::uint64_t root_pairs = 0;
    std::uint64_t root_work = 0;
    const bool audit_root =
        config.audit &&
        checked_multiply(level1[0].size(), level1[1].size(), root_pairs) &&
        checked_multiply(root_pairs, level1[2].size(), root_work) &&
        root_work <= 1'000'000U;
    std::vector<ProvenanceRecord> optimized_root_provenance;
    auto level0 = ternary_mitm(level1[0],
                               level1[1],
                               level1[2],
                               instance_target(instance),
                               instance.n,
                               root_counts,
                               config.memory_limit_bytes - level1_resident,
                               attempt_telemetry.level1_to_level0,
                               audit_root ? &optimized_root_provenance : nullptr,
                               root_status,
                               result.message);
    if (root_status != Status::Completed) {
      result.status = root_status;
      return result;
    }
    if (audit_root) {
      std::vector<ProvenanceRecord> reference_provenance;
      const auto reference = ternary_oracle(level1[0],
                                            level1[1],
                                            level1[2],
                                            instance_target(instance),
                                            instance.n,
                                            root_counts,
                                            &reference_provenance);
      if (!entries_equal(level0, reference) ||
          optimized_root_provenance != reference_provenance) {
        result.status = Status::InvariantViolation;
        result.message =
            "optimized L1-to-L0 set or provenance disagrees with scalar oracle";
        return result;
      }
    }
    attempt_telemetry.level0_size = level0.size();
    attempt_telemetry.level1_to_level0.output_fingerprint =
        stage_fingerprint(level0);
    attempt_telemetry.level1_to_level0.elapsed_ns = elapsed_ns(stage_start);
    attempt_telemetry.level0_allocated_bytes = estimated_entries_bytes(level0);
    std::uint64_t root_peak = level1_resident;
    if (!checked_add(root_peak,
                     attempt_telemetry.level0_allocated_bytes,
                     root_peak)) {
      result.status = Status::ResourceLimit;
      result.message = "root output memory accounting overflow";
      return result;
    }
    if (!checked_add(root_peak,
                     attempt_telemetry.level1_to_level0.allocated_bytes,
                     root_peak) ||
        root_peak > config.memory_limit_bytes) {
      result.status = Status::ResourceLimit;
      result.message = "root memory accounting overflow";
      return result;
    }
    result.telemetry.peak_working_bytes =
        std::max(result.telemetry.peak_working_bytes, root_peak);
    if (!level0.empty()) {
      const auto& candidate = level0.front();
      const auto candidate_digits = unpack_digits(candidate.digits);
      const auto verification_start = Clock::now();
      const auto verified = verify_full_ter_witness_with_coefficients(
          instance, candidate_digits, coefficients);
      attempt_telemetry.verification_ns += elapsed_ns(verification_start);
      if (verified.status != Status::Completed) {
        result.status = Status::InvariantViolation;
        result.message = verified.message;
        return result;
      }
      result.witness = candidate_digits;
      attempt_telemetry.status = Status::Found;
      attempt_telemetry.total_ns = elapsed_ns(attempt_start);
      result.telemetry.attempts.push_back(attempt_telemetry);
      result.status = Status::Found;
      result.telemetry.total_solver_ns = elapsed_ns(solver_start);
      result.telemetry.fingerprint = fingerprint(result.witness, result.telemetry);
      return result;
    }
    attempt_telemetry.status = Status::NoWitnessThisAttempt;
    attempt_telemetry.total_ns = elapsed_ns(attempt_start);
    result.telemetry.attempts.push_back(attempt_telemetry);
  }
  result.status = Status::AttemptLimitReached;
  result.message = "deterministic TER attempts exhausted without proving UNSAT";
  result.telemetry.total_solver_ns = elapsed_ns(solver_start);
  result.telemetry.fingerprint = fingerprint({}, result.telemetry);
  return result;
}

FullTerResult solve_full_ter(const Instance& instance, const FullTerConfig& config) {
  const auto call_start = Clock::now();
  const auto finish = [&](FullTerResult result) {
    if (result.telemetry.total_solver_ns == 0U) {
      result.telemetry.total_solver_ns = elapsed_ns(call_start);
    }
    result.telemetry.terminal_status = result.status;
    result.telemetry.witness_found = result.status == Status::Found;
    if (result.telemetry.fingerprint == 0U) {
      result.telemetry.fingerprint = fingerprint(result.witness, result.telemetry);
    }
    return result;
  };
  try {
    return finish(solve_full_ter_impl(instance, config));
  } catch (const std::bad_alloc&) {
    FullTerResult result;
    result.status = Status::ResourceLimit;
    result.message = "full TER allocation failed";
    result.telemetry.seed = config.seed;
    result.telemetry.n = instance.n;
    result.telemetry.modulus_bits = instance.n;
    result.telemetry.execution_mode = config.execution_mode;
    result.telemetry.family = config.family;
    return finish(std::move(result));
  } catch (const std::length_error& error) {
    FullTerResult result;
    result.status = Status::ResourceLimit;
    result.message = error.what();
    result.telemetry.seed = config.seed;
    result.telemetry.n = instance.n;
    result.telemetry.modulus_bits = instance.n;
    result.telemetry.execution_mode = config.execution_mode;
    result.telemetry.family = config.family;
    return finish(std::move(result));
  } catch (const std::invalid_argument& error) {
    FullTerResult result;
    result.status = Status::InvalidConfiguration;
    result.message = error.what();
    result.telemetry.seed = config.seed;
    result.telemetry.n = instance.n;
    result.telemetry.modulus_bits = instance.n;
    result.telemetry.execution_mode = config.execution_mode;
    result.telemetry.family = config.family;
    return finish(std::move(result));
  }
}

}  // namespace ter
