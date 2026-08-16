#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "game/GameCore.h"

// Test harness for GameCore, mirroring RulesFixture.h: an injected clock so
// timing is exact rather than slept for, and recorded outputs so a test asserts
// on what the machine did rather than on what it printed.
//
// The trough helpers matter more than they look. Without ServeCompletes() and
// Drain(), every test hand-rolls the same switch-and-settle dance and stops
// being readable as machine behaviour.

class GameHarness
{
 public:
  GameHarness()
  {
    m_core.SetClock([this]() { return m_nowMs; });
    // Fixed match digits so match tests are deterministic.
    m_core.SetRandom([this](uint32_t bound) { return bound == 0 ? 0u : m_randomValue % bound; });
    m_config = DefaultConfig();
  }

  // A single-outhole 3-ball machine with a start button, a coin switch, a tilt
  // bob and a knocker. Deliberately the simplest thing that is still a pinball
  // machine; tests that need more mutate config() before Start().
  static GameConfig DefaultConfig()
  {
    GameConfig config;
    config.enabled = true;
    config.ballsPerGame = 3;
    config.maxPlayers = 4;
    config.ballCount = 1;
    config.addPlayerThroughBall = 1;
    config.freePlay = false;
    config.creditsPerCoin = 1;

    config.startSwitch = 1;
    config.coinSwitches = {2};
    config.gameOnCoil = 10;
    config.knockerCoil = 9;
    config.gameOverLamp = 40;
    config.tiltLamp = 41;
    config.ballInPlayLamp = 42;
    config.shootAgainLamp = 43;

    config.trough.switches = {11};
    config.trough.kickCoil = 1;
    config.trough.kickPulseMs = 80;
    config.trough.settleMs = 400;
    config.trough.kickRetryMs = 1200;
    config.trough.kickRetries = 3;

    // Tilt switches themselves live in PlayfieldAssist; GameCore only owns what
    // happens once the machine has already tilted.
    config.tilt.inhibitSwitch = 250;

    config.match.enabled = false;  // opt in per test; it draws a random number
    return config;
  }

  GameConfig& config() { return m_config; }
  GameCore& core() { return m_core; }

  // Applies the config, seats a ball in the trough and enters attract.
  void Start()
  {
    m_core.SetConfig(m_config);
    for (int number : m_config.trough.switches)
    {
      m_core.OnSwitch(number, 1);
    }
    m_core.Start();
    Settle();
  }

  void Advance(uint64_t ms) { m_nowMs += ms; }

  void Update()
  {
    m_core.Update();
    Collect();
  }

  void AdvanceAndUpdate(uint64_t ms)
  {
    Advance(ms);
    Update();
  }

  // Lets the trough debounce expire and the state machine settle.
  //
  // The first Update() is load-bearing: the trough count is only believed after
  // it has held steady for settleMs, and that window starts when GameCore first
  // *observes* the new count. Advancing time before observing it would leave the
  // window starting at the new "now" and never expiring.
  void Settle(uint64_t ms = 600)
  {
    Update();
    AdvanceAndUpdate(ms);
    Update();
  }

  void Close(int number)
  {
    m_core.OnSwitch(number, 1);
    Update();
  }

  void Open(int number)
  {
    m_core.OnSwitch(number, 0);
    Update();
  }

  void Tap(int number)
  {
    Close(number);
    Open(number);
  }

  void PressStart() { Tap(m_config.startSwitch); }
  void InsertCoin() { Tap(m_config.coinSwitches.front()); }

  // The ball leaves the trough after a kick.
  void ServeCompletes()
  {
    for (int number : m_config.trough.switches)
    {
      m_core.OnSwitch(number, 0);
    }
    Settle();
  }

  // The ball comes home.
  void Drain()
  {
    for (int number : m_config.trough.switches)
    {
      m_core.OnSwitch(number, 1);
    }
    Settle();
    // Let the BallDraining confirmation window expire.
    Settle();
  }

  // Coin (when the machine takes them), start, and get a ball into play.
  // Deliberately skips the coin in free play so credit assertions in a test
  // measure what the game awarded, not what the harness pushed in.
  void StartGameAndServe()
  {
    if (!m_config.freePlay)
    {
      InsertCoin();
    }
    PressStart();
    Update();
    ServeCompletes();
  }

  // Finish the current ball: drain, then let bonus time out.
  void DrainAndFinishBall()
  {
    Drain();
    AdvanceAndUpdate(m_config.bonusTimeoutMs + 10);
    Update();
    ServeCompletes();
  }

  // ---- Assertions ----
  bool Saw(GameEventType type) const
  {
    return std::any_of(events.begin(), events.end(), [type](const GameEvent& e) { return e.type == type; });
  }

  int Count(GameEventType type) const
  {
    return static_cast<int>(
        std::count_if(events.begin(), events.end(), [type](const GameEvent& e) { return e.type == type; }));
  }

  const GameEvent* Last(GameEventType type) const
  {
    for (auto it = events.rbegin(); it != events.rend(); ++it)
    {
      if (it->type == type) return &(*it);
    }
    return nullptr;
  }

  int CountAction(GameActionType type, int number) const
  {
    return static_cast<int>(std::count_if(actions.begin(), actions.end(), [type, number](const GameAction& a)
                                          { return a.type == type && a.number == number; }));
  }

  const GameAction* LastAction(GameActionType type, int number) const
  {
    for (auto it = actions.rbegin(); it != actions.rend(); ++it)
    {
      if (it->type == type && it->number == number) return &(*it);
    }
    return nullptr;
  }

  // The order of (player, ball) pairs BallStart was emitted with. This is the
  // single most useful assertion in the suite: it is the whole ball rotation in
  // one line.
  std::vector<std::pair<int, int>> BallOrder() const
  {
    std::vector<std::pair<int, int>> order;
    for (const GameEvent& e : events)
    {
      if (e.type == GameEventType::BallStart) order.emplace_back(e.player, e.ball);
    }
    return order;
  }

  void ClearRecorded()
  {
    events.clear();
    actions.clear();
  }

  void SetRandomValue(uint32_t value) { m_randomValue = value; }

  std::vector<GameEvent> events;
  std::vector<GameAction> actions;

 private:
  void Collect()
  {
    for (GameEvent& e : m_core.TakeEvents()) events.push_back(e);
    for (GameAction& a : m_core.TakeActions()) actions.push_back(a);
  }

  GameCore m_core;
  GameConfig m_config;
  uint64_t m_nowMs = 10000;
  uint32_t m_randomValue = 0;
};
