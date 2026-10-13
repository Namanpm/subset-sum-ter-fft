#include "ter_arwlf_whole_root.hpp"

#include <algorithm>
#include <chrono>

namespace ter::arwlf_whole_root {

namespace {

using Clock = std::chrono::steady_clock;

}  // namespace

const char* label(const Backend backend) noexcept {
  switch (backend) {
    case Backend::K2:
      return "k2";
    case Backend::K2Wide:
      return "k2_wide";
    case Backend::Arwlf:
      return "arwlf";
    case Backend::Dispatch:
      return "dispatch";
  }
  return "?";
}

std::optional<Backend> parse_backend(const std::string& text) noexcept {
  for (const Backend b : {Backend::K2, Backend::K2Wide, Backend::Arwlf, Backend::Dispatch}) {
    if (text == label(b)) {
      return b;
    }
  }
  return std::nullopt;
}

bool family_chunked(const Backend backend) noexcept {
  return backend == Backend::K2 || backend == Backend::K2Wide;
}

void select_family(rg::RootIndex& index, const std::uint32_t u) {
  index.selected.fill(0U);
  for (std::uint32_t s = 0U; s < rg::kFamilies; ++s) {
    const std::uint32_t t = (u + rg::kFamilies - s) & (rg::kFamilies - 1U);  // (s + t) mod 64 == u
    index.selected[s * rg::kFamilies + t] = 1U;
  }
  index.selected_count = rg::kFamilies;
}

void select_all(rg::RootIndex& index) { rg::select_microproblems(index, rg::kFamilies * rg::kFamilies); }

Pass run_selection(const Backend backend, const mr8::Dataset& data, rg::RootIndex& index,
                   const a64::FamilyKeys& keys, rg::RadixTables& tables,
                   const ter::translated_tag::AuthorityTables& authority) {
  Pass pass;
  rg::RootSink sink;
  sink.data = &data;
  sink.index = &index;
  sink.authority = &authority;
  sink.witnesses = &pass.witnesses;
  sink.reset();
  // Each branch is the campaign's timed lambda for that backend, verbatim.
  const auto start = Clock::now();
  switch (backend) {
    case Backend::K2:
      sink.reset();
      rg::run_k2(data, sink);
      break;
    case Backend::K2Wide:
      sink.reset();
      a64::run_k2_wide(data, index, keys, sink, nullptr, false);
      break;
    case Backend::Arwlf:
      sink.reset();
      pass.arwlf_status = aw::run_arwlf(data, index, keys, aw::Config{}, sink, nullptr);
      break;
    case Backend::Dispatch:
      pass.dispatch = aw::dispatch_and_run(data, index, keys, &tables, sink);
      break;
  }
  pass.ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
  std::sort(pass.witnesses.begin(), pass.witnesses.end());
  pass.digest = sink.digest;
  return pass;
}

}  // namespace ter::arwlf_whole_root
