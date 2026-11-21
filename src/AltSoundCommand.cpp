#include "AltSoundCommand.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace AltSoundCommand
{
namespace
{

struct NamedGen
{
  const char* name;
  uint64_t gen;
};

// The first name of each generation is the canonical one HardwareName prints.
constexpr NamedGen kNames[] = {
    {"none", kGenNone},
    {"wpcalpha1", kGenWpcAlpha1},
    {"wpcalpha2", kGenWpcAlpha2},
    {"wpcdmd", kGenWpcDmd},
    {"wpcfliptronic", kGenWpcFliptron},
    {"wpcfliptron", kGenWpcFliptron},
    {"wpcdcs", kGenWpcDcs},
    {"dcs", kGenWpcDcs},
    {"wpcs", kGenWpcSecurity},
    {"wpcsecurity", kGenWpcSecurity},
    {"wpc95dcs", kGenWpc95Dcs},
    {"wpc95", kGenWpc95},
    {"s11", kGenS11},
    {"s11x", kGenS11X},
    {"s11b2", kGenS11B2},
    {"s11c", kGenS11C},
    {"de", kGenDe},
    {"dedmd16", kGenDeDmd16},
    {"dedmd32", kGenDeDmd32},
    {"dedmd64", kGenDeDmd64},
    {"by17", kGenBy17},
    {"by35", kGenBy35},
    {"gts80", kGenGts80},
    {"ws", kGenWs},
    {"ws1", kGenWs1},
    {"ws2", kGenWs2},
};

std::string Normalize(const std::string& text)
{
  std::string out;
  for (const char c : text)
  {
    if (c == '-' || c == '_' || std::isspace(static_cast<unsigned char>(c)))
    {
      continue;
    }
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

std::string Hex(uint32_t value)
{
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "0x%04X", value);
  return buffer;
}

bool Fail(std::string* error, const std::string& message)
{
  if (error != nullptr)
  {
    *error = message;
  }
  return false;
}

}  // namespace

bool ParseHardware(const std::string& text, uint64_t* hardwareGen)
{
  const std::string normalized = Normalize(text);
  if (normalized.empty() || hardwareGen == nullptr)
  {
    return false;
  }
  for (const NamedGen& entry : kNames)
  {
    if (normalized == entry.name)
    {
      *hardwareGen = entry.gen;
      return true;
    }
  }
  if (std::isdigit(static_cast<unsigned char>(normalized[0])))
  {
    char* end = nullptr;
    const unsigned long long value = std::strtoull(normalized.c_str(), &end, 0);
    if (end != nullptr && *end == '\0')
    {
      *hardwareGen = static_cast<uint64_t>(value);
      return true;
    }
  }
  return false;
}

std::string HardwareName(uint64_t hardwareGen)
{
  for (const NamedGen& entry : kNames)
  {
    if (entry.gen == hardwareGen)
    {
      return entry.name;
    }
  }
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "0x%llX", static_cast<unsigned long long>(hardwareGen));
  return buffer;
}

bool SendsBytePairs(uint64_t hardwareGen)
{
  switch (hardwareGen)
  {
    case kGenWpcDcs:
    case kGenWpcSecurity:
    case kGenWpc95Dcs:
    case kGenWpc95:
    case kGenWs:
    case kGenWs1:
    case kGenWs2:
      return true;
    default:
      return false;
  }
}

bool Encode(uint64_t hardwareGen, uint32_t cmd, std::vector<uint32_t>* bytes, std::string* error)
{
  if (bytes == nullptr)
  {
    return Fail(error, "no output");
  }
  bytes->clear();
  const uint32_t high = (cmd >> 8) & 0xFF;
  const uint32_t low = cmd & 0xFF;
  const std::string name = HardwareName(hardwareGen);

  switch (hardwareGen)
  {
    // DCS: every command is two bytes, high first. libaltsound pairs them by
    // count, so a single byte would pair with whatever comes next.
    case kGenWpcDcs:
    case kGenWpcSecurity:
    case kGenWpc95Dcs:
    case kGenWpc95:
      if (cmd > 0xFFFF)
      {
        return Fail(error, Hex(cmd) + " does not fit the 16 bits of a " + name + " sound command");
      }
      *bytes = {high, low};
      return true;

    // WPC with the pre-DCS sound board: one byte, or 0x7A then a second one.
    // A lone 0x7A is the first half of a pair and would swallow the next byte.
    case kGenWpcAlpha2:
    case kGenWpcDmd:
    case kGenWpcFliptron:
      if (cmd <= 0xFF && cmd != 0x7A)
      {
        *bytes = {cmd};
        return true;
      }
      if (high == 0x7A && cmd <= 0xFFFF)
      {
        *bytes = {high, low};
        return true;
      }
      return Fail(error, Hex(cmd) + " is no " + name + " sound command: one byte other than 0x7A, or 0x7Axx");

    // System 11 and early WPC alphanumeric: single bytes. A second sound board
    // on S11X/S11B2 is addressed with the board argument, not the number.
    case kGenWpcAlpha1:
    case kGenS11:
    case kGenS11X:
    case kGenS11B2:
    case kGenS11C:
      if (cmd > 0xFF)
      {
        return Fail(error, Hex(cmd) + " is no " + name +
                               " sound command: one byte (a second board's 0x01xx is "
                               "its low byte with board 1)");
      }
      *bytes = {cmd};
      return true;

    // Data East: single bytes; 0x00 and 0xFF are the bus idling and ignored.
    case kGenDe:
    case kGenDeDmd16:
    case kGenDeDmd32:
    case kGenDeDmd64:
      if (cmd == 0x00 || cmd >= 0xFF)
      {
        return Fail(error, Hex(cmd) + " is no " + name + " sound command: one byte from 0x01 to 0xFE");
      }
      *bytes = {cmd};
      return true;

    // Whitestar: always two bytes, and the first is always 0xFC..0xFF.
    case kGenWs:
    case kGenWs1:
    case kGenWs2:
      if (cmd > 0xFFFF || (high & 0xFC) != 0xFC)
      {
        return Fail(error, Hex(cmd) + " is no " + name + " sound command: 0xFCxx to 0xFFxx");
      }
      *bytes = {high, low};
      return true;

    // libaltsound filters 0 here as the clock the board sees all the time.
    case kGenGts80:
      if (cmd == 0)
      {
        return Fail(error, "0x0000 is the " + name + " idle value, not a sound command");
      }
      *bytes = {cmd};
      return true;

    // None, Bally, and anything libaltsound does not special-case: it takes
    // each value as a whole command, so the number goes over as given.
    default:
      *bytes = {cmd};
      return true;
  }
}

}  // namespace AltSoundCommand
