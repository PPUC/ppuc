#include "BenchBallsYaml.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>

namespace bench
{

namespace
{

struct BenchError
{
  std::string message;
};

[[noreturn]] void Fail(const YAML::Node& node, const std::string& message)
{
  const YAML::Mark mark = node.Mark();
  if (mark.line >= 0) throw BenchError{"line " + std::to_string(mark.line + 1) + ": bench." + message};
  throw BenchError{"bench." + message};
}

int ReadInt(const YAML::Node& node, const char* key, int fallback)
{
  const YAML::Node value = node[key];
  if (!value) return fallback;
  try
  {
    return value.as<int>();
  }
  catch (const std::exception&)
  {
    Fail(value, std::string(key) + " must be a whole number");
  }
}

// A number or a list of numbers.
std::vector<int> ReadNumbers(const YAML::Node& node, const char* key, std::vector<int> fallback)
{
  const YAML::Node value = node[key];
  if (!value) return fallback;
  std::vector<int> numbers;
  try
  {
    if (value.IsSequence())
    {
      for (const auto& item : value) numbers.push_back(item.as<int>());
    }
    else
    {
      numbers.push_back(value.as<int>());
    }
  }
  catch (const std::exception&)
  {
    Fail(value, std::string(key) + " must be a number or a list of numbers");
  }
  return numbers;
}

}  // namespace

bool LoadConfig(const std::string& path, const GameConfig* game, Config* pConfig, std::string* pError)
{
  Config config;
  const bool fromGame = game != nullptr && game->enabled;
  if (fromGame)
  {
    config.balls = game->ballCount;
    config.troughSwitches = game->trough.switches;
    config.troughKickCoil = game->trough.kickCoil;
    config.shooterSwitch = game->shooterLane.laneSwitch;
    config.startSwitch = game->startSwitch;
  }

  try
  {
    const YAML::Node file = YAML::LoadFile(path);
    const YAML::Node root = file["bench"];
    if (root && !root.IsMap()) Fail(root, "bench must be a map");
    config.present = fromGame || root;
    if (root)
    {
      config.balls = ReadInt(root, "balls", config.balls);
      if (const YAML::Node trough = root["trough"])
      {
        config.troughSwitches = ReadNumbers(trough, "switches", config.troughSwitches);
        config.troughKickCoil = ReadInt(trough, "kickCoil", config.troughKickCoil);
      }
      config.shooterSwitch = ReadInt(root, "shooterLane", config.shooterSwitch);
      config.startSwitch = ReadInt(root, "startSwitch", config.startSwitch);
      config.launchCoils = ReadNumbers(root, "launchCoils", config.launchCoils);
      if (const YAML::Node plunger = root["plunger"])
      {
        config.plungerSwitch = ReadInt(plunger, "switch", 0);
        config.plungerAfterMs =
            static_cast<uint32_t>(ReadInt(plunger, "afterMs", static_cast<int>(config.plungerAfterMs)));
      }
      config.serveMs = static_cast<uint32_t>(ReadInt(root, "serveMs", static_cast<int>(config.serveMs)));
      config.tapMs = static_cast<uint32_t>(ReadInt(root, "tapMs", static_cast<int>(config.tapMs)));
      config.drainSwitches = ReadNumbers(root, "drainSwitches", config.drainSwitches);
      if (const YAML::Node holes = root["holes"])
      {
        if (!holes.IsSequence()) Fail(holes, "holes must be a list");
        for (const auto& node : holes)
        {
          Hole hole;
          hole.sw = ReadInt(node, "switch", 0);
          hole.coil = ReadInt(node, "coil", 0);
          hole.to = ReadInt(node, "to", 0);
          if (hole.sw == 0 || hole.coil == 0) Fail(node, "holes: every hole needs a switch and a coil");
          config.holes.push_back(hole);
        }
        for (const Hole& hole : config.holes)
        {
          if (hole.to == 0) continue;
          const bool known = std::any_of(config.holes.begin(), config.holes.end(),
                                         [&](const Hole& other) { return other.sw == hole.to; });
          if (!known)
          {
            throw BenchError{"bench.holes: hole " + std::to_string(hole.sw) + " feeds " + std::to_string(hole.to) +
                             ", which is not a hole"};
          }
        }
      }
    }
    if (config.present)
    {
      if (config.troughSwitches.empty() || config.troughKickCoil == 0)
      {
        throw BenchError{
            "bench: no trough to serve a ball from; give bench.trough.switches and kickCoil, or an "
            "emGame block"};
      }
      if (config.balls < 1) throw BenchError{"bench.balls must be at least 1"};
    }
  }
  catch (const BenchError& error)
  {
    if (pError) *pError = path + ": " + error.message;
    return false;
  }
  catch (const std::exception& ex)
  {
    if (pError) *pError = path + ": " + ex.what();
    return false;
  }
  *pConfig = std::move(config);
  return true;
}

}  // namespace bench
