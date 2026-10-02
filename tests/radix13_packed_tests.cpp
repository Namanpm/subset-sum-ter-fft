// TER^2 Radix-13 packed-tag mask consumer: focused correctness tests.
//
// Truth never uses the machinery under test.  Witness truth is recomputed from
// the residual values themselves ((x + d) mod 2^19 asked of the value set);
// deck truth is recomputed from the values, not from the dense tag words; the
// rank/extraction truth is computed by counting bits, not by PEXT.
//
// Coverage follows section 35 and appendix B of the specification: exhaustive
// PEXT(M, O) rank mapping over every 8-position occupancy pattern and every
// submask; all 256 expansion masks and order-preserving byte extraction; the
// packed 6-bit relation for every (a, b, alpha, carry) and against neighbour
// lanes; the carry seam at every bit offset; multi-tag buckets; duplicate
// residuals; deck capacities of exactly 8, 9, 16 and 17 occupied ranks; wide
// words with more than 8 candidate lanes; zero, full and alternating
// occupancy; shifts r = 0, 1, 63, 64, 8191 with alpha at both extremes; and
// exact agreement of every packed route (portable and BMI2, one and two
// decks, counted and uncounted) with the truth and, witness for witness and in
// order, with the plain radix-13 dense kernel.

#include "ter_microreport8_kernels.hpp"
#include "ter_radix13_packed.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

namespace rx = ter::radix;
namespace pk = ter::radix13_packed;
using G = pk::G;
constexpr unsigned kPB = pk::kPB;

std::uint64_t g_checks = 0U;
std::uint64_t g_failures = 0U;
bool g_bmi2_executable = false;

void check(const bool condition, const std::string& what) {
  ++g_checks;
  if (!condition) {
    ++g_failures;
    if (g_failures <= 40U) {
      std::cerr << "FAIL: " << what << '\n';
    }
  }
}

[[nodiscard]] std::uint64_t next_random(std::uint64_t& state) noexcept {
  state += 0x9E3779B97F4A7C15ULL;
  std::uint64_t value = state;
  value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
  return value ^ (value >> 31U);
}

constexpr std::uint32_t kMask = rx::kResidualMask;

// ---------------------------------------------------------------------------
// Primitive-level tests.
// ---------------------------------------------------------------------------

// Rank of a set bit among the set bits of occupancy, by counting.
[[nodiscard]] std::uint32_t rank_by_counting(const std::uint64_t occupancy,
                                             const std::uint32_t bit) {
  std::uint32_t rank = 0U;
  for (std::uint32_t below = 0U; below < bit; ++below) {
    rank += static_cast<std::uint32_t>((occupancy >> below) & 1U);
  }
  return rank;
}

void test_rank_mapping_exhaustive() {
  // Every 8-position occupancy pattern O and every submask M of O.
  std::uint64_t pairs = 0U;
  bool all_good = true;
  for (std::uint32_t occupancy = 0U; occupancy < 256U; ++occupancy) {
    for (std::uint32_t m = occupancy;; m = (m - 1U) & occupancy) {
      std::uint64_t expected = 0U;
      for (std::uint32_t bit = 0U; bit < 8U; ++bit) {
        if (((m >> bit) & 1U) != 0U) {
          expected |= std::uint64_t{1} << rank_by_counting(occupancy, bit);
        }
      }
      const std::uint64_t ranks = pk::pext_reference(m, occupancy);
      all_good = all_good && ranks == expected;
      all_good = all_good && pk::pdep_reference(ranks, occupancy) == m;
      ++pairs;
      if (m == 0U) {
        break;
      }
    }
  }
  check(all_good, "PEXT(M, O) rank mapping over every 8-position O and every M <= O");
  check(pairs == 6561U, "exhaustive rank mapping visited 3^8 (O, M) pairs");

  // Wide random words, including survivor reconstruction by PDEP.
  std::uint64_t state = 0x1234ABCDULL;
  bool wide_good = true;
  for (std::uint32_t trial = 0U; trial < 20000U; ++trial) {
    const std::uint64_t occupancy = next_random(state) & next_random(state);
    const std::uint64_t m = occupancy & next_random(state);
    std::uint64_t expected = 0U;
    for (std::uint64_t bits = m; bits != 0U; bits &= bits - 1U) {
      const std::uint32_t bit = static_cast<std::uint32_t>(std::countr_zero(bits));
      expected |= std::uint64_t{1} << rank_by_counting(occupancy, bit);
    }
    wide_good = wide_good && pk::pext_reference(m, occupancy) == expected;
    // PDEP(h, M) marks exactly the chosen candidate lanes of M.
    const std::uint64_t h = next_random(state);
    std::uint64_t deposited = 0U;
    std::uint32_t lane = 0U;
    for (std::uint64_t bits = m; bits != 0U; bits &= bits - 1U, ++lane) {
      if (((h >> lane) & 1U) != 0U) {
        deposited |= std::uint64_t{1} << std::countr_zero(bits);
      }
    }
    wide_good = wide_good && pk::pdep_reference(h, m) == deposited;
  }
  check(wide_good, "rank mapping and PDEP survivor reconstruction on random 64-bit words");
}

void test_expansion_table() {
  bool good = true;
  for (std::uint32_t mask = 0U; mask < 256U; ++mask) {
    for (std::uint32_t lane = 0U; lane < 8U; ++lane) {
      const std::uint64_t byte = (pk::kExpand[mask] >> (8U * lane)) & 0xFFU;
      good = good && byte == ((((mask >> lane) & 1U) != 0U) ? 0xFFU : 0U);
    }
  }
  check(good, "expansion table: all 256 masks select exactly their byte lanes");

  // PEXT(deck, E[p]) packs the selected bytes contiguously, in rank order.
  std::uint64_t state = 0xFEEDULL;
  bool order = true;
  for (std::uint32_t trial = 0U; trial < 4096U; ++trial) {
    const std::uint64_t deck = next_random(state);
    const std::uint32_t mask = static_cast<std::uint32_t>(trial & 0xFFU);
    std::uint64_t expected = 0U;
    std::uint32_t out = 0U;
    for (std::uint32_t lane = 0U; lane < 8U; ++lane) {
      if (((mask >> lane) & 1U) != 0U) {
        expected |= ((deck >> (8U * lane)) & 0xFFU) << (8U * out++);
      }
    }
    order = order && pk::pext_reference(deck, pk::kExpand[mask]) == expected;
  }
  check(order, "payload extraction preserves candidate (rank) order");
}

void test_packed_relation() {
  // Every (a, b, alpha, carry) in every lane position, with random neighbours.
  std::uint64_t state = 0xC0FFEEULL;
  bool good = true;
  std::uint64_t cases = 0U;
  for (std::uint32_t alpha = 0U; alpha < 64U; ++alpha) {
    for (std::uint32_t carry = 0U; carry < 2U; ++carry) {
      for (std::uint32_t a = 0U; a < 64U; ++a) {
        for (std::uint32_t b = 0U; b < 64U; ++b) {
          const std::uint32_t lane = static_cast<std::uint32_t>(next_random(state) & 7U);
          // Random 6-bit neighbours, never flagged.
          std::uint64_t source = next_random(state) & pk::kLow6;
          std::uint64_t target = next_random(state) & pk::kLow6;
          std::uint64_t add = static_cast<std::uint64_t>(alpha) * pk::kOnes;
          const std::uint64_t carries = next_random(state) & pk::kOnes;
          add += carries;
          const std::uint64_t clear = ~(std::uint64_t{0xFF} << (8U * lane));
          source = (source & clear) | (static_cast<std::uint64_t>(a) << (8U * lane));
          target = (target & clear) | (static_cast<std::uint64_t>(b) << (8U * lane));
          add = (add & clear) | (static_cast<std::uint64_t>(alpha + carry) << (8U * lane));
          const std::uint64_t keep = pk::packed_keep_bit6(source, target, add);
          // Every lane, not only the tested one, must equal its scalar value.
          for (std::uint32_t k = 0U; k < 8U; ++k) {
            const std::uint32_t sa = static_cast<std::uint32_t>((source >> (8U * k)) & 0xFFU);
            const std::uint32_t sb = static_cast<std::uint32_t>((target >> (8U * k)) & 0xFFU);
            const std::uint32_t sadd = static_cast<std::uint32_t>((add >> (8U * k)) & 0xFFU);
            const bool expected = ((sa + sadd) & 63U) == sb;
            good = good && (((keep >> (8U * k + 6U)) & 1U) != 0U) == expected;
          }
          good = good && (keep & ~pk::kBit6) == 0U;
          ++cases;
        }
      }
    }
  }
  check(good, "packed 6-bit relation equals scalar for every (a, b, alpha, carry)");
  check(cases == 64U * 2U * 64U * 64U, "packed relation covered 524288 cases");

  // A flagged endpoint is always kept, whatever the tag bits say.
  bool flags = true;
  for (std::uint32_t trial = 0U; trial < 20000U; ++trial) {
    const std::uint32_t lane = static_cast<std::uint32_t>(trial & 7U);
    std::uint64_t source = next_random(state) & pk::kLow6;
    std::uint64_t target = next_random(state) & pk::kLow6;
    const std::uint64_t add =
        (static_cast<std::uint64_t>(next_random(state) & 63U) * pk::kOnes);
    if ((trial & 8U) != 0U) {
      source |= std::uint64_t{pk::kMultiTagFlag} << (8U * lane);
    } else {
      target |= std::uint64_t{pk::kMultiTagFlag} << (8U * lane);
    }
    flags = flags && ((pk::packed_keep_bit6(source, target, add) >> (8U * lane + 6U)) & 1U) != 0U;
  }
  check(flags, "multi-tag flag on either endpoint always keeps the lane");

  check(pk::payload_byte(0U) == 0U, "payload byte of an empty bucket");
  bool singles = true;
  for (std::uint32_t tag = 0U; tag < 64U; ++tag) {
    singles = singles && pk::payload_byte(std::uint64_t{1} << tag) == tag;
    singles = singles &&
              pk::payload_byte((std::uint64_t{1} << tag) | (std::uint64_t{1} << ((tag + 1U) & 63U))) ==
                  pk::kMultiTagFlag;
  }
  check(singles, "payload byte: singleton tags exact, two distinct tags flagged");
}

// ---------------------------------------------------------------------------
// Class-level fixture.
// ---------------------------------------------------------------------------
struct Fixture {
  rx::SourceClass<kPB> source;
  rx::TargetClass<kPB> target;
  std::vector<std::uint64_t> views;
  pk::SourceDecks source_decks;
  std::vector<std::uint64_t> target_deck0;
  std::vector<std::uint64_t> target_deck1;
  std::vector<std::uint32_t> hits;
  std::vector<std::uint32_t> reference_hits;
  std::vector<std::uint32_t> xs;
  std::vector<std::uint32_t> ys;
  bool built = false;
};

void make_fixture(std::vector<std::uint32_t> xs, std::vector<std::uint32_t> ys,
                  Fixture& out) {
  std::sort(xs.begin(), xs.end());
  std::sort(ys.begin(), ys.end());
  out.xs = xs;
  out.ys = ys;
  const bool source_ok = rx::build_source_class<kPB>(xs.data(), xs.size(), out.source);
  const bool target_ok = rx::build_target_class<kPB>(ys.data(), ys.size(), out.target);
  out.built = source_ok && target_ok;
  out.views.assign(G::kViewWordsPerClass, 0U);
  rx::build_shifted_views<kPB>(out.target, out.views.data());
  pk::build_source_decks(out.source, out.source_decks);
  out.target_deck0.assign(pk::kTargetDeckWordsPerClass, 0U);
  out.target_deck1.assign(pk::kTargetDeckWordsPerClass, 0U);
  pk::build_target_decks(out.target, out.views.data(), out.target_deck0.data(),
                         out.target_deck1.data());
  out.hits.assign(G::kHitCapacity, 0U);
  out.reference_hits.assign(G::kHitCapacity, 0U);
}

[[nodiscard]] pk::PackedArgs packed_args(const Fixture& fixture, const std::uint32_t d) {
  const rx::QueryPlan<kPB> plan = rx::make_query_plan<kPB>(d);
  const std::size_t plane = static_cast<std::size_t>(plan.shift) * G::kViewWords;
  pk::PackedArgs args;
  args.x_occupancy = fixture.source.occupancy.data();
  args.x_deck0 = fixture.source_decks.deck0.data();
  args.x_deck1 = fixture.source_decks.deck1.data();
  args.x_tags = fixture.source.tags_dense.data();
  args.y_view = fixture.views.data() + plane;
  args.y_deck0 = fixture.target_deck0.data() + plane;
  args.y_deck1 = fixture.target_deck1.data() + plane;
  args.y_tags = fixture.target.tags_dense.data();
  args.r = plan.r;
  args.alpha = plan.alpha;
  args.word_offset = plan.word_offset;
  return args;
}

[[nodiscard]] rx::RouteArgs<kPB> dense_args(const Fixture& fixture, const std::uint32_t d) {
  const rx::QueryPlan<kPB> plan = rx::make_query_plan<kPB>(d);
  rx::RouteArgs<kPB> args;
  args.x_occupancy = fixture.source.occupancy.data();
  args.x_tags = fixture.source.tags_dense.data();
  args.y_view = fixture.views.data() + static_cast<std::size_t>(plan.shift) * G::kViewWords;
  args.y_tags = fixture.target.tags_dense.data();
  args.r = plan.r;
  args.alpha = plan.alpha;
  args.word_offset = plan.word_offset;
  return args;
}

[[nodiscard]] std::vector<std::uint32_t> truth_hits(const Fixture& fixture,
                                                    const std::uint32_t d) {
  std::vector<std::uint32_t> distinct_y = fixture.ys;
  distinct_y.erase(std::unique(distinct_y.begin(), distinct_y.end()), distinct_y.end());
  std::vector<std::uint32_t> distinct_x = fixture.xs;
  distinct_x.erase(std::unique(distinct_x.begin(), distinct_x.end()), distinct_x.end());
  std::vector<std::uint32_t> hits;
  for (const std::uint32_t x : distinct_x) {
    const std::uint32_t y = (x + d) & kMask;
    if (std::binary_search(distinct_y.begin(), distinct_y.end(), y)) {
      hits.push_back(rx::pack_hit<kPB>(x & G::kPositionMask, x >> G::kPositionBits,
                                       y >> G::kPositionBits));
    }
  }
  std::sort(hits.begin(), hits.end());
  return hits;
}

// Deck truth recomputed from the values: the distinct tags at each position.
void check_decks(const Fixture& fixture, const std::string& what) {
  std::vector<std::vector<std::uint32_t>> source_tags(G::kR);
  for (const std::uint32_t x : fixture.xs) {
    source_tags[x & G::kPositionMask].push_back(x >> G::kPositionBits);
  }
  std::vector<std::vector<std::uint32_t>> target_tags(G::kR);
  for (const std::uint32_t y : fixture.ys) {
    target_tags[y & G::kPositionMask].push_back(y >> G::kPositionBits);
  }
  const auto expected_byte = [](std::vector<std::uint32_t> tags) -> std::uint64_t {
    std::sort(tags.begin(), tags.end());
    tags.erase(std::unique(tags.begin(), tags.end()), tags.end());
    return tags.size() == 1U ? tags[0] : pk::kMultiTagFlag;
  };
  const auto expected_decks = [&](const std::vector<std::uint32_t>& positions,
                                  const std::vector<std::vector<std::uint32_t>>& tags,
                                  std::uint64_t& deck0, std::uint64_t& deck1) {
    deck0 = 0U;
    deck1 = 0U;
    for (std::size_t rank = 0U; rank < positions.size() && rank < 16U; ++rank) {
      const std::uint64_t byte = expected_byte(tags[positions[rank]]);
      if (rank < 8U) {
        deck0 |= byte << (8U * rank);
      } else {
        deck1 |= byte << (8U * (rank - 8U));
      }
    }
  };

  bool source_good = true;
  for (std::uint32_t word = 0U; word < G::kWords; ++word) {
    std::vector<std::uint32_t> positions;
    for (std::uint32_t bit = 0U; bit < 64U; ++bit) {
      if (!source_tags[word * 64U + bit].empty()) {
        positions.push_back(word * 64U + bit);
      }
    }
    std::uint64_t deck0 = 0U;
    std::uint64_t deck1 = 0U;
    expected_decks(positions, source_tags, deck0, deck1);
    source_good = source_good && fixture.source_decks.deck0[word] == deck0 &&
                  fixture.source_decks.deck1[word] == deck1;
  }
  check(source_good, what + " [source decks match value-derived truth]");

  bool target_good = true;
  for (std::uint32_t shift = 0U; shift < G::kShifts; ++shift) {
    for (std::uint32_t word = 0U; word < G::kWords + G::kSliceWords; ++word) {
      std::vector<std::uint32_t> positions;
      for (std::uint32_t bit = 0U; bit < 64U; ++bit) {
        const std::uint32_t position =
            ((word & (G::kWords - 1U)) * 64U + bit + shift) & G::kPositionMask;
        if (!target_tags[position].empty()) {
          positions.push_back(position);
        }
      }
      std::uint64_t deck0 = 0U;
      std::uint64_t deck1 = 0U;
      expected_decks(positions, target_tags, deck0, deck1);
      const std::size_t index = static_cast<std::size_t>(shift) * G::kViewWords + word;
      target_good = target_good && fixture.target_deck0[index] == deck0 &&
                    fixture.target_deck1[index] == deck1;
    }
  }
  check(target_good, what + " [target decks, every shift and word incl. wrap pad]");
}

// Every packed route must equal the truth; the uncounted routes must also
// equal the plain radix-13 dense kernel witness for witness, in order.
void check_routes(Fixture& fixture, const std::uint32_t d, const std::string& what,
                  pk::PackedCounters* const two_deck_totals = nullptr,
                  pk::PackedCounters* const one_deck_totals = nullptr) {
  check(fixture.built, what + " [fixture built]");
  if (!fixture.built) {
    return;
  }
  const std::string tag = what + " d=" + std::to_string(d);
  const std::vector<std::uint32_t> expected = truth_hits(fixture, d);
  const pk::PackedArgs args = packed_args(fixture, d);
  const std::uint32_t reference_found =
      rx::route_query_dense_portable<kPB>(dense_args(fixture, d), fixture.reference_hits.data());
  const std::vector<std::uint32_t> reference_stream(
      fixture.reference_hits.begin(), fixture.reference_hits.begin() + reference_found);

  const auto verify = [&](const std::uint32_t found, const char* name, const bool ordered) {
    std::vector<std::uint32_t> produced(fixture.hits.begin(), fixture.hits.begin() + found);
    if (ordered) {
      check(produced == reference_stream, tag + " [" + name + " == dense stream, in order]");
    }
    std::sort(produced.begin(), produced.end());
    check(produced == expected, tag + " [" + name + " == truth]");
  };

  verify(pk::route_query_portable<false>(args, fixture.hits.data()), "portable one-deck", true);
  verify(pk::route_query_portable<true>(args, fixture.hits.data()), "portable two-deck", true);

  pk::PackedCounters one{};
  verify(pk::route_query_portable_counted<false>(args, fixture.hits.data(), one),
         "portable one-deck counted", true);
  pk::PackedCounters two{};
  verify(pk::route_query_portable_counted<true>(args, fixture.hits.data(), two),
         "portable two-deck counted", true);
  check(one.packed_scalar_disagreements == 0U, tag + " [one-deck packed == scalar per word]");
  check(two.packed_scalar_disagreements == 0U, tag + " [two-deck packed == scalar per word]");
  check(two.candidate_bits == two.rank_0_7_bits + two.rank_8_15_bits + two.rank_16_plus_bits,
        tag + " [rank buckets partition candidate bits]");
  check(two.witnesses == expected.size(), tag + " [counted witnesses]");
  check(two.representation_exception_bits <= one.representation_exception_bits,
        tag + " [second deck never adds exceptions]");
  check(two.resolver_inputs <= two.candidate_bits, tag + " [resolver inputs <= candidates]");
  if (two_deck_totals != nullptr) {
    two_deck_totals->merge(two);
  }
  if (one_deck_totals != nullptr) {
    one_deck_totals->merge(one);
  }

  if (g_bmi2_executable) {
    verify(pk::bmi2::route_query<false>(args, fixture.hits.data()), "bmi2 one-deck", true);
    verify(pk::bmi2::route_query<true>(args, fixture.hits.data()), "bmi2 two-deck", true);
    verify(pk::bmi2::route_query_queued<false>(args, fixture.hits.data()), "bmi2 queued one-deck",
           true);
    verify(pk::bmi2::route_query_queued<true>(args, fixture.hits.data()), "bmi2 queued two-deck",
           true);
    verify(pk::bmi2::route_query_queued_dense_control(args, fixture.hits.data()),
           "bmi2 queued dense control", true);
  }
}

// ---------------------------------------------------------------------------
// Class-level tests.
// ---------------------------------------------------------------------------

void test_shift_extremes_and_carry_seam() {
  // Values placed around every seam, with singleton, multi-tag and duplicate
  // buckets, so that each seam offset splits a word with live candidates.
  std::uint64_t state = 0xABCDEF01ULL;
  std::vector<std::uint32_t> xs;
  std::vector<std::uint32_t> ys;
  for (std::uint32_t index = 0U; index < 3000U; ++index) {
    xs.push_back(static_cast<std::uint32_t>(next_random(state)) & kMask);
    ys.push_back(static_cast<std::uint32_t>(next_random(state)) & kMask);
  }
  // Make every x have at least one partner for a family of d, by construction.
  for (std::uint32_t index = 0U; index < 600U; ++index) {
    const std::uint32_t x = xs[index];
    ys.push_back((x + 8191U * 3U + index) & kMask);
  }
  xs.push_back(5U);
  xs.push_back(5U);  // duplicate residual: one tag, two postings
  xs.push_back(5U + 8192U * 7U);  // second distinct tag at position 5
  Fixture fixture;
  make_fixture(xs, ys, fixture);
  check_decks(fixture, "seam fixture");

  const std::uint32_t shifts[] = {0U, 1U, 63U, 64U, 65U, 127U, 128U, 4096U, 8128U, 8191U};
  for (const std::uint32_t r : shifts) {
    for (const std::uint32_t alpha : {0U, 1U, 31U, 62U, 63U}) {
      check_routes(fixture, r + 8192U * alpha, "shift extreme r=" + std::to_string(r));
    }
  }
  // Carry seam at every bit offset: threshold = 8192 - r has every low 6-bit
  // value, and each lands in a different word.
  for (std::uint32_t offset = 0U; offset < 64U; ++offset) {
    const std::uint32_t threshold = 64U * (1U + offset * 2U) + offset;
    const std::uint32_t r = G::kR - threshold;
    for (const std::uint32_t alpha : {0U, 63U}) {
      check_routes(fixture, r + 8192U * alpha, "carry seam offset=" + std::to_string(offset));
    }
  }
}

void test_deck_capacities() {
  // One source word with exactly N occupied positions, N in {8, 9, 16, 17},
  // against a target that makes every one of them a candidate (and a witness
  // for half of them), plus a wide (> 8 lane) candidate word.
  for (const std::uint32_t occupied : {7U, 8U, 9U, 15U, 16U, 17U, 24U, 64U}) {
    std::vector<std::uint32_t> xs;
    std::vector<std::uint32_t> ys;
    const std::uint32_t word = 37U;
    const std::uint32_t d = 1000U + 8192U * 11U;
    for (std::uint32_t k = 0U; k < occupied; ++k) {
      const std::uint32_t bit = (k * 64U) / occupied;  // spread across the word
      const std::uint32_t x = word * 64U + bit + 8192U * ((k * 5U) & 63U);
      xs.push_back(x);
      const std::uint32_t y = (x + d) & kMask;
      // Witness for even k; right position but wrong tag for odd k.
      ys.push_back((k & 1U) == 0U ? y : ((y + 8192U) & kMask));
    }
    Fixture fixture;
    make_fixture(xs, ys, fixture);
    check_decks(fixture, "capacity " + std::to_string(occupied));
    pk::PackedCounters two{};
    pk::PackedCounters one{};
    check_routes(fixture, d, "deck capacity " + std::to_string(occupied), &two, &one);
    if (occupied <= 8U) {
      check(one.representation_exception_bits == 0U,
            "capacity " + std::to_string(occupied) + " fits one deck");
    } else {
      check(one.representation_exception_bits == occupied,
            "capacity " + std::to_string(occupied) + " falls back whole without deck 1");
    }
    if (occupied <= 8U) {
      check(two.representation_exception_bits == 0U,
            "capacity " + std::to_string(occupied) + " needs no fallback with two decks");
    } else {
      // More than 8 candidate lanes cannot be compared in one packed register.
      check(two.representation_exception_bits == occupied,
            "capacity " + std::to_string(occupied) + " wide word falls back whole");
    }
  }

  // Ranks 8..15 reached with at most 8 candidate lanes: the second deck path.
  for (const std::uint32_t occupied : {9U, 16U, 17U}) {
    std::vector<std::uint32_t> xs;
    std::vector<std::uint32_t> ys;
    const std::uint32_t word = 90U;
    const std::uint32_t d = 64U * 3U + 8192U * 5U;  // whole-word shift, no seam here
    for (std::uint32_t k = 0U; k < occupied; ++k) {
      xs.push_back(word * 64U + k * 3U + 8192U * (k & 63U));
    }
    // Only the last two occupied positions have a partner.
    for (std::uint32_t k = occupied - 2U; k < occupied; ++k) {
      ys.push_back((xs[k] + d) & kMask);
    }
    Fixture fixture;
    make_fixture(xs, ys, fixture);
    pk::PackedCounters two{};
    pk::PackedCounters one{};
    check_routes(fixture, d, "high rank " + std::to_string(occupied), &two, &one);
    if (occupied <= 16U) {
      check(two.deck1_words == 1U && two.representation_exception_bits == 0U,
            "high rank " + std::to_string(occupied) + " consumed through deck 1");
    } else {
      check(two.representation_exception_bits == 2U,
            "rank 16 falls back even with two decks");
    }
    check(one.representation_exception_bits == 2U,
          "high rank " + std::to_string(occupied) + " falls back with one deck");
    check(two.witnesses == 2U, "high rank witnesses");
  }
}

void test_degenerate_occupancy() {
  // Zero occupancy on either side.
  {
    Fixture fixture;
    make_fixture({}, {1U, 2U, 3U}, fixture);
    check_routes(fixture, 17U, "empty source");
    Fixture other;
    make_fixture({1U, 2U, 3U}, {}, other);
    check_routes(other, 17U, "empty target");
  }
  // Full occupancy: every low position occupied, single tags; and the target
  // fully occupied with two tags everywhere (every lane multi-tag).
  {
    std::vector<std::uint32_t> xs;
    std::vector<std::uint32_t> ys;
    for (std::uint32_t position = 0U; position < G::kR; ++position) {
      xs.push_back(position + 8192U * ((position * 7U) & 63U));
      ys.push_back(position + 8192U * ((position * 3U) & 63U));
      ys.push_back(position + 8192U * ((position * 3U + 1U) & 63U));
    }
    Fixture fixture;
    make_fixture(xs, ys, fixture);
    check_decks(fixture, "full occupancy");
    for (const std::uint32_t d : {0U, 1U, 8191U, 8192U * 63U + 4095U}) {
      check_routes(fixture, d, "full occupancy");
    }
  }
  // Alternating occupancy with singleton tags on both sides.
  {
    std::vector<std::uint32_t> xs;
    std::vector<std::uint32_t> ys;
    for (std::uint32_t position = 0U; position < G::kR; position += 2U) {
      xs.push_back(position + 8192U * (position & 63U));
      ys.push_back((position + 1U) + 8192U * ((position >> 1U) & 63U));
    }
    Fixture fixture;
    make_fixture(xs, ys, fixture);
    check_decks(fixture, "alternating occupancy");
    std::uint64_t state = 0x77ULL;
    for (std::uint32_t trial = 0U; trial < 64U; ++trial) {
      check_routes(fixture, static_cast<std::uint32_t>(next_random(state)) & kMask,
                   "alternating occupancy");
    }
  }
}

void test_randomized_agreement() {
  std::uint64_t state = 0x2545F4914F6CDD1DULL;
  pk::PackedCounters two{};
  pk::PackedCounters one{};
  for (std::uint32_t trial = 0U; trial < 40U; ++trial) {
    // Sizes span sparse (authentic-like, ~500) through dense (> 8 per word).
    const std::size_t x_count = 1U + (next_random(state) % 3000U);
    const std::size_t y_count = 1U + (next_random(state) % 3000U);
    std::vector<std::uint32_t> xs;
    std::vector<std::uint32_t> ys;
    // Low positions drawn from a narrower range on some trials, to force
    // crowded words, multi-tag buckets and deck overflow.
    const std::uint32_t position_range = (trial % 3U == 0U) ? 1024U : G::kR;
    for (std::size_t index = 0U; index < x_count; ++index) {
      const std::uint32_t position =
          static_cast<std::uint32_t>(next_random(state) % position_range);
      xs.push_back(position + 8192U * static_cast<std::uint32_t>(next_random(state) & 63U));
    }
    for (std::size_t index = 0U; index < y_count; ++index) {
      const std::uint32_t position =
          static_cast<std::uint32_t>(next_random(state) % position_range);
      ys.push_back(position + 8192U * static_cast<std::uint32_t>(next_random(state) & 63U));
    }
    // Plant witnesses for a few queries so the resolver has real work.
    const std::uint32_t planted_d = static_cast<std::uint32_t>(next_random(state)) & kMask;
    for (std::size_t index = 0U; index < xs.size(); index += 7U) {
      ys.push_back((xs[index] + planted_d) & kMask);
    }
    Fixture fixture;
    make_fixture(xs, ys, fixture);
    if (trial < 6U) {
      check_decks(fixture, "randomized trial " + std::to_string(trial));
    }
    check_routes(fixture, planted_d, "randomized planted", &two, &one);
    for (std::uint32_t query = 0U; query < 24U; ++query) {
      check_routes(fixture, static_cast<std::uint32_t>(next_random(state)) & kMask,
                   "randomized agreement", &two, &one);
    }
  }
  // The randomized corpus must actually have exercised every path.
  check(two.multi_tag_bits > 0U, "randomized corpus exercised multi-tag lanes");
  check(two.deck1_words > 0U, "randomized corpus exercised the second deck");
  check(two.representation_exception_bits > 0U, "randomized corpus exercised whole-word fallback");
  check(two.carry_seam_words > 0U, "randomized corpus exercised carry seam words");
  check(two.packed_exact_survivors > 0U, "randomized corpus produced exact packed survivors");
  check(one.representation_exception_bits > two.representation_exception_bits,
        "one-deck variant falls back more often than two-deck");
  std::cout << "# randomized corpus: candidate_bits=" << two.candidate_bits
            << " multi_tag_bits=" << two.multi_tag_bits
            << " deck1_words=" << two.deck1_words
            << " exception_bits(two/one)=" << two.representation_exception_bits << '/'
            << one.representation_exception_bits << " seam_words=" << two.carry_seam_words
            << " survivors=" << two.packed_exact_survivors << " witnesses=" << two.witnesses
            << '\n';
}

}  // namespace

int main() {
  const ter::microreport8::IsaReport isa = ter::microreport8::detect_isa();
  const bool compiled = pk::bmi2::translation_unit_is_bmi2();
  g_bmi2_executable = compiled && isa.usable && isa.bmi2;
  std::cout << "# ter_radix13_packed_tests -- packed-tag mask consumer correctness\n";
  std::cout << "# bmi2 translation unit compiled: " << (compiled ? "yes" : "no")
            << "; cpu avx2+bmi2 usable: " << (isa.usable && isa.bmi2 ? "yes" : "no")
            << "; bmi2 routes tested: " << (g_bmi2_executable ? "yes" : "NO") << '\n';

  test_rank_mapping_exhaustive();
  test_expansion_table();
  test_packed_relation();
  test_shift_extremes_and_carry_seam();
  test_deck_capacities();
  test_degenerate_occupancy();
  test_randomized_agreement();

  std::cout << "checks=" << g_checks << " failures=" << g_failures << '\n';
  if (g_failures != 0U || !g_bmi2_executable) {
    std::cout << (g_failures != 0U ? "RESULT: FAIL\n" : "RESULT: FAIL (BMI2 routes not tested)\n");
    return EXIT_FAILURE;
  }
  std::cout << "RESULT: PASS\n";
  return EXIT_SUCCESS;
}
