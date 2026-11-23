// Tests for the ToolSounds key in ppuc.ini: the sounds the service tools make.

#include "ToolSounds.h"
#include "doctest.h"

TEST_CASE("ToolSounds: Dirty Harry's menu sounds, open, enter, up, down, escape")
{
  ToolSounds sounds;
  std::string error;
  REQUIRE_MESSAGE(ToolSounds::Parse("0x03DA, 0x03DB, 0x03D4, 0x03D5, 0x03DC", &sounds, &error), error);
  CHECK(sounds.commands[ToolSounds::Open] == 0x03DA);
  CHECK(sounds.commands[ToolSounds::Enter] == 0x03DB);
  CHECK(sounds.commands[ToolSounds::Up] == 0x03D4);
  CHECK(sounds.commands[ToolSounds::Down] == 0x03D5);
  CHECK(sounds.commands[ToolSounds::Escape] == 0x03DC);
}

TEST_CASE("ToolSounds: decimal, spaces, and 0 for a silent one")
{
  ToolSounds sounds;
  std::string error;
  REQUIRE_MESSAGE(ToolSounds::Parse("0 986 980 981 988", &sounds, &error), error);
  CHECK(sounds.commands[ToolSounds::Open] == 0);
  CHECK(sounds.commands[ToolSounds::Up] == 980);
}

TEST_CASE("ToolSounds: wrong counts and values are refused")
{
  ToolSounds sounds;
  std::string error;
  CHECK_FALSE(ToolSounds::Parse("0x03DA, 0x03DB", &sounds, &error));
  CHECK(error.find("five sounds needed") != std::string::npos);
  CHECK_FALSE(ToolSounds::Parse("1, 2, 3, 4, 5, 6", &sounds, &error));
  CHECK_FALSE(ToolSounds::Parse("1, 2, x3, 4, 5", &sounds, &error));
  CHECK_FALSE(ToolSounds::Parse("1, 2, 3, 4, 0x10000", &sounds, &error));
}
