// Tests for tilt warnings and ball save.
//
// Both features work by deciding whether an engine gets to see a switch, which
// is what makes them engine-neutral: the same code gives an early-electronic ROM
// warnings and a ball saver it was never written to have, and feeds GameCore the
// same decisions on a ROM-less machine.
//
// The two assertions that matter most are the suppressions. A warning hit that
// reaches the engine tilts a ROM immediately; a saved drain that reaches the
// engine ends the ball. Either one makes the feature worse than not having it.

#include <cstdint>

#include "doctest.h"
#include "game/PlayfieldAssist.h"

namespace
{
constexpr int kBob = 4;
constexpr int kBallRoll = 5;
constexpr int kSlam = 3;
constexpr int kOuthole = 11;
constexpr int kShooterLane = 12;
constexpr int kTarget = 22;
constexpr int kKickCoil = 1;
constexpr int kSaveLamp = 45;
constexpr int kWarningLamp = 46;

class AssistHarness
{
 public:
  AssistHarness()
  {
    m_assist.SetClock([this]() { return m_nowMs; });

    TiltAssistConfig tilt;
    // Two tilt switches, as a real machine has: a plumb bob and a ball-roll
    // tilt. Both feed the same warning count.
    tilt.switches = {kBob, kBallRoll};
    tilt.slamSwitches = {kSlam};
    tilt.warnings = 2;
    tilt.debounceMs = 500;
    tilt.warningBlankingMs = 2000;
    tilt.warningLamp = kWarningLamp;
    m_assist.SetTiltConfig(tilt);

    BallSaveConfig save;
    save.enabled = true;
    save.durationMs = 8000;
    save.startOn = BallSaveStart::ShooterLane;
    save.drainSwitches = {kOuthole};
    save.kickCoil = kKickCoil;
    save.shooterLaneSwitch = kShooterLane;
    save.playfieldSwitches = {kTarget};
    save.lamp = kSaveLamp;
    m_assist.SetBallSaveConfig(save);

    m_assist.SetCurrentPlayer(1);
  }

  PlayfieldAssist& assist() { return m_assist; }

  void Configure(const BallSaveConfig& save) { m_assist.SetBallSaveConfig(save); }
  void Configure(const TiltAssistConfig& tilt) { m_assist.SetTiltConfig(tilt); }

  void StartBall(uint8_t ball = 1)
  {
    m_assist.OnBallStart(ball);
    Collect();
  }

  void Advance(uint64_t ms)
  {
    m_nowMs += ms;
    m_assist.Update();
    Collect();
  }

  // Returns true when the engine is allowed to see the switch.
  bool Send(int number, uint8_t state)
  {
    const bool forward = m_assist.ProcessSwitch(number, state).forwardToEngine;
    Collect();
    return forward;
  }

  bool Tap(int number)
  {
    const bool forward = Send(number, 1);
    Send(number, 0);
    return forward;
  }

  // A shove far enough after the last one to clear both the swing filter and any
  // warning blanking.
  bool Shove()
  {
    Advance(2500);
    return Tap(kBob);
  }

  // The ball is plunged: it arrives in the lane and then leaves.
  void LeaveShooterLane()
  {
    Send(kShooterLane, 1);
    Advance(200);
    Send(kShooterLane, 0);
  }

  int Count(PlayfieldAssist::EventType type) const
  {
    int count = 0;
    for (const auto& e : events)
    {
      if (e.type == type) ++count;
    }
    return count;
  }

  const PlayfieldAssist::Event* Last(PlayfieldAssist::EventType type) const
  {
    for (auto it = events.rbegin(); it != events.rend(); ++it)
    {
      if (it->type == type) return &(*it);
    }
    return nullptr;
  }

  int CountAction(PlayfieldAssist::ActionType type, int number) const
  {
    int count = 0;
    for (const auto& a : actions)
    {
      if (a.type == type && a.number == number) ++count;
    }
    return count;
  }

  const PlayfieldAssist::Action* LastAction(PlayfieldAssist::ActionType type, int number) const
  {
    for (auto it = actions.rbegin(); it != actions.rend(); ++it)
    {
      if (it->type == type && it->number == number) return &(*it);
    }
    return nullptr;
  }

  void ClearRecorded()
  {
    events.clear();
    actions.clear();
  }

  std::vector<PlayfieldAssist::Event> events;
  std::vector<PlayfieldAssist::Action> actions;

 private:
  void Collect()
  {
    for (auto& e : m_assist.TakeEvents()) events.push_back(e);
    for (auto& a : m_assist.TakeActions()) actions.push_back(a);
  }

  PlayfieldAssist m_assist;
  uint64_t m_nowMs = 10000;
};

using Type = PlayfieldAssist::EventType;
using ActionType = PlayfieldAssist::ActionType;
}  // namespace

// ---------------------------------------------------------------- tilt ----

TEST_CASE("a warning hit is never forwarded to the engine")
{
  // The whole point. If a warning reaches a ROM, the ROM tilts on the first
  // shove and the feature has made the machine worse.
  AssistHarness h;
  h.StartBall();

  CHECK_FALSE(h.Shove());
  CHECK(h.Count(Type::TiltWarning) == 1);
  CHECK(h.Count(Type::Tilt) == 0);
}

TEST_CASE("the hit that spends the last warning is forwarded and tilts")
{
  AssistHarness h;
  h.StartBall();

  CHECK_FALSE(h.Shove());
  CHECK_FALSE(h.Shove());
  CHECK(h.Count(Type::TiltWarning) == 2);

  // Forwarded, so a ROM with its own tilt handling does the right thing without
  // knowing the host counted warnings on its behalf.
  CHECK(h.Shove());
  CHECK(h.Count(Type::Tilt) == 1);
}

TEST_CASE("a warning reports how many are left")
{
  AssistHarness h;
  h.StartBall();

  h.Shove();
  const auto* first = h.Last(Type::TiltWarning);
  REQUIRE(first != nullptr);
  CHECK(first->value == 1);  // used
  CHECK(first->total == 1);  // remaining before the tilt

  h.Shove();
  const auto* second = h.Last(Type::TiltWarning);
  REQUIRE(second != nullptr);
  CHECK(second->value == 2);
  CHECK(second->total == 0);
}

TEST_CASE("a warning blanks the bob so a swinging plumb cannot burn the rest")
{
  // This is the behaviour a real bob needs: after a warning the pendulum is
  // still moving, and without blanking one shove spends every warning.
  AssistHarness h;
  h.StartBall();

  h.Shove();
  REQUIRE(h.Count(Type::TiltWarning) == 1);

  // Well past the swing filter, still inside the blanking window.
  h.Advance(600);
  CHECK_FALSE(h.Tap(kBob));
  h.Advance(600);
  CHECK_FALSE(h.Tap(kBob));
  CHECK(h.Count(Type::TiltWarning) == 1);

  // Once the blanking expires the bob counts again.
  h.Advance(1200);
  CHECK_FALSE(h.Tap(kBob));
  CHECK(h.Count(Type::TiltWarning) == 2);
}

TEST_CASE("blanking is configurable and can be switched off")
{
  AssistHarness h;
  TiltAssistConfig tilt = h.assist().GetTiltConfig();
  tilt.warningBlankingMs = 0;
  h.Configure(tilt);
  h.StartBall();

  h.Shove();
  h.Advance(600);  // only the swing filter applies now
  h.Tap(kBob);
  CHECK(h.Count(Type::TiltWarning) == 2);
}

TEST_CASE("one shove inside the swing filter is one warning")
{
  AssistHarness h;
  h.StartBall();

  h.Advance(2500);
  h.Tap(kBob);
  h.Advance(50);
  h.Tap(kBob);
  h.Advance(50);
  h.Tap(kBob);

  CHECK(h.Count(Type::TiltWarning) == 1);
}

TEST_CASE("the warning count is configurable")
{
  AssistHarness h;
  TiltAssistConfig tilt = h.assist().GetTiltConfig();
  tilt.warnings = 0;  // first hit tilts, as an unforgiving EM machine would
  h.Configure(tilt);
  h.StartBall();

  CHECK(h.Shove());
  CHECK(h.Count(Type::Tilt) == 1);
  CHECK(h.Count(Type::TiltWarning) == 0);
}

TEST_CASE("awarded warnings extend the allowance for the rest of the game")
{
  AssistHarness h;
  h.StartBall(1);

  h.assist().AwardTiltWarnings(1);
  CHECK(h.assist().GetWarningsAllowed(1) == 3);

  h.Shove();
  h.Shove();
  h.Shove();
  CHECK(h.Count(Type::TiltWarning) == 3);
  CHECK(h.Count(Type::Tilt) == 0);

  h.Shove();
  CHECK(h.Count(Type::Tilt) == 1);
}

TEST_CASE("awarded warnings survive into the next ball, the base allowance resets")
{
  AssistHarness h;
  h.StartBall(1);
  h.assist().AwardTiltWarnings(2);
  h.Shove();
  h.Shove();
  REQUIRE(h.assist().GetWarningsUsed(1) == 2);

  h.StartBall(2);
  CHECK(h.assist().GetWarningsUsed(1) == 0);   // used count resets
  CHECK(h.assist().GetWarningsAllowed(1) == 4);  // the award does not
}

TEST_CASE("warnings are counted per player")
{
  AssistHarness h;
  h.assist().SetCurrentPlayer(1);
  h.StartBall(1);
  h.Shove();
  h.Shove();
  REQUIRE(h.assist().GetWarningsUsed(1) == 2);

  h.assist().SetCurrentPlayer(2);
  h.StartBall(1);
  CHECK(h.assist().GetWarningsUsed(2) == 0);
  CHECK(h.assist().GetWarningsRemaining(2) == 2);
}

TEST_CASE("the bob is ignored outside play")
{
  AssistHarness h;
  h.assist().SetPlayActive(false);

  CHECK_FALSE(h.Shove());
  CHECK(h.Count(Type::TiltWarning) == 0);
  CHECK(h.Count(Type::Tilt) == 0);
}

TEST_CASE("further hits after a tilt are swallowed")
{
  AssistHarness h;
  h.StartBall();
  h.Shove();
  h.Shove();
  h.Shove();
  REQUIRE(h.Count(Type::Tilt) == 1);
  h.ClearRecorded();

  CHECK_FALSE(h.Shove());
  CHECK(h.Count(Type::Tilt) == 0);
}

TEST_CASE("slam tilt is never warned, never blanked and always forwarded")
{
  AssistHarness h;
  h.StartBall();
  h.Shove();  // a warning, which blanks the bob

  CHECK(h.Tap(kSlam));
  CHECK(h.Count(Type::SlamTilt) == 1);
  CHECK(h.Count(Type::TiltWarning) == 1);
}

TEST_CASE("a warning flashes the warning lamp and lets it expire")
{
  AssistHarness h;
  h.StartBall();
  h.Shove();

  const auto* on = h.LastAction(ActionType::SetLamp, kWarningLamp);
  REQUIRE(on != nullptr);
  CHECK(on->value == 1);

  h.Advance(2000);
  const auto* off = h.LastAction(ActionType::SetLamp, kWarningLamp);
  REQUIRE(off != nullptr);
  CHECK(off->value == 0);
}

// ----------------------------------------------------------- ball save ----

TEST_CASE("the save arms when the ball leaves the shooter lane")
{
  AssistHarness h;
  h.StartBall();
  CHECK_FALSE(h.assist().IsBallSaveActive());

  h.Send(kShooterLane, 1);
  CHECK_FALSE(h.assist().IsBallSaveActive());  // still at the plunger

  h.Advance(5000);  // taking a long time to plunge must not eat the timer
  h.Send(kShooterLane, 0);
  CHECK(h.assist().IsBallSaveActive());
  CHECK(h.assist().GetBallSaveRemainingMs() == 8000);
}

TEST_CASE("the start trigger is configurable")
{
  SUBCASE("troughExit")
  {
    AssistHarness h;
    BallSaveConfig save = h.assist().GetBallSaveConfig();
    save.startOn = BallSaveStart::TroughExit;
    h.Configure(save);
    h.StartBall();

    h.Send(kOuthole, 0);  // the ball leaves the trough
    CHECK(h.assist().IsBallSaveActive());
  }

  SUBCASE("firstPlayfieldSwitch")
  {
    AssistHarness h;
    BallSaveConfig save = h.assist().GetBallSaveConfig();
    save.startOn = BallSaveStart::FirstPlayfieldSwitch;
    h.Configure(save);
    h.StartBall();

    h.LeaveShooterLane();
    CHECK_FALSE(h.assist().IsBallSaveActive());  // nothing hit yet

    h.Tap(kTarget);
    CHECK(h.assist().IsBallSaveActive());
  }
}

TEST_CASE("a machine with no lane switch falls back rather than never arming")
{
  // Silently never arming would be indistinguishable from the feature being
  // broken, which is the worse failure.
  AssistHarness h;
  BallSaveConfig save = h.assist().GetBallSaveConfig();
  save.startOn = BallSaveStart::ShooterLane;
  save.shooterLaneSwitch = 0;
  h.Configure(save);
  CHECK(h.assist().GetBallSaveConfig().startOn == BallSaveStart::TroughExit);

  h.StartBall();
  h.Send(kOuthole, 0);
  CHECK(h.assist().IsBallSaveActive());
}

TEST_CASE("a drain inside the save is hidden from the engine and kicked back")
{
  AssistHarness h;
  h.StartBall();
  h.LeaveShooterLane();
  REQUIRE(h.assist().IsBallSaveActive());
  h.ClearRecorded();

  h.Advance(2000);
  // Not forwarded: GameCore would end the ball, and a ROM would move on.
  CHECK_FALSE(h.Send(kOuthole, 1));
  CHECK(h.Count(Type::BallSaved) == 1);
  CHECK(h.CountAction(ActionType::PulseCoil, kKickCoil) == 1);
}

TEST_CASE("a drain after the save has expired reaches the engine")
{
  AssistHarness h;
  h.StartBall();
  h.LeaveShooterLane();
  REQUIRE(h.assist().IsBallSaveActive());

  h.Advance(8100);
  CHECK_FALSE(h.assist().IsBallSaveActive());
  CHECK(h.Count(Type::BallSaveExpired) == 1);

  CHECK(h.Send(kOuthole, 1));
  CHECK(h.Count(Type::BallSaved) == 0);
}

TEST_CASE("the save lamp follows the save")
{
  AssistHarness h;
  h.StartBall();
  h.LeaveShooterLane();

  const auto* on = h.LastAction(ActionType::SetLamp, kSaveLamp);
  REQUIRE(on != nullptr);
  CHECK(on->value == 1);

  h.Advance(8100);
  const auto* off = h.LastAction(ActionType::SetLamp, kSaveLamp);
  REQUIRE(off != nullptr);
  CHECK(off->value == 0);
}

TEST_CASE("maxSavesPerBall caps how often one ball is rescued")
{
  AssistHarness h;
  BallSaveConfig save = h.assist().GetBallSaveConfig();
  save.maxSavesPerBall = 1;
  h.Configure(save);
  h.StartBall();
  h.LeaveShooterLane();

  CHECK_FALSE(h.Send(kOuthole, 1));  // saved
  h.Send(kOuthole, 0);
  CHECK_FALSE(h.assist().IsBallSaveActive());  // and the save is spent
  CHECK(h.Send(kOuthole, 1));                  // the next drain counts
}

TEST_CASE("unlimited saves are possible within the window")
{
  AssistHarness h;
  BallSaveConfig save = h.assist().GetBallSaveConfig();
  save.maxSavesPerBall = 0;
  h.Configure(save);
  h.StartBall();
  h.LeaveShooterLane();

  for (int i = 0; i < 3; ++i)
  {
    CHECK_FALSE(h.Send(kOuthole, 1));
    h.Send(kOuthole, 0);
    h.Advance(500);
  }
  CHECK(h.Count(Type::BallSaved) == 3);
}

TEST_CASE("onlyOnBalls restricts which balls are protected")
{
  AssistHarness h;
  BallSaveConfig save = h.assist().GetBallSaveConfig();
  save.onlyOnBalls = {1};
  h.Configure(save);

  h.StartBall(1);
  h.LeaveShooterLane();
  CHECK(h.assist().IsBallSaveActive());

  h.StartBall(2);
  h.LeaveShooterLane();
  CHECK_FALSE(h.assist().IsBallSaveActive());
}

TEST_CASE("a granted save works regardless of the configured trigger")
{
  // ppuc.ballSave(ms) from a rule: a mode was just started and wants to hand out
  // a few seconds of protection with it. Must work in either engine, and without
  // waiting for a shooter lane the ball left long ago.
  AssistHarness h;
  h.StartBall();
  h.LeaveShooterLane();
  h.Advance(8100);
  REQUIRE_FALSE(h.assist().IsBallSaveActive());

  h.assist().GrantBallSave(3000);
  h.Advance(0);
  CHECK(h.assist().IsBallSaveActive());
  CHECK(h.assist().GetBallSaveRemainingMs() == 3000);

  CHECK_FALSE(h.Send(kOuthole, 1));
  CHECK(h.Count(Type::BallSaved) == 1);
}

TEST_CASE("a grant extends an active save but never shortens it")
{
  AssistHarness h;
  h.StartBall();
  h.LeaveShooterLane();
  REQUIRE(h.assist().GetBallSaveRemainingMs() == 8000);

  h.assist().GrantBallSave(2000);
  CHECK(h.assist().GetBallSaveRemainingMs() == 8000);  // shorter grant ignored

  h.assist().GrantBallSave(12000);
  CHECK(h.assist().GetBallSaveRemainingMs() == 12000);
}

TEST_CASE("a granted save works when ball save is otherwise disabled")
{
  // A machine that does not want an automatic saver can still let a rule hand
  // one out for a specific mode.
  AssistHarness h;
  BallSaveConfig save = h.assist().GetBallSaveConfig();
  save.enabled = false;
  h.Configure(save);
  h.StartBall();

  h.assist().GrantBallSave(4000);
  CHECK(h.assist().IsBallSaveActive());
  CHECK_FALSE(h.Send(kOuthole, 1));
}

TEST_CASE("the save does not survive the end of the ball")
{
  AssistHarness h;
  h.StartBall();
  h.LeaveShooterLane();
  REQUIRE(h.assist().IsBallSaveActive());

  h.assist().OnBallEnd();
  CHECK_FALSE(h.assist().IsBallSaveActive());
}

TEST_CASE("a tilt cancels the save")
{
  // A tilted ball is meant to be lost. Rescuing it would be worse than useless.
  AssistHarness h;
  h.StartBall();
  h.LeaveShooterLane();
  REQUIRE(h.assist().IsBallSaveActive());

  h.Shove();
  h.Shove();
  h.Shove();
  REQUIRE(h.Count(Type::Tilt) == 1);

  CHECK_FALSE(h.assist().IsBallSaveActive());
  CHECK(h.Send(kOuthole, 1));  // the drain reaches the engine
}


TEST_CASE("every tilt switch feeds the same warning count")
{
  // A machine has a plumb bob and usually a ball-roll tilt as well. They are
  // not independent counters: three hits across both is still a tilt.
  AssistHarness h;
  h.StartBall();

  h.Advance(2500);
  CHECK_FALSE(h.Tap(kBob));
  h.Advance(2500);
  CHECK_FALSE(h.Tap(kBallRoll));
  CHECK(h.Count(Type::TiltWarning) == 2);

  h.Advance(2500);
  CHECK(h.Tap(kBob));
  CHECK(h.Count(Type::Tilt) == 1);
}

TEST_CASE("one shove tripping two tilt switches is one warning")
{
  // The swing filter is shared across switches on purpose: a shove that moves
  // the plumb bob usually rolls the ball tilt too, and that is one shove.
  AssistHarness h;
  h.StartBall();

  h.Advance(2500);
  h.Tap(kBob);
  h.Advance(50);
  h.Tap(kBallRoll);

  CHECK(h.Count(Type::TiltWarning) == 1);
}

TEST_CASE("blanking after a warning covers every tilt switch")
{
  AssistHarness h;
  h.StartBall();

  h.Shove();
  REQUIRE(h.Count(Type::TiltWarning) == 1);

  // Well past the swing filter, still inside the blanking window: the other
  // tilt switch must be ignored too, or the blanking buys nothing.
  h.Advance(700);
  CHECK_FALSE(h.Tap(kBallRoll));
  CHECK(h.Count(Type::TiltWarning) == 1);
}

TEST_CASE("a machine with a single tilt switch still works")
{
  AssistHarness h;
  TiltAssistConfig tilt = h.assist().GetTiltConfig();
  tilt.switches = {kBob};
  h.Configure(tilt);
  h.StartBall();

  h.Shove();
  h.Shove();
  CHECK(h.Count(Type::TiltWarning) == 2);
  CHECK(h.Shove());
  CHECK(h.Count(Type::Tilt) == 1);

  // The ball-roll switch is now an ordinary switch and reaches the engine.
  h.Advance(2500);
  CHECK(h.Tap(kBallRoll));
}
