#include "ter_arwlf_solve.hpp"

#include "ter_root_generalization_workload.hpp"
#include "ter_root_snapshot.hpp"
#include "ter_translated_tag_dataset.hpp"

#include "ter/full_ter.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <utility>

namespace ter::arwlf_solve {

namespace {

namespace a64 = ter::a64;
namespace aw = ter::arwlf;
namespace rg = ter::root_generalization;
namespace rs = ter::root_snapshot;
namespace tt = ter::translated_tag;
namespace wr = ter::arwlf_whole_root;

using Clock = std::chrono::steady_clock;

[[nodiscard]] double ms_since(const Clock::time_point start) noexcept {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

AttemptOutcome fail(AttemptOutcome outcome, const Status status, std::string message) {
  outcome.status = status;
  outcome.message = std::move(message);
  outcome.parents.clear();
  return outcome;
}

}  // namespace

AttemptOutcome solve_attempt(const std::string& snapshot_path, const wr::Backend backend) {
  AttemptOutcome out;
  out.backend = wr::label(backend);

  // --- 1-3: the root stage on the workload -----------------------------------
  std::vector<wr::Witness> accepted;  // residue-exact and structurally valid, sorted
  {
    auto start = Clock::now();
    std::unique_ptr<rg::Workload> workload = rg::load_snapshot_workload(snapshot_path, /*with_authority=*/true);
    out.load_ms = ms_since(start);
    out.n = workload->n;
    out.seed = workload->seed;
    switch (workload->status) {
      case rg::WorkloadStatus::Completed:
        break;
      case rg::WorkloadStatus::NotApplicable:
        return fail(std::move(out), Status::NotApplicable, workload->status_detail);
      case rg::WorkloadStatus::ResourceLimit:
        return fail(std::move(out), Status::ResourceLimit, workload->status_detail);
      case rg::WorkloadStatus::Failed:
        return fail(std::move(out), Status::InvalidConfiguration, workload->status_detail);
    }
    if (!workload->authority.ok) {
      return fail(std::move(out), Status::InvalidConfiguration, "authority tables unavailable: " + workload->authority.message);
    }
    start = Clock::now();
    const a64::FamilyKeys keys = a64::build_family_keys(*workload);
    if (!keys.ok) {
      return fail(std::move(out), Status::NotApplicable, "full root keys unavailable for this snapshot");
    }
    rg::RootIndex index;
    rg::build_root_index(*workload->data, index);
    rg::RadixTables tables;
    tables.allocate();
    wr::select_all(index);
    out.keys_ms = ms_since(start);

    const wr::Pass pass = wr::run_selection(backend, *workload->data, index, keys, tables, workload->authority);
    out.root_ms = pass.ms;
    out.residue_exact = pass.digest.residue;
    if (backend == wr::Backend::Dispatch) {
      out.dispatch_ran = aw::backend_label(pass.dispatch.ran);
    }
    if (backend == wr::Backend::Arwlf && pass.arwlf_status == aw::Status::NotApplicable) {
      return fail(std::move(out), Status::NotApplicable, "ARWLF is not applicable to this root (too few residual bits)");
    }
    if (backend == wr::Backend::Arwlf && pass.arwlf_status != aw::Status::Completed) {
      return fail(std::move(out), Status::InvariantViolation, "ARWLF did not complete the root");
    }
    if (pass.digest.directory_misses != 0U || pass.digest.residual_mismatches != 0U ||
        pass.witnesses.size() != pass.digest.residue) {
      return fail(std::move(out), Status::InvariantViolation, "root sink reports directory misses or an inconsistent count");
    }
    // The root's structural rule is solve_full_ter's (DigitCounts{0, n/2, n/2}):
    // the digit sum of the three entries is binary with weight n/2.  The sink's
    // structural count is not used: tt::structural_parent_valid tests the
    // FiniteSanity shape's level-1 distribution, which no binary root parent has.
    const tt::AuthorityTables& authority = workload->authority;
    const std::uint32_t n = authority.n;
    for (const wr::Witness& w : pass.witnesses) {
      const std::int8_t* const a = authority.digits[0].data() + static_cast<std::size_t>(w[0]) * n;
      const std::int8_t* const b = authority.digits[1].data() + static_cast<std::size_t>(w[1]) * n;
      const std::int8_t* const c = authority.digits[2].data() + static_cast<std::size_t>(w[2]) * n;
      std::uint32_t weight = 0U;
      bool binary = true;
      for (std::uint32_t i = 0U; i < n && binary; ++i) {
        const int digit = a[i] + b[i] + c[i];
        binary = digit == 0 || digit == 1;
        weight += digit == 1 ? 1U : 0U;
      }
      if (binary && weight == n / 2U) {
        accepted.push_back(w);
      }
    }
    out.structural_triples = accepted.size();
  }  // the workload, keys, index and tables are released here

  // --- 4: parents from the raw, checksummed records ------------------------------
  auto start = Clock::now();
  rs::Snapshot snapshot;
  std::string error;
  if (!rs::load(snapshot_path, snapshot, error)) {
    return fail(std::move(out), Status::InvalidConfiguration, "snapshot reload failed: " + error);
  }
  const rs::Header& header = snapshot.header;
  out.attempt = header.attempt;
  out.attempt_seed = header.attempt_seed;
  if (header.n != out.n || header.seed != out.seed) {
    return fail(std::move(out), Status::InvariantViolation, "snapshot reload disagrees with the loaded workload");
  }
  if (accepted.empty()) {
    out.rebuild_ms = ms_since(start);
    out.status = Status::NoWitnessThisAttempt;
    return out;
  }
  for (const wr::Witness& w : accepted) {
    for (std::size_t list = 0U; list < 3U; ++list) {
      if (w[list] >= header.entries[list]) {
        return fail(std::move(out), Status::InvariantViolation, "accepted triple refers past the end of a list");
      }
    }
    ReferenceTernaryVector parent(header.n);
    for (std::uint32_t i = 0U; i < header.n; ++i) {
      const int digit = snapshot.digit(0U, w[0], i) + snapshot.digit(1U, w[1], i) + snapshot.digit(2U, w[2], i);
      if (digit < -1 || digit > 1) {
        return fail(std::move(out), Status::InvariantViolation, "an accepted triple's parent digit leaves {-1, 0, +1}");
      }
      parent[i] = static_cast<std::int8_t>(digit);
    }
    out.parents.push_back(std::move(parent));
  }
  std::sort(out.parents.begin(), out.parents.end());
  out.parents.erase(std::unique(out.parents.begin(), out.parents.end()), out.parents.end());
  out.rebuild_ms = ms_since(start);

  // --- 5: every parent against the original instance ---------------------------
  start = Clock::now();
  const Instance instance = rg::authentic_instance(header.n, header.seed);
  if (instance.n != header.n) {
    return fail(std::move(out), Status::InvalidConfiguration, "the attempt's instance could not be rebuilt");
  }
  for (const ReferenceTernaryVector& parent : out.parents) {
    const OperationResult verified = verify_full_ter_witness(instance, parent);
    if (verified.status != Status::Completed) {
      return fail(std::move(out), Status::InvariantViolation, "a root parent failed witness verification: " + verified.message);
    }
    bool planted = true;
    for (std::uint32_t i = 0U; i < header.n; ++i) {
      planted = planted && parent[i] == static_cast<std::int8_t>(instance.planted_root[i]);
    }
    out.planted_found = out.planted_found || planted;
  }
  out.verify_ms = ms_since(start);
  out.status = Status::Found;
  return out;
}

}  // namespace ter::arwlf_solve
