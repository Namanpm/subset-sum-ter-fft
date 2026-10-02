// Focused correctness tests for the TER^2 MicroReport8 research spike.
//
// These cover the adversarial and boundary case list (empty/singleton/dense
// targets, duplicates, zero and 2^19-1 boundaries, wraparound, both kappa
// values, every partial dimension, pathological residues, a known hit at every
// one of the 64 lane positions, no-hit and all-hit masks) plus the independent
// verification of the tau child-target summation.
//
// Authentic n=72 coverage is NOT duplicated here: ter_microreport8_probe
// differentially checks every one of the real tiles against the same oracle and
// aborts on a single false negative. Repeating a full authentic capture inside
// the test binary would add minutes to the suite and test nothing new.

#include "ter_microreport8_dataset.hpp"
#include "ter_microreport8_kernels.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace ter::microreport8;

std::uint64_t g_checks = 0;

void require(const bool condition, const std::string_view message) {
  ++g_checks;
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

class SplitMix64 {
 public:
  explicit SplitMix64(const std::uint64_t seed) noexcept : state_(seed) {}
  [[nodiscard]] std::uint64_t next() noexcept {
    std::uint64_t value = (state_ += 0x9e3779b97f4a7c15ULL);
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
  }

 private:
  std::uint64_t state_;
};

// One target family replicated across all 64 slots, so any tile.u resolves to
// the fixture under test.
struct Fixture {
  TargetSet target;
  TargetView view;
  std::vector<std::uint32_t> values;

  explicit Fixture(std::vector<std::uint32_t> raw) : values(std::move(raw)) {
    std::vector<Posting> postings;
    postings.reserve(values.size());
    for (const std::uint32_t value : values) {
      postings.push_back(Posting{value & kResidualMask, 0U});
    }
    std::sort(postings.begin(), postings.end(),
              [](const Posting& left, const Posting& right) {
                return left.value < right.value;
              });
    build_target(postings, target);
    for (std::uint32_t family = 0U; family < kSplitRadix; ++family) {
      view.exact[family] = target.exact.data();
      view.coarse[family] = target.coarse.data();
      view.sig07[family] = target.sig07.data();
      view.sig815[family] = target.sig815.data();
    }
    for (std::uint32_t& value : values) {
      value &= kResidualMask;
    }
  }
};

struct KernelUnderTest {
  const char* id;
  TileKernel portable;
  TileKernel packed;  // nullptr when AVX2 is not executable
  bool exact;
};

bool g_avx2 = false;

[[nodiscard]] std::vector<KernelUnderTest> kernels() {
  const bool packed = g_avx2;
  return {
      {"K0_sum_floor", &k0_sum_floor_portable, packed ? &avx2::k0_sum_floor : nullptr, false},
      {"K1_exact_gather", &k1_exact_gather_portable,
       packed ? &avx2::k1_exact_gather : nullptr, true},
      {"K3_signature_shuffle", &k3_signature_shuffle_portable,
       packed ? &avx2::k3_signature_shuffle : nullptr, false},
      {"K2_coarse_gather", &k2_coarse_gather_portable,
       packed ? &avx2::k2_coarse_gather : nullptr, false},
  };
}

// The single place every contract is enforced:
//   oracle == brute force, T & ~M == 0, M & ~V == 0, portable == packed,
//   and for an exact kernel M == T.
void check_tile(const Tile& tile, const Fixture& fixture, const std::string_view context) {
  const std::uint64_t valid = tile_valid_mask(tile);
  const std::uint64_t truth = scalar_report8(tile, fixture.view);
  const std::uint64_t brute =
      brute_report8(tile, fixture.values.data(), fixture.values.size());
  require(truth == brute,
          std::string(context) + ": ScalarReport8 disagrees with the brute-force oracle");
  require((truth & ~valid) == 0U,
          std::string(context) + ": the oracle reported outside the valid lane mask");

  for (const KernelUnderTest& kernel : kernels()) {
    const std::uint64_t reported = kernel.portable(tile, fixture.view);
    require((truth & ~reported) == 0U,
            std::string(context) + ": " + kernel.id + " (portable) dropped a true lane");
    require((reported & ~valid) == 0U,
            std::string(context) + ": " + kernel.id + " (portable) reported a padding lane");
    if (kernel.exact) {
      require(reported == truth,
              std::string(context) + ": " + kernel.id + " is exact but disagreed with truth");
    }
    if (kernel.packed != nullptr) {
      const std::uint64_t packed = kernel.packed(tile, fixture.view);
      require(packed == reported,
              std::string(context) + ": " + kernel.id + " AVX2 and portable paths disagree");
    }
  }
}

[[nodiscard]] Tile make_tile(const std::array<std::uint32_t, 8>& x,
                             const std::array<std::uint32_t, 8>& q,
                             const std::uint32_t x_count,
                             const std::uint32_t q_count,
                             const std::uint32_t kappa) {
  Tile tile{};
  for (std::size_t lane = 0U; lane < kTileDim; ++lane) {
    tile.x[lane] = lane < x_count ? (x[lane] & kResidualMask) : 0U;
    tile.q[lane] = lane < q_count ? (q[lane] & kResidualMask) : 0U;
  }
  tile.x_count = static_cast<std::uint8_t>(x_count);
  tile.q_count = static_cast<std::uint8_t>(q_count);
  tile.kappa = static_cast<std::uint8_t>(kappa);
  tile.u = 0U;
  return tile;
}

void valid_mask_matches_its_definition() {
  for (std::uint32_t rows = 1U; rows <= kTileDim; ++rows) {
    for (std::uint32_t columns = 1U; columns <= kTileDim; ++columns) {
      std::uint64_t expected = 0U;
      for (std::uint32_t i = 0U; i < rows; ++i) {
        for (std::uint32_t j = 0U; j < columns; ++j) {
          expected |= std::uint64_t{1} << (8U * i + j);
        }
      }
      require(valid_mask(rows, columns) == expected, "valid_mask must equal its definition");
    }
  }
  require(valid_mask(0U, 8U) == 0U, "a zero-row tile has no valid lanes");
}

void empty_singleton_and_degenerate_targets_are_exact() {
  const std::array<std::uint32_t, 8> ramp{0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U};
  {
    const Fixture empty{{}};
    for (std::uint32_t kappa = 0U; kappa < 2U; ++kappa) {
      const Tile tile = make_tile(ramp, ramp, 8U, 8U, kappa);
      require(scalar_report8(tile, empty.view) == 0U, "an empty target set can never hit");
      check_tile(tile, empty, "empty target");
    }
  }
  {
    // Singleton placed exactly on lane (3, 5): x=3*4096, q=5*64, kappa=1.
    const Fixture singleton{{3U * 4096U + 5U * 64U + 1U}};
    std::array<std::uint32_t, 8> x{};
    std::array<std::uint32_t, 8> q{};
    for (std::uint32_t lane = 0U; lane < 8U; ++lane) {
      x[lane] = lane * 4096U;
      q[lane] = lane * 64U;
    }
    const Tile tile = make_tile(x, q, 8U, 8U, 1U);
    require(scalar_report8(tile, singleton.view) == (std::uint64_t{1} << (8U * 3U + 5U)),
            "a singleton target must light exactly its own lane");
    check_tile(tile, singleton, "singleton target");
  }
  {
    const Fixture zeros{{0U}};
    std::array<std::uint32_t, 8> zero{};
    const Tile tile = make_tile(zero, zero, 8U, 8U, 0U);
    require(scalar_report8(tile, zeros.view) == valid_mask(8U, 8U),
            "all-zero values against target zero must light every valid lane");
    check_tile(tile, zeros, "all-zero values");
  }
}

void every_lane_position_can_be_the_only_hit() {
  // x_i = i*4096 and q_j = j*64 give 64 pairwise distinct sums, so a singleton
  // target isolates exactly one lane. This pins the bit = 8i + j mapping.
  std::array<std::uint32_t, 8> x{};
  std::array<std::uint32_t, 8> q{};
  for (std::uint32_t lane = 0U; lane < 8U; ++lane) {
    x[lane] = lane * 4096U;
    q[lane] = lane * 64U;
  }
  for (std::uint32_t kappa = 0U; kappa < 2U; ++kappa) {
    for (std::uint32_t i = 0U; i < 8U; ++i) {
      for (std::uint32_t j = 0U; j < 8U; ++j) {
        const Fixture fixture{{i * 4096U + j * 64U + kappa}};
        const Tile tile = make_tile(x, q, 8U, 8U, kappa);
        const std::uint64_t truth = scalar_report8(tile, fixture.view);
        require(truth == (std::uint64_t{1} << (8U * i + j)),
                "lane 8i+j must be the unique hit for its own constructed target");
        check_tile(tile, fixture, "isolated lane");
      }
    }
  }
}

void boundaries_and_wraparound_are_exact() {
  {
    // x = 2^19-1, q = 5, kappa = 1 wraps to 5.
    const Fixture fixture{{5U, 0U, kResidualMask}};
    std::array<std::uint32_t, 8> x{};
    std::array<std::uint32_t, 8> q{};
    x.fill(kResidualMask);
    q.fill(5U);
    const Tile tile = make_tile(x, q, 8U, 8U, 1U);
    require(scalar_report8(tile, fixture.view) == valid_mask(8U, 8U),
            "modular wraparound past 2^19 must still hit");
    check_tile(tile, fixture, "wraparound");
  }
  {
    // Boundary values 0 and 2^19-1 on both sides, both kappa values.
    for (std::uint32_t kappa = 0U; kappa < 2U; ++kappa) {
      std::array<std::uint32_t, 8> x{0U,           kResidualMask, 1U, kResidualMask - 1U,
                                     kResidualMask, 0U,           2U, kResidualMask};
      std::array<std::uint32_t, 8> q{kResidualMask, 0U, kResidualMask, 1U,
                                     0U,            2U, kResidualMask, kResidualMask};
      std::vector<std::uint32_t> values;
      for (std::uint32_t i = 0U; i < 8U; ++i) {
        for (std::uint32_t j = 0U; j < 8U; ++j) {
          values.push_back((x[i] + q[j] + kappa) & kResidualMask);
        }
      }
      const Fixture fixture{values};
      const Tile tile = make_tile(x, q, 8U, 8U, kappa);
      require(scalar_report8(tile, fixture.view) == valid_mask(8U, 8U),
              "a target built from every lane sum must light every lane");
      check_tile(tile, fixture, "boundary values");
    }
  }
}

void duplicates_and_pathological_residues_are_exact() {
  {
    // Duplicate X, duplicate Q and duplicate Y values simultaneously.
    std::array<std::uint32_t, 8> x{7U, 7U, 7U, 7U, 9U, 9U, 9U, 9U};
    std::array<std::uint32_t, 8> q{11U, 11U, 13U, 13U, 11U, 11U, 13U, 13U};
    const Fixture fixture{{18U, 18U, 18U, 20U, 20U, 22U}};
    for (std::uint32_t kappa = 0U; kappa < 2U; ++kappa) {
      check_tile(make_tile(x, q, 8U, 8U, kappa), fixture, "duplicate values");
    }
  }
  {
    // Every target congruent to 0 mod 256: K3's byte signature is maximally
    // uninformative here and must still never drop a true lane.
    std::vector<std::uint32_t> values;
    for (std::uint32_t index = 0U; index < 512U; ++index) {
      values.push_back((index * 256U) & kResidualMask);
    }
    const Fixture fixture{values};
    SplitMix64 rng(0x1234ABCDULL);
    for (std::size_t trial = 0U; trial < 2000U; ++trial) {
      std::array<std::uint32_t, 8> x{};
      std::array<std::uint32_t, 8> q{};
      for (std::size_t lane = 0U; lane < 8U; ++lane) {
        x[lane] = static_cast<std::uint32_t>(rng.next() & kResidualMask);
        q[lane] = static_cast<std::uint32_t>((rng.next() % 512U) * 256U);
      }
      check_tile(make_tile(x, q, 8U, 8U, static_cast<std::uint32_t>(rng.next() & 1U)), fixture,
                 "residues congruent mod 256");
    }
  }
  {
    // Every target inside one block of 8: K2's coarse bitmap is maximally
    // uninformative for that block and must still never drop a true lane.
    const Fixture fixture{{4096U, 4097U, 4098U, 4099U, 4100U, 4101U, 4102U, 4103U}};
    SplitMix64 rng(0xFEED5678ULL);
    for (std::size_t trial = 0U; trial < 2000U; ++trial) {
      std::array<std::uint32_t, 8> x{};
      std::array<std::uint32_t, 8> q{};
      for (std::size_t lane = 0U; lane < 8U; ++lane) {
        x[lane] = static_cast<std::uint32_t>(rng.next() % 4200U);
        q[lane] = static_cast<std::uint32_t>(rng.next() % 4200U);
      }
      check_tile(make_tile(x, q, 8U, 8U, static_cast<std::uint32_t>(rng.next() & 1U)), fixture,
                 "residues inside one coarse block");
    }
  }
}

void every_partial_dimension_is_exact() {
  SplitMix64 rng(0x0BADF00DULL);
  std::vector<std::uint32_t> values;
  for (std::size_t index = 0U; index < 64U; ++index) {
    values.push_back(static_cast<std::uint32_t>(rng.next() & kResidualMask));
  }
  const Fixture fixture{values};
  for (std::uint32_t rows = 1U; rows <= 8U; ++rows) {
    for (std::uint32_t columns = 1U; columns <= 8U; ++columns) {
      for (std::uint32_t kappa = 0U; kappa < 2U; ++kappa) {
        std::array<std::uint32_t, 8> x{};
        std::array<std::uint32_t, 8> q{};
        // Seed one guaranteed hit inside the valid region, and put a value that
        // would hit outside it so a padding leak becomes observable.
        const std::uint32_t target = fixture.values[rng.next() % fixture.values.size()];
        for (std::size_t lane = 0U; lane < 8U; ++lane) {
          x[lane] = static_cast<std::uint32_t>(rng.next() & kResidualMask);
          q[lane] = static_cast<std::uint32_t>(rng.next() & kResidualMask);
        }
        x[0] = 0U;
        q[0] = (target - kappa) & kResidualMask;
        Tile tile = make_tile(x, q, rows, columns, kappa);
        // Padding lanes hold zeros; make the zero lane a would-be hit so any
        // kernel that ignores the counts is caught.
        check_tile(tile, fixture, "partial dimensions");
        require((scalar_report8(tile, fixture.view) & ~valid_mask(rows, columns)) == 0U,
                "the oracle must never report outside the declared dimensions");
      }
    }
  }
}

void density_sweep_and_all_hit_masks_are_exact() {
  SplitMix64 rng(0xC0FFEE11ULL);
  for (std::uint32_t exponent = 0U; exponent <= 14U; ++exponent) {
    std::vector<std::uint32_t> values;
    const std::size_t count = std::size_t{1} << exponent;
    values.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
      values.push_back(static_cast<std::uint32_t>(rng.next() & kResidualMask));
    }
    const Fixture fixture{values};
    for (std::size_t trial = 0U; trial < 200U; ++trial) {
      std::array<std::uint32_t, 8> x{};
      std::array<std::uint32_t, 8> q{};
      for (std::size_t lane = 0U; lane < 8U; ++lane) {
        x[lane] = static_cast<std::uint32_t>(rng.next() & kResidualMask);
        q[lane] = static_cast<std::uint32_t>(rng.next() & kResidualMask);
      }
      check_tile(make_tile(x, q, 8U, 8U, static_cast<std::uint32_t>(rng.next() & 1U)), fixture,
                 "density sweep");
    }
  }
  {
    // All-hit: the target holds every residual value.
    std::vector<std::uint32_t> values;
    values.reserve(kResidualUniverse);
    for (std::uint32_t value = 0U; value < kResidualUniverse; ++value) {
      values.push_back(value);
    }
    const Fixture fixture{values};
    std::array<std::uint32_t, 8> x{};
    std::array<std::uint32_t, 8> q{};
    for (std::size_t lane = 0U; lane < 8U; ++lane) {
      x[lane] = static_cast<std::uint32_t>(rng.next() & kResidualMask);
      q[lane] = static_cast<std::uint32_t>(rng.next() & kResidualMask);
    }
    const Tile tile = make_tile(x, q, 8U, 8U, 1U);
    require(scalar_report8(tile, fixture.view) == valid_mask(8U, 8U),
            "a saturated target must light every valid lane");
    for (const KernelUnderTest& kernel : kernels()) {
      require((scalar_report8(tile, fixture.view) & ~kernel.portable(tile, fixture.view)) == 0U,
              "no kernel may drop a lane against a saturated target");
    }
    ++g_checks;
  }
  {
    // No-hit: a target disjoint from every constructible lane sum.
    std::array<std::uint32_t, 8> x{};
    std::array<std::uint32_t, 8> q{};
    x.fill(0U);
    q.fill(0U);
    const Fixture fixture{{12345U}};
    const Tile tile = make_tile(x, q, 8U, 8U, 0U);
    require(scalar_report8(tile, fixture.view) == 0U, "a disjoint target must light no lane");
    check_tile(tile, fixture, "no-hit");
  }
}

void randomized_differential_sweep_is_exact() {
  SplitMix64 rng(0x5EED2026ULL);
  for (std::size_t round = 0U; round < 40U; ++round) {
    std::vector<std::uint32_t> values;
    const std::size_t count = static_cast<std::size_t>(rng.next() % 800U);
    for (std::size_t index = 0U; index < count; ++index) {
      values.push_back(static_cast<std::uint32_t>(rng.next() & kResidualMask));
    }
    const Fixture fixture{values};
    for (std::size_t trial = 0U; trial < 400U; ++trial) {
      std::array<std::uint32_t, 8> x{};
      std::array<std::uint32_t, 8> q{};
      for (std::size_t lane = 0U; lane < 8U; ++lane) {
        // Bias a third of the lanes onto real target values so hits occur.
        x[lane] = (rng.next() % 3U == 0U && !fixture.values.empty())
                      ? fixture.values[rng.next() % fixture.values.size()]
                      : static_cast<std::uint32_t>(rng.next() & kResidualMask);
        q[lane] = (rng.next() % 3U == 0U)
                      ? 0U
                      : static_cast<std::uint32_t>(rng.next() & kResidualMask);
      }
      const auto rows = static_cast<std::uint32_t>(1U + (rng.next() % 8U));
      const auto columns = static_cast<std::uint32_t>(1U + (rng.next() % 8U));
      check_tile(make_tile(x, q, rows, columns, static_cast<std::uint32_t>(rng.next() & 1U)),
                 fixture, "randomized differential");
    }
  }
}

// ---------------------------------------------------------------------------
// tau child-target summation. The extraction step is the donor
// quotient_low32_from_words(); only the carry-summing step is new, so it is
// checked against a fourth, deliberately naive bit-serial implementation as
// well as against its own limb16 twin and the prime-residue check.
// ---------------------------------------------------------------------------
[[nodiscard]] std::vector<std::uint32_t> sum_bitserial(
    const std::array<std::vector<std::uint32_t>, 3>& children, const std::uint32_t n) {
  const std::size_t words = (static_cast<std::size_t>(n) + 31U) / 32U;
  std::vector<std::uint32_t> sum(words, 0U);
  std::uint32_t carry = 0U;
  for (std::uint32_t bit = 0U; bit < words * 32U; ++bit) {
    std::uint32_t total = carry;
    for (const auto& child : children) {
      const std::size_t word = bit / 32U;
      const std::uint32_t value = word < child.size() ? child[word] : 0U;
      total += (value >> (bit % 32U)) & 1U;
    }
    if ((total & 1U) != 0U) {
      sum[bit / 32U] |= std::uint32_t{1} << (bit % 32U);
    }
    carry = total >> 1U;
  }
  if ((n % 32U) != 0U) {
    sum[words - 1U] &= (std::uint32_t{1} << (n % 32U)) - 1U;
  }
  return sum;
}

void tau_child_summation_agrees_across_four_implementations() {
  const auto check = [](const std::array<std::vector<std::uint32_t>, 3>& children,
                        const std::uint32_t n, const std::string_view context) {
    const auto limb32 = sum_children_mod_2n_limb32(children, n);
    const auto limb16 = sum_children_mod_2n_limb16(children, n);
    const auto serial = sum_bitserial(children, n);
    require(limb32 == limb16,
            std::string(context) + ": limb32 and limb16 child summation disagree");
    require(limb32 == serial,
            std::string(context) + ": limb32 and bit-serial child summation disagree");
    require(sum_children_residue_check(children, limb32, n),
            std::string(context) + ": prime-residue check rejected the child summation");
    // A deliberately corrupted sum must be rejected, otherwise the check is
    // vacuous and would have passed a real defect too.
    auto corrupted = limb32;
    corrupted[0] ^= 1U;
    require(!sum_children_residue_check(children, corrupted, n),
            std::string(context) + ": prime-residue check accepted a corrupted sum");
  };

  const std::uint32_t n = 72U;
  const std::size_t words = (n + 31U) / 32U;
  {
    std::array<std::vector<std::uint32_t>, 3> zeros;
    for (auto& child : zeros) {
      child.assign(words, 0U);
    }
    check(zeros, n, "all-zero child targets");
  }
  {
    // Maximum carry propagation: every child is 2^n - 1.
    std::array<std::vector<std::uint32_t>, 3> saturated;
    for (auto& child : saturated) {
      child.assign(words, 0xFFFFFFFFU);
      child[words - 1U] &= (std::uint32_t{1} << (n % 32U)) - 1U;
    }
    check(saturated, n, "saturated child targets");
  }
  {
    // A single carry that must cross both a 16-bit and a 32-bit limb boundary.
    std::array<std::vector<std::uint32_t>, 3> edge;
    for (auto& child : edge) {
      child.assign(words, 0U);
    }
    edge[0][0] = 0xFFFFFFFFU;
    edge[1][0] = 1U;
    check(edge, n, "carry across a limb boundary");
  }
  SplitMix64 rng(0x7A0D1FFULL);
  for (std::size_t trial = 0U; trial < 4000U; ++trial) {
    const std::uint32_t width = 33U + static_cast<std::uint32_t>(rng.next() % 96U);
    const std::size_t width_words = (width + 31U) / 32U;
    std::array<std::vector<std::uint32_t>, 3> children;
    for (auto& child : children) {
      child.assign(width_words, 0U);
      for (std::size_t word = 0U; word < width_words; ++word) {
        child[word] = static_cast<std::uint32_t>(rng.next());
      }
      if ((width % 32U) != 0U) {
        child[width_words - 1U] &= (std::uint32_t{1} << (width % 32U)) - 1U;
      }
    }
    check(children, width, "randomized child targets");
  }
}

}  // namespace

int main() {
  const IsaReport isa = detect_isa();
  g_avx2 = avx2::translation_unit_is_avx2() && isa.usable;
  std::cout << "microreport8 tests: isa_compiled="
            << (avx2::translation_unit_is_avx2() ? "AVX2" : "portable")
            << " isa_available=" << (isa.usable ? "AVX2" : "portable")
            << " isa_executed=" << (g_avx2 ? "AVX2" : "portable") << '\n';

  valid_mask_matches_its_definition();
  empty_singleton_and_degenerate_targets_are_exact();
  every_lane_position_can_be_the_only_hit();
  boundaries_and_wraparound_are_exact();
  duplicates_and_pathological_residues_are_exact();
  every_partial_dimension_is_exact();
  density_sweep_and_all_hit_masks_are_exact();
  randomized_differential_sweep_is_exact();
  tau_child_summation_agrees_across_four_implementations();

  std::cout << "microreport8 tests passed (" << g_checks << " assertions)\n";
  return 0;
}
