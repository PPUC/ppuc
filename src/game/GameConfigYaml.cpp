#include "GameConfigYaml.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <exception>

namespace
{
std::string Where(const YAML::Node& node)
{
  const YAML::Mark mark = node.Mark();
  if (mark.line < 0)
  {
    return "";
  }
  return " at line " + std::to_string(mark.line + 1) + ", column " + std::to_string(mark.column + 1);
}

class ConfigError : public std::exception
{
 public:
  explicit ConfigError(std::string message) : m_message(std::move(message)) {}
  const char* what() const noexcept override { return m_message.c_str(); }

 private:
  std::string m_message;
};

[[noreturn]] void Fail(const std::string& message) { throw ConfigError("invalid emGame configuration: " + message); }

template <typename T>
T Read(const YAML::Node& parent, const char* key, T fallback)
{
  const YAML::Node node = parent[key];
  if (!node || node.IsNull())
  {
    return fallback;
  }
  try
  {
    return node.as<T>();
  }
  catch (const std::exception&)
  {
    Fail(std::string("emGame.") + key + " is not a " + typeid(T).name() + Where(node));
  }
}

std::vector<int> ReadIntList(const YAML::Node& parent, const char* key)
{
  std::vector<int> values;
  const YAML::Node node = parent[key];
  if (!node || node.IsNull())
  {
    return values;
  }

  // A single scalar is accepted where a list is expected: a one-switch outhole
  // and a three-switch trough are the same concept, and making the common case
  // write `switches: 11` rather than `switches: [11]` removes a foot-gun.
  if (node.IsScalar())
  {
    values.push_back(node.as<int>());
    return values;
  }
  if (!node.IsSequence())
  {
    Fail(std::string(key) + " must be a number or a list of numbers" + Where(node));
  }
  for (const YAML::Node& entry : node)
  {
    values.push_back(entry.as<int>());
  }
  return values;
}

// Collects every device number the machine declares, from the same document
// the emGame block lives in.
//
// Deliberately not taken from PPUC::GetCoils()/GetSwitches()/GetLamps(): those
// are only populated during Connect(), and the tilt inhibit switch has to reach
// libppuc BEFORE Connect() so it can be sent as a config topic. Reading the YAML
// directly removes that ordering coupling, and it validates against exactly the
// same source of truth.
MachineDevices CollectDeclaredDevices(const YAML::Node& root)
{
  MachineDevices devices;

  if (const YAML::Node node = root["switches"]; node && node.IsSequence())
  {
    for (const YAML::Node& item : node)
    {
      if (item["number"]) devices.switches.push_back(item["number"].as<int>());
    }
  }

  if (const YAML::Node matrix = root["switchMatrix"]; matrix && matrix.IsMap())
  {
    if (const YAML::Node node = matrix["switches"]; node && node.IsSequence())
    {
      for (const YAML::Node& item : node)
      {
        if (item["number"]) devices.switches.push_back(item["number"].as<int>());
      }
    }
  }

  if (const YAML::Node node = root["pwmOutput"]; node && node.IsSequence())
  {
    for (const YAML::Node& item : node)
    {
      if (!item["number"]) continue;
      const int number = item["number"].as<int>();
      const std::string type = item["type"] ? item["type"].as<std::string>() : "coil";
      if (type == "lamp")
      {
        devices.lamps.push_back(number);
      }
      else
      {
        // coil, flasher, motor, shaker: all driven as solenoids.
        devices.coils.push_back(number);
      }
    }
  }

  // Addressable LED lamps count as lamps too, so a machine whose backbox is a
  // WS2812 strip can still name them.
  if (const YAML::Node stripes = root["ledStripes"]; stripes && stripes.IsSequence())
  {
    for (const YAML::Node& stripe : stripes)
    {
      for (const char* key : {"lamps", "flashers", "gi"})
      {
        if (const YAML::Node node = stripe[key]; node && node.IsSequence())
        {
          for (const YAML::Node& item : node)
          {
            if (item["number"]) devices.lamps.push_back(item["number"].as<int>());
          }
        }
      }
    }
  }

  return devices;
}

bool Declared(const std::vector<int>& declared, int number)
{
  return std::find(declared.begin(), declared.end(), number) != declared.end();
}

void RequireDevice(const std::vector<int>& declared, int number, const char* kind, const char* what)
{
  if (number == 0)
  {
    return;  // 0 always means "not fitted"
  }
  if (!Declared(declared, number))
  {
    Fail(std::string(what) + " references " + kind + " " + std::to_string(number) +
         ", which is not declared in this machine's configuration");
  }
}

void ReadNames(const YAML::Node& parent, const char* key, const std::vector<int>& declared, const char* kind,
               std::unordered_map<std::string, int>* pOut)
{
  const YAML::Node node = parent[key];
  if (!node || node.IsNull())
  {
    return;
  }
  if (!node.IsMap())
  {
    Fail(std::string("emGame.names.") + key + " must be a map of name to number" + Where(node));
  }

  for (const auto& entry : node)
  {
    const std::string name = entry.first.as<std::string>();
    const int number = entry.second.as<int>();
    RequireDevice(declared, number, kind, ("emGame.names." + std::string(key) + "." + name).c_str());
    (*pOut)[name] = number;
  }
}
}  // namespace

bool LoadGameConfigFromYaml(const std::string& path, GameConfig* pConfig, std::string* pError)
{
  if (pConfig == nullptr)
  {
    if (pError) *pError = "output config missing";
    return false;
  }

  *pConfig = GameConfig{};

  YAML::Node root;
  try
  {
    root = YAML::LoadFile(path);
  }
  catch (const std::exception& ex)
  {
    if (pError) *pError = std::string("failed to parse ") + path + ": " + ex.what();
    return false;
  }

  const MachineDevices devices = CollectDeclaredDevices(root);

  const YAML::Node em = root["emGame"];
  if (!em || em.IsNull())
  {
    return true;  // a ROM game; nothing to do
  }
  if (!em.IsMap())
  {
    if (pError) *pError = "invalid emGame configuration: emGame must be a map" + Where(em);
    return false;
  }

  try
  {
    GameConfig config;
    config.enabled = Read<bool>(em, "enabled", true);

    config.ballsPerGame = static_cast<uint8_t>(Read<int>(em, "ballsPerGame", 3));
    config.maxPlayers = static_cast<uint8_t>(Read<int>(em, "maxPlayers", 4));
    config.ballCount = static_cast<uint8_t>(Read<int>(em, "ballCount", 1));
    config.addPlayerThroughBall = static_cast<uint8_t>(Read<int>(em, "addPlayerThroughBall", 1));
    config.freePlay = Read<bool>(em, "freePlay", false);
    config.creditsPerCoin = static_cast<uint8_t>(Read<int>(em, "creditsPerCoin", 1));
    config.maxCredits = static_cast<uint8_t>(Read<int>(em, "maxCredits", 99));
    config.scoreDigits = static_cast<uint8_t>(Read<int>(em, "scoreDigits", 6));
    config.bonusTimeoutMs = static_cast<uint32_t>(Read<int>(em, "bonusTimeoutMs", 30000));
    config.gameOverHoldMs = static_cast<uint32_t>(Read<int>(em, "gameOverHoldMs", 6000));
    config.attractPageMs = static_cast<uint32_t>(Read<int>(em, "attractPageMs", 4000));

    if (config.ballsPerGame == 0) Fail("emGame.ballsPerGame must be at least 1");
    if (config.maxPlayers == 0) Fail("emGame.maxPlayers must be at least 1");
    if (config.ballCount == 0) Fail("emGame.ballCount must be at least 1");

    config.startSwitch = Read<int>(em, "startSwitch", 0);
    config.serviceCreditSwitch = Read<int>(em, "serviceCreditSwitch", 0);
    config.coinSwitches = ReadIntList(em, "coinSwitches");
    config.gameOnCoil = Read<int>(em, "gameOnCoil", 0);
    config.knockerCoil = Read<int>(em, "knockerCoil", 0);
    config.knockerPulseMs = static_cast<uint32_t>(Read<int>(em, "knockerPulseMs", 80));

    config.gameOverLamp = Read<int>(em, "gameOverLamp", 0);
    config.tiltLamp = Read<int>(em, "tiltLamp", 0);
    config.ballInPlayLamp = Read<int>(em, "ballInPlayLamp", 0);
    config.shootAgainLamp = Read<int>(em, "shootAgainLamp", 0);
    config.matchLamp = Read<int>(em, "matchLamp", 0);
    config.playerUpLamps = ReadIntList(em, "playerUpLamps");

    if (const YAML::Node gi = em["giStrings"]; gi && !gi.IsNull())
    {
      config.giStrings = ReadIntList(em, "giStrings");
    }
    config.giOnLevel = static_cast<uint8_t>(Read<int>(em, "giOnLevel", 8));
    if (config.giOnLevel > 8) Fail("emGame.giOnLevel must be between 0 and 8");

    if (const YAML::Node trough = em["trough"]; trough && trough.IsMap())
    {
      config.trough.switches = ReadIntList(trough, "switches");
      config.trough.kickCoil = Read<int>(trough, "kickCoil", 0);
      config.trough.kickPulseMs = static_cast<uint32_t>(Read<int>(trough, "kickPulseMs", 80));
      config.trough.settleMs = static_cast<uint32_t>(Read<int>(trough, "settleMs", 400));
      config.trough.kickRetryMs = static_cast<uint32_t>(Read<int>(trough, "kickRetryMs", 1200));
      config.trough.kickRetries = static_cast<uint8_t>(Read<int>(trough, "kickRetries", 3));
    }

    if (const YAML::Node lane = em["shooterLane"]; lane && lane.IsMap())
    {
      config.shooterLane.laneSwitch = Read<int>(lane, "switch", 0);
      config.shooterLane.serveSettleMs = static_cast<uint32_t>(Read<int>(lane, "serveSettleMs", 500));
      config.shooterLane.launchTimeoutMs = static_cast<uint32_t>(Read<int>(lane, "launchTimeoutMs", 60000));
    }

    if (const YAML::Node tilt = em["tilt"]; tilt && tilt.IsMap())
    {
      config.tilt.inhibitSwitch = Read<int>(tilt, "inhibitSwitch", 0);
      config.tilt.giOff = Read<bool>(tilt, "giOff", true);
      config.tilt.endsBallOnly = Read<bool>(tilt, "endsBallOnly", true);
      config.tilt.slamEndsGame = Read<bool>(tilt, "slamEndsGame", true);
      config.tilt.skipBonus = Read<bool>(tilt, "skipBonus", true);
      config.tilt.extraBallSurvivesTilt = Read<bool>(tilt, "extraBallSurvivesTilt", false);
      config.tilt.recoverTimeoutMs = static_cast<uint32_t>(Read<int>(tilt, "recoverTimeoutMs", 60000));
    }

    if (const YAML::Node replay = em["replay"]; replay && replay.IsMap())
    {
      for (int threshold : ReadIntList(replay, "thresholds"))
      {
        config.replay.thresholds.push_back(static_cast<uint64_t>(threshold));
      }
      // Sorted so that crossing several at once awards them in order, and so a
      // config that lists them out of order still behaves.
      std::sort(config.replay.thresholds.begin(), config.replay.thresholds.end());
      config.replay.awardCredit = Read<bool>(replay, "awardCredit", true);
      config.replay.awardExtraBall = Read<bool>(replay, "awardExtraBall", false);
    }

    if (const YAML::Node match = em["match"]; match && match.IsMap())
    {
      config.match.enabled = Read<bool>(match, "enabled", true);
      config.match.awardCredit = Read<bool>(match, "awardCredit", true);
    }

    if (const YAML::Node names = em["names"]; names && names.IsMap())
    {
      ReadNames(names, "coils", devices.coils, "coil", &config.names.coils);
      ReadNames(names, "switches", devices.switches, "switch", &config.names.switches);
      ReadNames(names, "lamps", devices.lamps, "lamp", &config.names.lamps);
    }

    // ---- Cross-reference every number against the declared machine ----
    RequireDevice(devices.switches, config.startSwitch, "switch", "emGame.startSwitch");
    RequireDevice(devices.switches, config.serviceCreditSwitch, "switch", "emGame.serviceCreditSwitch");
    for (int number : config.coinSwitches)
    {
      RequireDevice(devices.switches, number, "switch", "emGame.coinSwitches");
    }
    for (int number : config.trough.switches)
    {
      RequireDevice(devices.switches, number, "switch", "emGame.trough.switches");
    }
    RequireDevice(devices.switches, config.shooterLane.laneSwitch, "switch", "emGame.shooterLane.switch");

    RequireDevice(devices.coils, config.trough.kickCoil, "coil", "emGame.trough.kickCoil");
    RequireDevice(devices.coils, config.knockerCoil, "coil", "emGame.knockerCoil");
    RequireDevice(devices.coils, config.gameOnCoil, "coil", "emGame.gameOnCoil");

    RequireDevice(devices.lamps, config.gameOverLamp, "lamp", "emGame.gameOverLamp");
    RequireDevice(devices.lamps, config.tiltLamp, "lamp", "emGame.tiltLamp");
    RequireDevice(devices.lamps, config.ballInPlayLamp, "lamp", "emGame.ballInPlayLamp");
    RequireDevice(devices.lamps, config.shootAgainLamp, "lamp", "emGame.shootAgainLamp");
    RequireDevice(devices.lamps, config.matchLamp, "lamp", "emGame.matchLamp");
    for (int number : config.playerUpLamps)
    {
      RequireDevice(devices.lamps, number, "lamp", "emGame.playerUpLamps");
    }

    // The tilt inhibit switch is deliberately NOT cross-referenced: it is a
    // host-owned number on a board declared `virtual: true`, so it may well not
    // appear among the physically wired switches.

    if (config.enabled)
    {
      // A machine that cannot start a game or serve a ball is a configuration
      // error, not a runtime surprise to discover in a basement.
      if (config.startSwitch == 0)
      {
        Fail("emGame.startSwitch is required when emGame is enabled");
      }
      if (config.trough.switches.empty())
      {
        Fail("emGame.trough.switches is required when emGame is enabled");
      }
      if (config.trough.kickCoil == 0)
      {
        Fail("emGame.trough.kickCoil is required when emGame is enabled");
      }
    }

    *pConfig = config;
    return true;
  }
  catch (const std::exception& ex)
  {
    if (pError) *pError = ex.what();
    return false;
  }
}

bool LoadPlayfieldAssistFromYaml(const std::string& path, TiltAssistConfig* pTilt, BallSaveConfig* pBallSave,
                                 std::string* pError)
{
  if (pTilt == nullptr || pBallSave == nullptr)
  {
    if (pError) *pError = "output config missing";
    return false;
  }

  *pTilt = TiltAssistConfig{};
  *pBallSave = BallSaveConfig{};

  YAML::Node root;
  try
  {
    root = YAML::LoadFile(path);
  }
  catch (const std::exception& ex)
  {
    if (pError) *pError = std::string("failed to parse ") + path + ": " + ex.what();
    return false;
  }

  const MachineDevices devices = CollectDeclaredDevices(root);

  try
  {
    if (const YAML::Node tilt = root["tilt"]; tilt && tilt.IsMap())
    {
      // A machine usually has more than one tilt switch: a plumb bob, a
      // ball-roll tilt, sometimes a playfield tilt. They all feed the same
      // warning count. A bare number is accepted where a list is expected.
      pTilt->switches = ReadIntList(tilt, "switches");
      pTilt->slamSwitches = ReadIntList(tilt, "slamSwitches");
      pTilt->warnings = static_cast<uint8_t>(Read<int>(tilt, "warnings", 2));
      pTilt->debounceMs = static_cast<uint32_t>(Read<int>(tilt, "debounceMs", 500));
      pTilt->warningBlankingMs = static_cast<uint32_t>(Read<int>(tilt, "warningBlankingMs", 2000));
      pTilt->warningLamp = Read<int>(tilt, "warningLamp", 0);
      pTilt->warningLampMs = static_cast<uint32_t>(Read<int>(tilt, "warningLampMs", 1500));

      for (int number : pTilt->switches)
      {
        RequireDevice(devices.switches, number, "switch", "tilt.switches");
      }
      for (int number : pTilt->slamSwitches)
      {
        RequireDevice(devices.switches, number, "switch", "tilt.slamSwitches");
      }
      RequireDevice(devices.lamps, pTilt->warningLamp, "lamp", "tilt.warningLamp");

      // A tilt switch that is also a slam switch would both warn and end the
      // game on the same closure, and the machine's behaviour would depend on
      // evaluation order.
      for (int number : pTilt->switches)
      {
        if (std::find(pTilt->slamSwitches.begin(), pTilt->slamSwitches.end(), number) != pTilt->slamSwitches.end())
        {
          Fail("switch " + std::to_string(number) + " is listed in both tilt.switches and tilt.slamSwitches");
        }
      }
    }

    if (const YAML::Node save = root["ballSave"]; save && save.IsMap())
    {
      pBallSave->enabled = Read<bool>(save, "enabled", true);
      pBallSave->durationMs = static_cast<uint32_t>(Read<int>(save, "seconds", 8)) * 1000;
      if (save["durationMs"])
      {
        pBallSave->durationMs = static_cast<uint32_t>(Read<int>(save, "durationMs", 8000));
      }

      const std::string startOn = save["startOn"] ? save["startOn"].as<std::string>() : "shooterLane";
      if (!ParseBallSaveStart(startOn, &pBallSave->startOn))
      {
        Fail("ballSave.startOn must be troughExit, shooterLane or firstPlayfieldSwitch, not '" + startOn + "'");
      }

      pBallSave->drainSwitches = ReadIntList(save, "drainSwitches");
      pBallSave->kickCoil = Read<int>(save, "kickCoil", 0);
      pBallSave->kickPulseMs = static_cast<uint32_t>(Read<int>(save, "kickPulseMs", 80));
      pBallSave->shooterLaneSwitch = Read<int>(save, "shooterLaneSwitch", 0);
      pBallSave->playfieldSwitches = ReadIntList(save, "playfieldSwitches");
      pBallSave->lamp = Read<int>(save, "lamp", 0);
      pBallSave->maxSavesPerBall = static_cast<uint8_t>(Read<int>(save, "maxSavesPerBall", 1));
      pBallSave->onlyOnBalls = ReadIntList(save, "onlyOnBalls");

      for (int number : pBallSave->drainSwitches)
      {
        RequireDevice(devices.switches, number, "switch", "ballSave.drainSwitches");
      }
      for (int number : pBallSave->playfieldSwitches)
      {
        RequireDevice(devices.switches, number, "switch", "ballSave.playfieldSwitches");
      }
      RequireDevice(devices.switches, pBallSave->shooterLaneSwitch, "switch", "ballSave.shooterLaneSwitch");
      RequireDevice(devices.coils, pBallSave->kickCoil, "coil", "ballSave.kickCoil");
      RequireDevice(devices.lamps, pBallSave->lamp, "lamp", "ballSave.lamp");

      if (pBallSave->enabled)
      {
        // Without somewhere to detect the drain and something to kick the ball
        // back with, the feature cannot work at all. Say so at startup.
        if (pBallSave->drainSwitches.empty())
        {
          Fail("ballSave.drainSwitches is required when ballSave is enabled");
        }
        if (pBallSave->kickCoil == 0)
        {
          Fail("ballSave.kickCoil is required when ballSave is enabled");
        }
      }
    }

    return true;
  }
  catch (const std::exception& ex)
  {
    if (pError) *pError = ex.what();
    return false;
  }
}
