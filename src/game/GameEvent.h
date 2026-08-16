#pragma once

#include <cstdint>
#include <string>

// What GameCore tells the world, and what it asks the world to do.
//
// Both are plain values collected into vectors rather than callbacks fired
// inline. That is deliberate: LuaRulesEngine holds a non-recursive mutex across
// every handler dispatch, so a synchronous GameCore callback that reached Lua
// could come straight back in through ppuc.game.* and deadlock. Queueing also
// makes ordering deterministic, which is what makes the state machine testable.

enum class GameEventType
{
  AttractStart,
  AttractEnd,
  GameStart,
  PlayerAdded,
  StartRejected,
  BallStart,
  BallServed,
  BallServeFailed,
  BallStuck,
  BallEnd,
  BonusCount,
  ExtraBall,
  Score,
  TiltWarning,
  Tilt,
  SlamTilt,
  Replay,
  Match,
  GameEnd,
  CreditsChanged,
};

// Why a start-button press did not do what the player expected. Carried as a
// string because it reaches Lua and a Blockly block, where "noCredits" beats 3.
enum class StartRejectReason
{
  NoCredits,
  NotEnoughBalls,
  TooLate,
  TooManyPlayers,
  Tilted,
  ScriptVetoed,
  AlreadyStarting,
};

const char* StartRejectReasonName(StartRejectReason reason);

struct GameEvent
{
  GameEventType type = GameEventType::AttractStart;
  uint8_t player = 0;
  uint8_t ball = 0;
  int64_t value = 0;   // points, threshold, warning count, match digits
  int64_t total = 0;   // running score for Score, remaining warnings for TiltWarning
  StartRejectReason reason = StartRejectReason::NoCredits;
};

enum class GameActionType
{
  PulseCoil,
  SetCoil,
  SetLamp,
  SetGi,
  SetTiltInhibit,
  BallSearch,
};

struct GameAction
{
  GameActionType type = GameActionType::PulseCoil;
  int number = 0;      // coil, lamp or GI string number
  uint8_t value = 0;   // on/off, or GI level
  uint32_t durationMs = 0;
};

// The states a machine moves through in one game. Named rather than numbered
// because the name reaches Lua through ppuc.game.state().
enum class GameState
{
  Attract,
  GameStarting,
  BallStarting,
  BallInLane,
  BallInPlay,
  BallDraining,
  BallEnding,
  BonusCount,
  GameEnding,
  GameOver,
};

const char* GameStateName(GameState state);
