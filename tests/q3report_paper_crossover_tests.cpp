// Deterministic tests for the paper-scaling dense-NTT crossover experiment.
// Test identifiers follow Section 26 of the V2 implementation plan.

#include "ter/arithmetic.hpp"
#include "ter/full_ter.hpp"
#include "ter/instance.hpp"
#include "ter/q3report_paper_crossover.hpp"
#include "ter/reference_vector.hpp"
#include "ter/shape.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::uint64_t g_checks = 0;

void require(const bool condition, const std::string_view message) {
  ++g_checks;
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

struct LadderExpectation {
  std::uint64_t list_size;
  std::uint32_t q_bits;
  std::uint32_t r2;
  std::uint32_t r1;
};

// Declared in Section 3 before any timing result was seen.
constexpr std::array<LadderExpectation, 10> kDeclaredLadder{{
    {64U, 11U, 5U, 16U},
    {128U, 13U, 6U, 19U},
    {256U, 15U, 6U, 21U},
    {512U, 17U, 7U, 24U},
    {1'024U, 19U, 8U, 27U},
    {2'048U, 21U, 9U, 30U},
    {4'096U, 23U, 9U, 32U},
    {8'192U, 24U, 10U, 34U},
    {16'384U, 26U, 11U, 37U},
    {32'768U, 28U, 12U, 40U},
}};

[[nodiscard]] ter::PaperCrossoverSnapshotConfig small_config(const std::uint64_t list_size,
                                                             const std::uint64_t seed) {
  ter::PaperCrossoverSnapshotConfig config;
  config.requested_list_size = list_size;
  config.seed = seed;
  return config;
}

[[nodiscard]] ter::PaperCrossoverSnapshot must_build(
    const ter::PaperCrossoverSnapshotConfig& config) {
  const auto built = ter::build_paper_scaling_snapshot(config);
  require(built.status == ter::PaperCrossoverStatus::Completed,
          "paper-scaling snapshot must build: " + built.message);
  return built.value;
}

[[nodiscard]] std::vector<std::uint32_t> role_quotients(const ter::PaperCrossoverSnapshot& snapshot,
                                                        const std::size_t role) {
  std::vector<std::uint32_t> quotients;
  quotients.reserve(snapshot.input.lists[role].entries.size());
  for (const auto& entry : snapshot.input.lists[role].entries) {
    quotients.push_back(entry.q);
  }
  return quotients;
}

// --- A. Paper scaling derivation -------------------------------------------

void paper_scaling_derivation_matches_the_declared_ladder() {
  const auto ladder = ter::declared_paper_scaling_ladder();
  require(ladder.size() == kDeclaredLadder.size(), "declared ladder must have ten points");
  for (std::size_t index = 0; index < kDeclaredLadder.size(); ++index) {
    const auto& expected = kDeclaredLadder[index];
    require(ladder[index] == expected.list_size, "declared ladder list sizes must match");
    const auto point = ter::derive_paper_scaling_point(expected.list_size);
    require(point.list_size == expected.list_size, "derived list size must round-trip");
    require(point.q_bits == expected.q_bits, "derived q_bits must match the declared ladder");
    require(point.r2 == expected.r2, "derived r2 must match the declared ladder");
    require(point.r1 == expected.r1, "derived r1 must match the declared ladder");
    require(point.r1 == point.r2 + point.q_bits, "r1 must equal r2 + q_bits");
    require(point.q == (std::uint64_t{1} << expected.q_bits), "Q must equal 2^q_bits");
    require(point.effective_n > 0.0, "effective_n metadata must be positive");
  }
  require(ter::declared_paper_scaling_seeds().size() == 5U, "five declared primary seeds");
}

// --- B. Full TER parameter path --------------------------------------------

void scaling_generator_uses_the_full_ter_paper_rounded_parameter_path() {
  const auto snapshot = must_build(small_config(64U, 0x1001U));
  const auto derived = ter::derive_full_ter_parameters(ter::ShapeFamily::PaperRounded,
                                                       snapshot.shape_n, snapshot.shape_n);
  require(derived.status == ter::Status::Completed,
          "the chosen shape dimension must have a Full TER derivation");
  const auto& expected = derived.value;
  require(snapshot.parameters.level1.negative == expected.level1.negative &&
              snapshot.parameters.level1.zero == expected.level1.zero &&
              snapshot.parameters.level1.positive == expected.level1.positive,
          "snapshot level-1 counts must come from derive_full_ter_parameters");
  require(snapshot.parameters.level2.negative == expected.level2.negative &&
              snapshot.parameters.level2.zero == expected.level2.zero &&
              snapshot.parameters.level2.positive == expected.level2.positive,
          "snapshot level-2 counts must come from derive_full_ter_parameters");
  require(snapshot.parameters.level3_active_half.negative ==
                  expected.level3_active_half.negative &&
              snapshot.parameters.level3_active_half.zero == expected.level3_active_half.zero &&
              snapshot.parameters.level3_active_half.positive ==
                  expected.level3_active_half.positive,
          "snapshot half counts must come from derive_full_ter_parameters");
  require(snapshot.input.spec.shape.family == ter::ShapeFamily::PaperRounded,
          "the snapshot must record the paper-rounded family");
  require(snapshot.shape_n % 36U == 0U, "shape_n must be a valid Full TER dimension");
  require(snapshot.shape_n > snapshot.point.r1, "r1 must be strictly below shape_n");

  // The standalone FiniteSanity shape path is provably not the source.
  const auto sanity = ter::build_shape(ter::ShapeFamily::FiniteSanity, snapshot.shape_n);
  require(sanity.status == ter::Status::Completed, "FiniteSanity shape must build for comparison");
  require(sanity.shape.half.negative != snapshot.parameters.level3_active_half.negative ||
              sanity.shape.half.zero != snapshot.parameters.level3_active_half.zero ||
              sanity.shape.half.positive != snapshot.parameters.level3_active_half.positive,
          "the FiniteSanity half shape must differ from the Full TER level-3 active half");
  require(snapshot.family == ter::PaperCrossoverFamily::FullTerL2PaperScalingEmulation,
          "generated points are emulation, never AUTHENTIC_FULLTER");
  require(std::string_view{ter::paper_crossover_family_name(snapshot.family)} ==
              "FULLTER_L2_PAPER_SCALING_EMULATION",
          "the emulation family name is fixed by Section 5");
}

// --- C/F/G. Half structure, residue and quotient correctness ---------------

void every_generated_level2_entry_has_exact_full_ter_half_structure() {
  for (const std::uint64_t list_size : {64U, 128U, 256U}) {
    const auto snapshot = must_build(small_config(list_size, 0x2003U));
    const auto& arithmetic = snapshot.input.spec.arithmetic;
    const auto half = snapshot.shape_n / 2U;
    const std::array<std::uint64_t, 3> targets{arithmetic.target_a2, arithmetic.target_b2,
                                               arithmetic.target_c2};
    for (std::size_t role = 0; role < 3U; ++role) {
      const auto& list = snapshot.input.lists[role];
      require(list.entries.size() == list_size, "each role must reach the requested list size");
      std::set<ter::ReferenceTernaryVector> unique;
      std::set<ter::EntryId> identifiers;
      for (const auto& entry : list.entries) {
        const std::span<const std::int8_t> digits(entry.digits);
        require(digits.size() == snapshot.shape_n, "entry length must equal shape_n");
        for (const auto digit : digits) {
          require(digit == -1 || digit == 0 || digit == 1, "entries must be ternary");
        }
        const auto first = ter::count_digits(digits.first(half));
        const auto second = ter::count_digits(digits.subspan(half));
        const auto whole = ter::count_digits(digits);
        require(first.has_value() && second.has_value() && whole.has_value(),
                "digit counting must succeed");
        const auto& expected_half = snapshot.parameters.level3_active_half;
        require(first->negative == expected_half.negative && first->zero == expected_half.zero &&
                    first->positive == expected_half.positive,
                "first half must match level3_active_half exactly");
        require(second->negative == expected_half.negative && second->zero == expected_half.zero &&
                    second->positive == expected_half.positive,
                "second half must match level3_active_half exactly");
        const auto& expected_full = snapshot.parameters.level2;
        require(whole->negative == expected_full.negative && whole->zero == expected_full.zero &&
                    whole->positive == expected_full.positive,
                "full vector must match level2 exactly");
        // F. residue recomputed from raw digits and instance coefficients.
        const std::uint64_t residue = ter::inner_product_mod(
            snapshot.input.instance.residues_m1, entry.digits, arithmetic.mask_m1);
        require(residue == list.residues_m1[entry.id], "stored residue must be exact");
        require((residue & arithmetic.mask_m2) == targets[role],
                "residue must satisfy the role child target");
        // G. quotient recomputed with the canonical arithmetic.
        require(entry.q == ter::quotient_residue(residue, targets[role], arithmetic),
                "stored quotient must be canonical");
        require(static_cast<std::uint64_t>(entry.q) < arithmetic.q,
                "quotient must stay in the declared universe");
        require(unique.insert(entry.digits).second, "vectors must be unique within a list");
        require(identifiers.insert(entry.id).second, "EntryIds must be unique within a list");
      }
    }
    require(ter::audit_paper_crossover_snapshot(snapshot).status ==
                ter::PaperCrossoverStatus::Completed,
            "the independent snapshot audit must pass");
  }
}

// --- D. Genuine n=36 Full TER universe membership --------------------------

void sampled_level2_vectors_belong_to_the_genuine_n36_full_ter_universe() {
  constexpr std::uint32_t kShapeN = 36U;
  constexpr std::uint32_t kR2 = 3U;
  constexpr std::uint32_t kR1 = 12U;
  const auto parameters =
      ter::derive_full_ter_parameters(ter::ShapeFamily::PaperRounded, kShapeN, kShapeN);
  require(parameters.status == ter::Status::Completed, "n=36 Full TER parameters must derive");

  // The complete Full TER level-3 half sets, produced by the existing solver code.
  const auto generated = ter::generate_level3_lists(
      parameters.value, ter::TerExecutionMode::FullTer, 0U, 64U * 1024U * 1024U);
  require(generated.status == ter::Status::Completed, "complete n=36 level-3 generation");
  require(generated.value.complete, "the n=36 level-3 universe must be complete");
  const auto& first_half = generated.value.first_half;
  const auto& second_half = generated.value.second_half;
  require(!first_half.empty() && !second_half.empty(), "both level-3 halves must be populated");

  ter::PaperCrossoverSnapshotConfig config = small_config(8U, 0x3007U);
  config.override_shape_n = kShapeN;
  config.override_r2 = kR2;
  config.override_r1 = kR1;
  const auto snapshot = must_build(config);
  require(snapshot.shape_n == kShapeN, "override must select the genuine n=36 shape");
  require(snapshot.point.r2 == kR2 && snapshot.point.r1 == kR1, "override widths must apply");

  const auto& arithmetic = snapshot.input.spec.arithmetic;
  const std::array<std::uint64_t, 3> targets{arithmetic.target_a2, arithmetic.target_b2,
                                             arithmetic.target_c2};
  for (std::size_t role = 0; role < 3U; ++role) {
    // Independent enumeration of the complete target-conditioned L2 universe.
    std::set<ter::ReferenceTernaryVector> universe;
    for (const auto& left : first_half) {
      for (const auto& right : second_half) {
        ter::ReferenceTernaryVector combined(kShapeN, static_cast<std::int8_t>(0));
        bool valid = true;
        for (std::size_t coordinate = 0; coordinate < combined.size(); ++coordinate) {
          const int sum = static_cast<int>(left[coordinate]) + static_cast<int>(right[coordinate]);
          if (sum < -1 || sum > 1) {
            valid = false;
            break;
          }
          combined[coordinate] = static_cast<std::int8_t>(sum);
        }
        if (!valid) {
          continue;
        }
        const std::uint64_t residue = ter::inner_product_mod(
            snapshot.input.instance.residues_m1, combined, arithmetic.mask_m1);
        if ((residue & arithmetic.mask_m2) == targets[role]) {
          universe.insert(std::move(combined));
        }
      }
    }
    require(!universe.empty(), "the genuine target-conditioned universe must be non-empty");
    const auto& list = snapshot.input.lists[role];
    require(list.entries.size() == config.requested_list_size,
            "the sampler must supply the requested list size at n=36");
    const auto half = kShapeN / 2U;
    for (const auto& entry : list.entries) {
      require(universe.count(entry.digits) == 1U,
              "every sampled L2 vector must belong to the genuine Full TER universe");
      const std::span<const std::int8_t> digits(entry.digits);
      const auto first = ter::count_digits(digits.first(half));
      const auto second = ter::count_digits(digits.subspan(half));
      const auto whole = ter::count_digits(digits);
      const auto& expected_half = parameters.value.level3_active_half;
      require(first.has_value() && first->negative == expected_half.negative &&
                  first->zero == expected_half.zero && first->positive == expected_half.positive,
              "n=36 first-half counts must be exact");
      require(second.has_value() && second->negative == expected_half.negative &&
                  second->zero == expected_half.zero && second->positive == expected_half.positive,
              "n=36 second-half counts must be exact");
      require(whole.has_value() && whole->negative == parameters.value.level2.negative &&
                  whole->zero == parameters.value.level2.zero &&
                  whole->positive == parameters.value.level2.positive,
              "n=36 full counts must be exact");
      const std::uint64_t residue = ter::inner_product_mod(
          snapshot.input.instance.residues_m1, entry.digits, arithmetic.mask_m1);
      require(residue == list.residues_m1[entry.id], "n=36 stored residue must be exact");
      require(entry.q == ter::quotient_residue(residue, targets[role], arithmetic),
              "n=36 stored quotient must be exact");
    }
  }
}

// --- E. Determinism ---------------------------------------------------------

void identical_configurations_replay_and_distinct_seeds_diverge() {
  const auto config = small_config(128U, 0x4009U);
  const auto first = must_build(config);
  const auto second = must_build(config);
  require(first.fingerprint == second.fingerprint, "identical configurations must replay exactly");
  require(first.fingerprint == ter::paper_crossover_fingerprint(first),
          "the stored fingerprint must be reproducible");
  for (std::size_t role = 0; role < 3U; ++role) {
    require(first.input.lists[role].entries.size() == second.input.lists[role].entries.size(),
            "replayed lists must have identical sizes");
    for (std::size_t index = 0; index < first.input.lists[role].entries.size(); ++index) {
      require(first.input.lists[role].entries[index] == second.input.lists[role].entries[index],
              "replayed entries must be identical");
      require(first.input.lists[role].residues_m1[index] ==
                  second.input.lists[role].residues_m1[index],
              "replayed residues must be identical");
    }
  }
  const auto other = must_build(small_config(128U, 0x500BU));
  require(other.fingerprint != first.fingerprint,
          "the declared deterministic test seeds must produce distinct fingerprints");
}

// --- J/K. Verified moduli ---------------------------------------------------

void the_second_ntt_modulus_is_verified_mathematically() {
  require(ter::kPaperNttPrime2 == 2'013'265'921ULL, "p2 must be 2013265921");
  require(ter::kPaperNttPrime2 - 1U == 15ULL * (std::uint64_t{1} << 27U),
          "p2 - 1 must equal 15 * 2^27");
  require(ter::is_prime_u32(ter::kPaperNttPrime2), "p2 must be prime");
  require(!ter::is_prime_u32(ter::kPaperNttPrime2 - 1U), "p2 - 1 must not be prime");
  require(ter::is_prime_u32(ter::kPaperNttPrime1), "p1 must be prime");
  require(ter::kPaperNttPrime1 - 1U == 119ULL * (std::uint64_t{1} << 23U),
          "p1 - 1 must equal 119 * 2^23");
  // Deterministic primality decision cross-check against trial division.
  for (const std::uint64_t candidate : {2U, 3U, 4U, 9U, 97U, 561U, 1'105U, 2'821U, 65'537U,
                                        4'294'967'291U, 4'294'967'295U}) {
    bool trial = candidate >= 2U;
    for (std::uint64_t divisor = 2U; divisor * divisor <= candidate; ++divisor) {
      if (candidate % divisor == 0U) {
        trial = false;
        break;
      }
    }
    require(ter::is_prime_u32(candidate) == trial,
            "deterministic primality must agree with trial division");
  }

  constexpr std::array<std::uint64_t, 3> kFactors2{2U, 3U, 5U};
  for (const std::uint64_t factor : kFactors2) {
    require((ter::kPaperNttPrime2 - 1U) % factor == 0U, "p2 - 1 must be divisible by its factors");
    require(ter::modular_power(ter::kPaperNttRoot2, (ter::kPaperNttPrime2 - 1U) / factor,
                               ter::kPaperNttPrime2) != 1U,
            "31 must not collapse on any prime factor of p2 - 1");
  }
  {
    // {2,3,5} is the complete distinct-prime-factor set of p2 - 1.
    std::uint64_t residual = ter::kPaperNttPrime2 - 1U;
    for (const std::uint64_t factor : kFactors2) {
      while (residual % factor == 0U) {
        residual /= factor;
      }
    }
    require(residual == 1U, "2, 3 and 5 must exhaust p2 - 1");
  }
  require(ter::is_primitive_root_u32(ter::kPaperNttRoot2, ter::kPaperNttPrime2,
                                     std::span<const std::uint64_t>(kFactors2)),
          "31 must be a primitive root of p2");
  require(!ter::is_primitive_root_u32(4U, ter::kPaperNttPrime2,
                                      std::span<const std::uint64_t>(kFactors2)),
          "a perfect square cannot be a primitive root of p2");

  constexpr std::array<std::uint64_t, 3> kFactors1{2U, 7U, 17U};
  require(119ULL == 7ULL * 17ULL, "119 factors as 7 * 17");
  require(ter::is_primitive_root_u32(ter::kPaperNttRoot1, ter::kPaperNttPrime1,
                                     std::span<const std::uint64_t>(kFactors1)),
          "3 must be a primitive root of p1");
  // Transform-length support.
  require(ter::modular_power(
              ter::modular_power(ter::kPaperNttRoot2,
                                 (ter::kPaperNttPrime2 - 1U) / (std::uint64_t{1} << 27U),
                                 ter::kPaperNttPrime2),
              std::uint64_t{1} << 27U, ter::kPaperNttPrime2) == 1U,
          "p2 must support order-2^27 roots of unity");
  require(ter::modular_power(
              ter::modular_power(ter::kPaperNttRoot1,
                                 (ter::kPaperNttPrime1 - 1U) / (std::uint64_t{1} << 23U),
                                 ter::kPaperNttPrime1),
              std::uint64_t{1} << 22U, ter::kPaperNttPrime1) != 1U,
          "the p1 length-2^23 root must have exact order 2^23");
}

// --- L. Twiddle correctness -------------------------------------------------

void precomputed_twiddles_match_direct_modular_exponentiation() {
  for (const std::uint64_t prime : {ter::kPaperNttPrime1, ter::kPaperNttPrime2}) {
    const std::uint64_t root =
        prime == ter::kPaperNttPrime1 ? ter::kPaperNttRoot1 : ter::kPaperNttRoot2;
    for (const std::uint64_t q : {std::uint64_t{2}, std::uint64_t{8}, std::uint64_t{64},
                                  std::uint64_t{256}}) {
      const auto plan = ter::build_paper_ntt_plan(q, prime, root);
      require(plan.status == ter::PaperCrossoverStatus::Completed, "twiddle plan must build");
      require(plan.value.forward_twiddles.size() == static_cast<std::size_t>(q) - 1U,
              "the flattened forward table must hold exactly q-1 entries");
      for (std::uint64_t span = 2U; span <= q; span <<= 1U) {
        const std::uint64_t stage_root = ter::modular_power(root, (prime - 1U) / span, prime);
        for (std::uint64_t offset = 0; offset < span / 2U; ++offset) {
          const std::uint64_t expected = ter::modular_power(stage_root, offset, prime);
          require(plan.value.forward_twiddle(span, offset) == expected,
                  "precomputed forward twiddles must equal direct modular exponentiation");
          // The inverse twiddle identity is tested independently of its derivation.
          const std::uint64_t inverse = plan.value.inverse_twiddle(span, offset);
          require((expected * inverse) % prime == 1U,
                  "inverse twiddles must invert their forward counterparts");
          require(inverse == ter::modular_power(stage_root, span - offset, prime),
                  "inverse twiddles must equal w^(len-j)");
        }
      }
      // Bit reversal is an involution and a permutation.
      std::vector<bool> seen(static_cast<std::size_t>(q), false);
      for (std::uint32_t index = 0; index < static_cast<std::uint32_t>(q); ++index) {
        const std::uint32_t mirrored = plan.value.bit_reverse(index);
        require(static_cast<std::uint64_t>(mirrored) < q, "bit reversal must stay in range");
        require(plan.value.bit_reverse(mirrored) == index, "bit reversal must be an involution");
        require(!seen[mirrored], "bit reversal must be a permutation");
        seen[mirrored] = true;
        std::uint32_t reference = 0;
        for (std::uint32_t bit = 0; bit < plan.value.log_q; ++bit) {
          reference |= ((index >> bit) & 1U) << (plan.value.log_q - 1U - bit);
        }
        require(mirrored == reference, "table-driven reversal must equal direct bit reversal");
      }
    }
  }
}

// --- Transform ordering: Gentleman-Sande forward emits the bit-reversed
// --- spectrum, and composing it with the Cooley-Tukey inverse is the identity,
// --- so no explicit bit-reversal pass is required anywhere.

void gentleman_sande_forward_emits_the_bit_reversed_spectrum() {
  for (const std::uint64_t prime : {ter::kPaperNttPrime1, ter::kPaperNttPrime2}) {
    const std::uint64_t root =
        prime == ter::kPaperNttPrime1 ? ter::kPaperNttRoot1 : ter::kPaperNttRoot2;
    for (const std::uint64_t q : {std::uint64_t{2}, std::uint64_t{4}, std::uint64_t{8},
                                  std::uint64_t{16}, std::uint64_t{64}}) {
      const auto plan = ter::build_paper_ntt_plan(q, prime, root);
      require(plan.status == ter::PaperCrossoverStatus::Completed, "ordering plan must build");
      std::vector<std::uint32_t> input(static_cast<std::size_t>(q));
      std::uint64_t state = 0x9e3779b97f4a7c15ULL ^ q ^ prime;
      for (auto& value : input) {
        state ^= state << 13U;
        state ^= state >> 7U;
        state ^= state << 17U;
        value = static_cast<std::uint32_t>(state % prime);
      }

      // Independent naive transform: X[k] = sum_n x[n] * w^(n*k) mod p.
      const std::uint64_t omega = ter::modular_power(root, (prime - 1U) / q, prime);
      require(ter::modular_power(omega, q, prime) == 1U, "omega must have order dividing Q");
      std::vector<std::uint32_t> naive(static_cast<std::size_t>(q), 0U);
      for (std::uint64_t k = 0; k < q; ++k) {
        std::uint64_t accumulator = 0;
        for (std::uint64_t n = 0; n < q; ++n) {
          accumulator =
              (accumulator + static_cast<std::uint64_t>(input[static_cast<std::size_t>(n)]) *
                                 ter::modular_power(omega, (n * k) % q, prime)) %
              prime;
        }
        naive[static_cast<std::size_t>(k)] = static_cast<std::uint32_t>(accumulator);
      }

      const auto forward = ter::ntt_forward_transform(plan.value, input);
      require(forward.status == ter::PaperCrossoverStatus::Completed,
              "the forward transform must complete");
      require(forward.counts.size() == static_cast<std::size_t>(q),
              "the forward transform must preserve length");
      for (std::uint32_t index = 0; index < static_cast<std::uint32_t>(q); ++index) {
        const std::uint32_t mirrored = plan.value.bit_reverse(index);
        require(forward.counts[index] == naive[mirrored],
                "Gentleman-Sande output index i must equal the naive spectrum at bit_reverse(i)");
      }
      // A pure permutation of the naive spectrum: same multiset, different order.
      std::vector<std::uint32_t> sorted_forward = forward.counts;
      std::vector<std::uint32_t> sorted_naive = naive;
      std::sort(sorted_forward.begin(), sorted_forward.end());
      std::sort(sorted_naive.begin(), sorted_naive.end());
      require(sorted_forward == sorted_naive,
              "the forward spectrum must be a permutation of the naive spectrum");

      // Forward then inverse must be the identity with no permutation pass.
      // The inverse is exercised through the public convolution against a
      // delta list, which multiplies the spectrum by the all-ones transform.
      std::vector<std::uint32_t> delta{0U};
      std::vector<std::uint32_t> signal;
      for (std::uint32_t index = 0; index < static_cast<std::uint32_t>(q); ++index) {
        for (std::uint32_t repeat = 0; repeat < (index % 3U); ++repeat) {
          signal.push_back(index);
        }
      }
      if (signal.empty()) {
        continue;
      }
      const auto round_trip = ter::ntt_cyclic_pair_counts(plan.value, signal, delta);
      require(round_trip.status == ter::PaperCrossoverStatus::Completed,
              "the delta convolution must complete");
      std::vector<std::uint32_t> expected(static_cast<std::size_t>(q), 0U);
      for (const std::uint32_t value : signal) {
        ++expected[value];
      }
      require(round_trip.counts == expected,
              "convolving with a delta must return the original histogram in natural order");
    }
  }
}

// --- H/I/M/N/O/Q. Exact convolution behaviour -------------------------------

void ntt_convolution_is_exact_for_both_verified_primes() {
  struct Case {
    std::uint64_t q;
    std::uint64_t prime;
    std::uint64_t root;
  };
  const std::array<Case, 6> cases{{
      {8U, ter::kPaperNttPrime1, ter::kPaperNttRoot1},
      {64U, ter::kPaperNttPrime1, ter::kPaperNttRoot1},
      {1'024U, ter::kPaperNttPrime1, ter::kPaperNttRoot1},
      {8U, ter::kPaperNttPrime2, ter::kPaperNttRoot2},
      {64U, ter::kPaperNttPrime2, ter::kPaperNttRoot2},
      {1'024U, ter::kPaperNttPrime2, ter::kPaperNttRoot2},
  }};
  std::uint64_t state = 0x243f6a8885a308d3ULL;
  const auto next = [&state]() {
    state ^= state << 13U;
    state ^= state >> 7U;
    state ^= state << 17U;
    return state;
  };
  for (const auto& item : cases) {
    for (const std::size_t population : {std::size_t{1}, std::size_t{7}, std::size_t{97}}) {
      std::vector<std::uint32_t> left(population);
      std::vector<std::uint32_t> right(population);
      for (std::size_t index = 0; index < population; ++index) {
        left[index] = static_cast<std::uint32_t>(next() % item.q);
        // N. deliberately repeat residues so multiplicities are exercised.
        right[index] = static_cast<std::uint32_t>((index % 3U == 0U) ? 0U : next() % item.q);
      }
      const auto plan = ter::build_paper_ntt_plan(item.q, item.prime, item.root);
      require(plan.status == ter::PaperCrossoverStatus::Completed, "kernel plan must build");
      const auto transformed = ter::ntt_cyclic_pair_counts(plan.value, left, right);
      const auto direct = ter::direct_full_pair_counts(left, right, item.q);
      require(transformed.status == ter::PaperCrossoverStatus::Completed, "NTT must complete");
      require(direct.status == ter::PaperCrossoverStatus::Completed, "direct must complete");
      require(transformed.counts == direct.counts,
              "NTT must equal direct cyclic convolution coefficient-for-coefficient");
      std::uint64_t conserved = 0;
      for (const auto value : direct.counts) {
        conserved += value;
      }
      require(conserved == static_cast<std::uint64_t>(population) * population,
              "pair conservation must hold exactly");
      // Q. cold plan and warm plan must agree coefficient-for-coefficient.
      const auto cold_plan = ter::build_paper_ntt_plan(item.q, item.prime, item.root);
      require(cold_plan.status == ter::PaperCrossoverStatus::Completed, "cold plan must build");
      const auto cold = ter::ntt_cyclic_pair_counts(cold_plan.value, left, right);
      require(cold.status == ter::PaperCrossoverStatus::Completed, "cold NTT must complete");
      require(cold.counts == transformed.counts, "cold and warm NTT results must be identical");
    }
  }
}

void cyclic_wraparound_and_multiplicity_are_exact() {
  constexpr std::uint64_t kQ = 64U;
  const auto plan = ter::build_paper_ntt_plan(kQ, ter::kPaperNttPrime1, ter::kPaperNttRoot1);
  require(plan.status == ter::PaperCrossoverStatus::Completed, "wraparound plan must build");

  // M. residues that straddle 0 and Q-1.
  const std::vector<std::uint32_t> left{0U, 1U, static_cast<std::uint32_t>(kQ - 1U),
                                        static_cast<std::uint32_t>(kQ - 1U)};
  const std::vector<std::uint32_t> right{0U, static_cast<std::uint32_t>(kQ - 1U), 1U, 2U};
  const auto transformed = ter::ntt_cyclic_pair_counts(plan.value, left, right);
  const auto direct = ter::direct_full_pair_counts(left, right, kQ);
  require(transformed.status == ter::PaperCrossoverStatus::Completed, "wraparound NTT completes");
  require(transformed.counts == direct.counts, "wraparound coefficients must be identical");
  std::vector<std::uint32_t> expected(static_cast<std::size_t>(kQ), 0U);
  for (const auto a : left) {
    for (const auto b : right) {
      ++expected[(static_cast<std::uint64_t>(a) + b) % kQ];
    }
  }
  require(direct.counts == expected, "wraparound counts must match an independent modulo sum");
  require(direct.counts[0] == 4U,
          "residue 0 must collect (0,0), (1,Q-1) and both (Q-1,1) pairs");
  require(direct.counts[kQ - 1U] == 3U,
          "residue Q-1 must collect (0,Q-1) and both (Q-1,0) pairs");

  // N. exact multiplicities under heavy repetition.
  const std::vector<std::uint32_t> repeated_left(11U, 5U);
  const std::vector<std::uint32_t> repeated_right(13U, 7U);
  const auto repeated = ter::ntt_cyclic_pair_counts(plan.value, repeated_left, repeated_right);
  require(repeated.status == ter::PaperCrossoverStatus::Completed, "multiplicity NTT completes");
  for (std::size_t index = 0; index < repeated.counts.size(); ++index) {
    const std::uint32_t expectation = index == 12U ? 143U : 0U;
    require(repeated.counts[index] == expectation,
            "repeated quotient residues must produce exact multiplicities");
  }
}

// --- S/T. Explicit unsupported and unsafe states -----------------------------

void unsupported_lengths_and_unsafe_coefficients_fail_closed() {
  // S. q_bits > 27 is NotApplicable.
  const auto oversized = ter::select_paper_crossover_prime(std::uint64_t{1} << 28U, 1'024U);
  require(oversized.status == ter::PaperCrossoverStatus::NotApplicable,
          "transform lengths above 2^27 must report NotApplicable");
  require(ter::select_paper_crossover_prime(std::uint64_t{1} << 27U, 1'024U).prime ==
              ter::kPaperNttPrime2,
          "length 2^27 must select p2");
  require(ter::select_paper_crossover_prime(std::uint64_t{1} << 23U, 1'024U).prime ==
              ter::kPaperNttPrime1,
          "length 2^23 must select p1");
  require(ter::select_paper_crossover_prime(std::uint64_t{1} << 24U, 1'024U).prime ==
              ter::kPaperNttPrime2,
          "length 2^24 must select p2");
  require(ter::select_paper_crossover_prime(std::uint64_t{1} << 20U, ter::kPaperNttPrime1).prime ==
              ter::kPaperNttPrime2,
          "coefficients at or above p1 must escalate to p2");

  // T. coefficients that reach the chosen prime are an ArithmeticSafetyFailure.
  const auto unsafe =
      ter::select_paper_crossover_prime(std::uint64_t{1} << 20U, ter::kPaperNttPrime2);
  require(unsafe.status == ter::PaperCrossoverStatus::ArithmeticSafetyFailure,
          "coefficients at or above p2 must report ArithmeticSafetyFailure");
  require(unsafe.prime == 0U, "an unsafe selection must not name a prime");

  constexpr std::uint64_t kQ = std::uint64_t{1} << 15U;
  const auto plan = ter::build_paper_ntt_plan(kQ, ter::kPaperNttPrime1, ter::kPaperNttRoot1);
  require(plan.status == ter::PaperCrossoverStatus::Completed, "safety plan must build");
  const std::vector<std::uint32_t> wide(31'624U, 1U);
  require(static_cast<std::uint64_t>(wide.size()) * wide.size() >= ter::kPaperNttPrime1,
          "the safety probe must actually reach p1");
  const auto refused = ter::ntt_cyclic_pair_counts(plan.value, wide, wide);
  require(refused.status == ter::PaperCrossoverStatus::ArithmeticSafetyFailure,
          "the kernel must refuse coefficients that can wrap the modulus");
  require(refused.counts.empty(), "a refused kernel must not publish counts");

  const auto unusable = ter::build_paper_ntt_plan(std::uint64_t{1} << 24U, ter::kPaperNttPrime1,
                                            ter::kPaperNttRoot1);
  require(unusable.status == ter::PaperCrossoverStatus::NotApplicable,
          "p1 must refuse transform lengths beyond 2^23");

  // The declared L=32768 ladder point is present and explicitly NotApplicable.
  const auto declared = ter::derive_paper_scaling_point(32'768U);
  require(declared.q_bits == 28U, "L=32768 requires q_bits 28");
  require(ter::select_paper_crossover_prime(declared.q, 32'768ULL * 32'768ULL).status ==
              ter::PaperCrossoverStatus::NotApplicable,
          "L=32768 must be reported as NotApplicable, never silently omitted");
}

// --- P/R/U. End-to-end experiment behaviour ---------------------------------

void experiment_conserves_triples_and_never_mutates_its_snapshot() {
  const auto snapshot = must_build(small_config(256U, 0x1001U));
  const std::uint64_t before = ter::paper_crossover_fingerprint(snapshot);
  require(before == snapshot.fingerprint, "fingerprint must be stable before the run");

  ter::PaperCrossoverConfig config;
  config.warmup_repetitions = 1U;
  config.timed_repetitions = 2U;
  const auto first = ter::run_paper_crossover_experiment(snapshot, config);
  require(first.status == ter::PaperCrossoverStatus::Completed,
          "the experiment must complete: " + first.message);
  require(first.telemetry.exactness_passed, "exactness must hold on every executed point");
  require(first.telemetry.pair_conservation_passed, "pair conservation must hold");
  require(first.telemetry.triple_conservation_passed, "triple conservation must hold");
  require(first.telemetry.cold_warm_identical, "cold and warm results must be identical");
  require(first.telemetry.direct_full.total.samples == config.timed_repetitions,
          "direct full timing must record one total per measured repetition");
  require(first.telemetry.ntt_warm.total.median_ns > 0U, "warm NTT must record real work");
  require(first.telemetry.ntt_three_merge_amortized_ns >= first.telemetry.ntt_warm.total.median_ns,
          "the three-merge model must add a share of the plan build");
  require(first.telemetry.ntt_plan_bytes > 0U, "plan memory must never be reported as zero");
  require(first.telemetry.ntt_peak_bytes ==
              first.telemetry.ntt_plan_bytes + first.telemetry.ntt_run_bytes,
          "NTT peak must include its resident plan");

  // P. independent recomputation of the triple conservation identity.
  const auto a_quotients = role_quotients(snapshot, 0U);
  const auto b_quotients = role_quotients(snapshot, 1U);
  const auto c_quotients = role_quotients(snapshot, 2U);
  const auto& arithmetic = snapshot.input.spec.arithmetic;
  const auto mask = static_cast<std::uint32_t>(arithmetic.q - 1U);
  const auto tau = static_cast<std::uint32_t>(arithmetic.tau);
  const auto direct = ter::direct_full_pair_counts(a_quotients, b_quotients, arithmetic.q);
  require(direct.status == ter::PaperCrossoverStatus::Completed, "reference direct must complete");
  std::uint64_t selected = 0;
  for (const auto c : c_quotients) {
    selected += direct.counts[(tau - c) & mask];
  }
  std::uint64_t brute = 0;
  for (const auto a : a_quotients) {
    for (const auto b : b_quotients) {
      for (const auto c : c_quotients) {
        brute += ((static_cast<std::uint64_t>(a) + b + c) & mask) == tau ? 1U : 0U;
      }
    }
  }
  require(selected == brute, "selected-target sum must equal the exhaustive triple count");
  require(first.telemetry.raw_arithmetic_triples == brute,
          "DIRECT_PRODUCTION raw triples must equal the exhaustive triple count");

  const auto prime = ter::select_paper_crossover_prime(
      arithmetic.q, static_cast<std::uint64_t>(a_quotients.size()) * b_quotients.size());
  require(prime.status == ter::PaperCrossoverStatus::Completed, "reference prime must exist");
  require(prime.prime == first.telemetry.prime.prime, "the experiment must use the selected prime");
  const auto plan = ter::build_paper_ntt_plan(arithmetic.q, first.telemetry.prime.prime,
                                        first.telemetry.prime.primitive_root);
  require(plan.status == ter::PaperCrossoverStatus::Completed, "reference plan must build");
  const auto transformed = ter::ntt_cyclic_pair_counts(plan.value, a_quotients, b_quotients);
  require(transformed.status == ter::PaperCrossoverStatus::Completed, "reference NTT completes");
  std::uint64_t ntt_selected = 0;
  for (const auto c : c_quotients) {
    ntt_selected += transformed.counts[(tau - c) & mask];
  }
  require(ntt_selected == brute, "NTT selected-target sum must equal the exhaustive triple count");

  // R. repeated benchmark calls must not mutate the frozen snapshot.
  const auto second = ter::run_paper_crossover_experiment(snapshot, config);
  require(second.status == ter::PaperCrossoverStatus::Completed, "the repeated run must complete");
  require(second.telemetry.raw_arithmetic_triples == first.telemetry.raw_arithmetic_triples,
          "repeated runs must agree on the raw triple count");
  require(ter::paper_crossover_fingerprint(snapshot) == before,
          "repeated benchmark calls must not mutate the snapshot");
  require(ter::audit_paper_crossover_snapshot(snapshot).status ==
              ter::PaperCrossoverStatus::Completed,
          "the snapshot must still audit cleanly after repeated runs");
}

void tiny_memory_budgets_reject_every_method_independently() {
  const auto snapshot = must_build(small_config(64U, 0x2003U));
  ter::PaperCrossoverConfig config;
  config.memory_limit_bytes = 1U;
  const auto limited = ter::run_paper_crossover_experiment(snapshot, config);
  require(limited.status == ter::PaperCrossoverStatus::ResourceLimit,
          "a one-byte ceiling must stop the experiment");
  require(limited.telemetry.direct_full.status == ter::PaperCrossoverStatus::ResourceLimit,
          "DIRECT_FULL must be rejected independently");
  require(limited.telemetry.direct_production.status == ter::PaperCrossoverStatus::ResourceLimit,
          "DIRECT_PRODUCTION must be rejected independently");
  require(limited.telemetry.ntt_warm.status == ter::PaperCrossoverStatus::ResourceLimit,
          "warm NTT must be rejected independently");
  require(limited.telemetry.ntt_cold.status == ter::PaperCrossoverStatus::ResourceLimit,
          "cold NTT must be rejected independently");
  require(limited.telemetry.direct_full.peak_bytes != limited.telemetry.ntt_peak_bytes,
          "method peaks are reported independently, never summed into one figure");

  // A ceiling that admits only the direct paths still rejects the NTT plan.
  ter::PaperCrossoverConfig partial;
  partial.memory_limit_bytes = limited.telemetry.direct_production.peak_bytes;
  const auto narrowed = ter::run_paper_crossover_experiment(snapshot, partial);
  require(narrowed.status == ter::PaperCrossoverStatus::ResourceLimit,
          "an NTT-only overrun must still fail closed");
  require(narrowed.telemetry.direct_full.status != ter::PaperCrossoverStatus::ResourceLimit,
          "DIRECT_FULL must survive a ceiling sized for it");
  require(narrowed.telemetry.ntt_warm.status == ter::PaperCrossoverStatus::ResourceLimit,
          "the NTT plan must be rejected on its own accounting");

  ter::PaperCrossoverConfig invalid;
  invalid.timed_repetitions = 0U;
  require(ter::run_paper_crossover_experiment(snapshot, invalid).status ==
              ter::PaperCrossoverStatus::InvalidConfiguration,
          "a zero repetition count is an invalid configuration");

  ter::PaperCrossoverSnapshot corrupted = snapshot;
  corrupted.input.lists[0].entries[0].q ^= 1U;
  require(ter::run_paper_crossover_experiment(corrupted, {}).status ==
              ter::PaperCrossoverStatus::InvalidSnapshot,
          "post-construction mutation must fail the audit");
  const ter::PaperCrossoverSnapshot empty;
  require(ter::run_paper_crossover_experiment(empty, {}).status ==
              ter::PaperCrossoverStatus::InvalidSnapshot,
          "a default snapshot must never report completion");
}

// --- Section 21. Authentic Full TER anchor continuity ------------------------

[[nodiscard]] ter::Instance balanced_full_instance() {
  const auto built = ter::build_instance(36U, 10U, 0x5143415054555245ULL);
  require(built.status == ter::Status::Completed, "anchor instance must build");
  auto instance = built.value;
  std::fill(instance.planted_root.begin(), instance.planted_root.end(), std::uint8_t{0});
  instance.root_target_le.assign(instance.words_per_value, 0U);
  for (std::uint32_t half = 0; half < 2U; ++half) {
    for (std::uint32_t offset = 0; offset < 9U; ++offset) {
      const auto coordinate = half * 18U + offset;
      instance.planted_root[coordinate] = 1U;
      instance.root_target_le[0] += instance.values_le[coordinate];
    }
  }
  instance.root_target_le[0] &= (std::uint64_t{1} << 36U) - 1U;
  return instance;
}

void authentic_full_ter_anchors_run_on_the_same_kernels() {
  ter::FullTerConfig config;
  config.family = ter::ShapeFamily::FiniteSanity;
  config.execution_mode = ter::TerExecutionMode::FullTer;
  config.target_mode = ter::TerTargetMode::GuidedPlanted;
  config.seed = 0x51335245504f5254ULL;
  config.memory_limit_bytes = 64U * 1024U * 1024U;
  std::uint32_t captured = 0;
  for (std::uint32_t group = 0; group < 3U; ++group) {
    const auto capture =
        ter::capture_full_ter_l2_merge_snapshot(balanced_full_instance(), config, group);
    if (capture.status != ter::Status::Completed) {
      continue;
    }
    ++captured;
    const auto adopted = ter::adopt_authentic_full_ter_snapshot(capture.snapshot,
                                                                capture.parameters, config.seed);
    require(adopted.status == ter::PaperCrossoverStatus::Completed,
            "an authentic capture must be adoptable");
    require(adopted.value.family == ter::PaperCrossoverFamily::AuthenticFullTer,
            "anchors must be labelled AUTHENTIC_FULLTER");
    require(std::string_view{ter::paper_crossover_family_name(adopted.value.family)} ==
                "AUTHENTIC_FULLTER",
            "the anchor family name is fixed");
    require(ter::audit_paper_crossover_snapshot(adopted.value).status ==
                ter::PaperCrossoverStatus::Completed,
            "an authentic capture must survive the independent audit");
    ter::PaperCrossoverConfig run_config;
    run_config.warmup_repetitions = 1U;
    run_config.timed_repetitions = 2U;
    const auto measured = ter::run_paper_crossover_experiment(adopted.value, run_config);
    require(measured.status == ter::PaperCrossoverStatus::Completed,
            "the authentic anchor must run: " + measured.message);
    require(measured.telemetry.exactness_passed, "anchor exactness must hold");
    require(measured.telemetry.pair_conservation_passed, "anchor pair conservation must hold");
    require(measured.telemetry.triple_conservation_passed, "anchor triple conservation must hold");
    require(measured.telemetry.cold_warm_identical, "anchor cold/warm identity must hold");
  }
  require(captured > 0U, "at least one genuine Full TER group must be capturable");
}

}  // namespace

int main() {
  paper_scaling_derivation_matches_the_declared_ladder();
  scaling_generator_uses_the_full_ter_paper_rounded_parameter_path();
  every_generated_level2_entry_has_exact_full_ter_half_structure();
  sampled_level2_vectors_belong_to_the_genuine_n36_full_ter_universe();
  identical_configurations_replay_and_distinct_seeds_diverge();
  the_second_ntt_modulus_is_verified_mathematically();
  precomputed_twiddles_match_direct_modular_exponentiation();
  gentleman_sande_forward_emits_the_bit_reversed_spectrum();
  ntt_convolution_is_exact_for_both_verified_primes();
  cyclic_wraparound_and_multiplicity_are_exact();
  unsupported_lengths_and_unsafe_coefficients_fail_closed();
  experiment_conserves_triples_and_never_mutates_its_snapshot();
  tiny_memory_budgets_reject_every_method_independently();
  authentic_full_ter_anchors_run_on_the_same_kernels();
  std::cout << "q3report paper crossover tests passed (" << g_checks << " assertions)\n";
  return 0;
}
