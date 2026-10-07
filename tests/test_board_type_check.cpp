// Tests for the comparison of a board's reported type with its configured one.
//
// The rule is small, and the cases that matter are the ones where there is
// nothing to compare: refusing to start because an old firmware did not say
// what it is would strand a machine that has always worked.

#include "BoardTypeCheck.h"
#include "doctest.h"

using boardtype::Compare;
using boardtype::Verdict;

TEST_CASE("a board of the configured type matches")
{
  for (uint8_t type = 1; type <= 4; ++type)
  {
    CHECK(Compare(type, type) == Verdict::Match);
  }
}

TEST_CASE("a board of another type is a mismatch")
{
  // An IO_16_8_1 configuration reaching an Out_8x10: every coil port is a lamp
  // driver there.
  CHECK(Compare(1, 3) == Verdict::Mismatch);
  CHECK(Compare(3, 1) == Verdict::Mismatch);
  CHECK(Compare(2, 4) == Verdict::Mismatch);
}

TEST_CASE("nothing is concluded without both sides")
{
  // Not listed in the configuration.
  CHECK(Compare(0, 1) == Verdict::Unknown);
  // Firmware that did not report a type.
  CHECK(Compare(1, 0) == Verdict::Unknown);
  CHECK(Compare(0, 0) == Verdict::Unknown);
}
