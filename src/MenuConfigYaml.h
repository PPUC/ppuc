#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "MenuGames.h"

// The YAML side of ppuc-menu's board connection, kept apart from MenuGames so
// the selection logic stays testable without yaml-cpp.

// Every switch of a game's io-boards.yaml -- direct inputs and switchMatrix
// switches -- with whether an output fires on it, plus emGame.startSwitch
// (0 when the game has none).
bool LoadMenuSwitches(const std::string& configPath, std::vector<menugames::SwitchInfo>* pSwitches,
                      uint32_t* pEmGameStart, std::string* pError);

// Writes the configuration ppuc-menu connects with: the platform, serial port
// and game-on solenoid of the source game, the boards the three buttons sit
// on, and those three switches. Nothing else -- no coil, lamp, LED string or
// flipper -- so nothing on the playfield moves or lights while the menu runs,
// and high power stays off: the game-on solenoid is never switched.
bool WriteMenuConfig(const std::string& sourcePath, const std::string& targetPath,
                     const menugames::MenuButtons& buttons, std::string* pError);
