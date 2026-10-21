#include "LedStringWalk.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

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

bool Contains(const std::vector<int>& values, int value)
{
  return std::find(values.begin(), values.end(), value) != values.end();
}

}  // namespace

bool PlanPixels(const std::vector<String>& strings, const std::vector<int>& skippedBoards,
                const std::vector<int>& reservedLamps, std::vector<Pixel>* pPixels, std::string* pError)
{
  pPixels->clear();

  std::vector<Pixel> pixels;
  for (const String* string : InBoardOrder(strings))
  {
    if (Contains(skippedBoards, string->board))
    {
      continue;
    }
    for (int led = 0; led < string->amount; ++led)
    {
      Pixel pixel;
      pixel.board = string->board;
      pixel.port = string->port;
      pixel.amount = string->amount;
      pixel.stringDescription = string->description;
      pixel.ledNumber = led;
      for (const Assignment& assignment : string->assignments)
      {
        if (assignment.ledNumber == led)
        {
          pixel.assignments.push_back(assignment);
        }
      }
      if (!pixel.assignments.empty())
      {
        pixel.color = pixel.assignments.front().color;
      }
      pixels.push_back(std::move(pixel));
    }
  }

  int next = kFirstTestLamp;
  for (Pixel& pixel : pixels)
  {
    while (next <= kLastTestLamp && Contains(reservedLamps, next))
    {
      ++next;
    }
    if (next > kLastTestLamp)
    {
      int free = 0;
      for (int number = kFirstTestLamp; number <= kLastTestLamp; ++number)
      {
        free += Contains(reservedLamps, number) ? 0 : 1;
      }
      if (pError)
      {
        *pError = "the LED strings have " + std::to_string(pixels.size()) + " LEDs, but only " + std::to_string(free) +
                  " lamp numbers are free to give each its own; test fewer boards at a time with --skip-boards";
      }
      return false;
    }
    pixel.testLamp = next++;
  }

  *pPixels = std::move(pixels);
  return true;
}

std::vector<OutOfRangeAssignment> OutOfRange(const std::vector<String>& strings)
{
  std::vector<OutOfRangeAssignment> outside;
  for (const String* string : InBoardOrder(strings))
  {
    std::vector<Assignment> sorted = string->assignments;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const Assignment& a, const Assignment& b) { return a.ledNumber < b.ledNumber; });
    for (const Assignment& assignment : sorted)
    {
      if (assignment.ledNumber >= 0 && assignment.ledNumber < string->amount)
      {
        continue;
      }
      outside.push_back({string->board, string->port, string->amount, string->description, assignment});
    }
  }
  return outside;
}

std::vector<int> ParseBoardList(const char* csv)
{
  std::vector<int> boards;
  if (csv == nullptr)
  {
    return boards;
  }
  const char* p = csv;
  while (*p != '\0')
  {
    if (std::isdigit(static_cast<unsigned char>(*p)))
    {
      char* end = nullptr;
      boards.push_back(static_cast<int>(std::strtol(p, &end, 10)));
      p = end;
    }
    else
    {
      ++p;
    }
  }
  return boards;
}

}  // namespace LedStringWalk
