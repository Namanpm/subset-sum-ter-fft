#pragma once
// Whole-root runs of the frozen root backends (ARWLF_WHOLE_ROOT.md): every
// microproblem of one root in one backend pass, called exactly as the regime
// campaign's timed rounds call them.  Wiring only; no frozen code changes.
//
// Checkpointing.  Frozen K2 and K2_WIDE walk the root family-major: for
// u = 0..63, for every s, microproblem (s, t = u - s mod 64) if selected.
// Selecting only the 64 cells of family u and calling the backend once per u,
// in ascending u, therefore performs the same microproblems in the same order
// as one call over the whole root; each per-u call is one resumable chunk.
// ARWLF (s-major, with a per-call leaf cache) and DISPATCH (one decision for
// the whole selection) are never chunked.

#include "ter_a64_chqr.hpp"
#include "ter_arwlf.hpp"
#include "ter_root_queue_kernels.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ter::arwlf_whole_root {

namespace rg = ter::root_generalization;
namespace mr8 = ter::microreport8;
namespace a64 = ter::a64;
namespace aw = ter::arwlf;

using Witness = std::array<EntryId, 3>;

enum class Backend : std::uint8_t { K2, K2Wide, Arwlf, Dispatch };

[[nodiscard]] const char* label(Backend backend) noexcept;
[[nodiscard]] std::optional<Backend> parse_backend(const std::string& text) noexcept;
// K2 and K2_WIDE run one chunk per target family u; ARWLF and DISPATCH one pass.
[[nodiscard]] bool family_chunked(Backend backend) noexcept;

// Selects every microproblem (s, t) with (s + t) mod 64 == u (64 cells).
void select_family(rg::RootIndex& index, std::uint32_t u);
// Selects all 4096 microproblems.
void select_all(rg::RootIndex& index);

struct Pass {
  double ms = 0.0;                  // the backend call, timed exactly as a campaign round
  std::vector<Witness> witnesses;   // residue-exact final triples, sorted
  rg::OutputDigest digest;
  aw::Dispatch dispatch;            // DISPATCH only
  aw::Status arwlf_status = aw::Status::Completed;  // ARWLF only
};

// Runs `backend` once on the index's current selection.  The timed region is
// the campaign's: sink.reset() and the backend call (DISPATCH resets its own
// sink).  Sink construction, witness sorting and the digest copy are untimed.
[[nodiscard]] Pass run_selection(Backend backend, const mr8::Dataset& data, rg::RootIndex& index,
                                 const a64::FamilyKeys& keys, rg::RadixTables& tables,
                                 const ter::translated_tag::AuthorityTables& authority);

}  // namespace ter::arwlf_whole_root
