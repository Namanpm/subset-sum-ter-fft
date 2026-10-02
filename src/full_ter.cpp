#include "ter/full_ter.hpp"

#include "ter/arithmetic.hpp"
#include "ter/limitations.hpp"
#include "ter/packed.hpp"
#include "ter/reference_vector.hpp"
#include "ter/tiled_q3report.hpp"
#include "ter/validator.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
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
using limits::checked::checked_add;
using limits::checked::checked_add_bytes;
using limits::checked::checked_multiply;

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

// Extract [offset, offset + bits) from the complete multiword residue.  This
// deliberately reads words above bit 63 before shifting, unlike to_uint64().
// M2 only requests at most 64 quotient bits, so the result is portable uint64.
[[nodiscard]] bool extract_bits_u64(const BigInt& value,
                                    const std::uint32_t offset,
                                    const std::uint32_t bits,
                                    std::uint64_t* const out) noexcept {
  if (out == nullptr || bits > limits::arithmetic::kMachineWordBits) {
    return false;
  }
  std::uint64_t result = 0U;
  std::uint32_t written = 0U;
  while (written < bits) {
    const std::uint32_t source_bit = offset + written;
    const std::size_t word_index = source_bit / 32U;
    const std::uint32_t in_word = source_bit % 32U;
    const std::uint32_t available = 32U - in_word;
    const std::uint32_t take = std::min(available, bits - written);
    const std::uint32_t word =
        word_index < value.words().size() ? value.words()[word_index] : 0U;
    std::uint32_t chunk = word >> in_word;
    if (take != 32U) {
      chunk &= (std::uint32_t{1} << take) - 1U;
    }
    result |= static_cast<std::uint64_t>(chunk) << written;
    written += take;
  }
  *out = result;
  return true;
}

[[nodiscard]] std::uint64_t low_mask(const std::uint32_t bits) noexcept {
  return bits == limits::arithmetic::kMachineWordBits
             ? std::numeric_limits<std::uint64_t>::max()
             : (std::uint64_t{1} << bits) - 1U;
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

// Canonical capacity-aware accounting for a retained std::vector<BigEntry>
// (repair item P24).
//
// The previous version charged sizeof(BigEntry) per *live* element and sized
// residues with words().size().  Both understate what the process still holds:
// a merge output keeps its grown allocation after canonicalization erases
// elements, and a BigInt keeps its word capacity.  A declared memory cap built
// on the old numbers was therefore not conservative.  Every resident/peak
// computation must use this one function so audit and non-audit modes, and the
// root and non-root stages, all mean the same thing by "bytes".
[[nodiscard]] bool entries_live_bytes(const std::vector<BigEntry>& entries,
                                      std::uint64_t* const bytes) noexcept {
  if (bytes == nullptr) {
    return false;
  }
  std::uint64_t total = 0U;
  if (!checked_multiply(entries.capacity(), sizeof(BigEntry), total)) {
    return false;
  }
  for (const auto& entry : entries) {
    std::uint64_t packed_bytes = 0U;
    std::uint64_t residue_bytes = 0U;
    if (!checked_multiply(entry.digits.words.capacity(), sizeof(std::uint64_t),
                          packed_bytes) ||
        !checked_multiply(entry.residue.words().capacity(), sizeof(std::uint32_t),
                          residue_bytes) ||
        !checked_add(total, packed_bytes, total) ||
        !checked_add(total, residue_bytes, total)) {
      return false;
    }
  }
  *bytes = total;
  return true;
}

[[nodiscard]] std::uint64_t estimated_entries_bytes(
    const std::vector<BigEntry>& entries) {
  std::uint64_t bytes = 0U;
  if (!entries_live_bytes(entries, &bytes)) {
    return std::numeric_limits<std::uint64_t>::max();
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

// Per-surviving-output admission cost, including provenance when audit mode
// retains it (repair item P26).
//
// A ProvenanceRecord and its packed parent are real retained bytes, so a merge
// that ignored them admitted more outputs under audit than the declared cap
// actually allows.  A memory cap must mean the same thing with audit on and
// off; only the native tiled L2->L1 path charged this before.
[[nodiscard]] bool merge_output_unit_bytes(const std::uint32_t n,
                                           const bool retains_provenance,
                                           std::uint64_t* const bytes) noexcept {
  if (bytes == nullptr) {
    return false;
  }
  std::uint64_t total = estimated_entry_bytes(n);
  if (retains_provenance) {
    const std::uint64_t packed_word_bytes =
        2U * ((static_cast<std::uint64_t>(n) + 63U) / 64U) * sizeof(std::uint64_t);
    if (!checked_add(total, sizeof(ProvenanceRecord), total) ||
        !checked_add(total, packed_word_bytes, total)) {
      return false;
    }
  }
  *bytes = total;
  return true;
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
      !limits::checked::fits_memory_budget(0U, index_bytes, memory_limit_bytes) ||
      left.size() > std::numeric_limits<std::uint32_t>::max() ||
      right.size() > std::numeric_limits<std::uint32_t>::max()) {
    status = Status::ResourceLimit;
    message = "L3-to-L2 residue index exceeds the declared resource limit";
    return {};
  }
  right_index.reserve(right.size());
  std::uint64_t index_remaining = 0U;
  if (!limits::checked::remaining_memory(index_bytes, memory_limit_bytes, &index_remaining)) {
    status = Status::ResourceLimit;
    message = "L3-to-L2 residue index exceeds the declared resource limit";
    return {};
  }
  std::uint64_t entry_bytes = 0U;
  if (!merge_output_unit_bytes(left.front().digits.n, provenance != nullptr,
                               &entry_bytes)) {
    status = Status::ResourceLimit;
    message = "L3-to-L2 output accounting overflow";
    return {};
  }
  const std::uint64_t maximum_outputs = entry_bytes == 0U ? 0U : index_remaining / entry_bytes;
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
  return bits == limits::arithmetic::kMachineWordBits ? std::numeric_limits<std::uint64_t>::max()
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
      !limits::checked::fits_memory_budget(0U, pair_bytes, memory_limit_bytes) ||
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
  std::uint64_t pair_remaining = 0U;
  if (!limits::checked::remaining_memory(pair_bytes, memory_limit_bytes, &pair_remaining)) {
    status = Status::ResourceLimit;
    message = "native ternary pair index exceeds the declared resource limit";
    return {};
  }
  std::uint64_t entry_bytes = 0U;
  if (!merge_output_unit_bytes(third.front().digits.n, provenance != nullptr,
                               &entry_bytes)) {
    status = Status::ResourceLimit;
    message = "native ternary output accounting overflow";
    return {};
  }
  const std::uint64_t maximum_outputs = entry_bytes == 0U ? 0U : pair_remaining / entry_bytes;
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

// Native-width L2-to-L1 tiled reporter.  This owns no ternary arithmetic of
// its own: it asks tiled_q3report for exact quotient witnesses, then retains
// the established packed add_three/canonicalization/provenance semantics.
struct FullTerTiledContext {
  const std::vector<BigEntry>* first = nullptr;
  const std::vector<BigEntry>* second = nullptr;
  const std::vector<BigEntry>* third = nullptr;
  const DigitCounts* expected = nullptr;
  std::vector<BigEntry>* result = nullptr;
  std::vector<ProvenanceRecord>* provenance = nullptr;
  FullTerStageTelemetry* telemetry = nullptr;
  std::uint64_t maximum_outputs = 0;
  // Repair item P13. When r1-r2 <= 32 the projected quotient is the whole
  // quotient and the tiled match decides. Above that the match is an exact
  // prefilter on the low 32 bits, and the parent residue must still be shown
  // congruent to the L1 target modulo 2^r1 before the parent may be retained.
  const BigInt* parent_target_r1 = nullptr;
  std::uint32_t modulus_bits = 0U;
  std::uint32_t r1 = 0U;
  bool projection_is_decisive = true;
};

TiledVisitDecision full_ter_tiled_visitor(void* const opaque,
                                          const EntryId first_id,
                                          const EntryId second_id,
                                          const EntryId third_id) {
  auto* const context = static_cast<FullTerTiledContext*>(opaque);
  // Stop before constructing another parent once the pre-reserved output
  // budget is full.  A resource limit must not silently drop a valid parent.
  if (context->result->size() >= context->maximum_outputs) {
    return TiledVisitDecision::ResourceLimit;
  }
  ++context->telemetry->modular_candidates;
  auto parent = add_three((*context->first)[first_id].digits,
                          (*context->second)[second_id].digits,
                          (*context->third)[third_id].digits,
                          *context->expected,
                          *context->telemetry);
  if (!parent.has_value()) {
    return TiledVisitDecision::Continue;
  }
  if (context->projection_is_decisive) [[likely]] {
    // r1-r2 <= 32: the tiled match proved the whole quotient congruence, so
    // this stays the original single-expression construction. Kept separate
    // from the wide path below because interposing a named residue here cost a
    // measured 6.9% of L2-to-L1 time at n=72.
    context->result->push_back(BigEntry{
        std::move(*parent),
        add_mod(add_mod((*context->first)[first_id].residue,
                        (*context->second)[second_id].residue,
                        (*context->third)[third_id].digits.n),
                (*context->third)[third_id].residue,
                (*context->third)[third_id].digits.n)});
  } else {
    // r1-r2 > 32: the tiled match only proved the low 32 quotient bits. An L1
    // entry is well formed only when its residue is congruent to the L1 target
    // modulo 2^r1, so prove that exactly over the arbitrary-width words rather
    // than trusting the projection.
    BigInt parent_residue =
        add_mod(add_mod((*context->first)[first_id].residue,
                        (*context->second)[second_id].residue,
                        (*context->third)[third_id].digits.n),
                (*context->third)[third_id].residue,
                (*context->third)[third_id].digits.n);
    if (!masked(subtract_mod(parent_residue, *context->parent_target_r1,
                             context->modulus_bits),
                context->r1)
             .is_zero()) {
      return TiledVisitDecision::Continue;
    }
    context->result->push_back(BigEntry{std::move(*parent), std::move(parent_residue)});
  }
  if (context->provenance != nullptr) {
    context->provenance->push_back(ProvenanceRecord{context->result->back().digits,
                                                    first_id,
                                                    second_id,
                                                    third_id});
  }
  return TiledVisitDecision::Continue;
}

[[nodiscard]] std::vector<BigEntry> ternary_tiled_l2_to_l1_native(
    const std::vector<BigEntry>& first,
    const std::vector<BigEntry>& second,
    const std::vector<BigEntry>& third,
    const BigInt& first_target_r2,
    const BigInt& second_target_r2,
    const BigInt& third_target_r2,
    const BigInt& parent_target_r1,
    const FullTerParameters& parameters,
    const DigitCounts& expected,
    const std::uint64_t memory_limit_bytes,
    FullTerStageTelemetry& telemetry,
    std::vector<ProvenanceRecord>* provenance,
    Status& status,
    std::string& message) {
  if (first.empty() || second.empty() || third.empty()) {
    status = Status::Completed;
    return {};
  }
  if (!limits::tiled::native_full_ter_l2_l1_supported(parameters.r1, parameters.r2)) {
    status = Status::InvalidConfiguration;
    message = "native tiled L2-to-L1 called outside its supported quotient width";
    return {};
  }
  if (first.size() > std::numeric_limits<EntryId>::max() ||
      second.size() > std::numeric_limits<EntryId>::max() ||
      third.size() > std::numeric_limits<EntryId>::max()) {
    status = Status::ResourceLimit;
    message = "native tiled L2-to-L1 list index exceeds EntryId range";
    return {};
  }

  // Repair item P13: project onto b1' = min(r1, r2 + 32). Masking a residue to
  // 2^b1' and shifting by r2 yields exactly the low 32 bits of the true
  // quotient, so this is identical to the old construction whenever
  // r1-r2 <= 32 and an exact prefilter above it.
  const std::uint32_t projected_b1 =
      std::min(parameters.r1, parameters.r2 + limits::arithmetic::kMaximumQuotientBits);
  const bool projection_is_decisive =
      parameters.r1 - parameters.r2 <= limits::arithmetic::kMaximumQuotientBits;
  const auto arithmetic = build_arithmetic(
      parameters.n,
      static_cast<std::uint8_t>(parameters.r2),
      static_cast<std::uint8_t>(projected_b1),
      first_target_r2.to_uint64(),
      second_target_r2.to_uint64(),
      parent_target_r1.to_uint64());
  if (arithmetic.status != Status::Completed) {
    status = arithmetic.status;
    message = "native tiled L2-to-L1 arithmetic construction failed: " + arithmetic.message;
    return {};
  }
  if (arithmetic.value.target_c2 != third_target_r2.to_uint64()) {
    status = Status::InvariantViolation;
    message = "derived third L2 target disagrees with the stored Full TER target";
    return {};
  }

  std::uint64_t pair_candidates = 0;
  if (!checked_multiply(first.size(), second.size(), pair_candidates) ||
      !checked_add(telemetry.pair_candidates, pair_candidates, telemetry.pair_candidates)) {
    status = Status::ResourceLimit;
    message = "native tiled L2-to-L1 pair count overflow";
    return {};
  }

  std::uint64_t count_auxiliary = 0;
  std::uint64_t report_auxiliary = 0;
  if (!tiled_q3report_auxiliary_bytes(first.size(), second.size(), third.size(),
                                      arithmetic.value.q,
                                      limits::tiled::kDefaultPreferredTileBits, true,
                                      &count_auxiliary, &report_auxiliary)) {
    status = Status::ResourceLimit;
    message = "native tiled L2-to-L1 auxiliary accounting overflow";
    return {};
  }
  std::uint64_t quotient_count = 0;
  std::uint64_t quotient_bytes = 0;
  if (!checked_add(first.size(), second.size(), quotient_count) ||
      !checked_add(quotient_count, third.size(), quotient_count) ||
      !checked_multiply(quotient_count, sizeof(TiledQEntry), quotient_bytes)) {
    status = Status::ResourceLimit;
    message = "native tiled L2-to-L1 quotient-array accounting overflow";
    return {};
  }
  std::uint64_t fixed_bytes = report_auxiliary;
  std::uint64_t remaining = 0U;
  if (!checked_add(fixed_bytes, quotient_bytes, fixed_bytes) ||
      !limits::checked::fits_memory_budget(0U, fixed_bytes, memory_limit_bytes) ||
      !limits::checked::remaining_memory(fixed_bytes, memory_limit_bytes, &remaining)) {
    status = Status::ResourceLimit;
    message = "native tiled L2-to-L1 fixed workspace exceeds the declared limit";
    return {};
  }

  std::uint64_t per_output = estimated_entry_bytes(parameters.n);
  const std::uint64_t packed_word_bytes =
      2U * ((static_cast<std::uint64_t>(parameters.n) + 63U) / 64U) * sizeof(std::uint64_t);
  if (provenance != nullptr &&
      (!checked_add(per_output, sizeof(ProvenanceRecord), per_output) ||
       !checked_add(per_output, packed_word_bytes, per_output))) {
    status = Status::ResourceLimit;
    message = "native tiled L2-to-L1 provenance accounting overflow";
    return {};
  }
  const std::uint64_t maximum_outputs = per_output == 0U ? 0U : remaining / per_output;
  std::uint64_t output_budget = 0;
  if (!checked_multiply(maximum_outputs, per_output, output_budget) ||
      !checked_add(fixed_bytes, output_budget, output_budget) ||
      maximum_outputs > std::numeric_limits<std::size_t>::max()) {
    status = Status::ResourceLimit;
    message = "native tiled L2-to-L1 output budget overflow";
    return {};
  }

  std::vector<TiledQEntry> first_q;
  std::vector<TiledQEntry> second_q;
  std::vector<TiledQEntry> third_q;
  first_q.reserve(first.size());
  second_q.reserve(second.size());
  third_q.reserve(third.size());
  for (std::size_t index = 0; index < first.size(); ++index) {
    first_q.push_back(TiledQEntry{quotient_residue(
                                      first[index].residue.to_uint64() & arithmetic.value.mask_m1,
                                      arithmetic.value.target_a2,
                                      arithmetic.value),
                                  static_cast<EntryId>(index)});
  }
  for (std::size_t index = 0; index < second.size(); ++index) {
    second_q.push_back(TiledQEntry{quotient_residue(
                                       second[index].residue.to_uint64() & arithmetic.value.mask_m1,
                                       arithmetic.value.target_b2,
                                       arithmetic.value),
                                   static_cast<EntryId>(index)});
  }
  for (std::size_t index = 0; index < third.size(); ++index) {
    third_q.push_back(TiledQEntry{quotient_residue(
                                      third[index].residue.to_uint64() & arithmetic.value.mask_m1,
                                      arithmetic.value.target_c2,
                                      arithmetic.value),
                                  static_cast<EntryId>(index)});
  }

  // Repair item P25: maximum_outputs is the memory-budget ceiling, not an
  // expected cardinality.  Reserving it up front retained an allocation sized
  // by the cap (gigabytes at a 4 GiB budget) even when the stage produced a
  // handful of survivors.  The vectors now grow geometrically like the root
  // output path; maximum_outputs stays purely as the admission bound enforced
  // by FullTerTiledContext.
  // Repair item P25, performance follow-up -- MEASURED AND REJECTED.
  //
  // Growing from zero costs a measured +8.2% of this stage at n=72. An
  // input-derived initial reserve was tried: under the reporter's projected
  // quotient modulus q the expected triple count is |A|*|B|*|C|/q, which at
  // n=72 predicts 111,437 against 110,160 measured modular candidates -- an
  // accurate estimate. Reserving it was still worse, because the structural
  // filter keeps only 19,109 of those, so the reserve over-allocates 5.8x and
  // the resulting sparse L1 lists hurt the downstream root scan:
  //
  //     stage        pre-repair   geometric growth   input-derived reserve
  //     L2->L1        98.28 ms    106.35 ms (+8.2%)  111.56 ms (+13.5%)
  //     L1->L0       531.79 ms    530.89 ms (-0.2%)  584.76 ms (+10.0%)
  //     whole solver 647.86 ms    658.40 ms (+1.6%)  713.52 ms (+10.1%)
  //
  // Geometric growth therefore stays. The +8.2% is accepted as the price of
  // removing a 6.9 GB commit; reserving the budget ceiling must never return.
  std::vector<BigEntry> result;
  FullTerTiledContext context;
  context.first = &first;
  context.second = &second;
  context.third = &third;
  context.expected = &expected;
  context.result = &result;
  context.provenance = provenance;
  context.telemetry = &telemetry;
  context.maximum_outputs = maximum_outputs;
  context.parent_target_r1 = &parent_target_r1;
  context.modulus_bits = parameters.n;
  context.r1 = parameters.r1;
  context.projection_is_decisive = projection_is_decisive;

  TiledQ3ReportConfig tiled_config;
  tiled_config.preferred_tile_bits = limits::tiled::kDefaultPreferredTileBits;
  tiled_config.memory_limit_bytes = memory_limit_bytes;
  const std::uint64_t modular_before = telemetry.modular_candidates;
  const auto reported = tiled_q3report_report(first_q, second_q, third_q,
                                              arithmetic.value.tau, arithmetic.value.q,
                                              tiled_config, full_ter_tiled_visitor, &context);
  telemetry.tiled_used = true;
  telemetry.pair_records = 0U;
  telemetry.tile_bits = reported.telemetry.tile_bits;
  telemetry.tiled_pair_probes += reported.telemetry.executed_pairs;
  telemetry.tiled_nonempty_windows += reported.telemetry.nonempty_windows;
  telemetry.tiled_auxiliary_bytes = std::max(telemetry.tiled_auxiliary_bytes, fixed_bytes);
  telemetry.tiled_setup_ns += reported.telemetry.setup_ns;
  telemetry.tiled_scan_ns += reported.telemetry.scan_ns;
  telemetry.allocated_bytes = std::max(telemetry.allocated_bytes, fixed_bytes);
  if (reported.status != TiledQ3ReportStatus::Completed) {
    status = reported.status == TiledQ3ReportStatus::ResourceLimit
                 ? Status::ResourceLimit
                 : reported.status == TiledQ3ReportStatus::Cancelled ? Status::Cancelled
                                                                       : Status::InvariantViolation;
    message = reported.message;
    return {};
  }
  if (telemetry.modular_candidates - modular_before != reported.telemetry.raw_triples) {
    status = Status::InvariantViolation;
    message = "native tiled L2-to-L1 modular witness count disagrees with the reporter";
    return {};
  }
  telemetry.survivors_before_dedup += result.size();
  if (provenance != nullptr) {
    canonicalize_provenance(*provenance);
  }
  canonicalize(result);
  telemetry.survivors_after_dedup += result.size();
  status = Status::Completed;
  return result;
}

[[nodiscard]] bool root_output_policy_supported(const RootOutputPolicy policy) noexcept {
  return policy == RootOutputPolicy::FirstWitness || policy == RootOutputPolicy::CountOnly ||
         policy == RootOutputPolicy::FullCanonicalOutput;
}

[[nodiscard]] bool root_output_live_bytes(const std::vector<BigEntry>& entries,
                                          const std::vector<ProvenanceRecord>* provenance,
                                          std::uint64_t* const bytes) noexcept {
  if (bytes == nullptr) {
    return false;
  }
  // Shares entries_live_bytes() so the root stage and every other stage agree
  // on what a retained entry vector costs (repair item P24).
  std::uint64_t total = 0U;
  if (!entries_live_bytes(entries, &total)) {
    return false;
  }
  if (provenance != nullptr) {
    std::uint64_t outer_bytes = 0U;
    if (!checked_multiply(provenance->capacity(), sizeof(ProvenanceRecord), outer_bytes) ||
        !checked_add(total, outer_bytes, total)) {
      return false;
    }
    for (const auto& record : *provenance) {
      std::uint64_t packed_bytes = 0U;
      if (!checked_multiply(record.parent.words.capacity(), sizeof(std::uint64_t), packed_bytes) ||
          !checked_add(total, packed_bytes, total)) {
        return false;
      }
    }
  }
  *bytes = total;
  return true;
}

[[nodiscard]] bool root_next_capacity(const std::size_t size,
                                      const std::size_t capacity,
                                      std::size_t* const next) noexcept {
  if (next == nullptr) {
    return false;
  }
  if (size < capacity) {
    *next = capacity;
    return true;
  }
  if (capacity == 0U) {
    *next = 1U;
    return true;
  }
  if (capacity > std::numeric_limits<std::size_t>::max() / 2U) {
    return false;
  }
  *next = capacity * 2U;
  return true;
}

// M2 root reporter. The low-w tiled equation is only a no-false-negative
// prefilter; the visitor checks the complete q-bit equation before any ternary
// parent is constructed. No |A|*|B| pair record is allocated on this path.
struct FullTerRootTiledContext {
  const std::vector<BigEntry>* first = nullptr;
  const std::vector<BigEntry>* second = nullptr;
  const std::vector<BigEntry>* third = nullptr;
  const std::vector<std::uint64_t>* first_quotients = nullptr;
  const std::vector<std::uint64_t>* second_quotients = nullptr;
  const std::vector<std::uint64_t>* third_quotients = nullptr;
  const DigitCounts* expected = nullptr;
  const BigInt* root_target = nullptr;
  std::vector<BigEntry>* result = nullptr;
  std::vector<ProvenanceRecord>* provenance = nullptr;
  FullTerStageTelemetry* telemetry = nullptr;
  const std::atomic_bool* cancellation = nullptr;
  RootOutputPolicy output_policy = RootOutputPolicy::FullCanonicalOutput;
  std::uint64_t tau = 0U;
  std::uint64_t quotient_mask = 0U;
  // Repair item P12. When the root quotient is at most 64 bits wide the
  // machine-word quotient congruence is exact and decisive, so a residue
  // disagreement downstream is a bug. Above 64 bits the stored quotient holds
  // only the low 64 bits: agreement is then necessary but not sufficient, and
  // the arbitrary-width residue words become the deciding authority.
  bool quotient_is_decisive = true;
  std::uint32_t modulus_bits = 0U;
  std::uint64_t maximum_outputs = 0U;
  std::uint64_t fixed_bytes = 0U;
  std::uint64_t memory_limit_bytes = 0U;
  std::uint64_t packed_digits_bytes = 0U;
  std::uint64_t residue_bytes = 0U;
};

[[nodiscard]] bool root_record_working_peak(FullTerRootTiledContext* const context,
                                             const std::uint64_t output_bytes) noexcept {
  std::uint64_t working_bytes = 0U;
  if (!checked_add(context->fixed_bytes, output_bytes, working_bytes) ||
      !limits::checked::fits_memory_budget(0U, working_bytes, context->memory_limit_bytes)) {
    return false;
  }
  context->telemetry->output_peak_bytes =
      std::max(context->telemetry->output_peak_bytes, output_bytes);
  context->telemetry->working_peak_bytes =
      std::max(context->telemetry->working_peak_bytes, working_bytes);
  return true;
}

[[nodiscard]] bool root_refresh_output_memory(FullTerRootTiledContext* const context) noexcept {
  std::uint64_t output_bytes = 0U;
  if (!root_output_live_bytes(*context->result, context->provenance, &output_bytes) ||
      !root_record_working_peak(context, output_bytes)) {
    return false;
  }
  context->telemetry->output_live_bytes = output_bytes;
  return true;
}

// Before add_three() allocates a packed parent, charge its maximum temporary
// payload and any vector reallocation overlap needed if that parent survives.
[[nodiscard]] bool root_preflight_candidate(FullTerRootTiledContext* const context,
                                             const bool may_retain) noexcept {
  std::uint64_t output_bytes = 0U;
  if (!root_output_live_bytes(*context->result, context->provenance, &output_bytes)) {
    return false;
  }
  if (may_retain) {
    if (context->result->size() == context->result->capacity()) {
      std::size_t next = 0U;
      std::uint64_t outer_bytes = 0U;
      if (!root_next_capacity(context->result->size(), context->result->capacity(), &next) ||
          !checked_multiply(next, sizeof(BigEntry), outer_bytes) ||
          !checked_add(output_bytes, outer_bytes, output_bytes)) {
        return false;
      }
    }
    if (context->provenance != nullptr &&
        context->provenance->size() == context->provenance->capacity()) {
      std::size_t next = 0U;
      std::uint64_t outer_bytes = 0U;
      if (!root_next_capacity(context->provenance->size(), context->provenance->capacity(),
                              &next) ||
          !checked_multiply(next, sizeof(ProvenanceRecord), outer_bytes) ||
          !checked_add(output_bytes, outer_bytes, output_bytes)) {
        return false;
      }
    }
  }
  // One packed parent, the two simultaneously live add_mod residues, and the
  // copied packed parent retained by provenance when auditing are all charged.
  std::uint64_t transient = context->packed_digits_bytes;
  if (!checked_add(transient, context->residue_bytes, transient) ||
      !checked_add(transient, context->residue_bytes, transient) ||
      (may_retain && context->provenance != nullptr &&
       !checked_add(transient, context->packed_digits_bytes, transient)) ||
      !checked_add(output_bytes, transient, output_bytes)) {
    return false;
  }
  return root_record_working_peak(context, output_bytes);
}

[[nodiscard]] bool root_reserve_for_append(FullTerRootTiledContext* const context) {
  if (context->result->size() >= context->maximum_outputs) {
    return false;
  }
  if (context->result->size() == context->result->capacity()) {
    std::size_t next = 0U;
    if (!root_next_capacity(context->result->size(), context->result->capacity(), &next)) {
      return false;
    }
    context->result->reserve(next);
  }
  if (context->provenance != nullptr &&
      context->provenance->size() == context->provenance->capacity()) {
    std::size_t next = 0U;
    if (!root_next_capacity(context->provenance->size(), context->provenance->capacity(),
                            &next)) {
      return false;
    }
    context->provenance->reserve(next);
  }
  return root_refresh_output_memory(context);
}

TiledVisitDecision full_ter_root_tiled_visitor(void* const opaque,
                                               const EntryId first_id,
                                               const EntryId second_id,
                                               const EntryId third_id) {
  auto* const context = static_cast<FullTerRootTiledContext*>(opaque);
  if (context->cancellation != nullptr &&
      context->cancellation->load(std::memory_order_relaxed)) {
    return TiledVisitDecision::Cancelled;
  }
  ++context->telemetry->low_prefilter_candidates;
  const std::uint64_t sum = (*context->first_quotients)[first_id] +
                            (*context->second_quotients)[second_id] +
                            (*context->third_quotients)[third_id];
  if ((sum & context->quotient_mask) != context->tau) {
    ++context->telemetry->full_quotient_rejects;
    return TiledVisitDecision::Continue;
  }
  if (!context->quotient_is_decisive) {
    // Q > 64: the low-64 quotient agreement above is an exact no-false-negative
    // prefilter only. Decide with the original arbitrary-width residue words --
    // never by truncation and never by a fingerprint. Rejections are counted as
    // quotient rejects so the reporter's candidate accounting stays exact.
    const BigInt wide_residue =
        add_mod(add_mod((*context->first)[first_id].residue,
                        (*context->second)[second_id].residue,
                        context->modulus_bits),
                (*context->third)[third_id].residue,
                context->modulus_bits);
    if (wide_residue != *context->root_target) {
      ++context->telemetry->full_quotient_rejects;
      return TiledVisitDecision::Continue;
    }
  }
  ++context->telemetry->modular_candidates;
  const bool may_retain = context->output_policy != RootOutputPolicy::CountOnly &&
                          context->result->size() < context->maximum_outputs;
  if (!root_preflight_candidate(context, may_retain)) {
    return TiledVisitDecision::ResourceLimit;
  }
  auto parent = add_three((*context->first)[first_id].digits,
                          (*context->second)[second_id].digits,
                          (*context->third)[third_id].digits,
                          *context->expected,
                          *context->telemetry);
  if (!parent.has_value()) {
    return TiledVisitDecision::Continue;
  }
  BigInt residue =
      add_mod(add_mod((*context->first)[first_id].residue,
                      (*context->second)[second_id].residue,
                      parent->n),
              (*context->third)[third_id].residue,
              parent->n);
  if (residue != *context->root_target) {
    return TiledVisitDecision::InvariantViolation;
  }
  if (!checked_add(context->telemetry->valid_root_count, 1U,
                   context->telemetry->valid_root_count) ||
      !checked_add(context->telemetry->survivors_before_dedup, 1U,
                   context->telemetry->survivors_before_dedup)) {
    return TiledVisitDecision::ResourceLimit;
  }
  if (context->output_policy == RootOutputPolicy::CountOnly) {
    return TiledVisitDecision::Continue;
  }
  if (!root_reserve_for_append(context)) {
    return TiledVisitDecision::ResourceLimit;
  }
  context->result->push_back(BigEntry{
      std::move(*parent),
      std::move(residue)});
  if (context->provenance != nullptr) {
    context->provenance->push_back(ProvenanceRecord{context->result->back().digits,
                                                    first_id,
                                                    second_id,
                                                    third_id});
  }
  if (!root_refresh_output_memory(context)) {
    return TiledVisitDecision::ResourceLimit;
  }
  if (context->output_policy == RootOutputPolicy::FirstWitness) {
    context->telemetry->early_stopped = true;
    return TiledVisitDecision::Stop;
  }
  return TiledVisitDecision::Continue;
}

[[nodiscard]] std::vector<BigEntry> ternary_tiled_l1_to_l0(
    const std::vector<BigEntry>& first,
    const std::vector<BigEntry>& second,
    const std::vector<BigEntry>& third,
    const std::array<BigInt, 3>& child_targets_r1,
    const BigInt& root_target,
    const FullTerParameters& parameters,
    const FullTerConfig& config,
    const RootOutputPolicy output_policy,
    const DigitCounts& expected,
    const std::uint64_t memory_limit_bytes,
    FullTerStageTelemetry& telemetry,
    std::vector<ProvenanceRecord>* provenance,
    Status& status,
    std::string& message) {
  if (!root_output_policy_supported(output_policy)) {
    status = Status::InvalidConfiguration;
    message = "tiled root output policy is invalid";
    return {};
  }
  if (first.empty() || second.empty() || third.empty()) {
    status = Status::Completed;
    return {};
  }
  // Repair item P12/P14: the bounded tiled root no longer refuses a quotient
  // wider than one machine word. It streams A x B and never materializes a pair
  // index, so the only real requirement is a well-formed 0 < r1 < n.
  if (!limits::full_ter::bounded_root_supported(parameters.n, parameters.r1)) {
    status = Status::InvalidConfiguration;
    message = "tiled root requires 0 < r1 < n";
    return {};
  }
  if (first.size() > std::numeric_limits<EntryId>::max() ||
      second.size() > std::numeric_limits<EntryId>::max() ||
      third.size() > std::numeric_limits<EntryId>::max()) {
    status = Status::ResourceLimit;
    message = "tiled root list index exceeds the EntryId range";
    return {};
  }
  const std::uint32_t quotient_bits = parameters.n - parameters.r1;
  const std::uint32_t requested_prefilter =
      std::min(config.root_prefilter_bits, quotient_bits);
  const std::uint32_t prefilter_bits = std::min(
      requested_prefilter, limits::arithmetic::kMaximumQuotientBits);
  if (!limits::full_ter::root_prefilter_configuration_supported(
          config.root_prefilter_bits, config.root_preferred_tile_bits, quotient_bits)) {
    status = Status::InvalidConfiguration;
    message = "tiled root prefilter or tile configuration is invalid";
    return {};
  }
  const std::uint64_t prefilter_q = std::uint64_t{1} << prefilter_bits;
  const std::uint64_t prefilter_mask = prefilter_q - 1U;
  // The stored quotient keeps min(Q, 64) bits. For Q <= 64 that is the whole
  // quotient and the congruence decides; for Q > 64 it is an exact prefilter
  // whose survivors are settled by the arbitrary-width residue comparison.
  const std::uint32_t checked_quotient_bits =
      std::min(quotient_bits, limits::arithmetic::kMachineWordBits);
  const bool quotient_is_decisive = quotient_bits <= limits::arithmetic::kMachineWordBits;
  const std::uint64_t quotient_mask = low_mask(checked_quotient_bits);
  telemetry.quotient_bits = quotient_bits;
  telemetry.prefilter_bits = prefilter_bits;

  std::uint64_t pair_candidates = 0U;
  if (!checked_multiply(first.size(), second.size(), pair_candidates) ||
      !checked_add(telemetry.pair_candidates, pair_candidates, telemetry.pair_candidates)) {
    status = Status::ResourceLimit;
    message = "tiled root logical pair count overflows";
    return {};
  }

  std::uint64_t count_auxiliary = 0U;
  std::uint64_t report_auxiliary = 0U;
  if (!tiled_q3report_auxiliary_bytes(first.size(), second.size(), third.size(), prefilter_q,
                                      config.root_preferred_tile_bits, true, &count_auxiliary,
                                      &report_auxiliary)) {
    status = Status::ResourceLimit;
    message = "tiled root auxiliary accounting overflows";
    return {};
  }
  std::uint64_t entry_count = 0U;
  std::uint64_t full_quotient_bytes = 0U;
  std::uint64_t projected_quotient_bytes = 0U;
  if (!checked_add(first.size(), second.size(), entry_count) ||
      !checked_add(entry_count, third.size(), entry_count) ||
      !checked_multiply(entry_count, sizeof(std::uint64_t), full_quotient_bytes) ||
      !checked_multiply(entry_count, sizeof(TiledQEntry), projected_quotient_bytes)) {
    status = Status::ResourceLimit;
    message = "tiled root quotient-array accounting overflows";
    return {};
  }
  std::uint64_t fixed_bytes = report_auxiliary;
  std::uint64_t remaining = 0U;
  if (!checked_add(fixed_bytes, full_quotient_bytes, fixed_bytes) ||
      !checked_add(fixed_bytes, projected_quotient_bytes, fixed_bytes) ||
      !limits::checked::fits_memory_budget(0U, fixed_bytes, memory_limit_bytes) ||
      !limits::checked::remaining_memory(fixed_bytes, memory_limit_bytes, &remaining)) {
    status = Status::ResourceLimit;
    message = "tiled root fixed workspace exceeds the declared memory limit";
    return {};
  }
  const std::uint64_t packed_digits_bytes =
      2U * ((static_cast<std::uint64_t>(parameters.n) + 63U) / 64U) * sizeof(std::uint64_t);
  const std::uint64_t residue_bytes =
      ((static_cast<std::uint64_t>(parameters.n) + 31U) / 32U) * sizeof(std::uint32_t);
  std::uint64_t per_output = sizeof(BigEntry);
  if (!checked_add(per_output, packed_digits_bytes, per_output) ||
      !checked_add(per_output, residue_bytes, per_output) ||
      (provenance != nullptr &&
       (!checked_add(per_output, sizeof(ProvenanceRecord), per_output) ||
        !checked_add(per_output, packed_digits_bytes, per_output)))) {
    status = Status::ResourceLimit;
    message = "tiled root output accounting overflows";
    return {};
  }
  const std::uint64_t maximum_outputs =
      output_policy == RootOutputPolicy::CountOnly ? 0U : remaining / per_output;
  if (output_policy != RootOutputPolicy::CountOnly &&
      maximum_outputs > std::numeric_limits<std::size_t>::max()) {
    status = Status::ResourceLimit;
    message = "tiled root output capacity exceeds addressable range";
    return {};
  }

  const auto derive_quotient = [&parameters, checked_quotient_bits](
                                   const BigEntry& entry,
                                   const BigInt& child_target,
                                   std::uint64_t* const out) {
    return extract_bits_u64(subtract_mod(entry.residue, child_target, parameters.n),
                            parameters.r1, checked_quotient_bits, out);
  };
  BigInt tau_residue = root_target;
  for (const auto& child_target : child_targets_r1) {
    tau_residue = subtract_mod(tau_residue, child_target, parameters.n);
  }
  std::uint64_t tau = 0U;
  if (!extract_bits_u64(tau_residue, parameters.r1, checked_quotient_bits, &tau)) {
    status = Status::InvariantViolation;
    message = "tiled root could not extract the full quotient target";
    return {};
  }
  const bool quotient_starts_beyond_uint64 =
      parameters.r1 >= limits::arithmetic::kMachineWordBits;
  const std::uint32_t high_source_quotient_bit = quotient_starts_beyond_uint64
                                                    ? 0U
                                                    : limits::arithmetic::kMachineWordBits -
                                                          parameters.r1;
  if ((quotient_starts_beyond_uint64 || quotient_bits > high_source_quotient_bit) &&
      (tau >> high_source_quotient_bit) != 0U) {
    telemetry.full_quotient_uses_bits_above_63 = true;
  }

  std::vector<std::uint64_t> first_full;
  std::vector<std::uint64_t> second_full;
  std::vector<std::uint64_t> third_full;
  std::vector<TiledQEntry> first_low;
  std::vector<TiledQEntry> second_low;
  std::vector<TiledQEntry> third_low;
  first_full.reserve(first.size());
  second_full.reserve(second.size());
  third_full.reserve(third.size());
  first_low.reserve(first.size());
  second_low.reserve(second.size());
  third_low.reserve(third.size());
  const auto append = [&derive_quotient, &telemetry, high_source_quotient_bit, quotient_bits,
                       prefilter_mask](const std::vector<BigEntry>& entries,
                                        const BigInt& child_target,
                                        std::vector<std::uint64_t>* full,
                                        std::vector<TiledQEntry>* low) {
    for (std::size_t id = 0U; id < entries.size(); ++id) {
      std::uint64_t quotient = 0U;
      if (!derive_quotient(entries[id], child_target, &quotient)) {
        return false;
      }
      if ((high_source_quotient_bit == 0U || quotient_bits > high_source_quotient_bit) &&
          (quotient >> high_source_quotient_bit) != 0U) {
        telemetry.full_quotient_uses_bits_above_63 = true;
      }
      full->push_back(quotient);
      low->push_back(TiledQEntry{static_cast<QuotientResidue>(quotient & prefilter_mask),
                                 static_cast<EntryId>(id)});
    }
    return true;
  };
  if (!append(first, child_targets_r1[0], &first_full, &first_low) ||
      !append(second, child_targets_r1[1], &second_full, &second_low) ||
      !append(third, child_targets_r1[2], &third_full, &third_low)) {
    status = Status::InvariantViolation;
    message = "tiled root could not extract a complete child quotient";
    return {};
  }

  std::vector<BigEntry> result;
  FullTerRootTiledContext context;
  context.first = &first;
  context.second = &second;
  context.third = &third;
  context.first_quotients = &first_full;
  context.second_quotients = &second_full;
  context.third_quotients = &third_full;
  context.expected = &expected;
  context.root_target = &root_target;
  context.result = &result;
  context.provenance = provenance;
  context.telemetry = &telemetry;
  context.cancellation = config.cancellation;
  context.output_policy = output_policy;
  context.tau = tau & quotient_mask;
  context.quotient_mask = quotient_mask;
  context.quotient_is_decisive = quotient_is_decisive;
  context.modulus_bits = parameters.n;
  context.maximum_outputs = maximum_outputs;
  context.fixed_bytes = fixed_bytes;
  context.memory_limit_bytes = memory_limit_bytes;
  context.packed_digits_bytes = packed_digits_bytes;
  context.residue_bytes = residue_bytes;
  telemetry.effective_root_output_policy = output_policy;
  if (output_policy == RootOutputPolicy::FirstWitness) {
    std::uint64_t ceiling = sizeof(BigEntry);
    if (!checked_add(ceiling, packed_digits_bytes, ceiling) ||
        !checked_add(ceiling, residue_bytes, ceiling) ||
        !checked_add(ceiling, residue_bytes, ceiling) ||
        (provenance != nullptr &&
         (!checked_add(ceiling, sizeof(ProvenanceRecord), ceiling) ||
          !checked_add(ceiling, packed_digits_bytes, ceiling)))) {
      status = Status::ResourceLimit;
      message = "tiled root one-witness output ceiling overflows";
      return {};
    }
    telemetry.first_witness_output_ceiling_bytes = ceiling;
  }
  if (!root_refresh_output_memory(&context)) {
    status = Status::ResourceLimit;
    message = "tiled root initial output accounting exceeds the declared memory limit";
    return {};
  }

  TiledQ3ReportConfig tiled_config;
  tiled_config.preferred_tile_bits = config.root_preferred_tile_bits;
  tiled_config.memory_limit_bytes = memory_limit_bytes;
  const auto reported = tiled_q3report_report(first_low, second_low, third_low,
                                               tau & prefilter_mask, prefilter_q, tiled_config,
                                               full_ter_root_tiled_visitor, &context);
  telemetry.tiled_used = true;
  telemetry.pair_records = 0U;
  telemetry.tile_bits = reported.telemetry.tile_bits;
  telemetry.tiled_pair_probes += reported.telemetry.executed_pairs;
  telemetry.tiled_nonempty_windows += reported.telemetry.nonempty_windows;
  telemetry.tiled_auxiliary_bytes = std::max(telemetry.tiled_auxiliary_bytes, fixed_bytes);
  telemetry.tiled_setup_ns += reported.telemetry.setup_ns;
  telemetry.tiled_scan_ns += reported.telemetry.scan_ns;
  telemetry.allocated_bytes = std::max(telemetry.allocated_bytes, fixed_bytes);
  if (reported.status != TiledQ3ReportStatus::Completed &&
      reported.status != TiledQ3ReportStatus::StoppedByVisitor) {
    status = reported.status == TiledQ3ReportStatus::ResourceLimit
                 ? Status::ResourceLimit
                 : reported.status == TiledQ3ReportStatus::Cancelled ? Status::Cancelled
                                                                       : Status::InvariantViolation;
    message = reported.message;
    return {};
  }
  if (telemetry.low_prefilter_candidates != reported.telemetry.raw_triples ||
      telemetry.low_prefilter_candidates != telemetry.full_quotient_rejects +
                                               telemetry.modular_candidates) {
    status = Status::InvariantViolation;
    message = "tiled root candidate accounting disagrees with the reporter";
    return {};
  }
  if (output_policy == RootOutputPolicy::FirstWitness) {
    if (reported.status == TiledQ3ReportStatus::StoppedByVisitor) {
      if (!telemetry.early_stopped || result.size() != 1U ||
          telemetry.valid_root_count != 1U) {
        status = Status::InvariantViolation;
        message = "first-witness root stop did not retain exactly one valid parent";
        return {};
      }
    } else if (telemetry.early_stopped || !result.empty()) {
      status = Status::InvariantViolation;
      message = "first-witness root completed without a valid stop state";
      return {};
    }
  } else if (reported.status == TiledQ3ReportStatus::StoppedByVisitor || telemetry.early_stopped) {
    status = Status::InvariantViolation;
    message = "non-first-witness root unexpectedly stopped early";
    return {};
  }
  if (output_policy == RootOutputPolicy::FullCanonicalOutput && provenance != nullptr) {
    canonicalize_provenance(*provenance);
  }
  if (output_policy == RootOutputPolicy::FullCanonicalOutput) {
    canonicalize(result);
    telemetry.survivors_after_dedup = result.size();
  } else if (output_policy == RootOutputPolicy::FirstWitness) {
    telemetry.survivors_after_dedup = result.size();
  }
  if (!root_refresh_output_memory(&context)) {
    status = Status::ResourceLimit;
    message = "tiled root final output accounting exceeds the declared memory limit";
    return {};
  }
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
  if (modulus_bits <= limits::arithmetic::kMachineWordBits) {
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
      !limits::checked::fits_memory_budget(0U, pair_bytes, memory_limit_bytes) ||
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

  std::uint64_t pair_remaining = 0U;
  if (!limits::checked::remaining_memory(pair_bytes, memory_limit_bytes, &pair_remaining)) {
    status = Status::ResourceLimit;
    message = "ternary pair index exceeds the declared resource limit";
    return {};
  }
  std::vector<BigEntry> result;
  std::uint64_t entry_bytes = 0U;
  if (!merge_output_unit_bytes(third.front().digits.n, provenance != nullptr,
                               &entry_bytes)) {
    status = Status::ResourceLimit;
    message = "ternary output accounting overflow";
    return {};
  }
  const std::uint64_t maximum_outputs = entry_bytes == 0U ? 0U : pair_remaining / entry_bytes;
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

[[nodiscard]] bool root_capture_bytes(const std::array<std::vector<BigEntry>, 3>& children,
                                      const std::array<BigInt, 3>& child_targets,
                                      const BigInt& root_target,
                                      std::uint64_t* const bytes) noexcept {
  if (bytes == nullptr) {
    return false;
  }
  std::uint64_t total = sizeof(FullTerRootCapture);
  const auto add_words = [&total](const BigInt& value) {
    return checked_add_bytes(&total, value.words().size(), sizeof(std::uint32_t));
  };
  for (const auto& target : child_targets) {
    if (!add_words(target)) {
      return false;
    }
  }
  if (!add_words(root_target)) {
    return false;
  }
  for (const auto& list : children) {
    if (!checked_add_bytes(&total, list.size(), sizeof(ReferenceTernaryVector)) ||
        !checked_add_bytes(&total, list.size(), sizeof(std::vector<std::uint32_t>))) {
      return false;
    }
    for (const auto& entry : list) {
      if (!checked_add_bytes(&total, entry.digits.n, sizeof(std::int8_t)) ||
          !add_words(entry.residue)) {
        return false;
      }
    }
  }
  *bytes = total;
  return true;
}

void populate_root_capture(FullTerRootCapture* const capture,
                           const std::array<std::vector<BigEntry>, 3>& children,
                           const std::array<BigInt, 3>& child_targets,
                           const BigInt& root_target,
                           const FullTerParameters& parameters,
                           const std::uint64_t seed,
                           const TerExecutionMode execution_mode) {
  *capture = {};
  capture->parameters = parameters;
  capture->seed = seed;
  capture->execution_mode = execution_mode;
  for (std::size_t group = 0U; group < children.size(); ++group) {
    capture->child_targets_words_le[group] = child_targets[group].words();
    capture->children[group].reserve(children[group].size());
    capture->residues_words_le[group].reserve(children[group].size());
    for (const auto& entry : children[group]) {
      capture->children[group].push_back(unpack_digits(entry.digits));
      capture->residues_words_le[group].push_back(entry.residue.words());
    }
  }
  capture->root_target_words_le = root_target.words();
  capture->status = Status::Completed;
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
    mix(attempt.level1_to_level0.quotient_bits);
    mix(attempt.level1_to_level0.prefilter_bits);
    mix(attempt.level1_to_level0.low_prefilter_candidates);
    mix(attempt.level1_to_level0.full_quotient_rejects);
    mix(attempt.level1_to_level0.pair_records);
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
      !limits::full_ter::finite_dimension_supported(n) ||
      !limits::full_ter::modulus_width_supported(n, modulus_bits)) {
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
      !limits::full_ter::runtime_limit_supported(memory_limit_bytes) ||
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
  if (!limits::full_ter::complete_level3_cardinality_representable(cardinality_overflow) &&
      mode == TerExecutionMode::FullTer) {
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
      !limits::checked::fits_memory_budget(0U, total_bytes, memory_limit_bytes) ||
      generated > std::numeric_limits<std::size_t>::max()) {
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
       !root_output_policy_supported(config.root_output_policy) ||
       config.root_output_policy == RootOutputPolicy::CountOnly ||
       !limits::full_ter::runtime_limit_supported(config.attempt_limit) ||
      !limits::full_ter::runtime_limit_supported(config.memory_limit_bytes) ||
      !limits::full_ter::instance_word_layout_supported(instance.n, instance.words_per_value) ||
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
  if (limits::full_ter::bounded_root_supported(parameters.value.n,
                                              parameters.value.r1) &&
      !limits::full_ter::root_prefilter_configuration_supported(
          config.root_prefilter_bits,
          config.root_preferred_tile_bits,
          parameters.value.n - parameters.value.r1)) {
    result.status = Status::InvalidConfiguration;
    result.message = "full TER tiled-root configuration is invalid";
    return result;
  }

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
        !limits::checked::fits_memory_budget(0U, generated_bytes,
                                              config.memory_limit_bytes)) {
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
      !limits::checked::fits_memory_budget(0U, preparation_peak,
                                            config.memory_limit_bytes)) {
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
      !limits::checked::fits_memory_budget(0U, guided_preparation_peak,
                                            config.memory_limit_bytes)) {
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
      !limits::checked::fits_memory_budget(0U, base_resident,
                                            config.memory_limit_bytes)) {
    result.status = Status::ResourceLimit;
    result.message = "materialized level-3 entries exceed the declared memory limit";
    return result;
  }
  result.telemetry.peak_working_bytes = std::max(base_resident, preparation_peak);
  result.telemetry.level3_allocated_bytes = level3_entries_bytes;
  result.telemetry.arithmetic_allocated_bytes = arithmetic_bytes;
  result.telemetry.level3_generation_ns = elapsed_ns(generation_start);

  for (std::uint32_t attempt = 0;
       limits::attempts::within_budget(attempt, config.attempt_limit);
       ++attempt) {
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
         !limits::checked::fits_memory_budget(0U, attempt_base_resident,
                                               config.memory_limit_bytes))) {
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
      std::uint64_t level2_merge_budget = 0U;
      if (!limits::checked::remaining_memory(level2_resident, config.memory_limit_bytes,
                                              &level2_merge_budget)) {
        result.status = Status::ResourceLimit;
        result.message = "resident level-3/level-2 lists leave no merge workspace";
        return result;
      }
      std::uint64_t oracle_work = 0;
      const bool audit_stage =
          config.audit &&
          checked_multiply(left_base.size(), right_base.size(), oracle_work) &&
          limits::oracle::cubic_work_supported(oracle_work);
      std::vector<ProvenanceRecord> optimized_provenance;
      Status stage_status = Status::InvariantViolation;
      level2[index] = binary_mitm(left_base,
                                  right_base,
                                  targets.level2[index],
                                  parameters.value.r2,
                                  parameters.value.level2,
                                  level2_merge_budget,
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
          !limits::checked::fits_memory_budget(0U, level2_resident,
                                                config.memory_limit_bytes)) {
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
        !limits::checked::fits_memory_budget(0U, level2_peak,
                                              config.memory_limit_bytes)) {
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
      std::uint64_t level1_merge_budget = 0U;
      if (!limits::checked::remaining_memory(level1_resident, config.memory_limit_bytes,
                                              &level1_merge_budget)) {
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
          limits::oracle::cubic_work_supported(oracle_work);
      std::vector<ProvenanceRecord> optimized_provenance;
      Status stage_status = Status::InvariantViolation;
      // Repair item P13: wide r1-r2 now stays on the bounded projected path
      // instead of diverting to the pair-materializing generic merge.
      if (limits::tiled::bounded_full_ter_l2_l1_supported(parameters.value.n,
                                                          parameters.value.r1,
                                                          parameters.value.r2)) {
        level1[group] = ternary_tiled_l2_to_l1_native(
            level2[group * 3U],
            level2[group * 3U + 1U],
            level2[group * 3U + 2U],
            targets.level2[group * 3U],
            targets.level2[group * 3U + 1U],
            targets.level2[group * 3U + 2U],
            targets.level1[group],
            parameters.value,
            parameters.value.level1,
            level1_merge_budget,
            attempt_telemetry.level2_to_level1,
            audit_stage ? &optimized_provenance : nullptr,
            stage_status,
            result.message);
      } else {
        level1[group] = ternary_mitm(level2[group * 3U],
                                     level2[group * 3U + 1U],
                                     level2[group * 3U + 2U],
                                     targets.level1[group],
                                     parameters.value.r1,
                                     parameters.value.level1,
                                     level1_merge_budget,
                                     attempt_telemetry.level2_to_level1,
                                     audit_stage ? &optimized_provenance : nullptr,
                                     stage_status,
                                     result.message);
      }
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
          !limits::checked::fits_memory_budget(0U, level1_resident,
                                                config.memory_limit_bytes)) {
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
        !limits::checked::fits_memory_budget(0U, level1_peak,
                                              config.memory_limit_bytes)) {
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
          limits::oracle::cubic_work_supported(root_work);
    // Repair item P12/P14: any well-formed shape now takes the bounded
    // streaming root. A quotient wider than one machine word no longer diverts
    // the production path into the pair-materializing generic merge.
    const bool tiled_root_supported =
        limits::full_ter::bounded_root_supported(parameters.value.n, parameters.value.r1);
    // The legacy multiword root reference remains canonical. The M2 root is
    // the bounded Phase-5 execution path; audit always wins over its default
    // first-witness policy.
    const RootOutputPolicy effective_root_policy =
        (config.audit || !tiled_root_supported) ? RootOutputPolicy::FullCanonicalOutput
                                                 : config.root_output_policy;
    std::vector<ProvenanceRecord> optimized_root_provenance;
    std::uint64_t root_merge_budget = 0U;
    if (!limits::checked::remaining_memory(level1_resident, config.memory_limit_bytes,
                                            &root_merge_budget)) {
      result.status = Status::ResourceLimit;
      result.message = "level-1 resident memory accounting exceeds the declared limit";
      return result;
    }
    std::uint64_t root_capture_bytes_value = 0U;
    if (config.root_capture != nullptr) {
      if (!root_capture_bytes(level1, targets.level1, instance_target(instance),
                              &root_capture_bytes_value) ||
          !limits::checked::remaining_memory(root_capture_bytes_value, root_merge_budget,
                                             &root_merge_budget)) {
        result.status = Status::ResourceLimit;
        result.message = "root capture exceeds the declared memory limit";
        return result;
      }
      populate_root_capture(config.root_capture, level1, targets.level1, instance_target(instance),
                            parameters.value, attempt_telemetry.seed, config.execution_mode);
    }
    std::vector<BigEntry> level0;
    if (tiled_root_supported) {
      level0 = ternary_tiled_l1_to_l0(
          level1[0], level1[1], level1[2], targets.level1, instance_target(instance),
          parameters.value, config, effective_root_policy, root_counts, root_merge_budget,
          attempt_telemetry.level1_to_level0,
          audit_root ? &optimized_root_provenance : nullptr, root_status, result.message);
    } else {
      attempt_telemetry.level1_to_level0.effective_root_output_policy =
          RootOutputPolicy::FullCanonicalOutput;
      level0 = ternary_mitm(level1[0], level1[1], level1[2], instance_target(instance),
                             instance.n, root_counts, root_merge_budget,
                             attempt_telemetry.level1_to_level0,
                             audit_root ? &optimized_root_provenance : nullptr,
                             root_status, result.message);
    }
    if (root_status != Status::Completed) {
      result.status = root_status;
      return result;
    }
    if (!tiled_root_supported) {
      const std::uint64_t output_bytes = estimated_entries_bytes(level0);
      std::uint64_t working_bytes = 0U;
      if (output_bytes == std::numeric_limits<std::uint64_t>::max() ||
          !checked_add(attempt_telemetry.level1_to_level0.allocated_bytes,
                       output_bytes,
                       working_bytes) ||
          !limits::checked::fits_memory_budget(0U, working_bytes, root_merge_budget)) {
        result.status = Status::ResourceLimit;
        result.message = "legacy root output accounting exceeds the declared memory limit";
        return result;
      }
      attempt_telemetry.level1_to_level0.output_live_bytes = output_bytes;
      attempt_telemetry.level1_to_level0.output_peak_bytes = output_bytes;
      attempt_telemetry.level1_to_level0.working_peak_bytes = working_bytes;
    }
    if (config.root_capture != nullptr &&
        !checked_add(attempt_telemetry.level1_to_level0.allocated_bytes,
                     root_capture_bytes_value,
                     attempt_telemetry.level1_to_level0.allocated_bytes)) {
      result.status = Status::ResourceLimit;
      result.message = "root capture accounting overflow";
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
    attempt_telemetry.level0_allocated_bytes =
        attempt_telemetry.level1_to_level0.output_live_bytes;
    std::uint64_t root_peak = level1_resident;
    if (!checked_add(root_peak,
                     root_capture_bytes_value,
                     root_peak)) {
      result.status = Status::ResourceLimit;
      result.message = "root capture memory accounting overflow";
      return result;
    }
    if (!checked_add(root_peak,
                     attempt_telemetry.level1_to_level0.working_peak_bytes,
                     root_peak) ||
        !limits::checked::fits_memory_budget(0U, root_peak,
                                              config.memory_limit_bytes)) {
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

FullTerL2CaptureResult capture_full_ter_l2_merge_snapshot(
    const Instance& instance,
    const FullTerConfig& config,
    const std::uint32_t group) {
  try {
    FullTerL2CaptureResult result;
    result.group = group;
    if (group >= 3U || config.execution_mode != TerExecutionMode::FullTer ||
        !limits::full_ter::runtime_limit_supported(config.memory_limit_bytes) ||
        !limits::full_ter::instance_word_layout_supported(instance.n, instance.words_per_value) ||
        instance.values_le.size() !=
            static_cast<std::size_t>(instance.n) * instance.words_per_value ||
        instance.root_target_le.size() != instance.words_per_value) {
      result.message =
          "capture requires one complete Full TER group and a valid full instance";
      return result;
    }
    const auto parameters = derive_full_ter_parameters(config.family, instance.n, instance.n);
    if (parameters.status != Status::Completed) {
      result.status = parameters.status;
      result.message = parameters.message;
      return result;
    }
    result.parameters = parameters.value;
    // Q3REPORT has a deliberately native, signed 64-bit quotient contract.
    if (!limits::q3report::native_capture_quotient_supported(parameters.value.r1,
                                                              parameters.value.r2)) {
      result.message =
          "this genuine Full TER instance is outside Q3REPORT's native quotient range";
      return result;
    }
    if (config.cancellation != nullptr &&
        config.cancellation->load(std::memory_order_relaxed)) {
      result.status = Status::Cancelled;
      result.message = "full TER capture was cancelled";
      return result;
    }
    auto generated = generate_level3_lists(parameters.value,
                                           TerExecutionMode::FullTer,
                                           config.fixture_level3_limit,
                                           config.memory_limit_bytes);
    if (generated.status != Status::Completed) {
      result.status = generated.status;
      result.message = generated.message;
      return result;
    }
    const auto coefficients = instance_coefficients(instance);
    Targets targets;
    if (config.target_mode == TerTargetMode::GuidedPlanted) {
      const auto guided = build_guided_tree(instance, parameters.value);
      if (!guided.has_value()) {
        result.message =
            "guided native capture requires a planted binary root balanced across halves";
        return result;
      }
      targets = guided_targets(*guided, coefficients, parameters.value);
    } else if (config.target_mode == TerTargetMode::Random) {
      targets = random_targets(instance, parameters.value, config.seed);
    } else {
      result.message = "native capture target mode is invalid";
      return result;
    }
    if (!targets_are_consistent(targets, instance, parameters.value)) {
      result.status = Status::InvariantViolation;
      result.message = "captured Full TER targets are inconsistent";
      return result;
    }
    const BigInt full_mask = pow2_mask(instance.n);
    const auto left_base = materialize_entries(
        std::move(generated.value.first_half), coefficients, full_mask);
    const auto right_base = materialize_entries(
        std::move(generated.value.second_half), coefficients, full_mask);
    std::uint64_t resident = 0;
    if (!checked_add(estimated_entries_bytes(left_base),
                     estimated_entries_bytes(right_base), resident) ||
        !limits::checked::fits_memory_budget(0U, resident,
                                              config.memory_limit_bytes)) {
      result.status = Status::ResourceLimit;
      result.message = "native capture level-3 buffers exceed the memory ceiling";
      return result;
    }
    std::array<std::vector<BigEntry>, 3> children;
    for (std::size_t child = 0; child < children.size(); ++child) {
      if (resident >= config.memory_limit_bytes) {
        result.status = Status::ResourceLimit;
        result.message = "native capture has no remaining level-2 workspace";
        return result;
      }
      std::uint64_t child_merge_budget = 0U;
      if (!limits::checked::remaining_memory(resident, config.memory_limit_bytes,
                                              &child_merge_budget)) {
        result.status = Status::ResourceLimit;
        result.message = "native capture has no remaining level-2 workspace";
        return result;
      }
      FullTerStageTelemetry telemetry;
      Status stage_status = Status::InvariantViolation;
      children[child] = binary_mitm(
          left_base,
          right_base,
          targets.level2[group * 3U + child],
          parameters.value.r2,
          parameters.value.level2,
          child_merge_budget,
          telemetry,
          nullptr,
          stage_status,
          result.message);
      if (stage_status != Status::Completed) {
        result.status = stage_status;
        return result;
      }
      if (!checked_add(resident, estimated_entries_bytes(children[child]), resident) ||
          !limits::checked::fits_memory_budget(0U, resident,
                                                config.memory_limit_bytes)) {
        result.status = Status::ResourceLimit;
        result.message = "native capture level-2 residents exceed the memory ceiling";
        return result;
      }
    }

    const auto arithmetic = build_arithmetic(
        instance.n,
        static_cast<std::uint8_t>(parameters.value.r2),
        static_cast<std::uint8_t>(parameters.value.r1),
        targets.level2[group * 3U].to_uint64(),
        targets.level2[group * 3U + 1U].to_uint64(),
        targets.level1[group].to_uint64());
    if (arithmetic.status != Status::Completed ||
        arithmetic.value.target_c2 != targets.level2[group * 3U + 2U].to_uint64()) {
      result.status = Status::InvariantViolation;
      result.message = "native capture could not derive the exact L2-to-L1 merge arithmetic";
      return result;
    }
    InputSnapshot snapshot;
    snapshot.instance = instance;
    snapshot.instance.b1 = static_cast<std::uint8_t>(parameters.value.r1);
    snapshot.instance.residues_m1.clear();
    snapshot.instance.residues_m1.reserve(coefficients.size());
    for (const auto& coefficient : coefficients) {
      snapshot.instance.residues_m1.push_back(
          coefficient.to_uint64() & arithmetic.value.mask_m1);
    }
    snapshot.spec.shape.family = config.family;
    snapshot.spec.shape.n = instance.n;
    snapshot.spec.shape.half = parameters.value.level3_active_half;
    snapshot.spec.shape.child = parameters.value.level2;
    snapshot.spec.shape.parent = parameters.value.level1;
    snapshot.spec.arithmetic = arithmetic.value;
    snapshot.spec.memory_limit_bytes = config.memory_limit_bytes;
    constexpr std::array<ListRole, 3> roles{ListRole::A, ListRole::B, ListRole::C};
    const std::array<std::uint64_t, 3> child_targets{
        arithmetic.value.target_a2, arithmetic.value.target_b2, arithmetic.value.target_c2};
    for (std::size_t child = 0; child < children.size(); ++child) {
      struct ConvertedEntry {
        ReferenceTernaryVector digits;
        std::uint64_t residue = 0;
      };
      std::vector<ConvertedEntry> converted;
      converted.reserve(children[child].size());
      for (const auto& entry : children[child]) {
        converted.push_back(
            {unpack_digits(entry.digits), entry.residue.to_uint64() & arithmetic.value.mask_m1});
      }
      std::sort(converted.begin(), converted.end(), [](const auto& left, const auto& right) {
        return left.digits < right.digits;
      });
      auto& list = snapshot.lists[child];
      list.role = roles[child];
      list.target_m2 = child_targets[child];
      list.entries.reserve(converted.size());
      list.residues_m1.reserve(converted.size());
      for (std::size_t id = 0; id < converted.size(); ++id) {
        list.residues_m1.push_back(converted[id].residue);
        list.entries.push_back(ReferenceEntry{
            static_cast<EntryId>(id),
            quotient_residue(converted[id].residue, child_targets[child], arithmetic.value),
            std::move(converted[id].digits)});
      }
    }
    const auto audited = audit_snapshot_children(snapshot);
    if (audited.status != Status::Completed) {
      result.status = Status::InvariantViolation;
      result.message = "native capture did not survive the shared TER child audit: " +
                       audited.message;
      return result;
    }
    result.snapshot = std::move(snapshot);
    result.status = Status::Completed;
    return result;
  } catch (const std::bad_alloc&) {
    return {Status::ResourceLimit, {}, {}, group, "native Full TER capture allocation failed"};
  } catch (const std::length_error& error) {
    return {Status::ResourceLimit, {}, {}, group, error.what()};
  } catch (const std::invalid_argument& error) {
    return {Status::InvariantViolation, {}, {}, group, error.what()};
  }
}

FullTerRootReportResult report_full_ter_root(const FullTerRootCapture& capture,
                                             const FullTerConfig& config,
                                             const RootOutputPolicy output_policy) {
  try {
    FullTerRootReportResult result;
    if (capture.status != Status::Completed ||
        !root_output_policy_supported(output_policy) ||
        !limits::full_ter::runtime_limit_supported(config.memory_limit_bytes) ||
        capture.parameters.n == 0U || capture.parameters.r1 >= capture.parameters.n ||
        !limits::full_ter::bounded_root_supported(capture.parameters.n,
                                                  capture.parameters.r1) ||
        !limits::full_ter::root_prefilter_configuration_supported(
            config.root_prefilter_bits, config.root_preferred_tile_bits,
            capture.parameters.n - capture.parameters.r1)) {
      result.message = "root report requires a completed native root capture and valid tiled limits";
      return result;
    }
    if (config.cancellation != nullptr &&
        config.cancellation->load(std::memory_order_relaxed)) {
      result.status = Status::Cancelled;
      result.message = "root report was cancelled";
      return result;
    }

    std::uint64_t entry_count = 0U;
    for (std::size_t group = 0U; group < capture.children.size(); ++group) {
      if (capture.children[group].size() != capture.residues_words_le[group].size() ||
          !checked_add(entry_count, capture.children[group].size(), entry_count)) {
        result.status = Status::InvalidConfiguration;
        result.message = "root capture child/residue lists are inconsistent";
        return result;
      }
    }
    std::uint64_t conversion_bytes = 0U;
    if (!checked_multiply(entry_count, estimated_entry_bytes(capture.parameters.n),
                          conversion_bytes) ||
        !limits::checked::fits_memory_budget(0U, conversion_bytes,
                                              config.memory_limit_bytes)) {
      result.status = Status::ResourceLimit;
      result.message = "root report capture conversion exceeds the declared memory limit";
      return result;
    }
    std::uint64_t root_budget = 0U;
    if (!limits::checked::remaining_memory(conversion_bytes, config.memory_limit_bytes,
                                            &root_budget)) {
      result.status = Status::ResourceLimit;
      result.message = "root report has no tiled workspace after capture conversion";
      return result;
    }

    std::array<BigInt, 3> child_targets;
    for (std::size_t group = 0U; group < child_targets.size(); ++group) {
      child_targets[group] = BigInt::from_words(capture.child_targets_words_le[group]);
      if (child_targets[group].bit_length() > capture.parameters.n) {
        result.status = Status::InvalidConfiguration;
        result.message = "root count child target exceeds the captured modulus";
        return result;
      }
    }
    const BigInt root_target = BigInt::from_words(capture.root_target_words_le);
    if (root_target.bit_length() > capture.parameters.n) {
      result.status = Status::InvalidConfiguration;
      result.message = "root count target exceeds the captured modulus";
      return result;
    }

    std::array<std::vector<BigEntry>, 3> children;
    for (std::size_t group = 0U; group < children.size(); ++group) {
      children[group].reserve(capture.children[group].size());
      for (std::size_t entry = 0U; entry < capture.children[group].size(); ++entry) {
        const auto& digits = capture.children[group][entry];
        if (digits.size() != capture.parameters.n) {
          result.status = Status::InvalidConfiguration;
          result.message = "root report capture digit width is invalid";
          return result;
        }
        BigInt residue = BigInt::from_words(capture.residues_words_le[group][entry]);
        if (residue.bit_length() > capture.parameters.n ||
            !masked(subtract_mod(residue, child_targets[group], capture.parameters.n),
                    capture.parameters.r1)
                 .is_zero()) {
          result.status = Status::InvalidConfiguration;
          result.message = "root report capture violates the child target invariant";
          return result;
        }
        children[group].push_back(BigEntry{pack_digits(digits), std::move(residue)});
      }
    }

    Status root_status = Status::InvariantViolation;
    const DigitCounts root_counts{0U, capture.parameters.n / 2U, capture.parameters.n / 2U};
    std::string message;
    const auto ignored_output = ternary_tiled_l1_to_l0(
        children[0], children[1], children[2], child_targets, root_target,
        capture.parameters, config, output_policy, root_counts, root_budget,
        result.telemetry, nullptr, root_status, message);
    if (output_policy == RootOutputPolicy::CountOnly && !ignored_output.empty()) {
      result.status = Status::InvariantViolation;
      result.message = "count-only root retained a parent";
      return result;
    }
    if (root_status != Status::Completed) {
      result.status = root_status;
      result.message = message;
      return result;
    }
    if (!checked_add(result.telemetry.working_peak_bytes, conversion_bytes,
                     result.telemetry.working_peak_bytes) ||
        !checked_add(result.telemetry.allocated_bytes, conversion_bytes,
                     result.telemetry.allocated_bytes) ||
        !limits::checked::fits_memory_budget(0U, result.telemetry.working_peak_bytes,
                                              config.memory_limit_bytes)) {
      result.status = Status::ResourceLimit;
      result.message = "root report working-memory accounting overflow";
      return result;
    }
    result.effective_root_output_policy = output_policy;
    result.valid_root_count = result.telemetry.valid_root_count;
    result.retained_root_count = ignored_output.size();
    result.status = Status::Completed;
    return result;
  } catch (const std::bad_alloc&) {
    FullTerRootReportResult result;
    result.status = Status::ResourceLimit;
    result.message = "root report allocation failed";
    return result;
  } catch (const std::length_error& error) {
    FullTerRootReportResult result;
    result.status = Status::ResourceLimit;
    result.message = error.what();
    return result;
  } catch (const std::invalid_argument& error) {
    FullTerRootReportResult result;
    result.status = Status::InvalidConfiguration;
    result.message = error.what();
    return result;
  }
}

FullTerRootCountResult count_full_ter_root(const FullTerRootCapture& capture,
                                           const FullTerConfig& config) {
  const auto report = report_full_ter_root(capture, config, RootOutputPolicy::CountOnly);
  return {report.status, report.valid_root_count, report.telemetry, report.message};
}

FullTerResult solve_full_ter(const Instance& instance, const FullTerConfig& config) {
  const auto call_start = Clock::now();
  const auto finish = [&](FullTerResult result) {
    if (result.telemetry.total_solver_ns == 0U) {
      // An immediate validation refusal can fall within the platform clock's
      // observable resolution. Preserve the established nonzero run-telemetry
      // contract without changing the terminal status or solver behavior.
      result.telemetry.total_solver_ns = std::max<std::uint64_t>(1U, elapsed_ns(call_start));
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

namespace {

// Releases a list's storage, not merely its size: clear() would keep the
// capacity, and with it the memory the staging exists to give back.
void release_entries(std::vector<BigEntry>& entries) noexcept {
  std::vector<BigEntry>().swap(entries);
}

FullTerRootStreamResult stream_full_ter_root_lists_impl(const Instance& instance,
                                                        const FullTerConfig& config,
                                                        const std::uint32_t attempt,
                                                        const FullTerRootStream& stream,
                                                        const std::uint64_t buffer_request) {
  FullTerRootStreamResult result;
  const auto start = Clock::now();
  if (stream.begin_list == nullptr || stream.write_records == nullptr ||
      config.execution_mode != TerExecutionMode::FullTer ||
      config.target_mode != TerTargetMode::Random ||
      !limits::full_ter::runtime_limit_supported(config.memory_limit_bytes) ||
      !limits::full_ter::instance_word_layout_supported(instance.n, instance.words_per_value) ||
      instance.values_le.size() !=
          static_cast<std::size_t>(instance.n) * instance.words_per_value ||
      instance.root_target_le.size() != instance.words_per_value ||
      !limits::attempts::within_budget(attempt, config.attempt_limit)) {
    result.message =
        "root stream requires Full TER, random targets, a valid instance, an attempt within "
        "the budget, and a stream";
    return result;
  }
  const auto parameters = derive_full_ter_parameters(config.family, instance.n, instance.n);
  if (parameters.status != Status::Completed) {
    result.status = parameters.status;
    result.message = parameters.message;
    return result;
  }
  result.parameters = parameters.value;
  if (config.cancellation != nullptr && config.cancellation->load(std::memory_order_relaxed)) {
    result.status = Status::Cancelled;
    result.message = "root stream was cancelled";
    return result;
  }

  const std::uint32_t words64 = (instance.n + 63U) / 64U;
  const std::uint32_t words32 = (instance.n + 31U) / 32U;
  result.record_bytes = words64 * 2U * 8U + words32 * 4U;
  const std::uint64_t buffer_records =
      std::max<std::uint64_t>(1U, buffer_request / result.record_bytes);
  result.stream_buffer_bytes = buffer_records * result.record_bytes;

  // Level-3 generation and its accounting, exactly as solve_full_ter does it.
  auto generated = generate_level3_lists(parameters.value, config.execution_mode,
                                         config.fixture_level3_limit, config.memory_limit_bytes);
  if (generated.status != Status::Completed) {
    result.status = generated.status;
    result.message = generated.message;
    return result;
  }
  std::uint64_t level3_vector_count = 0;
  std::uint64_t scalar_level3_bytes = 0;
  std::uint64_t coefficient_estimate = 0;
  std::uint64_t entry_estimate = 0;
  const std::uint64_t scalar_vector_bytes =
      sizeof(ReferenceTernaryVector) + static_cast<std::uint64_t>(instance.n);
  const std::uint64_t coefficient_bytes =
      sizeof(BigInt) +
      ((static_cast<std::uint64_t>(instance.n) + 31U) / 32U) * sizeof(std::uint32_t);
  std::uint64_t preparation_peak = 0;
  if (!checked_add(generated.value.first_half.size(), generated.value.second_half.size(),
                   level3_vector_count) ||
      !checked_multiply(level3_vector_count, scalar_vector_bytes, scalar_level3_bytes) ||
      !checked_multiply(instance.n, coefficient_bytes, coefficient_estimate) ||
      !checked_multiply(level3_vector_count, estimated_entry_bytes(instance.n),
                        entry_estimate) ||
      !checked_add(scalar_level3_bytes, coefficient_estimate, preparation_peak) ||
      !checked_add(preparation_peak, entry_estimate, preparation_peak) ||
      !limits::checked::fits_memory_budget(0U, preparation_peak, config.memory_limit_bytes)) {
    result.status = Status::ResourceLimit;
    result.message = "level-3 preparation exceeds the declared memory limit";
    return result;
  }
  const auto coefficients = instance_coefficients(instance);
  const BigInt full_mask = pow2_mask(instance.n);
  const auto left_base =
      materialize_entries(std::move(generated.value.first_half), coefficients, full_mask);
  const auto right_base =
      materialize_entries(std::move(generated.value.second_half), coefficients, full_mask);

  const std::uint64_t attempt_seed = config.seed + attempt;
  result.attempt_seed = attempt_seed;
  const Targets targets = random_targets(instance, parameters.value, attempt_seed);
  if (!targets_are_consistent(targets, instance, parameters.value)) {
    result.status = Status::InvariantViolation;
    result.message = "sampled TER targets violate the paper's sum relationships";
    return result;
  }
  std::uint64_t base_resident = 0;
  std::uint64_t level3_entries_bytes = 0;
  if (!checked_add(estimated_entries_bytes(left_base), estimated_entries_bytes(right_base),
                   level3_entries_bytes) ||
      !checked_add(level3_entries_bytes, estimated_bigints_bytes(coefficients), base_resident) ||
      !checked_add(base_resident, estimated_targets_bytes(targets), base_resident) ||
      !checked_add(base_resident, result.stream_buffer_bytes, base_resident) ||
      !limits::checked::fits_memory_budget(0U, base_resident, config.memory_limit_bytes)) {
    result.status = Status::ResourceLimit;
    result.message = "level-3 entries, targets and stream buffer exceed the declared limit";
    return result;
  }
  result.base_resident_bytes = base_resident;
  result.peak_working_bytes = std::max(preparation_peak, base_resident);
  for (std::size_t group = 0; group < 3U; ++group) {
    result.child_targets_words_le[group] = targets.level1[group].words();
  }
  result.root_target_words_le = instance_target(instance).words();

  std::vector<std::uint8_t> buffer(static_cast<std::size_t>(result.stream_buffer_bytes));
  for (std::uint32_t group = 0; group < 3U; ++group) {
    std::array<std::vector<BigEntry>, 3> level2;
    std::uint64_t resident = base_resident;
    std::uint64_t group_peak = base_resident;
    for (std::uint32_t child = 0; child < 3U; ++child) {
      const std::uint32_t index = group * 3U + child;
      if (config.cancellation != nullptr &&
          config.cancellation->load(std::memory_order_relaxed)) {
        result.status = Status::Cancelled;
        result.message = "root stream was cancelled";
        return result;
      }
      std::uint64_t merge_budget = 0U;
      if (resident >= config.memory_limit_bytes ||
          !limits::checked::remaining_memory(resident, config.memory_limit_bytes,
                                              &merge_budget)) {
        result.status = Status::ResourceLimit;
        result.message = "group-staged level-2 residents leave no merge workspace";
        return result;
      }
      std::uint64_t oracle_work = 0;
      const bool audit_stage = config.audit &&
                               checked_multiply(left_base.size(), right_base.size(), oracle_work) &&
                               limits::oracle::cubic_work_supported(oracle_work);
      std::vector<ProvenanceRecord> optimized_provenance;
      FullTerStageTelemetry telemetry;
      Status stage_status = Status::InvariantViolation;
      level2[child] = binary_mitm(left_base, right_base, targets.level2[index],
                                  parameters.value.r2, parameters.value.level2, merge_budget,
                                  telemetry, audit_stage ? &optimized_provenance : nullptr,
                                  stage_status, result.message);
      if (stage_status != Status::Completed) {
        result.status = stage_status;
        return result;
      }
      if (audit_stage) {
        std::vector<ProvenanceRecord> reference_provenance;
        const auto reference =
            binary_oracle(left_base, right_base, targets.level2[index], parameters.value.r2,
                          parameters.value.level2, &reference_provenance);
        if (!entries_equal(level2[child], reference) ||
            optimized_provenance != reference_provenance) {
          result.status = Status::InvariantViolation;
          result.message = "optimized L3-to-L2 set or provenance disagrees with scalar oracle";
          return result;
        }
      }
      result.level2_sizes[index] = level2[child].size();
      std::uint64_t stage_peak = 0;
      if (!checked_add(resident, telemetry.allocated_bytes, stage_peak) ||
          !checked_add(resident, estimated_entries_bytes(level2[child]), resident) ||
          !limits::checked::fits_memory_budget(0U, resident, config.memory_limit_bytes) ||
          !limits::checked::fits_memory_budget(0U, stage_peak, config.memory_limit_bytes)) {
        result.status = Status::ResourceLimit;
        result.message = "group-staged level-2 construction exceeds the declared limit";
        return result;
      }
      group_peak = std::max({group_peak, stage_peak, resident});
    }

    std::uint64_t merge_budget = 0U;
    if (resident >= config.memory_limit_bytes ||
        !limits::checked::remaining_memory(resident, config.memory_limit_bytes, &merge_budget)) {
      result.status = Status::ResourceLimit;
      result.message = "group-staged level-2 residents leave no level-1 workspace";
      return result;
    }
    std::uint64_t oracle_pairs = 0;
    std::uint64_t oracle_work = 0;
    const bool audit_stage =
        config.audit &&
        checked_multiply(level2[0].size(), level2[1].size(), oracle_pairs) &&
        checked_multiply(oracle_pairs, level2[2].size(), oracle_work) &&
        limits::oracle::cubic_work_supported(oracle_work);
    std::vector<ProvenanceRecord> optimized_provenance;
    FullTerStageTelemetry telemetry;
    Status stage_status = Status::InvariantViolation;
    std::vector<BigEntry> level1;
    if (limits::tiled::bounded_full_ter_l2_l1_supported(parameters.value.n, parameters.value.r1,
                                                        parameters.value.r2)) {
      level1 = ternary_tiled_l2_to_l1_native(
          level2[0], level2[1], level2[2], targets.level2[group * 3U],
          targets.level2[group * 3U + 1U], targets.level2[group * 3U + 2U],
          targets.level1[group], parameters.value, parameters.value.level1, merge_budget,
          telemetry, audit_stage ? &optimized_provenance : nullptr, stage_status,
          result.message);
    } else {
      level1 = ternary_mitm(level2[0], level2[1], level2[2], targets.level1[group],
                            parameters.value.r1, parameters.value.level1, merge_budget,
                            telemetry, audit_stage ? &optimized_provenance : nullptr,
                            stage_status, result.message);
    }
    if (stage_status != Status::Completed) {
      result.status = stage_status;
      return result;
    }
    if (audit_stage) {
      std::vector<ProvenanceRecord> reference_provenance;
      const auto reference = ternary_oracle(level2[0], level2[1], level2[2],
                                            targets.level1[group], parameters.value.r1,
                                            parameters.value.level1, &reference_provenance);
      if (!entries_equal(level1, reference) || optimized_provenance != reference_provenance) {
        result.status = Status::InvariantViolation;
        result.message = "optimized L2-to-L1 set or provenance disagrees with scalar oracle";
        return result;
      }
    }
    result.level1_sizes[group] = level1.size();
    std::uint64_t stage_peak = 0;
    if (!checked_add(resident, telemetry.allocated_bytes, stage_peak) ||
        !checked_add(resident, estimated_entries_bytes(level1), resident) ||
        !limits::checked::fits_memory_budget(0U, resident, config.memory_limit_bytes) ||
        !limits::checked::fits_memory_budget(0U, stage_peak, config.memory_limit_bytes)) {
      result.status = Status::ResourceLimit;
      result.message = "group-staged level-1 construction exceeds the declared limit";
      return result;
    }
    result.group_peak_bytes[group] = std::max({group_peak, stage_peak, resident});
    result.peak_working_bytes = std::max(result.peak_working_bytes, result.group_peak_bytes[group]);

    // The inputs of this group are dead once L1[g] exists.
    for (auto& list : level2) {
      release_entries(list);
    }
    if (!stream.begin_list(stream.context, group, level1.size(),
                           result.child_targets_words_le[group])) {
      result.status = Status::GenerationIncomplete;
      result.message = "root stream sink rejected the list header";
      return result;
    }
    std::size_t filled = 0U;
    for (const BigEntry& entry : level1) {
      if (entry.digits.n != instance.n || entry.digits.words.size() != words64 * 2U ||
          entry.residue.words().size() > words32) {
        result.status = Status::InvariantViolation;
        result.message = "level-1 entry has malformed packed dimensions";
        return result;
      }
      std::uint8_t* const record = buffer.data() + filled * result.record_bytes;
      std::memcpy(record, entry.digits.words.data(), words64 * 2U * 8U);
      std::uint8_t* const residue = record + words64 * 2U * 8U;
      std::memset(residue, 0, words32 * 4U);
      std::memcpy(residue, entry.residue.words().data(), entry.residue.words().size() * 4U);
      if (++filled == buffer_records) {
        if (!stream.write_records(stream.context, buffer.data(), filled)) {
          result.status = Status::GenerationIncomplete;
          result.message = "root stream sink rejected records";
          return result;
        }
        filled = 0U;
      }
    }
    if (filled != 0U && !stream.write_records(stream.context, buffer.data(), filled)) {
      result.status = Status::GenerationIncomplete;
      result.message = "root stream sink rejected records";
      return result;
    }
    release_entries(level1);
  }
  result.status = Status::Completed;
  result.elapsed_ns = elapsed_ns(start);
  return result;
}

}  // namespace

FullTerRootStreamResult stream_full_ter_root_lists(const Instance& instance,
                                                   const FullTerConfig& config,
                                                   const std::uint32_t attempt,
                                                   const FullTerRootStream& stream,
                                                   const std::uint64_t stream_buffer_bytes) {
  try {
    return stream_full_ter_root_lists_impl(instance, config, attempt, stream,
                                           stream_buffer_bytes);
  } catch (const std::bad_alloc&) {
    FullTerRootStreamResult result;
    result.status = Status::ResourceLimit;
    result.message = "root stream allocation failed";
    return result;
  } catch (const std::length_error& error) {
    FullTerRootStreamResult result;
    result.status = Status::ResourceLimit;
    result.message = error.what();
    return result;
  } catch (const std::invalid_argument& error) {
    FullTerRootStreamResult result;
    result.status = Status::InvariantViolation;
    result.message = error.what();
    return result;
  }
}

}  // namespace ter
