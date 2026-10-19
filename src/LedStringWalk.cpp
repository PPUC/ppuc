#include "LedStringWalk.h"

#include <algorithm>

namespace LedStringWalk
{

const char* KindName(Kind kind)
{
  switch (kind)
  {
    case Kind::Lamp:
      return "lamp";
    case Kind::Flasher:
      return "flasher";
    case Kind::Gi:
      return "GI string";
  }
  return "?";
}

namespace
{

bool InRange(const String& string, const Assignment& assignment)
{
  return assignment.ledNumber >= 0 && assignment.ledNumber < string.amount;
}

Position MakePosition(const String& string, int ledNumber)
{
  Position position;
  position.board = string.board;
  position.port = string.port;
  position.amount = string.amount;
  position.stringDescription = string.description;
  position.ledNumber = ledNumber;
  return position;
}

std::vector<const String*> InBoardOrder(const std::vector<String>& strings)
{
  std::vector<const String*> ordered;
  ordered.reserve(strings.size());
  for (const String& string : strings)
  {
    ordered.push_back(&string);
  }
  std::stable_sort(ordered.begin(), ordered.end(), [](const String* a, const String* b)
                   { return a->board != b->board ? a->board < b->board : a->port < b->port; });
  return ordered;
}

// The LEDs a board lights for this kind and number, other than `self`. Only
// in-range ones: an LED beyond the string's end lights nothing.
std::vector<LedRef> SharedWith(const std::vector<const String*>& strings, const Assignment& assignment,
                               const LedRef& self)
{
  std::vector<LedRef> shared;
  for (const String* string : strings)
  {
    for (const Assignment& other : string->assignments)
    {
      if (other.kind != assignment.kind || other.number != assignment.number || !InRange(*string, other))
      {
        continue;
      }
      const LedRef ref{string->board, string->port, other.ledNumber};
      if (ref == self || std::find(shared.begin(), shared.end(), ref) != shared.end())
      {
        continue;
      }
      shared.push_back(ref);
    }
  }
  std::sort(shared.begin(), shared.end(),
            [](const LedRef& a, const LedRef& b)
            {
              if (a.board != b.board) return a.board < b.board;
              if (a.port != b.port) return a.port < b.port;
              return a.ledNumber < b.ledNumber;
            });
  return shared;
}

}  // namespace

std::vector<Position> Walk(const std::vector<String>& strings)
{
  const std::vector<const String*> ordered = InBoardOrder(strings);
  std::vector<Position> walk;
  for (const String* string : ordered)
  {
    for (int led = 0; led < string->amount; ++led)
    {
      bool any = false;
      for (const Assignment& assignment : string->assignments)
      {
        if (assignment.ledNumber != led)
        {
          continue;
        }
        Position position = MakePosition(*string, led);
        position.assigned = true;
        position.assignment = assignment;
        position.sharedWith = SharedWith(ordered, assignment, {string->board, string->port, led});
        walk.push_back(std::move(position));
        any = true;
      }
      if (!any)
      {
        walk.push_back(MakePosition(*string, led));
      }
    }
  }
  return walk;
}

std::vector<Position> OutOfRange(const std::vector<String>& strings)
{
  std::vector<Position> outside;
  for (const String* string : InBoardOrder(strings))
  {
    for (const Assignment& assignment : string->assignments)
    {
      if (InRange(*string, assignment))
      {
        continue;
      }
      Position position = MakePosition(*string, assignment.ledNumber);
      position.assigned = true;
      position.assignment = assignment;
      outside.push_back(std::move(position));
    }
  }
  std::stable_sort(outside.begin(), outside.end(),
                   [](const Position& a, const Position& b)
                   {
                     if (a.board != b.board) return a.board < b.board;
                     if (a.port != b.port) return a.port < b.port;
                     return a.ledNumber < b.ledNumber;
                   });
  return outside;
}

}  // namespace LedStringWalk
