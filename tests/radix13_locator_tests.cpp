// TER^2 radix occupancy locator: focused correctness tests.
//
// Every test compares against exhaustive truth recomputed from the residual
// values themselves -- (x + d) mod 2^19 asked of the value set directly -- so
// the truth never uses the occupancy words, the tag words or the shifted views
// and a defect in the fast machinery cannot hide in both.
//
// The whole suite is a template over the position width and runs for every
// radix the benchmark sweeps.  A radix comparison is only meaningful if each
// radix is independently proved exact, and the H-bit tag rotation in particular
// has a different implementation below H = 64 than at H = 64.
//
// Coverage: builder invariants and the position-major/value-ascending group
// permutation; exhaustive shifted-view correctness over every (shift, word);
// the addressing and carry identity; rotation extremes including alpha = 0 and
// alpha = H-1 with a carry, which is the one place a naive rotate would shift
// by the full tag width; the circular seam where a 256-bit view load crosses
// the last position; fully dense tag buckets; duplicate residuals; empty and
// degenerate classes; and exact agreement of all six kernels.

#include "ter_radix13_locator.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

namespace rx = ter::radix;

std::uint64_t g_checks = 0U;
std::uint64_t g_failures = 0U;
std::string g_context;

void check(const bool condition, const std::string& what) {
  ++g_checks;
  if (!condition) {
    ++g_failures;
    if (g_failures <= 40U) {
      std::cerr << "FAIL: " << g_context << ": " << what << '\n';
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

// Exhaustive truth: every (x, y) value pair with y == (x + d) mod 2^19, packed
// the way the kernels pack their witnesses.
template <unsigned PB>
[[nodiscard]] std::vector<std::uint32_t> truth_hits(const std::vector<std::uint32_t>& xs,
                                                    const std::vector<std::uint32_t>& ys,
                                                    const std::uint32_t d) {
  using G = rx::Geom<PB>;
  std::vector<std::uint32_t> distinct_x = xs;
  std::sort(distinct_x.begin(), distinct_x.end());
  distinct_x.erase(std::unique(distinct_x.begin(), distinct_x.end()), distinct_x.end());
  std::vector<std::uint32_t> distinct_y = ys;
  std::sort(distinct_y.begin(), distinct_y.end());
  distinct_y.erase(std::unique(distinct_y.begin(), distinct_y.end()), distinct_y.end());

  std::vector<std::uint32_t> hits;
  for (const std::uint32_t x : distinct_x) {
    const std::uint32_t y = (x + d) & kMask;
    if (!std::binary_search(distinct_y.begin(), distinct_y.end(), y)) {
      continue;
    }
    hits.push_back(rx::pack_hit<PB>(x & G::kPositionMask, x >> G::kPositionBits,
                                    y >> G::kPositionBits));
  }
  std::sort(hits.begin(), hits.end());
  return hits;
}

template <unsigned PB>
struct Fixture {
  rx::SourceClass<PB> source;
  rx::TargetClass<PB> target;
  std::vector<std::uint64_t> views;
  std::vector<std::uint32_t> hits;
  std::vector<std::uint32_t> xs;
  std::vector<std::uint32_t> ys;
  bool built = false;
};

// Values must arrive ascending with duplicates adjacent, which is how the
// authentic families are stored.
template <unsigned PB>
void make_fixture(std::vector<std::uint32_t> xs, std::vector<std::uint32_t> ys,
                  Fixture<PB>& out) {
  using G = rx::Geom<PB>;
  std::sort(xs.begin(), xs.end());
  std::sort(ys.begin(), ys.end());
  out.xs = xs;
  out.ys = ys;
  const bool source_ok = rx::build_source_class<PB>(xs.data(), xs.size(), out.source);
  const bool target_ok = rx::build_target_class<PB>(ys.data(), ys.size(), out.target);
  out.built = source_ok && target_ok;
  out.views.assign(G::kViewWordsPerClass, 0U);
  rx::build_shifted_views<PB>(out.target, out.views.data());
  out.hits.assign(G::kHitCapacity, 0U);
}

template <unsigned PB>
[[nodiscard]] rx::RouteArgs<PB> args_for(Fixture<PB>& fixture, const std::uint32_t d,
                                         const bool compact) {
  using G = rx::Geom<PB>;
  const rx::QueryPlan<PB> plan = rx::make_query_plan<PB>(d);
  rx::RouteArgs<PB> args;
  args.x_occupancy = fixture.source.occupancy.data();
  args.x_prefix = fixture.source.occupied_prefix.data();
  args.y_view =
      fixture.views.data() + static_cast<std::size_t>(plan.shift) * G::kViewWords;
  args.y_bucket_rank = fixture.target.bucket_rank.data();
  args.r = plan.r;
  args.alpha = plan.alpha;
  args.word_offset = plan.word_offset;
  if (compact) {
    args.x_tags = fixture.source.tags_compact.data();
    args.y_tags = fixture.target.tags_compact.data();
  } else {
    args.x_tags = fixture.source.tags_dense.data();
    args.y_tags = fixture.target.tags_dense.data();
  }
  return args;
}

template <unsigned PB>
[[nodiscard]] std::vector<std::uint32_t> run_sorted(const rx::RouteArgs<PB>& args,
                                                    std::uint32_t* const buffer,
                                                    const int which) {
  std::uint32_t found = 0U;
  switch (which) {
    case 0:
      found = rx::route_query_dense_portable<PB>(args, buffer);
      break;
    case 1:
      found = rx::route_query_compact_portable<PB>(args, buffer);
      break;
    case 2:
      found = rx::avx2::route_query_dense<PB>(args, buffer);
      break;
    case 3:
      found = rx::avx2::route_query_compact<PB>(args, buffer);
      break;
    case 4:
      found = rx::route_query_batched_portable<PB>(args, buffer);
      break;
    default:
      found = rx::avx2::route_query_skip<PB>(args, buffer);
      break;
  }
  std::vector<std::uint32_t> hits(buffer, buffer + found);
  std::sort(hits.begin(), hits.end());
  return hits;
}

// Runs every kernel for one query and requires all of them to equal the truth.
template <unsigned PB>
void check_all_kernels(Fixture<PB>& fixture, const std::uint32_t d, const std::string& what) {
  check(fixture.built, what + " [fixture built]");
  if (!fixture.built) {
    return;
  }
  const std::vector<std::uint32_t> expected = truth_hits<PB>(fixture.xs, fixture.ys, d);
  static const char* const kNames[] = {"portable dense",  "portable compact", "avx2 dense",
                                       "avx2 compact",    "portable batched", "avx2 skip"};
  for (int which = 0; which < 6; ++which) {
    const rx::RouteArgs<PB> args = args_for<PB>(fixture, d, which == 1 || which == 3);
    const std::vector<std::uint32_t> produced =
        run_sorted<PB>(args, fixture.hits.data(), which);
    check(produced == expected, what + " [" + kNames[which] + "] d=" + std::to_string(d));
  }
  // The definitional scalar reference shares no bit-parallel machinery at all.
  std::vector<std::uint32_t> reference =
      rx::route_query_scalar<PB>(fixture.source, fixture.target, d);
  std::sort(reference.begin(), reference.end());
  check(reference == expected, what + " [scalar reference] d=" + std::to_string(d));
}

// ---------------------------------------------------------------------------

template <unsigned PB>
void test_builder_invariants() {
  using G = rx::Geom<PB>;
  std::uint64_t state = 0x5DEECE66DULL;
  std::vector<std::uint32_t> xs;
  for (std::uint32_t index = 0U; index < 4000U; ++index) {
    xs.push_back(static_cast<std::uint32_t>(next_random(state)) & kMask);
    if ((index % 7U) == 0U) {
      xs.push_back(xs.back());  // duplicates must not be collapsed
    }
  }
  Fixture<PB> fixture;
  make_fixture<PB>(xs, xs, fixture);
  check(fixture.built, "builders accepted a 4000-value class");

  std::vector<std::uint32_t> distinct = xs;
  std::sort(distinct.begin(), distinct.end());
  distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());
  check(fixture.source.distinct == distinct.size(), "distinct count");
  check(fixture.source.postings == xs.size(), "postings preserved");

  std::uint64_t span = 0U;
  for (const rx::tt::ResidualGroup& group : fixture.source.groups) {
    span += group.count;
  }
  check(span == xs.size(), "posting spans cover every input");

  // Occupancy, dense tags, compact tags and the rank directories must agree.
  std::uint32_t occupied = 0U;
  std::uint64_t mismatches = 0U;
  for (std::uint32_t position = 0U; position < G::kR; ++position) {
    const bool occupancy_bit =
        ((fixture.source.occupancy[position >> 6U] >> (position & 63U)) & 1U) != 0U;
    const bool has_tags = fixture.source.tags_dense[position] != 0U;
    if (occupancy_bit != has_tags) {
      ++mismatches;
    }
    // No tag word may carry a bit outside the H-bit tag universe.
    if ((fixture.source.tags_dense[position] & ~G::kTagWordMask) != 0U) {
      ++mismatches;
    }
    if (has_tags) {
      const std::uint32_t rank =
          fixture.source.occupied_prefix[position >> 6U] +
          static_cast<std::uint32_t>(
              std::popcount(fixture.source.occupancy[position >> 6U] &
                            ((std::uint64_t{1} << (position & 63U)) - 1U)));
      if (rank != occupied ||
          fixture.source.tags_compact[rank] != fixture.source.tags_dense[position] ||
          fixture.target.bucket_rank[position] != occupied ||
          fixture.target.tags_compact[fixture.target.bucket_rank[position]] !=
              fixture.target.tags_dense[position]) {
        ++mismatches;
      }
      ++occupied;
    }
  }
  check(mismatches == 0U, "occupancy, dense tags, compact tags and ranks all agree");
  check(occupied == fixture.source.occupied, "occupied position count");

  // The group permutation must recover exactly the group the value belongs to.
  std::uint64_t permutation_faults = 0U;
  for (std::uint32_t group = 0U; group < fixture.source.distinct; ++group) {
    const std::uint32_t value = fixture.source.groups[group].value;
    const std::uint32_t position = value & G::kPositionMask;
    const std::uint32_t tag = value >> G::kPositionBits;
    if (rx::source_group<PB>(fixture.source, position, tag) != group ||
        rx::target_group<PB>(fixture.target, position, tag) != group) {
      ++permutation_faults;
    }
  }
  check(permutation_faults == 0U, "group permutation is exact in both directions");
}

template <unsigned PB>
void test_shifted_views_exhaustive() {
  using G = rx::Geom<PB>;
  std::uint64_t state = 0xABCDEF0123456789ULL;
  std::vector<std::uint32_t> ys;
  for (std::uint32_t index = 0U; index < 3000U; ++index) {
    ys.push_back(static_cast<std::uint32_t>(next_random(state)) & kMask);
  }
  Fixture<PB> fixture;
  make_fixture<PB>(ys, ys, fixture);

  // view[shift][word] bit b must be occupancy bit (64*word + b + shift) mod R.
  std::uint64_t bad = 0U;
  for (std::uint32_t shift = 0U; shift < G::kShifts; ++shift) {
    const std::uint64_t* const view =
        fixture.views.data() + static_cast<std::size_t>(shift) * G::kViewWords;
    for (std::uint32_t word = 0U; word < G::kWords; ++word) {
      for (std::uint32_t bit = 0U; bit < 64U; ++bit) {
        const std::uint32_t source_position =
            (word * 64U + bit + shift) & G::kPositionMask;
        const bool expected =
            ((fixture.target.occupancy[source_position >> 6U] >>
              (source_position & 63U)) &
             1U) != 0U;
        const bool actual = ((view[word] >> bit) & 1U) != 0U;
        if (expected != actual) {
          ++bad;
        }
      }
    }
    for (std::uint32_t pad = 0U; pad < G::kSliceWords; ++pad) {
      if (view[G::kWords + pad] != view[pad]) {
        ++bad;
      }
    }
  }
  check(bad == 0U, "shifted views exact over every (shift, word, bit) and the wrap pad");
}

template <unsigned PB>
void test_addressing_and_carry_identity() {
  using G = rx::Geom<PB>;
  std::uint64_t bad = 0U;
  for (std::uint32_t r = 0U; r < G::kR; ++r) {
    const rx::QueryPlan<PB> plan = rx::make_query_plan<PB>(r);
    if (plan.r != r || plan.shift != (r & 63U) || plan.word_offset != (r >> 6U) ||
        plan.shift + 64U * plan.word_offset != r) {
      ++bad;
    }
    // A 256-bit load at any slice must stay inside the padded view.
    const std::uint32_t base = ((G::kWords - G::kSliceWords) + plan.word_offset) &
                               (G::kWords - 1U);
    if (base + G::kSliceWords > G::kViewWords) {
      ++bad;
    }
  }
  const std::uint32_t step = G::kR > 4096U ? 7U : 1U;
  for (std::uint32_t r = 0U; r < G::kR; r += step) {
    for (std::uint32_t i = 0U; i < G::kR; i += 13U) {
      const std::uint32_t sum = i + r;
      if ((sum & G::kPositionMask) != (i + r) % G::kR) {
        ++bad;
      }
      if ((sum >> G::kPositionBits) != ((i + r >= G::kR) ? 1U : 0U)) {
        ++bad;
      }
    }
  }
  // The H-bit rotation must be a genuine rotation for every amount, including
  // the zero amount and the amount that a 64-bit rotate would get wrong.
  for (std::uint32_t amount = 0U; amount < G::kH; ++amount) {
    for (std::uint32_t tag = 0U; tag < G::kH; ++tag) {
      const std::uint64_t rotated =
          rx::rotate_tags<PB>(std::uint64_t{1} << tag, amount);
      if (rotated != (std::uint64_t{1} << ((tag + amount) & G::kTagMask))) {
        ++bad;
      }
    }
    if (rx::rotate_tags<PB>(G::kTagWordMask, amount) != G::kTagWordMask) {
      ++bad;  // a full tag word must rotate to itself, with no bits lost or gained
    }
  }
  check(bad == 0U, "addressing, carry, view bounds and H-bit rotation are exact");
}

template <unsigned PB>
void test_rotation_and_carry_extremes() {
  using G = rx::Geom<PB>;
  // A position that carries with the maximal alpha needs a rotation by H, which
  // must behave as the identity rather than as an undefined shift.
  std::vector<std::uint32_t> xs;
  std::vector<std::uint32_t> ys;
  for (std::uint32_t tag = 0U; tag < G::kH; ++tag) {
    xs.push_back((G::kR - 1U) + G::kR * tag);  // the last position, every tag
    xs.push_back(0U + G::kR * tag);            // the first position, every tag
  }
  std::uint64_t state = 0x2468ACE013579BDFULL;
  for (std::uint32_t index = 0U; index < 4000U; ++index) {
    ys.push_back(static_cast<std::uint32_t>(next_random(state)) & kMask);
  }
  for (const std::uint32_t x : xs) {
    ys.push_back(x);  // guarantee hits at both ring ends
  }
  Fixture<PB> fixture;
  make_fixture<PB>(xs, ys, fixture);
  check(fixture.built, "dense tag fixture built");

  const std::uint32_t top_alpha = G::kH - 1U;
  check_all_kernels<PB>(fixture, (top_alpha << G::kPositionBits) | 1U,
                        "maximal alpha with carry");
  check_all_kernels<PB>(fixture, 0U, "alpha=0, r=0, zero rotation");
  check_all_kernels<PB>(fixture, 1U, "alpha=0 with a carry seam");
  check_all_kernels<PB>(fixture, top_alpha << G::kPositionBits, "maximal alpha, r=0");
  check_all_kernels<PB>(fixture, kMask, "maximal query");
}

template <unsigned PB>
void test_circular_seam() {
  using G = rx::Geom<PB>;
  // Positions clustered at both ends of the ring, so a 256-bit view load has to
  // cross the seam at the last position for many word offsets.
  std::vector<std::uint32_t> xs;
  std::vector<std::uint32_t> ys;
  for (std::uint32_t offset = 0U; offset < 200U; ++offset) {
    xs.push_back(G::kR - 1U - offset);
    xs.push_back(offset);
    ys.push_back(G::kR - 1U - offset + G::kR * (5U % G::kH));
    ys.push_back(offset + G::kR * (1U % G::kH));
  }
  Fixture<PB> fixture;
  make_fixture<PB>(xs, ys, fixture);
  std::uint64_t state = 0x13579BDF2468ACEULL;
  for (std::uint32_t trial = 0U; trial < 200U; ++trial) {
    check_all_kernels<PB>(fixture, static_cast<std::uint32_t>(next_random(state)) & kMask,
                          "circular seam");
  }
  // Every whole-word offset explicitly, so none can escape the wrap pad.
  const std::uint32_t step = G::kWords > 256U ? 4U : 1U;
  for (std::uint32_t word_offset = 0U; word_offset < G::kWords; word_offset += step) {
    check_all_kernels<PB>(fixture, word_offset * 64U, "every whole-word offset");
  }
}

template <unsigned PB>
void test_degenerate_classes() {
  using G = rx::Geom<PB>;
  Fixture<PB> empty_source;
  make_fixture<PB>({}, {5U, 9U, 100000U}, empty_source);
  check_all_kernels<PB>(empty_source, 12345U, "empty source");

  Fixture<PB> empty_target;
  make_fixture<PB>({5U, 9U, 100000U}, {}, empty_target);
  check_all_kernels<PB>(empty_target, 12345U, "empty target");

  Fixture<PB> singleton;
  make_fixture<PB>({77U}, {77U + 5U}, singleton);
  check_all_kernels<PB>(singleton, 5U, "singleton hit");
  check_all_kernels<PB>(singleton, 6U, "singleton miss");

  // One position holding every one of its H tags, matched against the same.
  std::vector<std::uint32_t> saturated;
  for (std::uint32_t tag = 0U; tag < G::kH; ++tag) {
    saturated.push_back((G::kR / 2U) + G::kR * tag);
  }
  Fixture<PB> dense;
  make_fixture<PB>(saturated, saturated, dense);
  check_all_kernels<PB>(dense, 0U, "saturated bucket, identity query");
  for (std::uint32_t alpha = 0U; alpha < G::kH; ++alpha) {
    check_all_kernels<PB>(dense, alpha << G::kPositionBits, "saturated bucket, rotated");
  }

  // Duplicates only: the value set is one element but the postings are many.
  std::vector<std::uint32_t> repeated(500U, 31337U);
  Fixture<PB> duplicates;
  make_fixture<PB>(repeated, repeated, duplicates);
  check(duplicates.source.distinct == 1U, "duplicates collapse to one group");
  check(duplicates.source.postings == 500U, "duplicate postings preserved");
  check_all_kernels<PB>(duplicates, 0U, "duplicate residuals");
}

template <unsigned PB>
void test_randomized_agreement() {
  std::uint64_t state = 0xFEEDFACECAFEBEEFULL;
  for (std::uint32_t trial = 0U; trial < 25U; ++trial) {
    const std::size_t x_count = 1U + (next_random(state) % 1500U);
    const std::size_t y_count = 1U + (next_random(state) % 1500U);
    std::vector<std::uint32_t> xs;
    std::vector<std::uint32_t> ys;
    for (std::size_t index = 0U; index < x_count; ++index) {
      xs.push_back(static_cast<std::uint32_t>(next_random(state)) & kMask);
    }
    for (std::size_t index = 0U; index < y_count; ++index) {
      ys.push_back(static_cast<std::uint32_t>(next_random(state)) & kMask);
    }
    Fixture<PB> fixture;
    make_fixture<PB>(xs, ys, fixture);
    for (std::uint32_t query = 0U; query < 10U; ++query) {
      check_all_kernels<PB>(fixture, static_cast<std::uint32_t>(next_random(state)) & kMask,
                            "randomized agreement");
    }
  }
}

template <unsigned PB>
void test_all_hit_geometry() {
  // Every source value has a matching target value, so the witness count equals
  // the distinct source count and no leaf may be dropped.
  std::vector<std::uint32_t> xs;
  for (std::uint32_t index = 0U; index < 2000U; ++index) {
    xs.push_back((index * 211U) & kMask);
  }
  const std::uint32_t d = 424242U & kMask;
  std::vector<std::uint32_t> ys;
  for (const std::uint32_t x : xs) {
    ys.push_back((x + d) & kMask);
  }
  Fixture<PB> fixture;
  make_fixture<PB>(xs, ys, fixture);
  const std::vector<std::uint32_t> expected = truth_hits<PB>(xs, ys, d);
  check(expected.size() == fixture.source.distinct, "all-hit geometry hits every value");
  check_all_kernels<PB>(fixture, d, "all-hit geometry");

  // The complementary no-hit geometry over the same source class.
  std::vector<std::uint32_t> filtered;
  for (const std::uint32_t x : xs) {
    const std::uint32_t candidate = (x + d + 1U) & kMask;
    if (truth_hits<PB>(xs, {candidate}, d).empty()) {
      filtered.push_back(candidate);
    }
  }
  Fixture<PB> miss;
  make_fixture<PB>(xs, filtered, miss);
  check(truth_hits<PB>(xs, filtered, d).empty(), "no-hit geometry really has no hits");
  check_all_kernels<PB>(miss, d, "no-hit geometry");
}

template <unsigned PB>
void run_suite() {
  using G = rx::Geom<PB>;
  g_context = "R=" + std::to_string(G::kR) + " H=" + std::to_string(G::kH);
  std::cout << "# " << g_context << " (" << G::kPositionBits << " position bits + "
            << G::kTagBits << " tag bits, " << G::kSlices << " guard vectors/query)\n";
  const std::uint64_t before = g_failures;
  test_builder_invariants<PB>();
  test_shifted_views_exhaustive<PB>();
  test_addressing_and_carry_identity<PB>();
  test_rotation_and_carry_extremes<PB>();
  test_circular_seam<PB>();
  test_degenerate_classes<PB>();
  test_randomized_agreement<PB>();
  test_all_hit_geometry<PB>();
  std::cout << "#   " << (g_failures == before ? "PASS" : "FAIL") << '\n';
}

}  // namespace

int main() {
  std::cout << "# ter_radix13_tests -- radix occupancy locator correctness\n";
  std::cout << "# avx2 translation unit compiled for AVX2: "
            << (rx::avx2::translation_unit_is_avx2() ? "yes" : "no") << '\n';

  run_suite<13>();
  run_suite<14>();
  run_suite<15>();
  run_suite<16>();

  std::cout << "checks=" << g_checks << " failures=" << g_failures << '\n';
  if (g_failures != 0U) {
    std::cout << "RESULT: FAIL\n";
    return EXIT_FAILURE;
  }
  std::cout << "RESULT: PASS\n";
  return EXIT_SUCCESS;
}
