#pragma once

// What ppuc-menu offers and how it is steered from the playfield.
//
// One machine, one playfield, several games: the original ROM under PinMAME, a
// ROM-less game written in Lua, a custom ROM build. Each is a game folder of its
// own, the same folder `ppuc-pinmame --game` takes, and ppuc-menu lists them.
//
// Whatever is installed, the cabinet always has two flipper buttons and a start
// button, and they sit on the same board inputs in every game folder -- the
// switch *numbers* may differ between games, the wiring cannot. The menu finds
// those three switches in one game's io-boards.yaml and configures the boards
// with nothing else, so it can be driven without a keyboard.
//
// Depends on nothing but the standard library, so ppuc_tests can cover it
// without libppuc or yaml-cpp. Reading and writing YAML is MenuConfigYaml.

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace menugames
{

// One switch of a game's io-boards.yaml, as much as the menu needs of it.
struct SwitchInfo
{
  uint32_t number = 0;
  uint32_t board = 0;
  uint32_t port = 0;
  uint32_t debounce = 0;
  std::string description;
  bool button = false;
  // A switchMatrix switch rather than a direct input.
  bool matrix = false;
  // Named as fastFlipSwitch by an output: it fires a flipper on the board.
  bool drivesOutput = false;
};

enum class ButtonRole
{
  LeftFlipper,
  RightFlipper,
  Start,
};

const char* ButtonRoleName(ButtonRole role);

// The switch numbers a game's ppuc.ini names for the three buttons, 0 for
// "find it yourself".
struct ButtonOverrides
{
  uint32_t leftFlipper = 0;
  uint32_t rightFlipper = 0;
  uint32_t start = 0;
  // emGame.startSwitch of a ROM-less game: the start button by definition.
  uint32_t emGameStart = 0;
};

struct MenuButtons
{
  SwitchInfo leftFlipper;
  SwitchInfo rightFlipper;
  SwitchInfo start;
};

// Finds the left flipper, right flipper and start buttons among a game's
// switches.
//
// An override wins and must name a switch that exists. Otherwise a flipper
// button is a switch whose description says "flipper" and either "button" or
// that it fires a flipper, and which side it is on; an upper or top flipper's
// button is passed over when the main one is there too, and EOS switches never
// count. The start button is emGame.startSwitch, or the one switch whose
// description says "start". Anything left ambiguous fails with a message naming
// the candidates and the ppuc.ini key that settles it -- a menu that guesses
// wrong starts a game nobody chose.
bool ResolveMenuButtons(const std::vector<SwitchInfo>& switches, const ButtonOverrides& overrides,
                        MenuButtons* pButtons, std::string* pError);

// Whether two games put a button on the same board input.
bool SameInput(const SwitchInfo& a, const SwitchInfo& b);

// A ppuc.ini, by section and key. Later values replace earlier ones; empty
// values are dropped, as ppuc-pinmame does.
using IniFile = std::map<std::string, std::map<std::string, std::string>>;

bool ParseIni(const std::filesystem::path& path, IniFile* pIni, std::string* pError);
std::string IniValue(const IniFile& ini, const std::string& section, const std::string& key,
                     const std::string& fallback = std::string());
uint32_t IniUnsigned(const IniFile& ini, const std::string& section, const std::string& key, uint32_t fallback = 0);
bool IniBool(const IniFile& ini, const std::string& section, const std::string& key, bool fallback);

// A game folder as the menu presents it.
struct GameFolder
{
  std::filesystem::path folder;
  std::filesystem::path configFile;
  std::string title;
  std::string info;
  std::filesystem::path image;
  std::filesystem::path selectedImage;
  // Extra ppuc-pinmame arguments, as one command line.
  std::string arguments;
  int order = 0;
  ButtonOverrides buttons;
  // [Paths] Serial and the [Runtime] keys the menu's own connection honours.
  std::string serial;
  std::string skipBoards;
  uint32_t switchReplyDelayUs = 0;
  // [Translite]: the backbox screen the game draws on, so the menu draws on
  // the same one. 0 for the sizes and -1 otherwise when the game does not say;
  // transliteWindow is 0 or 1 when it does.
  int transliteWidth = 0;
  int transliteHeight = 0;
  int transliteScreen = -1;
  int transliteWindow = -1;
};

// Reads one game folder. Returns false, with pError set, for a folder that is
// not a game: no io-boards.yaml (or [Paths] ConfigFile), or [Menu] Hidden.
//
// [Menu] in the game's ppuc.ini:
//   Title, Info          shown on the card; Title defaults to the folder name
//   Image, SelectedImage relative to the folder; default translite-off.* and
//                        translite-on.*, the translite the game itself shows
//   Arguments            appended to `ppuc-pinmame --game <folder>`
//   Order                sort key, then title; default 0
//   Hidden               leave the folder out
//   LeftFlipperSwitch, RightFlipperSwitch, StartSwitch
//                        switch numbers of the three buttons, when the
//                        descriptions in io-boards.yaml do not say
bool ReadGameFolder(const std::filesystem::path& folder, GameFolder* pGame, std::string* pError);

// Every game folder directly inside gamesDir, sorted by Order and title.
// Subfolders that are not games are skipped and reported in pSkipped.
std::vector<GameFolder> ScanGamesDirectory(const std::filesystem::path& gamesDir, std::vector<std::string>* pSkipped);

}  // namespace menugames
