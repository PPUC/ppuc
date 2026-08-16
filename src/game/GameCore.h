#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "GameConfig.h"
#include "GameEvent.h"

// The ROM-less game: players, ball flow, scoring, tilt, attract.
//
// Depends on nothing -- not libppuc, not DMDUtil, not Lua, not yaml-cpp. Same
// discipline as LuaRulesEngine, for the same reason: this is the piece whose
// behaviour is worth testing exhaustively, and it can only be tested cheaply if
// it has no way to reach hardware.
//
// The contract with the outside world is three calls:
//   OnSwitch(number, state)  - record an edge; does no work
//   Update()                 - advance time, run transitions, produce output
//   TakeEvents()/TakeActions() - drain what Update() produced
//
// Nothing is dispatched from inside OnSwitch or from inside a mutator, so a Lua
// handler calling back into ppuc.game.* can never re-enter a transition.

class GameCore
{
 public:
  using ClockFn = std::function<uint64_t()>;
  // Returns a value in [0, bound). Injectable so match digits are deterministic
  // under test.
  using RandomFn = std::function<uint32_t(uint32_t bound)>;

  struct PlayerRecord
  {
    uint64_t score = 0;
    uint8_t ball = 0;
    uint8_t extraBalls = 0;
    bool tilted = false;
    bool tiltedThroughGame = false;
    bool finished = false;
    size_t replaysAwarded = 0;
    std::unordered_map<std::string, int64_t> vars;
  };

  GameCore();

  void SetConfig(const GameConfig& config);
  const GameConfig& GetConfig() const { return m_config; }
  void SetClock(ClockFn clock);
  void SetRandom(RandomFn random);

  // Enters attract and emits the initial lamp/GI state. Call once after
  // SetConfig.
  void Start();

  // Records a switch edge. Cheap and side-effect free by design.
  void OnSwitch(int number, uint8_t state);
  uint8_t GetSwitchState(int number) const;

  // Advances the machine. Everything happens here.
  void Update();

  std::vector<GameEvent> TakeEvents();
  std::vector<GameAction> TakeActions();

  // ---- Queries (also the backing for ppuc.game.*) ----
  GameState GetState() const { return m_state; }
  bool IsAttract() const { return m_state == GameState::Attract; }
  bool IsInGame() const;
  uint8_t GetPlayerCount() const { return m_playerCount; }
  uint8_t GetCurrentPlayer() const { return m_currentPlayer; }
  uint8_t GetCurrentBall() const;
  uint64_t GetScore(uint8_t player) const;
  uint64_t GetCurrentScore() const { return GetScore(m_currentPlayer); }
  uint8_t GetHighPlayer() const;
  uint64_t GetHighScore() const;
  bool IsTilted(uint8_t player) const;
  uint8_t GetExtraBalls(uint8_t player) const;
  uint16_t GetCredits() const { return m_credits; }
  uint8_t GetBallsInTrough() const { return m_believedTrough; }
  uint8_t GetBallsInPlay() const { return m_ballsInPlay; }
  int GetMatchDigits() const { return m_matchDigits; }
  const PlayerRecord* GetPlayer(uint8_t player) const;

  // ---- Commands (also the backing for ppuc.game.*) ----
  void AddScore(int64_t points, uint8_t player = 0);
  void SetScore(uint64_t points, uint8_t player);
  void AwardExtraBall(uint8_t player = 0);
  void AddCredits(int delta);
  void Knock();
  bool StartGame();
  bool AddPlayer();
  void Tilt();
  void SlamTilt();
  void EndBall();
  void EndGame();
  void ServeBall();
  void BonusDone();

  // Per-player and per-ball variables, for rules that need somewhere to keep
  // mode state without inventing their own storage.
  int64_t GetVar(const std::string& name, uint8_t player = 0) const;
  void SetVar(const std::string& name, int64_t value, uint8_t player = 0);
  int64_t AddVar(const std::string& name, int64_t delta, uint8_t player = 0);
  int64_t GetBallVar(const std::string& name) const;
  void SetBallVar(const std::string& name, int64_t value);
  int64_t AddBallVar(const std::string& name, int64_t delta);

  // Name lookups from emGame.names. Zero when unknown.
  int ResolveCoil(const std::string& name) const;
  int ResolveSwitch(const std::string& name) const;
  int ResolveLamp(const std::string& name) const;

  // Tilt warnings, the swing filter and the bob itself live in PlayfieldAssist,
  // so that a ROM game gets them too. By the time Tilt() is called here the
  // decision has already been made; GameCore owns only the consequences.
  //
  // Set by ScriptEngine to let Lua veto a start or an add-player. Absent means
  // allow: credit policy is a rules decision, but having no rules must not make
  // the machine unplayable.
  using AllowFn = std::function<bool(const char* what, int arg)>;
  void SetAllowCallback(AllowFn allow);

 private:
  struct SwitchEdge
  {
    int number;
    uint8_t state;
  };

  void Emit(GameEvent event);
  void Act(GameAction action);
  void PulseCoil(int number, uint32_t durationMs);
  void SetLamp(int number, uint8_t state);
  void SetGi(uint8_t level);
  bool Allow(const char* what, int arg);

  void ProcessSwitchEdges();
  void UpdateTroughCount();
  void HandleStartPressed();
  void HandleSlamTilt();
  void RunStateMachine();

  void EnterState(GameState state);
  void EnterAttract();
  void BeginGame();
  void BeginBall();
  void ServeKick();
  void FinishBall();
  void ApplyTilt(bool slam);
  void ClearTilt();
  void AdvancePlayer();
  uint8_t BallsOut() const;
  void CheckReplay(uint8_t player);
  void DrawMatch();
  void RefreshBackboxLamps();
  uint64_t Now() const;
  bool ScoringAllowed() const;
  PlayerRecord& CurrentPlayer();

  GameConfig m_config;
  ClockFn m_clock;
  RandomFn m_random;
  AllowFn m_allow;

  GameState m_state = GameState::Attract;
  uint64_t m_stateSinceMs = 0;
  uint64_t m_deadlineMs = 0;   // 0 means no deadline in this state

  std::vector<PlayerRecord> m_players;
  uint8_t m_playerCount = 0;
  uint8_t m_currentPlayer = 0;
  uint16_t m_credits = 0;
  int m_matchDigits = -1;

  std::unordered_map<int, uint8_t> m_switchStates;
  std::deque<SwitchEdge> m_edges;

  // Trough occupancy is a debounced count, never a set of positions: a ball
  // resting between two optos makes position-derived state lie.
  uint8_t m_rawTrough = 0;
  uint8_t m_stableTrough = 0;
  uint8_t m_believedTrough = 0;
  uint64_t m_troughStableSinceMs = 0;
  uint8_t m_ballsInPlay = 0;

  uint8_t m_kickAttempts = 0;
  bool m_tiltInhibitAsserted = false;
  bool m_bonusDone = false;
  bool m_startHandledThisTick = false;
  uint64_t m_attractPageSinceMs = 0;

  std::unordered_map<std::string, int64_t> m_ballVars;

  std::vector<GameEvent> m_events;
  std::vector<GameAction> m_actions;
};
