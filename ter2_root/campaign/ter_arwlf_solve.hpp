#pragma once
// One Full TER attempt's root stage on a frozen root backend, ending in the
// solver's own witness verification (ARWLF_FULL_SOLVE.md).  Wiring only.
//
// Input: the packed snapshot of one attempt's three genuine L1 lists, written by
// ter::stream_full_ter_root_lists (ter_root_snapshot_capture --attempt=K): the
// lists solve_full_ter builds in that attempt, identity-gated against the
// classic capture where that capture fits (n = 36, 72).
//
// Steps, in this order:
//   1. load the snapshot as a root workload (keys, index, authority tables);
//   2. run the backend once over all 4096 microproblems (the timed root stage);
//   3. keep the residue-exact triples whose parent (the digit sum of the three
//      entries) is binary with weight n/2: solve_full_ter's root rule, not the
//      sink's FiniteSanity structural count;
//   4. release the workload, reload the raw snapshot records (checksummed),
//      and rebuild every kept triple's parent as the digit sum of its three
//      records, independently of the tables the backend used;
//   5. deduplicate the parents and verify every one with
//      ter::verify_full_ter_witness against the attempt's original instance.
//
// Outcome semantics mirror solve_full_ter's per-attempt semantics:
//   Found                 at least one parent, every parent verified;
//   NoWitnessThisAttempt  no structurally valid triple (never UNSAT);
//   NotApplicable         the root backends do not apply (e.g. Q < 25 at n = 36,
//                         or a forced ARWLF with too few residual bits);
//   ResourceLimit         the workload did not fit its declared limit;
//   InvalidConfiguration  the snapshot cannot be read or loaded;
//   InvariantViolation    any internal disagreement (count mismatch, a parent
//                         digit outside {-1, 0, +1}, a failed verification).

#include "ter_arwlf_whole_root.hpp"

#include "ter/reference_vector.hpp"
#include "ter/types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ter::arwlf_solve {

struct AttemptOutcome {
  Status status = Status::InvalidConfiguration;
  std::string message;
  std::uint32_t n = 0;
  std::uint32_t attempt = 0;
  std::uint64_t seed = 0;          // instance / config seed
  std::uint64_t attempt_seed = 0;  // seed + attempt
  std::string backend;
  std::string dispatch_ran;  // DISPATCH only
  double load_ms = 0.0;      // snapshot -> workload, untimed setup
  double keys_ms = 0.0;      // family keys + index + tables, untimed setup
  double root_ms = 0.0;      // the backend pass, timed exactly as the campaign
  double rebuild_ms = 0.0;   // raw reload + parent reconstruction
  double verify_ms = 0.0;    // verify_full_ter_witness over every parent
  std::uint64_t residue_exact = 0;       // sink digest
  std::uint64_t structural_triples = 0;  // accepted triples, before dedup
  std::vector<ReferenceTernaryVector> parents;  // unique, ascending, all verified
  bool planted_found = false;  // some parent equals the instance's planted root
};

[[nodiscard]] AttemptOutcome solve_attempt(const std::string& snapshot_path,
                                           arwlf_whole_root::Backend backend);

}  // namespace ter::arwlf_solve
