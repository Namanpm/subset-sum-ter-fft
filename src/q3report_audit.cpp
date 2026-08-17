#include "ter/q3report_audit.hpp"

#include "ter/packed.hpp"
#include "ter/reference_vector.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ter {
namespace {

constexpr std::uint64_t kNttPrime = 998'244'353ULL;
constexpr std::uint64_t kNttPrimitiveRoot = 3ULL;
constexpr std::uint32_t kNttMaximumPower = 23U;

using Clock = std::chrono::steady_clock;

[[nodiscard]] std::uint64_t elapsed_ns(const Clock::time_point start) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
}

[[nodiscard]] bool checked_add(const std::uint64_t left,
                               const std::uint64_t right,
                               std::uint64_t* const result) noexcept {
  if (left > std::numeric_limits<std::uint64_t>::max() - right) {
    return false;
  }
  *result = left + right;
  return true;
}

[[nodiscard]] bool checked_multiply(const std::uint64_t left,
                                    const std::uint64_t right,
                                    std::uint64_t* const result) noexcept {
  if (left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left) {
    return false;
  }
  *result = left * right;
  return true;
}

[[nodiscard]] bool add_bytes(std::uint64_t* const total,
                             const std::uint64_t count,
                             const std::uint64_t element_size) noexcept {
  std::uint64_t bytes = 0;
  return checked_multiply(count, element_size, &bytes) && checked_add(*total, bytes, total);
}

[[nodiscard]] bool is_power_of_two(const std::uint64_t value) noexcept {
  return value != 0U && (value & (value - 1U)) == 0U;
}

[[nodiscard]] std::uint64_t positive_mod(const std::int64_t value,
                                         const std::uint64_t modulus) noexcept {
  const auto signed_modulus = static_cast<std::int64_t>(modulus);
  std::int64_t result = value % signed_modulus;
  if (result < 0) {
    result += signed_modulus;
  }
  return static_cast<std::uint64_t>(result);
}

struct Q3Arithmetic {
  std::uint8_t r2 = 0;
  std::uint8_t r1 = 0;
  std::uint64_t m2 = 0;
  std::uint64_t m1 = 0;
  std::uint64_t mask_m2 = 0;
  std::uint64_t mask_m1 = 0;
  std::uint64_t q = 0;
  std::uint64_t tau = 0;
  std::array<std::uint64_t, 3> child_targets{};
  std::uint64_t parent_target = 0;
};

[[nodiscard]] Q3ReportStatus derive_q3_arithmetic(const InputSnapshot& input,
                                                   Q3Arithmetic* const value,
                                                   std::string* const message) {
  const auto& source = input.spec.arithmetic;
  const auto r1 = source.b1;
  const auto r2 = source.b2;
  if (r2 == 0U || r2 >= r1 || r1 > 62U || r1 >= input.spec.shape.n ||
      input.instance.b1 != r1) {
    *message = "Q3 requires 0 < r2 < r1 <= 62, r1 < n, and matching instance arithmetic";
    return Q3ReportStatus::InvalidConfiguration;
  }
  const std::uint32_t quotient_bits = static_cast<std::uint32_t>(r1) - r2;
  if (quotient_bits >= 63U) {
    *message = "Q3 quotient width is not representable";
    return Q3ReportStatus::InvalidConfiguration;
  }
  value->r1 = r1;
  value->r2 = r2;
  value->m2 = std::uint64_t{1} << r2;
  value->m1 = std::uint64_t{1} << r1;
  value->mask_m2 = value->m2 - 1U;
  value->mask_m1 = value->m1 - 1U;
  value->q = std::uint64_t{1} << quotient_bits;
  value->child_targets = {source.target_a2 & value->mask_m2,
                          source.target_b2 & value->mask_m2,
                          source.target_c2 & value->mask_m2};
  value->parent_target = source.target_1 & value->mask_m1;

  // This is deliberately signed and widened.  The exact division is checked below;
  // it is not the old unsigned wraparound form of tau.
  const std::int64_t delta = static_cast<std::int64_t>(value->parent_target) -
                             static_cast<std::int64_t>(value->child_targets[0]) -
                             static_cast<std::int64_t>(value->child_targets[1]) -
                             static_cast<std::int64_t>(value->child_targets[2]);
  if (delta % static_cast<std::int64_t>(value->m2) != 0) {
    *message = "parent and child modular targets do not define an integral tau";
    return Q3ReportStatus::InvalidSnapshot;
  }
  value->tau = positive_mod(delta / static_cast<std::int64_t>(value->m2), value->q);
  return Q3ReportStatus::Completed;
}

[[nodiscard]] Q3ReportStatus quotient_from_residue(const std::uint64_t residue,
                                                    const std::uint64_t target,
                                                    const Q3Arithmetic& arithmetic,
                                                    std::uint64_t* const result,
                                                    std::string* const message) {
  const std::int64_t delta = static_cast<std::int64_t>(residue & arithmetic.mask_m1) -
                             static_cast<std::int64_t>(target & arithmetic.mask_m2);
  if (delta % static_cast<std::int64_t>(arithmetic.m2) != 0) {
    *message = "child residue is not congruent to its level-2 target";
    return Q3ReportStatus::InvalidSnapshot;
  }
  *result = positive_mod(delta / static_cast<std::int64_t>(arithmetic.m2), arithmetic.q);
  return Q3ReportStatus::Completed;
}

[[nodiscard]] std::uint64_t fingerprint_snapshot(const InputSnapshot& input,
                                                 const Q3SnapshotFamily family,
                                                 const std::uint64_t seed) noexcept {
  std::uint64_t state = 14'695'981'039'346'656'037ULL;
  const auto mix = [&state](const std::uint64_t word) {
    state ^= word;
    state *= 1'099'511'628'211ULL;
  };
  mix(static_cast<std::uint64_t>(family));
  mix(seed);
  mix(input.instance.n);
  mix(input.instance.b1);
  mix(input.spec.arithmetic.b2);
  mix(input.spec.arithmetic.b1);
  mix(input.spec.arithmetic.target_a2);
  mix(input.spec.arithmetic.target_b2);
  mix(input.spec.arithmetic.target_c2);
  mix(input.spec.arithmetic.target_1);
  for (const auto residue : input.instance.residues_m1) {
    mix(residue);
  }
  for (const auto& list : input.lists) {
    mix(static_cast<std::uint64_t>(list.role));
    mix(list.target_m2);
    mix(list.entries.size());
    for (std::size_t entry_index = 0; entry_index < list.entries.size(); ++entry_index) {
      const auto& entry = list.entries[entry_index];
      mix(entry.id);
      mix(entry.q);
      mix(entry_index < list.residues_m1.size() ? list.residues_m1[entry_index] : 0U);
      for (const auto digit : entry.digits) {
        mix(static_cast<std::uint64_t>(static_cast<std::int64_t>(digit) + 1));
      }
    }
  }
  return state;
}

[[nodiscard]] Q3ReportStatus audit_frozen_snapshot(const FrozenL2MergeSnapshot& snapshot,
                                                    Q3Arithmetic* const arithmetic,
                                                    std::array<std::vector<std::uint64_t>, 3>* const quotients,
                                                    const Q3ReportConfig& config,
                                                    std::string* const message) {
  if (snapshot.fingerprint != fingerprint_snapshot(snapshot.input, snapshot.family, snapshot.seed)) {
    *message = "frozen snapshot fingerprint does not match its current contents";
    return Q3ReportStatus::InvalidSnapshot;
  }
  const auto status = derive_q3_arithmetic(snapshot.input, arithmetic, message);
  if (status != Q3ReportStatus::Completed) {
    return status;
  }
  const auto& input = snapshot.input;
  const auto& shape = input.spec.shape;
  if (input.instance.n != shape.n || shape.n == 0U || input.instance.residues_m1.size() != shape.n ||
      input.spec.arithmetic.n != shape.n || shape.child.total() != shape.n ||
      shape.parent.total() != shape.n || shape.half.total() * 2U != shape.n) {
    *message = "snapshot dimensions or TER distributions are inconsistent";
    return Q3ReportStatus::InvalidSnapshot;
  }
  constexpr std::array<ListRole, 3> kRoles{ListRole::A, ListRole::B, ListRole::C};
  std::uint64_t quotient_storage = 0;
  std::uint64_t packed_storage = 0;
  if (!add_bytes(&packed_storage, input.instance.values_le.size(), sizeof(std::uint64_t)) ||
      !add_bytes(&packed_storage, input.instance.residues_m1.size(), sizeof(std::uint64_t)) ||
      !add_bytes(&packed_storage, input.instance.planted_root.size(), sizeof(std::uint8_t)) ||
      !add_bytes(&packed_storage, input.instance.root_target_le.size(), sizeof(std::uint64_t))) {
    *message = "packed audit instance-copy accounting overflows";
    return Q3ReportStatus::ResourceLimit;
  }
  const std::uint64_t packed_words_per_entry =
      (static_cast<std::uint64_t>(shape.n) + 63U) / 64U;
  for (const auto& list : input.lists) {
    if (!add_bytes(&quotient_storage, list.entries.size(), sizeof(std::uint64_t)) ||
        !add_bytes(&packed_storage, list.entries.size(), sizeof(TerEntry)) ||
        !add_bytes(&packed_storage, list.entries.size(), sizeof(std::uint64_t)) ||
        !add_bytes(&packed_storage, list.entries.size(),
                   2U * packed_words_per_entry * sizeof(std::uint64_t))) {
      *message = "snapshot audit memory accounting overflows";
      return Q3ReportStatus::ResourceLimit;
    }
  }
  if (quotient_storage > config.memory_limit_bytes ||
      packed_storage > config.memory_limit_bytes - quotient_storage) {
    *message = "snapshot audit's quotient and packed-mask views exceed the memory ceiling";
    return Q3ReportStatus::ResourceLimit;
  }
  for (std::size_t list_index = 0; list_index < input.lists.size(); ++list_index) {
    const auto& list = input.lists[list_index];
    if (list.role != kRoles[list_index] || list.target_m2 != arithmetic->child_targets[list_index] ||
        list.entries.size() != list.residues_m1.size()) {
      *message = "child-list role, target, or residue metadata is invalid";
      return Q3ReportStatus::InvalidSnapshot;
    }
    auto& list_quotients = (*quotients)[list_index];
    list_quotients.clear();
    list_quotients.reserve(list.entries.size());
    for (std::size_t entry_index = 0; entry_index < list.entries.size(); ++entry_index) {
      const auto& entry = list.entries[entry_index];
      if (entry.id != entry_index || entry.digits.size() != shape.n) {
        *message = "child entry identity or vector length is invalid";
        return Q3ReportStatus::InvalidSnapshot;
      }
      const auto counts = count_digits(entry.digits);
      if (!counts.has_value() || counts->negative != shape.child.negative ||
          counts->zero != shape.child.zero || counts->positive != shape.child.positive ||
          !has_balanced_halves(entry.digits, shape.half)) {
        *message = "child entry violates the exact level-2 TER distribution";
        return Q3ReportStatus::InvalidSnapshot;
      }
      const std::uint64_t residue = inner_product_mod(
          input.instance.residues_m1, entry.digits, arithmetic->mask_m1);
      if (residue != list.residues_m1[entry_index] ||
          (residue & arithmetic->mask_m2) != list.target_m2) {
        *message = "child residue metadata does not match the original modular instance";
        return Q3ReportStatus::InvalidSnapshot;
      }
      std::uint64_t quotient = 0;
      const auto quotient_status = quotient_from_residue(
          residue, list.target_m2, *arithmetic, &quotient, message);
      if (quotient_status != Q3ReportStatus::Completed || entry.q != quotient) {
        if (quotient_status == Q3ReportStatus::Completed) {
          *message = "stored child quotient does not match signed exact quotient arithmetic";
        }
        return Q3ReportStatus::InvalidSnapshot;
      }
      if (entry_index > 0U && !(list.entries[entry_index - 1U].digits < entry.digits)) {
        *message = "child list is not strictly canonical";
        return Q3ReportStatus::InvalidSnapshot;
      }
      list_quotients.push_back(quotient);
    }
  }

  // A second representation audits positive/negative masks and unused bits.  This is
  // outside every timed kernel and catches scalar/packed representation disagreement.
  const auto packed = pack_snapshot(input);
  if (packed.status != Status::Completed) {
    *message = "unable to construct packed audit view: " + packed.message;
    return Q3ReportStatus::InvalidSnapshot;
  }
  const auto packed_audit = audit_packed_snapshot(packed.snapshot);
  if (packed_audit.status != Status::Completed) {
    *message = "packed positive/negative mask audit failed: " + packed_audit.message;
    return Q3ReportStatus::InvalidSnapshot;
  }
  return Q3ReportStatus::Completed;
}

struct ParentTruth {
  std::uint64_t raw = 0;
  std::uint64_t structural = 0;
  std::set<ReferenceTernaryVector> parents;
};

enum class ParentCheck : std::uint8_t {
  Valid,
  StructuralReject,
  DistributionReject,
  ArithmeticReject,
};

[[nodiscard]] ParentCheck form_exact_parent(const InputSnapshot& input,
                                             const ReferenceEntry& a,
                                             const ReferenceEntry& b,
                                             const ReferenceEntry& c,
                                             ReferenceTernaryVector* const parent) {
  parent->clear();
  parent->resize(input.spec.shape.n);
  DigitCounts counts;
  for (std::size_t coordinate = 0; coordinate < parent->size(); ++coordinate) {
    const int sum = static_cast<int>(a.digits[coordinate]) + static_cast<int>(b.digits[coordinate]) +
                    static_cast<int>(c.digits[coordinate]);
    if (sum < -1 || sum > 1) {
      parent->clear();
      return ParentCheck::StructuralReject;
    }
    const auto digit = static_cast<std::int8_t>(sum);
    (*parent)[coordinate] = digit;
    if (digit < 0) {
      ++counts.negative;
    } else if (digit > 0) {
      ++counts.positive;
    } else {
      ++counts.zero;
    }
  }
  const auto& expected = input.spec.shape.parent;
  if (counts.negative != expected.negative || counts.zero != expected.zero ||
      counts.positive != expected.positive) {
    parent->clear();
    return ParentCheck::DistributionReject;
  }
  if (inner_product_mod(input.instance.residues_m1, *parent,
                        input.spec.arithmetic.mask_m1) !=
      input.spec.arithmetic.target_1) {
    parent->clear();
    return ParentCheck::ArithmeticReject;
  }
  return ParentCheck::Valid;
}

[[nodiscard]] Q3ReportStatus remember_parent(ParentTruth* const truth,
                                              ReferenceTernaryVector parent,
                                              const Q3ReportConfig& config,
                                              std::string* const message) {
  if (truth->structural == std::numeric_limits<std::uint64_t>::max()) {
    *message = "structurally valid triple counter overflows";
    return Q3ReportStatus::ResourceLimit;
  }
  ++truth->structural;
  if (truth->parents.size() >= config.max_stored_parents) {
    *message = "exact parent-record ceiling reached before deduplication";
    return Q3ReportStatus::ResourceLimit;
  }
  truth->parents.insert(std::move(parent));
  return Q3ReportStatus::Completed;
}

[[nodiscard]] Q3ReportStatus run_independent_cubic_oracle(
    const FrozenL2MergeSnapshot& snapshot,
    const Q3ReportConfig& config,
    ParentTruth* const truth,
    bool* const ran,
    std::string* const message) {
  const auto& lists = snapshot.input.lists;
  std::uint64_t ab = 0;
  std::uint64_t triples = 0;
  if (!checked_multiply(lists[0].entries.size(), lists[1].entries.size(), &ab) ||
      !checked_multiply(ab, lists[2].entries.size(), &triples)) {
    *message = "cubic oracle triple count overflows";
    return Q3ReportStatus::ResourceLimit;
  }
  if (triples > config.max_oracle_triples) {
    *ran = false;
    return Q3ReportStatus::Completed;
  }
  *ran = true;
  const auto mask = snapshot.input.spec.arithmetic.mask_m1;
  const auto target = snapshot.input.spec.arithmetic.target_1;
  for (const auto& a : lists[0].entries) {
    const auto residue_a = inner_product_mod(snapshot.input.instance.residues_m1, a.digits, mask);
    for (const auto& b : lists[1].entries) {
      const auto residue_b = inner_product_mod(snapshot.input.instance.residues_m1, b.digits, mask);
      for (const auto& c : lists[2].entries) {
        const auto residue_c = inner_product_mod(snapshot.input.instance.residues_m1, c.digits, mask);
        // Deliberately not derived from q or tau: this is the original mod-2^r1 test.
        if (((((residue_a + residue_b) & mask) + residue_c) & mask) != target) {
          continue;
        }
        ++truth->raw;
        ReferenceTernaryVector parent;
        if (form_exact_parent(snapshot.input, a, b, c, &parent) != ParentCheck::Valid) {
          continue;
        }
        const auto remember = remember_parent(truth, std::move(parent), config, message);
        if (remember != Q3ReportStatus::Completed) {
          return remember;
        }
      }
    }
  }
  return Q3ReportStatus::Completed;
}

[[nodiscard]] Q3ReportStatus direct_exact_baseline(
    const FrozenL2MergeSnapshot& snapshot,
    const Q3Arithmetic& arithmetic,
    const std::array<std::vector<std::uint64_t>, 3>& quotients,
    const Q3ReportConfig& config,
    ParentTruth* const truth,
    Q3ReportTelemetry* const telemetry,
    std::string* const message) {
  const auto q = arithmetic.q;
  std::uint64_t offsets_count = 0;
  std::uint64_t direct_bytes = 0;
  if (!checked_add(q, 1U, &offsets_count) ||
      !add_bytes(&direct_bytes, offsets_count, sizeof(std::uint64_t)) ||
      !add_bytes(&direct_bytes, offsets_count, sizeof(std::uint64_t)) ||
      !add_bytes(&direct_bytes, quotients[2].size(), sizeof(std::uint64_t)) ||
      !add_bytes(&direct_bytes, quotients[0].size(), sizeof(std::uint64_t)) ||
      !add_bytes(&direct_bytes, quotients[1].size(), sizeof(std::uint64_t)) ||
      !add_bytes(&direct_bytes, quotients[2].size(), sizeof(std::uint64_t))) {
    *message = "direct baseline memory accounting overflows";
    return Q3ReportStatus::ResourceLimit;
  }
  std::uint64_t bounded_parent_bytes = 0;
  if (!checked_add(snapshot.input.spec.shape.n, 128U, &bounded_parent_bytes) ||
      !add_bytes(&direct_bytes, config.max_stored_parents, bounded_parent_bytes) ||
      direct_bytes > config.memory_limit_bytes) {
    *message = "direct baseline cannot fit its declared exact parent-record ceiling";
    return Q3ReportStatus::ResourceLimit;
  }
  telemetry->memory.direct_peak_bytes = direct_bytes;

  std::vector<std::uint64_t> offsets(static_cast<std::size_t>(offsets_count), 0U);
  for (const auto quotient : quotients[2]) {
    ++offsets[static_cast<std::size_t>(quotient) + 1U];
  }
  for (std::size_t index = 1; index < offsets.size(); ++index) {
    offsets[index] += offsets[index - 1U];
  }
  std::vector<std::uint64_t> cursor = offsets;
  std::vector<std::uint64_t> ids(quotients[2].size());
  for (std::size_t id = 0; id < quotients[2].size(); ++id) {
    ids[cursor[static_cast<std::size_t>(quotients[2][id])]++] = id;
  }

  const auto arithmetic_start = Clock::now();
  std::uint64_t raw = 0;
  std::uint64_t structural_checks = 0;
  for (std::size_t a_id = 0; a_id < quotients[0].size(); ++a_id) {
    for (std::size_t b_id = 0; b_id < quotients[1].size(); ++b_id) {
      const std::uint64_t c_q = (arithmetic.tau + arithmetic.q - quotients[0][a_id] -
                                 quotients[1][b_id]) & (arithmetic.q - 1U);
      const auto matching_c =
          offsets[static_cast<std::size_t>(c_q) + 1U] - offsets[static_cast<std::size_t>(c_q)];
      if (!checked_add(raw, matching_c, &raw)) {
        *message = "direct arithmetic triple counter overflows";
        return Q3ReportStatus::ResourceLimit;
      }
    }
  }
  telemetry->timing.direct_arithmetic_ns = elapsed_ns(arithmetic_start);

  const auto structural_start = Clock::now();
  for (std::size_t a_id = 0; a_id < quotients[0].size(); ++a_id) {
    for (std::size_t b_id = 0; b_id < quotients[1].size(); ++b_id) {
      const std::uint64_t c_q = (arithmetic.tau + arithmetic.q - quotients[0][a_id] -
                                 quotients[1][b_id]) & (arithmetic.q - 1U);
      for (std::uint64_t cursor_id = offsets[static_cast<std::size_t>(c_q)];
           cursor_id < offsets[static_cast<std::size_t>(c_q) + 1U]; ++cursor_id) {
        if (++structural_checks > config.max_structural_checks) {
          *message = "direct structural-check ceiling reached";
          return Q3ReportStatus::ResourceLimit;
        }
        ReferenceTernaryVector parent;
        const auto parent_check = form_exact_parent(
            snapshot.input,
            snapshot.input.lists[0].entries[a_id],
            snapshot.input.lists[1].entries[b_id],
            snapshot.input.lists[2].entries[ids[cursor_id]],
            &parent);
        if (parent_check != ParentCheck::Valid) {
          telemetry->structural_rejects += parent_check == ParentCheck::StructuralReject;
          telemetry->distribution_rejects += parent_check == ParentCheck::DistributionReject;
          telemetry->parent_arithmetic_rejects += parent_check == ParentCheck::ArithmeticReject;
          continue;
        }
        const auto dedup_start = Clock::now();
        const auto remember = remember_parent(truth, std::move(parent), config, message);
        telemetry->timing.direct_dedup_ns += elapsed_ns(dedup_start);
        if (remember != Q3ReportStatus::Completed) {
          return remember;
        }
      }
    }
  }
  const auto structural_wall_ns = elapsed_ns(structural_start);
  telemetry->timing.direct_structural_ns =
      structural_wall_ns >= telemetry->timing.direct_dedup_ns
          ? structural_wall_ns - telemetry->timing.direct_dedup_ns
          : 0U;
  truth->raw = raw;
  telemetry->raw_arithmetic_triples = raw;
  telemetry->structurally_valid_triples = truth->structural;
  const auto dedup_start = Clock::now();
  telemetry->unique_parents = truth->parents.size();
  telemetry->timing.direct_dedup_ns = elapsed_ns(dedup_start);
  return Q3ReportStatus::Completed;
}

[[nodiscard]] std::vector<std::uint64_t> scalar_cyclic_convolution(
    const std::vector<std::uint64_t>& left,
    const std::vector<std::uint64_t>& right,
    const std::uint64_t q) {
  std::vector<std::uint64_t> result(static_cast<std::size_t>(q), 0U);
  for (const auto a : left) {
    for (const auto b : right) {
      ++result[static_cast<std::size_t>((a + b) & (q - 1U))];
    }
  }
  return result;
}

[[nodiscard]] std::uint64_t mod_pow(std::uint64_t base, std::uint64_t exponent) noexcept {
  std::uint64_t result = 1U;
  while (exponent != 0U) {
    if ((exponent & 1U) != 0U) {
      result = (result * base) % kNttPrime;
    }
    base = (base * base) % kNttPrime;
    exponent >>= 1U;
  }
  return result;
}

void ntt_in_place(std::vector<std::uint64_t>* const values, const bool inverse) {
  auto& data = *values;
  for (std::size_t index = 1, reversed = 0; index < data.size(); ++index) {
    std::size_t bit = data.size() >> 1U;
    for (; (reversed & bit) != 0U; bit >>= 1U) {
      reversed ^= bit;
    }
    reversed ^= bit;
    if (index < reversed) {
      std::swap(data[index], data[reversed]);
    }
  }
  for (std::size_t length = 2; length <= data.size(); length <<= 1U) {
    std::uint64_t root = mod_pow(kNttPrimitiveRoot, (kNttPrime - 1U) / length);
    if (inverse) {
      root = mod_pow(root, kNttPrime - 2U);
    }
    const std::size_t half = length / 2U;
    for (std::size_t start = 0; start < data.size(); start += length) {
      std::uint64_t omega = 1U;
      for (std::size_t offset = 0; offset < half; ++offset) {
        const auto even = data[start + offset];
        const auto odd = (data[start + offset + half] * omega) % kNttPrime;
        data[start + offset] = (even + odd) % kNttPrime;
        data[start + offset + half] = (even + kNttPrime - odd) % kNttPrime;
        omega = (omega * root) % kNttPrime;
      }
    }
  }
  if (inverse) {
    const auto inverse_size = mod_pow(data.size(), kNttPrime - 2U);
    for (auto& value : data) {
      value = (value * inverse_size) % kNttPrime;
    }
  }
}

[[nodiscard]] Q3ReportStatus ntt_cyclic_convolution(
    const FrozenL2MergeSnapshot& snapshot,
    const std::array<std::vector<std::uint64_t>, 3>& quotients,
    const Q3Arithmetic& arithmetic,
    const Q3ReportConfig& config,
    Q3ReportTelemetry* const telemetry,
    std::string* const message) {
  const auto q = arithmetic.q;
  if (!is_power_of_two(q) || q > (std::uint64_t{1} << kNttMaximumPower) ||
      q > std::numeric_limits<std::size_t>::max()) {
    *message = "NTT length is unsupported by the exact 998244353 transform";
    return Q3ReportStatus::NotApplicable;
  }
  std::uint64_t pair_bound = 0;
  if (!checked_multiply(quotients[0].size(), quotients[1].size(), &pair_bound)) {
    *message = "pair coefficient bound overflows";
    return Q3ReportStatus::ArithmeticSafetyFailure;
  }
  // 998244353 = 119 * 2^23 + 1 and 3 is a primitive root.  A single modulus
  // reconstructs exact nonnegative counts only when every coefficient is < p.
  if (pair_bound >= kNttPrime) {
    *message = "single-prime exact reconstruction is unsafe; CRT is not implemented";
    return Q3ReportStatus::ArithmeticSafetyFailure;
  }
  std::uint64_t ntt_bytes = 0;
  if (!add_bytes(&ntt_bytes, q, sizeof(std::uint64_t)) ||  // scalar result
      !add_bytes(&ntt_bytes, q, sizeof(std::uint64_t)) ||  // NTT result
      !add_bytes(&ntt_bytes, q, sizeof(std::uint64_t)) ||  // frequency/transform A
      !add_bytes(&ntt_bytes, q, sizeof(std::uint64_t)) ||  // frequency/transform B
      !add_bytes(&ntt_bytes, config.max_stored_parents,
                 static_cast<std::uint64_t>(
                     snapshot.input.spec.shape.n) + 128U) ||
      ntt_bytes > config.memory_limit_bytes) {
    *message = "NTT output and transform buffers exceed the declared memory ceiling";
    return Q3ReportStatus::ResourceLimit;
  }
  telemetry->memory.ntt_persistent_bytes = 2U * q * sizeof(std::uint64_t);
  telemetry->memory.ntt_scratch_bytes = 2U * q * sizeof(std::uint64_t);

  const auto frequency_start = Clock::now();
  std::vector<std::uint64_t> left(static_cast<std::size_t>(q), 0U);
  std::vector<std::uint64_t> right(static_cast<std::size_t>(q), 0U);
  for (const auto quotient : quotients[0]) {
    ++left[static_cast<std::size_t>(quotient)];
  }
  for (const auto quotient : quotients[1]) {
    ++right[static_cast<std::size_t>(quotient)];
  }
  telemetry->timing.ntt_frequency_build_ns = elapsed_ns(frequency_start);
  const auto transform_start = Clock::now();
  ntt_in_place(&left, false);
  ntt_in_place(&right, false);
  for (std::size_t index = 0; index < left.size(); ++index) {
    left[index] = (left[index] * right[index]) % kNttPrime;
  }
  ntt_in_place(&left, true);
  telemetry->timing.ntt_transform_ns = elapsed_ns(transform_start);
  const auto reconstruction_start = Clock::now();
  telemetry->ntt_pair_counts = std::move(left);
  telemetry->timing.ntt_reconstruction_ns = elapsed_ns(reconstruction_start);
  return Q3ReportStatus::Completed;
}

[[nodiscard]] Q3Histogram histogram(const std::vector<std::uint64_t>& values) {
  Q3Histogram result;
  result.population = values.size();
  if (values.empty()) {
    return result;
  }
  std::vector<std::uint64_t> sorted = values;
  std::sort(sorted.begin(), sorted.end());
  result.maximum = sorted.back();
  for (const auto value : values) {
    result.zero += value == 0U;
    result.one += value == 1U;
    result.at_most_two += value <= 2U;
    result.at_most_four += value <= 4U;
  }
  const auto percentile = [&sorted](const std::uint64_t numerator) {
    const auto index = static_cast<std::size_t>((numerator * (sorted.size() - 1U)) / 100U);
    return sorted[index];
  };
  result.p50 = percentile(50U);
  result.p90 = percentile(90U);
  result.p99 = percentile(99U);
  return result;
}

void fill_selected_telemetry(const std::vector<std::uint64_t>& c_quotients,
                             const Q3Arithmetic& arithmetic,
                             Q3ReportTelemetry* const telemetry) {
  std::vector<bool> seen(static_cast<std::size_t>(arithmetic.q), false);
  std::vector<std::uint64_t> unique;
  std::vector<std::uint64_t> weighted;
  unique.reserve(c_quotients.size());
  weighted.reserve(c_quotients.size());
  for (const auto c_q : c_quotients) {
    const auto target = (arithmetic.tau + arithmetic.q - c_q) & (arithmetic.q - 1U);
    weighted.push_back(telemetry->ntt_pair_counts[static_cast<std::size_t>(target)]);
    if (!seen[static_cast<std::size_t>(target)]) {
      seen[static_cast<std::size_t>(target)] = true;
      unique.push_back(telemetry->ntt_pair_counts[static_cast<std::size_t>(target)]);
    }
  }
  telemetry->unique_selected_targets = unique.size();
  telemetry->unique_selected = histogram(unique);
  telemetry->c_weighted_selected = histogram(weighted);
}

}  // namespace

FrozenL2MergeSnapshotResult freeze_l2_merge_snapshot(InputSnapshot input,
                                                      const Q3SnapshotFamily family,
                                                      const std::uint64_t seed) {
  FrozenL2MergeSnapshotResult result;
  result.value.input = std::move(input);
  result.value.family = family;
  result.value.seed = seed;
  result.value.fingerprint = fingerprint_snapshot(result.value.input, family, seed);
  result.status = Q3ReportStatus::Completed;
  return result;
}

Q3ReportResult run_q3report_count_only_experiment(const FrozenL2MergeSnapshot& snapshot,
                                                   const Q3ReportConfig& config) {
  try {
    Q3ReportResult result;
    if (!config.full_residue_audit || config.memory_limit_bytes == 0U ||
        config.max_structural_checks == 0U || config.max_stored_parents == 0U ||
        config.max_oracle_triples == 0U) {
      result.status = Q3ReportStatus::InvalidConfiguration;
      result.message = "Q3 requires a full residue audit and nonzero explicit resource ceilings";
      return result;
    }
    const auto audit_start = Clock::now();
    Q3Arithmetic arithmetic;
    std::array<std::vector<std::uint64_t>, 3> quotients;
    auto status = audit_frozen_snapshot(snapshot, &arithmetic, &quotients, config,
                                        &result.message);
    result.telemetry.timing.snapshot_audit_ns = elapsed_ns(audit_start);
    if (status != Q3ReportStatus::Completed) {
      result.status = status;
      return result;
    }
    result.telemetry.a_size = quotients[0].size();
    result.telemetry.b_size = quotients[1].size();
    result.telemetry.c_size = quotients[2].size();
    result.telemetry.r2 = arithmetic.r2;
    result.telemetry.r1 = arithmetic.r1;
    result.telemetry.q = arithmetic.q;
    result.telemetry.tau = arithmetic.tau;
    result.telemetry.snapshot_fingerprint = snapshot.fingerprint;
    if (!checked_multiply(result.telemetry.a_size, result.telemetry.b_size,
                          &result.telemetry.pair_universe)) {
      result.status = Q3ReportStatus::ResourceLimit;
      result.message = "pair universe overflows";
      return result;
    }
    result.telemetry.lambda = static_cast<long double>(result.telemetry.pair_universe) /
                              static_cast<long double>(arithmetic.q);
    result.telemetry.predicted_support_fraction =
        1.0L - std::exp(-result.telemetry.lambda);

    ParentTruth direct_truth;
    status = direct_exact_baseline(snapshot, arithmetic, quotients, config, &direct_truth,
                                   &result.telemetry, &result.message);
    if (status != Q3ReportStatus::Completed) {
      result.status = status;
      return result;
    }

    ParentTruth oracle_truth;
    bool oracle_ran = false;
    status = run_independent_cubic_oracle(snapshot, config, &oracle_truth, &oracle_ran,
                                          &result.message);
    result.telemetry.independent_oracle_ran = oracle_ran;
    if (status != Q3ReportStatus::Completed) {
      result.status = status;
      return result;
    }
    if (oracle_ran && (oracle_truth.raw != direct_truth.raw ||
                       oracle_truth.structural != direct_truth.structural ||
                       oracle_truth.parents != direct_truth.parents)) {
      result.status = Q3ReportStatus::InternalInvariantFailure;
      result.message = "direct quotient baseline disagrees with the independent original-modulus oracle";
      return result;
    }

    if (config.run_scalar_reference) {
      const auto scalar_start = Clock::now();
      result.telemetry.scalar_pair_counts = scalar_cyclic_convolution(
          quotients[0], quotients[1], arithmetic.q);
      result.telemetry.timing.scalar_convolution_ns = elapsed_ns(scalar_start);
    }
    if (config.run_ntt) {
      status = ntt_cyclic_convolution(snapshot, quotients, arithmetic, config, &result.telemetry,
                                      &result.message);
      if (status != Q3ReportStatus::Completed) {
        result.status = status;
        return result;
      }
    }
    if (!config.run_scalar_reference || !config.run_ntt) {
      result.status = Q3ReportStatus::InvalidConfiguration;
      result.message = "this exact comparison experiment requires scalar reference and NTT enabled";
      return result;
    }
    if (result.telemetry.scalar_pair_counts != result.telemetry.ntt_pair_counts) {
      result.status = Q3ReportStatus::InternalInvariantFailure;
      result.message = "NTT coefficients disagree with the scalar cyclic convolution";
      return result;
    }
    const auto histogram_start = Clock::now();
    result.telemetry.all_residues = histogram(result.telemetry.ntt_pair_counts);
    result.telemetry.occupied_pair_residues =
        result.telemetry.all_residues.population - result.telemetry.all_residues.zero;
    result.telemetry.timing.full_histogram_ns = elapsed_ns(histogram_start);
    const auto selected_start = Clock::now();
    fill_selected_telemetry(quotients[2], arithmetic, &result.telemetry);
    result.telemetry.timing.selected_target_lookup_ns = elapsed_ns(selected_start);
    result.telemetry.exactness_passed = true;
    result.telemetry.cheap_kill_passed =
        result.telemetry.timing.ntt_frequency_build_ns + result.telemetry.timing.ntt_transform_ns +
            result.telemetry.timing.ntt_reconstruction_ns <
        result.telemetry.timing.direct_arithmetic_ns;
    result.status = Q3ReportStatus::Completed;
    return result;
  } catch (const std::bad_alloc&) {
    return {Q3ReportStatus::ResourceLimit, {}, "Q3 allocation failed after preflight"};
  } catch (const std::length_error&) {
    return {Q3ReportStatus::ResourceLimit, {}, "Q3 allocation length is unsupported"};
  } catch (const std::exception& exception) {
    return {Q3ReportStatus::InternalInvariantFailure, {},
            std::string{"Q3 internal exception: "} + exception.what()};
  }
}

}  // namespace ter
