#pragma once

// Whether the board on an address is the hardware the configuration was
// written for.
//
// A port in the game YAML is a GPIO number, and what a GPIO is depends on the
// board: the same number is a coil driver on one type and a lamp column or a
// switch input on another. A configuration sent to the wrong type of board is
// refused pin by pin by current firmware, but the machine would still come up
// half configured, and older firmware does not check at all.
//
// Depends on nothing but <cstdint>, so ppuc_tests can cover it without
// libppuc.

#include <cstdint>

namespace boardtype
{

enum class Verdict
{
  Match,
  // The board says it is a different type than the configuration gives it.
  Mismatch,
  // Nothing to compare: the configuration does not list the board, or the
  // board did not say what it is. Both are zero, ppuc::v2::kBoardTypeUnknown.
  Unknown,
};

inline Verdict Compare(uint8_t configuredType, uint8_t reportedType)
{
  if (configuredType == 0 || reportedType == 0)
  {
    return Verdict::Unknown;
  }
  return configuredType == reportedType ? Verdict::Match : Verdict::Mismatch;
}

}  // namespace boardtype
