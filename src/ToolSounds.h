#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>

// The sounds the service tools make, as the game's own menu does: one sound
// command each for opening the menu, ENTER, UP, DOWN and ESCAPE, sent to the
// sound board's AltSound pack like a rule's ppuc.soundCommand. They are the
// game's, so they come from ppuc.ini:
//
//   [Runtime]
//   ToolSounds = 0x03DA, 0x03DB, 0x03D4, 0x03D5, 0x03DC
//
// Dirty Harry's, read from its ROM's test menu. 0 leaves that one silent, and
// without the key the tools make no sound at all.
struct ToolSounds
{
  enum Sound
  {
    Open,
    Enter,
    Up,
    Down,
    Escape,
    Count
  };

  uint32_t commands[Count] = {0, 0, 0, 0, 0};

  // Five numbers, decimal or 0x hex, separated by commas or spaces.
  static bool Parse(const std::string& text, ToolSounds* out, std::string* error)
  {
    ToolSounds parsed;
    int count = 0;
    size_t i = 0;
    while (i < text.size())
    {
      while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == ','))
      {
        ++i;
      }
      if (i >= text.size())
      {
        break;
      }
      const char* start = text.c_str() + i;
      char* end = nullptr;
      const unsigned long value = std::strtoul(start, &end, 0);
      if (end == start || (*end != '\0' && *end != ',' && *end != ' ' && *end != '\t'))
      {
        if (error) *error = "ToolSounds: '" + text + "' is not a list of numbers";
        return false;
      }
      if (value > 0xFFFF)
      {
        if (error) *error = "ToolSounds: " + std::string(start, static_cast<size_t>(end - start)) + " is not a sound command 0x0000-0xFFFF";
        return false;
      }
      if (count == Count)
      {
        if (error) *error = "ToolSounds: more than five sounds (open, enter, up, down, escape)";
        return false;
      }
      parsed.commands[count++] = static_cast<uint32_t>(value);
      i = static_cast<size_t>(end - text.c_str());
    }
    if (count != Count)
    {
      if (error) *error = "ToolSounds: five sounds needed (open, enter, up, down, escape), got " + std::to_string(count);
      return false;
    }
    if (out) *out = parsed;
    return true;
  }
};
