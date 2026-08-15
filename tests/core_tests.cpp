#include "ter/arithmetic.hpp"
#include "ter/instance.hpp"
#include "ter/reference_vector.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void nested_targets_are_exact() {
  const auto result = ter::build_arithmetic(36, 3, 7, 7, 6, 100);
  expect(result.status == ter::Status::Completed, "valid nested moduli complete");
  const auto& arithmetic = result.value;
  expect(arithmetic.m2 == 8, "M2 is 2^b2");
  expect(arithmetic.m1 == 128, "M1 is 2^b1");
  expect(arithmetic.q == 16, "Q is M1/M2");
  expect(arithmetic.target_c2 == 7, "C target closes the M2 equation");
  expect(arithmetic.tau == 10, "tau is the exact quotient target");
  expect(ter::quotient_residue(47, arithmetic.target_a2, arithmetic) == 5,
         "quotient residue uses exact shift");
  expect(ter::quotient_residue(3, arithmetic.target_a2, arithmetic) == 15,
         "quotient residue wraps modulo M1");
  expect(ter::pair_sum(5, 15, arithmetic) == 4, "pair sums wrap modulo Q");
  expect(ter::required_pair_residue(6, arithmetic) == 4,
         "required pair residue closes qA+qB+qC=tau");
}

void invalid_moduli_are_not_completed() {
  expect(ter::build_arithmetic(36, 0, 7, 0, 0, 0).status ==
             ter::Status::InvalidConfiguration,
         "b2=0 is invalid");
  expect(ter::build_arithmetic(36, 7, 7, 0, 0, 0).status ==
             ter::Status::InvalidConfiguration,
         "b2 must be less than b1");
  expect(ter::build_arithmetic(36, 3, 36, 0, 0, 0).status ==
             ter::Status::InvalidConfiguration,
         "b1 must be less than n");
  expect(ter::build_arithmetic(72, 1, 34, 0, 0, 0).status ==
             ter::Status::InvalidConfiguration,
         "V1 quotient residues are bounded to 32 bits");
}

void reference_vectors_have_obvious_semantics() {
  const ter::ReferenceTernaryVector vector{-1, 0, 1, 1, 0, -1};
  const auto counts = ter::count_digits(vector);
  expect(counts.has_value(), "valid ternary vector has counts");
  expect(counts->negative == 2 && counts->zero == 2 && counts->positive == 2,
         "reference digit counts are exact");
  expect(ter::has_balanced_halves(vector, ter::DigitCounts{1, 1, 1}),
         "balanced half distribution is checked exactly");

  ter::ReferenceTernaryVector invalid = vector;
  invalid[2] = 2;
  expect(!ter::count_digits(invalid).has_value(), "digit +2 is rejected");

  const std::vector<std::uint64_t> residues{3, 5, 7};
  const ter::ReferenceTernaryVector small{-1, 0, 1};
  expect(ter::inner_product_mod(residues, small, 15) == 4,
         "reference inner product handles negative digits modulo powers of two");
}

void density_one_instances_replay_exactly() {
  const auto first = ter::build_instance(36, 12, 0x123456789abcdef0ULL);
  const auto replay = ter::build_instance(36, 12, 0x123456789abcdef0ULL);
  const auto other = ter::build_instance(36, 12, 0x123456789abcdef1ULL);
  expect(first.status == ter::Status::Completed, "density-one instance builds");
  expect(first.value.values_le == replay.value.values_le,
         "same seed reproduces all n-bit values");
  expect(first.value.residues_m1 == replay.value.residues_m1,
         "same seed reproduces M1 residues");
  expect(first.value.root_target_le == replay.value.root_target_le,
         "same seed reproduces the planted root target");
  expect(first.value.values_le != other.value.values_le,
         "different seed changes the instance");
  expect(first.value.words_per_value == 1, "n=36 uses one 64-bit limb per value");
  expect(std::count(first.value.planted_root.begin(), first.value.planted_root.end(), 1U) ==
             18,
         "planted binary root has weight n/2");
  for (std::size_t i = 0; i < first.value.n; ++i) {
    expect((first.value.values_le[i] >> 36U) == 0U,
           "unused high bits of each n-bit value are zero");
    expect(first.value.residues_m1[i] == (first.value.values_le[i] & 4095U),
           "hot M1 residue equals the low b1 bits");
  }
}

}  // namespace

int main() {
  nested_targets_are_exact();
  invalid_moduli_are_not_completed();
  reference_vectors_have_obvious_semantics();
  density_one_instances_replay_exactly();

  if (failures != 0) {
    std::cerr << failures << " core assertion(s) failed\n";
    return 1;
  }
  std::cout << "core tests passed\n";
  return 0;
}
