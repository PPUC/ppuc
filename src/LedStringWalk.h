#pragma once

#include <cstdint>
#include <string>
#include <vector>

// The LEDs of the `ledStripes` strings, one at a time, in the order they sit on
// the wire.
//
// The lamp test walks lamp numbers, which on an LED string is an order nobody
// can follow along with a finger: lamp 11 may be the tenth pixel, lamp 12 the
// ninth. When a string misbehaves -- everything dark after one pixel, a pixel
// that never changes -- the question is always "which position is it", and
// that needs the walk to go 0, 1, 2, ... down the string, one pixel each.
//
// A board lights LEDs only by the number they are assigned to, so a flasher on
// three LEDs or a GI string on forty cannot light one of them. The test
// therefore configures the boards differently for its own session: every LED of
// every string becomes a lamp of its own, numbered from a range the rest of the
// machine does not use. Board configuration lives in RAM and is sent on every
// start, so the next normal start puts the real one back.
//
// Free of yaml-cpp and libppuc so the planning can be tested on its own;
// LedStringWalkYaml reads the strings and writes the test configuration.
namespace LedStringWalk
{

enum class Kind
{
  Lamp,
  Flasher,
  Gi,
};

const char* KindName(Kind kind);

struct Assignment
{
  Kind kind = Kind::Lamp;
  int number = 0;
  int ledNumber = 0;
  std::string description;
  uint32_t color = 0;
};

struct String
{
  int board = 0;
  int port = 0;
  // How many LEDs the board drives. Assignments at or beyond it never light.
  int amount = 0;
  std::string description;
  std::vector<Assignment> assignments;
};

// One LED of one string.
struct Pixel
{
  int board = 0;
  int port = 0;
  int amount = 0;
  std::string stringDescription;
  int ledNumber = 0;
  // What the game assigns to this LED, if anything.
  std::vector<Assignment> assignments;
  // The first assignment's colour, so a colour order problem shows; white for
  // an LED nothing is assigned to.
  uint32_t color = 0xFFFFFF;
  // The lamp number the test configures this LED as. Unique across all pixels.
  int testLamp = 0;
};

// Lamp numbers travel as one byte, and 0 is not a lamp.
constexpr int kFirstTestLamp = 1;
constexpr int kLastTestLamp = 255;

// Every LED 0 .. amount-1 of every string not on a board in `skippedBoards`,
// strings in board and then port order, each given its own lamp number that is
// not in `reservedLamps`. Fails, naming the shortfall, when there are more LEDs
// than free numbers.
bool PlanPixels(const std::vector<String>& strings, const std::vector<int>& skippedBoards,
                const std::vector<int>& reservedLamps, std::vector<Pixel>* pPixels, std::string* pError);

// Assignments outside 0 .. amount-1. They are configured, and they can never
// light, because the board only drives `amount` LEDs.
struct OutOfRangeAssignment
{
  int board = 0;
  int port = 0;
  int amount = 0;
  std::string stringDescription;
  Assignment assignment;
};
std::vector<OutOfRangeAssignment> OutOfRange(const std::vector<String>& strings);

// "1,3, 5" -> {1, 3, 5}. Anything that is not a number is ignored.
std::vector<int> ParseBoardList(const char* csv);

}  // namespace LedStringWalk
