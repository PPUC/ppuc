#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// The machine profile GameCore runs on: plain numbers and plain structs, no
// YAML, no libppuc. GameConfigYaml.cpp builds one of these from the `emGame:`
// block of a game's io-boards.yaml and cross-checks every number against the
// devices libppuc actually parsed; GameCore itself never sees a YAML node.
//
// Everything here is "a number the machine has". Decisions the game makes --
// what a switch scores, how bonus counts down, whether a start press is allowed
// -- are Lua hooks, not fields.

struct TroughConfig
{
  // Switches that are closed when a ball is resting in the trough or outhole.
  // A single-switch outhole and a multi-switch trough are the same model: what
  // matters is how many are closed.
  std::vector<int> switches;

  // Pulsed to put a ball into play. The host only requests the pulse; the board
  // owns how long copper is actually energised via minPulseTime/maxPulseTime.
  int kickCoil = 0;
  uint32_t kickPulseMs = 80;

  // How long the closed-switch count must hold steady before it is believed.
  // An outhole switch chatters, and this is the one place a false positive
  // costs the player a ball.
  uint32_t settleMs = 400;

  // If the count has not dropped this long after a kick, kick again.
  uint32_t kickRetryMs = 1200;
  uint8_t kickRetries = 3;
};

struct ShooterLaneConfig
{
  // Closed while a served ball waits in the shooter lane. Zero means the
  // machine has no such switch, in which case GameCore waits serveSettleMs
  // after a successful serve instead of waiting for the lane to clear.
  int laneSwitch = 0;
  uint32_t serveSettleMs = 500;
  uint32_t launchTimeoutMs = 60000;
};

struct TiltConfig
{
  // The tilt switches themselves live in TiltAssistConfig, not here: warning
  // counting works under both engines, so it cannot be part of the ROM-less
  // game's configuration. What remains here is what happens once the machine
  // has already tilted, which only GameCore does.

  // A switch number the host owns (declared on a board marked `virtual: true`)
  // that the boards watch to inhibit fast-flip outputs. This is the only lever
  // the host has over board-local flipper firing; see docs/EM_GAMES.md.
  int inhibitSwitch = 0;

  bool giOff = true;
  bool endsBallOnly = true;
  bool slamEndsGame = true;
  bool skipBonus = true;
  bool extraBallSurvivesTilt = false;

  // Safety net only: tilt normally clears when every ball is back in the
  // trough. A ball that never gets there would otherwise strand the machine.
  uint32_t recoverTimeoutMs = 60000;
};

struct ReplayConfig
{
  std::vector<uint64_t> thresholds;
  bool awardCredit = true;
  bool awardExtraBall = false;
};

struct MatchConfig
{
  bool enabled = true;
  bool awardCredit = true;
};

// Friendly names for coils, switches and lamps, so rules can say
// ppuc.game.coil("outhole") and survive a rewire.
struct MachineNames
{
  std::unordered_map<std::string, int> coils;
  std::unordered_map<std::string, int> switches;
  std::unordered_map<std::string, int> lamps;
};

struct GameConfig
{
  bool enabled = false;

  uint8_t ballsPerGame = 3;
  uint8_t maxPlayers = 4;

  // Physical balls in the machine. Tilt clears when this many are in the
  // trough, which is the right condition for a multiball machine and costs no
  // more than counting one.
  uint8_t ballCount = 1;

  // A player may still be added while the current player is on this ball or
  // earlier. 1 is the historical default: add players during ball one.
  uint8_t addPlayerThroughBall = 1;

  bool freePlay = false;
  uint8_t creditsPerCoin = 1;
  uint8_t maxCredits = 99;

  // Start button. Also the add-player button while a game is running.
  int startSwitch = 0;
  std::vector<int> coinSwitches;
  int serviceCreditSwitch = 0;

  // Asserted for the duration of a game when the machine has a real game-on
  // relay. Zero means the boards fake high power on for the whole session,
  // which is the recommended wiring for an EM conversion.
  int gameOnCoil = 0;

  // Pulsed once per replay/match award. Optional.
  int knockerCoil = 0;
  uint32_t knockerPulseMs = 80;

  // Backbox lamps GameCore drives directly. Zero means "not fitted".
  int gameOverLamp = 0;
  int tiltLamp = 0;
  int ballInPlayLamp = 0;
  int shootAgainLamp = 0;
  int matchLamp = 0;
  std::vector<int> playerUpLamps;

  // GI strings to darken on tilt, and the level to restore them to.
  std::vector<int> giStrings{1, 2, 3, 4, 5};
  uint8_t giOnLevel = 8;

  TroughConfig trough;
  ShooterLaneConfig shooterLane;
  TiltConfig tilt;
  ReplayConfig replay;
  MatchConfig match;
  MachineNames names;

  uint32_t bonusTimeoutMs = 30000;
  uint32_t gameOverHoldMs = 6000;
  uint32_t attractPageMs = 4000;

  // Score digits before the display rolls over, matching EM reel behaviour.
  uint8_t scoreDigits = 6;
};
