// Tests for the LED string walk.
//
// The walk exists to find the one pixel on a string that is wrong, so what
// matters is that it goes strictly by position: every position once, in order,
// including the ones nothing is assigned to -- skipping a dark gap would make
// the operator's count drift from the walk's.

#include "LedStringWalk.h"
#include "doctest.h"

namespace
{
using LedStringWalk::Assignment;
using LedStringWalk::Kind;
using LedStringWalk::LedRef;
using LedStringWalk::String;

Assignment Lamp(int number, int led) { return {Kind::Lamp, number, led, "", 0}; }
Assignment Flasher(int number, int led) { return {Kind::Flasher, number, led, "", 0}; }
Assignment Gi(int number, int led) { return {Kind::Gi, number, led, "", 0}; }

String MakeString(int board, int port, int amount, std::vector<Assignment> assignments)
{
  String string;
  string.board = board;
  string.port = port;
  string.amount = amount;
  string.assignments = std::move(assignments);
  return string;
}
}  // namespace

TEST_CASE("positions are walked in wire order, not in lamp number order")
{
  const auto walk = LedStringWalk::Walk({MakeString(4, 29, 3, {Lamp(13, 2), Lamp(26, 0), Lamp(11, 1)})});
  REQUIRE(walk.size() == 3);
  CHECK(walk[0].ledNumber == 0);
  CHECK(walk[0].assignment.number == 26);
  CHECK(walk[1].ledNumber == 1);
  CHECK(walk[1].assignment.number == 11);
  CHECK(walk[2].ledNumber == 2);
  CHECK(walk[2].assignment.number == 13);
}

TEST_CASE("an unassigned position is still walked, and marked as such")
{
  const auto walk = LedStringWalk::Walk({MakeString(4, 29, 3, {Lamp(11, 0), Lamp(12, 2)})});
  REQUIRE(walk.size() == 3);
  CHECK(walk[0].assigned);
  CHECK_FALSE(walk[1].assigned);
  CHECK(walk[1].ledNumber == 1);
  CHECK(walk[2].assigned);
}

TEST_CASE("an LED with two assignments yields a position for each")
{
  const auto walk = LedStringWalk::Walk({MakeString(4, 29, 1, {Lamp(11, 0), Flasher(17, 0)})});
  REQUIRE(walk.size() == 2);
  CHECK(walk[0].assignment.kind == Kind::Lamp);
  CHECK(walk[1].assignment.kind == Kind::Flasher);
}

TEST_CASE("LEDs sharing a number name each other")
{
  const auto walk = LedStringWalk::Walk({MakeString(4, 29, 3, {Flasher(17, 0), Flasher(17, 1), Lamp(17, 2)})});
  REQUIRE(walk.size() == 3);
  REQUIRE(walk[0].sharedWith.size() == 1);
  CHECK(walk[0].sharedWith[0] == LedRef{4, 29, 1});
  REQUIRE(walk[1].sharedWith.size() == 1);
  CHECK(walk[1].sharedWith[0] == LedRef{4, 29, 0});
  // Lamp 17 and flasher 17 are different outputs.
  CHECK(walk[2].sharedWith.empty());
}

TEST_CASE("sharing reaches across strings, but not to LEDs beyond a string's end")
{
  const auto walk = LedStringWalk::Walk({
      MakeString(4, 29, 2, {Gi(1, 0), Lamp(11, 1)}),
      MakeString(3, 29, 2, {Gi(1, 1), Gi(1, 9)}),
  });
  // Board 3 first.
  REQUIRE(walk.size() == 4);
  CHECK(walk[0].board == 3);
  CHECK_FALSE(walk[0].assigned);
  CHECK(walk[1].board == 3);
  REQUIRE(walk[1].sharedWith.size() == 1);
  CHECK(walk[1].sharedWith[0] == LedRef{4, 29, 0});
  CHECK(walk[2].board == 4);
  REQUIRE(walk[2].sharedWith.size() == 1);
  CHECK(walk[2].sharedWith[0] == LedRef{3, 29, 1});
}

TEST_CASE("assignments beyond the string's amount are reported, not walked")
{
  const std::vector<String> strings{MakeString(2, 29, 7, {Flasher(17, 0), Lamp(38, 56), Lamp(84, 53), Lamp(1, -1)})};
  const auto walk = LedStringWalk::Walk(strings);
  CHECK(walk.size() == 7);

  const auto outside = LedStringWalk::OutOfRange(strings);
  REQUIRE(outside.size() == 3);
  CHECK(outside[0].ledNumber == -1);
  CHECK(outside[1].ledNumber == 53);
  CHECK(outside[1].assignment.number == 84);
  CHECK(outside[2].ledNumber == 56);
}

TEST_CASE("an empty string walks nothing")
{
  CHECK(LedStringWalk::Walk({MakeString(4, 29, 0, {Lamp(11, 0)})}).empty());
  CHECK(LedStringWalk::Walk({}).empty());
}
