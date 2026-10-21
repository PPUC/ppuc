// Tests for the LED string walk.
//
// The walk exists to find the one pixel on a string that is wrong, so what
// matters is that it goes strictly by position, one pixel per step: every
// position once, in order, including the ones nothing is assigned to, and each
// with a lamp number nothing else answers to -- a pixel sharing its number with
// another, or with a bulb, would light more than one thing.

#include <algorithm>

#include "LedStringWalk.h"
#include "doctest.h"

namespace
{
using LedStringWalk::Assignment;
using LedStringWalk::Kind;
using LedStringWalk::Pixel;
using LedStringWalk::String;

Assignment Lamp(int number, int led) { return {Kind::Lamp, number, led, "", 0x00FF00}; }
Assignment Flasher(int number, int led) { return {Kind::Flasher, number, led, "", 0xFF0000}; }
Assignment Gi(int number, int led) { return {Kind::Gi, number, led, "", 0xFFFFFF}; }

String MakeString(int board, int port, int amount, std::vector<Assignment> assignments)
{
  String string;
  string.board = board;
  string.port = port;
  string.amount = amount;
  string.assignments = std::move(assignments);
  return string;
}

std::vector<Pixel> Plan(const std::vector<String>& strings, const std::vector<int>& skipped = {},
                        const std::vector<int>& reserved = {})
{
  std::vector<Pixel> pixels;
  std::string error;
  REQUIRE(LedStringWalk::PlanPixels(strings, skipped, reserved, &pixels, &error));
  return pixels;
}
}  // namespace

TEST_CASE("pixels are walked in wire order, not in lamp number order")
{
  const auto pixels = Plan({MakeString(4, 29, 3, {Lamp(13, 2), Lamp(26, 0), Lamp(11, 1)})});
  REQUIRE(pixels.size() == 3);
  CHECK(pixels[0].ledNumber == 0);
  CHECK(pixels[0].assignments.at(0).number == 26);
  CHECK(pixels[1].ledNumber == 1);
  CHECK(pixels[1].assignments.at(0).number == 11);
  CHECK(pixels[2].ledNumber == 2);
  CHECK(pixels[2].assignments.at(0).number == 13);
}

TEST_CASE("a flasher or GI string on several LEDs is still one pixel per step")
{
  const auto pixels = Plan({MakeString(3, 29, 5, {Gi(1, 0), Gi(1, 1), Gi(1, 2), Flasher(17, 3), Flasher(17, 4)})});
  REQUIRE(pixels.size() == 5);
  std::vector<int> lamps;
  for (const Pixel& pixel : pixels)
  {
    lamps.push_back(pixel.testLamp);
  }
  std::sort(lamps.begin(), lamps.end());
  CHECK(std::adjacent_find(lamps.begin(), lamps.end()) == lamps.end());
}

TEST_CASE("test lamp numbers are unique across strings and avoid reserved ones")
{
  const auto pixels = Plan({MakeString(4, 29, 3, {}), MakeString(3, 29, 2, {})}, {}, {1, 2, 4});
  REQUIRE(pixels.size() == 5);
  // Board 3 first, then board 4.
  CHECK(pixels[0].board == 3);
  CHECK(pixels[0].testLamp == 3);
  CHECK(pixels[1].testLamp == 5);
  CHECK(pixels[2].board == 4);
  CHECK(pixels[2].testLamp == 6);
  CHECK(pixels[4].testLamp == 8);
}

TEST_CASE("an unassigned LED is walked, in white")
{
  const auto pixels = Plan({MakeString(4, 29, 3, {Lamp(11, 0), Lamp(12, 2)})});
  REQUIRE(pixels.size() == 3);
  CHECK(pixels[0].color == 0x00FF00);
  CHECK(pixels[1].assignments.empty());
  CHECK(pixels[1].color == 0xFFFFFF);
}

TEST_CASE("an LED with two assignments is one pixel naming both")
{
  const auto pixels = Plan({MakeString(4, 29, 1, {Lamp(11, 0), Flasher(17, 0)})});
  REQUIRE(pixels.size() == 1);
  CHECK(pixels[0].assignments.size() == 2);
}

TEST_CASE("strings on skipped boards are left out")
{
  const auto pixels = Plan({MakeString(1, 29, 3, {}), MakeString(4, 29, 2, {})}, {1, 2, 3, 5});
  REQUIRE(pixels.size() == 2);
  CHECK(pixels[0].board == 4);
  CHECK(pixels[0].testLamp == 1);
}

TEST_CASE("more LEDs than free lamp numbers is refused")
{
  std::vector<Pixel> pixels;
  std::string error;
  CHECK_FALSE(LedStringWalk::PlanPixels({MakeString(4, 29, 300, {})}, {}, {}, &pixels, &error));
  CHECK(pixels.empty());
  CHECK(error.find("300 LEDs") != std::string::npos);
  CHECK(error.find("255") != std::string::npos);
}

TEST_CASE("assignments beyond the string's amount are reported")
{
  const auto outside =
      LedStringWalk::OutOfRange({MakeString(2, 29, 7, {Flasher(17, 0), Lamp(38, 56), Lamp(84, 53), Lamp(1, -1)})});
  REQUIRE(outside.size() == 3);
  CHECK(outside[0].assignment.ledNumber == -1);
  CHECK(outside[1].assignment.ledNumber == 53);
  CHECK(outside[1].assignment.number == 84);
  CHECK(outside[2].assignment.ledNumber == 56);
}

TEST_CASE("board lists parse the way --skip-boards is written")
{
  CHECK(LedStringWalk::ParseBoardList("1,2,3,5") == std::vector<int>{1, 2, 3, 5});
  CHECK(LedStringWalk::ParseBoardList(" 1, 10 ,x") == std::vector<int>{1, 10});
  CHECK(LedStringWalk::ParseBoardList(nullptr).empty());
  CHECK(LedStringWalk::ParseBoardList("").empty());
}
