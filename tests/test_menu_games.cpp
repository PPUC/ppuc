// Tests for how ppuc-menu finds its games and the three playfield buttons that
// steer it.
//
// The descriptions below are the ones real game folders use. A wrong guess here
// is a menu that starts a game when somebody pressed a flipper, so ambiguity
// must fail and say which ppuc.ini key settles it.

#include <chrono>
#include <filesystem>
#include <fstream>

#include "MenuGames.h"
#include "doctest.h"

namespace fs = std::filesystem;
using menugames::ButtonOverrides;
using menugames::MenuButtons;
using menugames::ResolveMenuButtons;
using menugames::SwitchInfo;

namespace
{

SwitchInfo Switch(uint32_t number, uint32_t board, uint32_t port, const char* description, bool button = false,
                  bool drivesOutput = false)
{
  SwitchInfo sw;
  sw.number = number;
  sw.board = board;
  sw.port = port;
  sw.debounce = 10;
  sw.description = description;
  sw.button = button;
  sw.drivesOutput = drivesOutput;
  return sw;
}

// Time Warp as exported by the config tool: no button flags at all.
std::vector<SwitchInfo> TimeWarp()
{
  return {
      Switch(1, 1, 3, "Bomb Tilt"),
      Switch(3, 1, 5, "Credit (Start) Button"),
      Switch(4, 1, 6, "Right Coin Switch"),
      Switch(9, 4, 17, "OUTHOLE"),
      Switch(203, 4, 3, "LEFT FLIPPER BUTTON", false, true),
      Switch(204, 2, 3, "RIGHT FLIPPER BUTTON", false, true),
  };
}

struct TempDir
{
  fs::path path;
  TempDir()
  {
    path = fs::temp_directory_path() /
           ("ppuc_menu_games_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(path);
  }
  ~TempDir()
  {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

void WriteFile(const fs::path& path, const std::string& content)
{
  fs::create_directories(path.parent_path());
  std::ofstream(path) << content;
}

}  // namespace

TEST_CASE("the buttons are found by their descriptions")
{
  MenuButtons buttons;
  std::string error;
  REQUIRE(ResolveMenuButtons(TimeWarp(), {}, &buttons, &error));
  CHECK(buttons.leftFlipper.number == 203);
  CHECK(buttons.rightFlipper.number == 204);
  CHECK(buttons.start.number == 3);
}

TEST_CASE("an upper flipper's button is passed over for the main one")
{
  // Flash: the upper right flipper has a button input of its own.
  std::vector<SwitchInfo> switches = {
      Switch(3, 1, 5, "Credit (Start) Button", true),
      Switch(201, 2, 3, "Left Flipper Button", true, true),
      Switch(202, 3, 3, "Right Upper Flipper Button", true, true),
      Switch(203, 4, 3, "Right Flipper Button", true, true),
  };
  MenuButtons buttons;
  std::string error;
  REQUIRE(ResolveMenuButtons(switches, {}, &buttons, &error));
  CHECK(buttons.rightFlipper.number == 203);
}

TEST_CASE("EOS switches are never buttons")
{
  std::vector<SwitchInfo> switches = TimeWarp();
  switches.push_back(Switch(220, 4, 4, "Left Flipper EOS"));
  switches.push_back(Switch(221, 2, 4, "Right Flipper End of Stroke"));
  MenuButtons buttons;
  std::string error;
  REQUIRE(ResolveMenuButtons(switches, {}, &buttons, &error));
  CHECK(buttons.leftFlipper.number == 203);
  CHECK(buttons.rightFlipper.number == 204);
}

TEST_CASE("a flipper switch counts without the word button when it fires a flipper")
{
  std::vector<SwitchInfo> switches = {
      Switch(1, 1, 3, "Start"),
      Switch(10, 1, 4, "Left Flipper", false, true),
      Switch(11, 1, 5, "Right Flipper", false, true),
  };
  MenuButtons buttons;
  std::string error;
  REQUIRE(ResolveMenuButtons(switches, {}, &buttons, &error));
  CHECK(buttons.leftFlipper.number == 10);
  CHECK(buttons.rightFlipper.number == 11);
}

TEST_CASE("ambiguity fails and names the ppuc.ini key")
{
  std::vector<SwitchInfo> switches = TimeWarp();
  switches.push_back(Switch(30, 1, 9, "Tournament Start"));
  MenuButtons buttons;
  std::string error;
  CHECK_FALSE(ResolveMenuButtons(switches, {}, &buttons, &error));
  CHECK(error.find("StartSwitch") != std::string::npos);
  CHECK(error.find("Tournament Start") != std::string::npos);

  ButtonOverrides overrides;
  overrides.start = 3;
  REQUIRE(ResolveMenuButtons(switches, overrides, &buttons, &error));
  CHECK(buttons.start.number == 3);
}

TEST_CASE("a missing button fails and names the ppuc.ini key")
{
  std::vector<SwitchInfo> switches = {Switch(3, 1, 5, "Credit (Start) Button"),
                                      Switch(203, 4, 3, "LEFT FLIPPER BUTTON")};
  MenuButtons buttons;
  std::string error;
  CHECK_FALSE(ResolveMenuButtons(switches, {}, &buttons, &error));
  CHECK(error.find("RightFlipperSwitch") != std::string::npos);
}

TEST_CASE("an override must name an existing switch")
{
  ButtonOverrides overrides;
  overrides.leftFlipper = 99;
  MenuButtons buttons;
  std::string error;
  CHECK_FALSE(ResolveMenuButtons(TimeWarp(), overrides, &buttons, &error));
  CHECK(error.find("LeftFlipperSwitch = 99") != std::string::npos);
}

TEST_CASE("emGame.startSwitch is the start button of a Lua game")
{
  std::vector<SwitchInfo> switches = {
      Switch(1, 1, 3, "Coin door button"),
      Switch(13, 1, 12, "Left Flipper Button", true, true),
      Switch(14, 1, 13, "Right Flipper Button", true, true),
  };
  ButtonOverrides overrides;
  overrides.emGameStart = 1;
  MenuButtons buttons;
  std::string error;
  REQUIRE(ResolveMenuButtons(switches, overrides, &buttons, &error));
  CHECK(buttons.start.number == 1);
}

TEST_CASE("the three buttons must be three inputs")
{
  ButtonOverrides overrides;
  overrides.start = 203;
  MenuButtons buttons;
  std::string error;
  CHECK_FALSE(ResolveMenuButtons(TimeWarp(), overrides, &buttons, &error));
  CHECK(error.find("three different inputs") != std::string::npos);
}

TEST_CASE("game folders are read with their [Menu] settings")
{
  TempDir dir;
  WriteFile(dir.path / "tmwrp" / "io-boards.yaml", "rom: dummy\n");
  WriteFile(dir.path / "tmwrp" / "translite-off.png", "");
  WriteFile(dir.path / "tmwrp" / "translite-on.png", "");
  WriteFile(dir.path / "tmwrp" / "ppuc.ini",
            "[Game]\nRom = tmwrp_l2\n[Paths]\nSerial = /dev/ttyUSB0\n"
            "[Translite]\nWidth = 1280\nHeight = 1024\nScreen = 1\nWindow = true\n");

  WriteFile(dir.path / "tmwrp-lua" / "io-boards.yaml", "rom: tmwrp\n");
  WriteFile(dir.path / "tmwrp-lua" / "ppuc.ini",
            "[Game]\nEngine = script\n[Menu]\nTitle = Time Warp Deluxe\nOrder = -1\n"
            "Arguments = --no-sound\nStartSwitch = 3\n");

  WriteFile(dir.path / "bench" / "io-boards.yaml", "rom: dummy\n");
  WriteFile(dir.path / "bench" / "ppuc.ini", "[Menu]\nHidden = true\n");

  // Not a game: nothing to configure the boards with.
  fs::create_directories(dir.path / "assets");
  fs::create_directories(dir.path / ".git");

  std::vector<std::string> skipped;
  const std::vector<menugames::GameFolder> games = menugames::ScanGamesDirectory(dir.path, &skipped);
  REQUIRE(games.size() == 2);
  CHECK(skipped.size() == 2);

  // Order first, then title.
  CHECK(games[0].title == "Time Warp Deluxe");
  CHECK(games[0].info == "Lua game");
  CHECK(games[0].arguments == "--no-sound");
  CHECK(games[0].buttons.start == 3);
  CHECK(games[0].image.empty());

  CHECK(games[1].title == "tmwrp");
  CHECK(games[1].info == "ROM tmwrp_l2");
  CHECK(games[1].serial == "/dev/ttyUSB0");
  CHECK(games[1].image.filename() == "translite-off.png");
  CHECK(games[1].selectedImage.filename() == "translite-on.png");
  CHECK(games[1].configFile.filename() == "io-boards.yaml");
  // The menu draws where the game draws its translite.
  CHECK(games[1].transliteWidth == 1280);
  CHECK(games[1].transliteHeight == 1024);
  CHECK(games[1].transliteScreen == 1);
  CHECK(games[1].transliteWindow == 1);
  CHECK(games[0].transliteScreen == -1);
  CHECK(games[0].transliteWindow == -1);
}
