#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

// Tilt warnings and ball save, for any engine.
//
// Both features sit in the switch path between the boards and whatever is
// running the game, and both work by deciding whether an engine gets to see a
// switch at all:
//
//   * a tilt-bob hit below the allowance is a warning and is hidden from the
//     engine entirely; only the hit that actually tilts is forwarded.
//   * a drain while ball save is armed is hidden from the engine and the ball is
//     kicked back out, so the game never learns the ball was lost.
//
// That is why this is engine-neutral rather than part of GameCore. Under
// `engine: pinmame` it gives an early-electronic ROM warnings and a ball saver it
// was never written to have; under `engine: script` GameCore consumes the same
// decisions. One implementation, one config block, one set of tests.
//
// Depends on nothing: no libppuc, no Lua, no YAML. Clock injected.

enum class BallSaveStart
{
  // The moment the ball leaves the trough. Works on any machine, but a slow
  // plunger eats the timer.
  TroughExit,
  // The ball leaves the shooter lane. The best option where the machine has a
  // lane switch, because the clock starts when the ball is actually in play.
  ShooterLane,
  // The first playfield switch the ball touches. Most generous; a ball that
  // drains straight down an outlane without hitting anything never arms it.
  FirstPlayfieldSwitch,
};

const char* BallSaveStartName(BallSaveStart start);
bool ParseBallSaveStart(const std::string& text, BallSaveStart* pStart);

struct TiltAssistConfig
{
  // Every switch that means "the cabinet was moved": the plumb bob, a ball-roll
  // tilt, a playfield tilt. A machine usually has more than one, and they all
  // feed the same warning count -- which is also why the swing filter and the
  // blanking below are shared rather than per switch. One shove that trips both
  // the bob and the ball-roll is one warning, not two.
  std::vector<int> switches;

  // Slam tilt is separate in kind, not just in severity: never warned, never
  // blanked, and it ends the game rather than the ball. Usually one switch on
  // the coin door, but a list because some machines have more.
  std::vector<int> slamSwitches;

  // Hits allowed before the machine tilts. 2 means the third hit tilts.
  uint8_t warnings = 2;

  // Swing filter: one shove sends the bob through the ring several times, and
  // that is one warning.
  uint32_t debounceMs = 500;

  // After a warning is issued the bob is ignored completely for this long, so a
  // still-swinging plumb cannot burn the remaining warnings. Longer than
  // debounceMs on purpose: this is "let it settle", not "ignore contact bounce".
  uint32_t warningBlankingMs = 2000;

  // Flashed on each warning. Optional.
  int warningLamp = 0;
  uint32_t warningLampMs = 1500;
};

struct BallSaveConfig
{
  bool enabled = false;
  uint32_t durationMs = 8000;
  BallSaveStart startOn = BallSaveStart::ShooterLane;

  // Switches that mean "the ball came home".
  std::vector<int> drainSwitches;
  // Pulsed to put it back in play.
  int kickCoil = 0;
  uint32_t kickPulseMs = 80;

  // Needed by ShooterLane and FirstPlayfieldSwitch respectively. A machine with
  // no lane switch falls back to TroughExit with a startup warning rather than
  // silently never arming.
  int shooterLaneSwitch = 0;
  std::vector<int> playfieldSwitches;

  // Lit while a save is active.
  int lamp = 0;

  // How many times one ball may be saved. 0 means unlimited.
  uint8_t maxSavesPerBall = 1;

  // Save only on these ball numbers. Empty means every ball.
  std::vector<int> onlyOnBalls;
};

class PlayfieldAssist
{
 public:
  using ClockFn = std::function<uint64_t()>;

  enum class EventType
  {
    TiltWarning,
    Tilt,
    SlamTilt,
    BallSaveArmed,
    BallSaveExpired,
    BallSaved,
    TiltWarningsAwarded,
  };

  struct Event
  {
    EventType type = EventType::TiltWarning;
    uint8_t player = 0;
    int64_t value = 0;  // warnings used, or remaining ms, or saves used
    int64_t total = 0;  // warnings remaining
  };

  enum class ActionType
  {
    PulseCoil,
    SetLamp,
  };

  struct Action
  {
    ActionType type = ActionType::PulseCoil;
    int number = 0;
    uint8_t value = 0;
    uint32_t durationMs = 0;
  };

  struct SwitchDecision
  {
    // False means the engine must not see this switch at all: a warning hit, or
    // a drain that was saved.
    bool forwardToEngine = true;
  };

  PlayfieldAssist();

  void SetTiltConfig(const TiltAssistConfig& config);
  void SetBallSaveConfig(const BallSaveConfig& config);
  const TiltAssistConfig& GetTiltConfig() const { return m_tilt; }
  const BallSaveConfig& GetBallSaveConfig() const { return m_ballSave; }
  void SetClock(ClockFn clock);

  // Whose warnings are being counted. Both engines already know this: GameCore
  // owns it, and the PinMAME path decodes it from NVRAM.
  void SetCurrentPlayer(uint8_t player);
  uint8_t GetCurrentPlayer() const { return m_currentPlayer; }

  // Resets per-ball state and arms the save according to startOn.
  void OnBallStart(uint8_t ball);
  void OnBallEnd();
  // Attract, game over, or a tilted ball: neither feature should act.
  void SetPlayActive(bool active);

  // Grants extra warnings to the current player for the rest of the game.
  void AwardTiltWarnings(int count);
  bool IsTiltSwitch(int number) const;
  bool IsSlamSwitch(int number) const;
  uint8_t GetWarningsUsed(uint8_t player) const;
  uint8_t GetWarningsAllowed(uint8_t player) const;
  uint8_t GetWarningsRemaining(uint8_t player) const;

  // Starts or extends a ball save for `durationMs` right now, regardless of the
  // configured start trigger, and regardless of which engine is running. This is
  // what ppuc.ballSave(ms) calls: a rule that just awarded a mode wants to hand
  // out a few seconds of protection with it.
  void GrantBallSave(uint32_t durationMs);
  void CancelBallSave();
  bool IsBallSaveActive() const { return m_ballSaveUntilMs != 0; }
  uint32_t GetBallSaveRemainingMs() const;

  SwitchDecision ProcessSwitch(int number, uint8_t state);
  void Update();

  std::vector<Event> TakeEvents();
  std::vector<Action> TakeActions();

 private:
  struct PlayerWarnings
  {
    uint8_t used = 0;
    uint8_t awarded = 0;
  };

  uint64_t Now() const;
  void Emit(Event event);
  void Act(Action action);
  void ArmBallSave(uint32_t durationMs);
  bool BallSaveAllowedThisBall() const;
  bool IsDrainSwitch(int number) const;
  bool IsPlayfieldSwitch(int number) const;
  PlayerWarnings& Warnings(uint8_t player);

  TiltAssistConfig m_tilt;
  BallSaveConfig m_ballSave;
  ClockFn m_clock;

  uint8_t m_currentPlayer = 0;
  uint8_t m_currentBall = 0;
  bool m_playActive = false;
  bool m_tilted = false;

  std::unordered_map<uint8_t, PlayerWarnings> m_warnings;
  uint64_t m_lastTiltHitMs = 0;
  uint64_t m_tiltBlankedUntilMs = 0;

  uint64_t m_ballSaveUntilMs = 0;
  uint8_t m_savesThisBall = 0;
  bool m_ballSavePending = false;  // waiting for the configured start trigger
  bool m_warningLampOn = false;
  uint64_t m_warningLampUntilMs = 0;

  std::vector<Event> m_events;
  std::vector<Action> m_actions;
};
