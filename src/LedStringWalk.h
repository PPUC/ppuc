#pragma once

#include <cstdint>
#include <string>
#include <vector>

// The LEDs of the `ledStripes` strings, in the order they sit on the wire.
//
// The lamp test walks lamp numbers, which on an LED string is an order nobody
// can follow along with a finger: lamp 11 may be the tenth pixel, lamp 12 the
// ninth. When a string misbehaves -- everything dark after one pixel, a pixel
// that never changes -- the question is always "which position is it", and
// that needs the walk to go 0, 1, 2, ... down the string.
//
// A board lights LEDs by the number they are assigned to, never by position, so
// a step here drives the lamp, flasher or GI string the LED belongs to. That
// lights every other LED sharing that number too, on any string; the walk names
// them so a second LED coming on is expected rather than alarming.
//
// Free of yaml-cpp and libppuc so the ordering can be tested on its own;
// LedStringWalkYaml reads the strings from the game config.
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

struct LedRef
{
  int board = 0;
  int port = 0;
  int ledNumber = 0;

  bool operator==(const LedRef& other) const = default;
};

struct Position
{
  int board = 0;
  int port = 0;
  int amount = 0;
  std::string stringDescription;
  int ledNumber = 0;
  // False for a position nothing is assigned to: it has no number, so nothing
  // can light it, and it is expected to stay dark.
  bool assigned = false;
  Assignment assignment;
  // Every other LED that lights with this one because it shares the number.
  std::vector<LedRef> sharedWith;
};

// Positions 0 .. amount-1 of every string, strings in board and then port
// order. An LED with two assignments yields one position for each.
std::vector<Position> Walk(const std::vector<String>& strings);

// Assignments outside 0 .. amount-1. They are configured, and they can never
// light, because the board only drives `amount` LEDs.
std::vector<Position> OutOfRange(const std::vector<String>& strings);

}  // namespace LedStringWalk
