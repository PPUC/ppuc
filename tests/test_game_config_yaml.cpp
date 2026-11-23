// Tests for the emGame cross-checks in GameConfigYaml.cpp: a role that names a
// device the machine does not have fails at startup.

#include <filesystem>
#include <fstream>
#include <string>

#include "doctest.h"
#include "game/GameConfigYaml.h"

namespace fs = std::filesystem;

namespace
{

// A WPC machine with no ROM: trough, start button and ball release, and the
// game-on solenoid 31, which has no pwmOutput because it is the boards' own
// high-power gate rather than a winding.
std::string Machine(const std::string& gameOnCoil, const std::string& gameOnSolenoid = "31")
{
  return "gameOnSolenoid: " + gameOnSolenoid + "\n" +
         "switches:\n"
         "  - {number: 13, board: 1, port: 1}\n"
         "  - {number: 32, board: 1, port: 2}\n"
         "pwmOutput:\n"
         "  - {number: 1, board: 1, port: 3, type: coil}\n"
         "emGame:\n"
         "  enabled: true\n"
         "  ballsPerGame: 3\n"
         "  ballCount: 1\n"
         "  maxPlayers: 1\n"
         "  startSwitch: 13\n"
         "  gameOnCoil: " +
         gameOnCoil +
         "\n"
         "  trough: {switches: [32], kickCoil: 1}\n";
}

bool Load(const std::string& yaml, GameConfig* pConfig, std::string* pError)
{
  const fs::path path = fs::temp_directory_path() / "ppuc_test_game_config.yaml";
  {
    std::ofstream out(path);
    out << yaml;
  }
  const bool ok = LoadGameConfigFromYaml(path.string(), pConfig, pError);
  fs::remove(path);
  return ok;
}

}  // namespace

TEST_CASE("emGame.gameOnCoil may be the machine's game-on solenoid without a pwmOutput")
{
  GameConfig config;
  std::string error;
  REQUIRE_MESSAGE(Load(Machine("31"), &config, &error), error);
  CHECK(config.enabled);
  CHECK(config.gameOnCoil == 31);
}

TEST_CASE("emGame.gameOnCoil still fails for a coil the machine does not have")
{
  GameConfig config;
  std::string error;
  CHECK_FALSE(Load(Machine("30"), &config, &error));
  CHECK(error.find("emGame.gameOnCoil references coil 30") != std::string::npos);
}

TEST_CASE("a game-on solenoid of 0 declares no coil 0")
{
  GameConfig config;
  std::string error;
  REQUIRE_MESSAGE(Load(Machine("0", "0"), &config, &error), error);
  CHECK(config.gameOnCoil == 0);
}
