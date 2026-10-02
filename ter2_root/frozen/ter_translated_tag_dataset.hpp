#pragma once

// TER^2 translated-tag locator research spike: authoritative verification tables.
//
// The MicroReport8 donor dataset (bench/ter_microreport8_dataset.*) is the single
// source of the authentic n=72 residual families and of tau.  It projects every
// entry to 25 bits, which is exactly what the candidate-discovery frontends need
// and deliberately less than what a final answer needs.
//
// This file adds only the downstream authority the matched-scope comparison
// requires, and adds it identically for all three comparator paths:
//
//   low-32 quotient agreement   an exact, no-false-negative prefilter
//   original-residue agreement  the authority, arbitrary width, never truncated
//   ternary structural closure  parent digits valid and shaped as expected
//
// It obtains them from its own FullTerRootCapture using the same verbatim
// fixture transplant the donor uses, then proves the capture is the same one by
// requiring its low-25 projection to reproduce the donor's entry for entry.
// The donor file is not modified and production is not touched.

#include "ter/full_ter.hpp"
#include "ter/reference_vector.hpp"
#include "ter/shape.hpp"
#include "ter/types.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ter::translated_tag {

struct AuthorityTables {
  bool ok = false;
  std::string message;

  std::uint32_t n = 0;
  std::uint32_t r1 = 0;
  std::uint32_t quotient_bits = 0;
  std::uint32_t tau_low32 = 0;
  std::uint32_t residue_words = 0;  // 32-bit words per residue, covering n bits
  std::array<std::uint64_t, 3> list_sizes{};

  // Indexed by EntryId within each captured list (0 = A, 1 = B, 2 = C).
  std::array<std::vector<std::uint32_t>, 3> quotient;  // low 32 quotient bits
  std::array<std::vector<std::uint32_t>, 3> residue;   // flattened, residue_words each
  std::array<std::vector<std::int8_t>, 3> digits;      // flattened, n each

  std::vector<std::uint32_t> root_target;  // residue_words words
  DigitCounts expected_parent{};
  bool shape_available = false;

  std::uint64_t bytes = 0;
};

// Builds the tables from an independent capture of the same authentic root.
[[nodiscard]] AuthorityTables build_authority_tables(std::uint32_t root_n,
                                                     std::uint64_t root_seed);

// Authority stage 2: sum of the three original residues mod 2^n equals the root
// target.  Never truncated to a machine word, so it stays correct for wide Q.
[[nodiscard]] bool residue_sum_matches_root(const AuthorityTables& tables,
                                            EntryId first,
                                            EntryId second,
                                            EntryId third) noexcept;

// Authority stage 3: digit-wise ternary closure of the parent plus the expected
// parent digit distribution.  This is the structural rule production's packed
// add_three enforces, evaluated here on the public unpacked representation.
[[nodiscard]] bool structural_parent_valid(const AuthorityTables& tables,
                                           EntryId first,
                                           EntryId second,
                                           EntryId third) noexcept;

}  // namespace ter::translated_tag
