#pragma once

#include <cstdint>

namespace ter {

using EntryId = std::uint32_t;
using QuotientResidue = std::uint32_t;

enum class Status : std::uint8_t {
  Completed,
  InvalidConfiguration,
  InvalidShape,
  GenerationIncomplete,
  ResourceLimit,
  Cancelled,
  InvariantViolation,
};

enum class ShapeFamily : std::uint8_t {
  FiniteSanity,
  PaperRounded,
};

enum class ListRole : std::uint8_t {
  A,
  B,
  C,
};

}  // namespace ter
