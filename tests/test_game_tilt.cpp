// Tests for tilt.
//
// Tilt is the one part of GameCore with a physical failure mode: get it wrong
// and either the flippers stay alive after a shove, or the ball-recovery coils
// die and the machine strands a ball. Both of those are asserted here.
//
// What tilt drops is deliberately narrow -- flipper inhibit and GI, nothing
// else -- so the outhole kicker, trough eject and knocker stay drivable.

#include "GameFixture.h"
#include "doctest.h"

namespace
{
constexpr int kInhibitSwitch = 250;
constexpr int kTiltLamp = 41;
constexpr int kSlamSwitch = 7;

// Tilts the machine. Warning counting, the swing filter and the bob itself live
// in PlayfieldAssist so a ROM game gets them too -- see test_playfield_assist.cpp.
// By the time GameCore is involved the decision has already been made, so these
// tests call Tilt() directly rather than pretending to shove the cabinet.
void TiltNow(GameHarness& h)
{
  h.core().Tilt();
  h.Update();
}
}  // namespace

TEST_CASE("tilt asserts the flipper inhibit and darkens GI in the same update")
{
  GameHarness h;
  h.config().freePlay = true;
  h.config().tilt.giOff = true;
  h.Start();
  h.StartGameAndServe();
  h.ClearRecorded();

  TiltNow(h);
  REQUIRE(h.Saw(GameEventType::Tilt));

  const GameAction* inhibit = h.LastAction(GameActionType::SetTiltInhibit, kInhibitSwitch);
  REQUIRE(inhibit != nullptr);
  CHECK(inhibit->value == 1);

  for (int string : h.config().giStrings)
  {
    const GameAction* gi = h.LastAction(GameActionType::SetGi, string);
    REQUIRE(gi != nullptr);
    CHECK(gi->value == 0);
  }

  const GameAction* tiltLamp = h.LastAction(GameActionType::SetLamp, kTiltLamp);
  REQUIRE(tiltLamp != nullptr);
  CHECK(tiltLamp->value == 1);
}

TEST_CASE("tilt leaves the ball-recovery coils drivable")
{
  // The regression this guards: killing game-on on tilt would take the outhole
  // kicker with it, and a machine that cannot return its own ball is stuck.
  GameHarness h;
  h.config().freePlay = true;
  h.Start();
  h.StartGameAndServe();
  h.ClearRecorded();

  TiltNow(h);
  REQUIRE(h.Saw(GameEventType::Tilt));

  CHECK(h.CountAction(GameActionType::SetCoil, h.config().gameOnCoil) == 0);

  // And the serve path still pulses the kick coil when the ball comes home.
  h.Drain();
  h.AdvanceAndUpdate(h.config().bonusTimeoutMs + 10);
  h.Update();
  CHECK(h.CountAction(GameActionType::PulseCoil, h.config().trough.kickCoil) >= 1);
}

TEST_CASE("scoring is discarded while tilted")
{
  GameHarness h;
  h.config().freePlay = true;
  h.Start();
  h.StartGameAndServe();

  h.core().AddScore(1000);
  REQUIRE(h.core().GetScore(1) == 1000);

  TiltNow(h);
  h.ClearRecorded();

  h.core().AddScore(5000);
  CHECK(h.core().GetScore(1) == 1000);
  // No Score event either: presentation stays quiet rather than announcing
  // points the player did not get.
  CHECK_FALSE(h.Saw(GameEventType::Score));
}

TEST_CASE("bonus is skipped when tilted and configured to skip")
{
  GameHarness h;
  h.config().freePlay = true;
  h.config().tilt.skipBonus = true;
  h.Start();
  h.StartGameAndServe();
  TiltNow(h);
  h.ClearRecorded();

  h.Drain();
  CHECK_FALSE(h.Saw(GameEventType::BonusCount));
}

TEST_CASE("bonus still runs when tilted if skipBonus is off")
{
  GameHarness h;
  h.config().freePlay = true;
  h.config().tilt.skipBonus = false;
  h.Start();
  h.StartGameAndServe();
  TiltNow(h);
  h.ClearRecorded();

  h.Drain();
  CHECK(h.Saw(GameEventType::BonusCount));
}

TEST_CASE("tilt is per player: the next player starts clean")
{
  GameHarness h;
  h.config().freePlay = true;
  h.Start();
  h.StartGameAndServe();
  h.PressStart();
  REQUIRE(h.core().GetPlayerCount() == 2);

  TiltNow(h);
  REQUIRE(h.core().IsTilted(1));

  h.DrainAndFinishBall();
  REQUIRE(h.core().GetCurrentPlayer() == 2);
  CHECK_FALSE(h.core().IsTilted(0));
}

TEST_CASE("the flipper inhibit is released once every ball is home")
{
  GameHarness h;
  h.config().freePlay = true;
  h.config().ballCount = 1;
  h.Start();
  h.StartGameAndServe();
  TiltNow(h);
  REQUIRE(h.core().IsTilted(0));
  h.ClearRecorded();

  h.Drain();
  h.AdvanceAndUpdate(h.config().bonusTimeoutMs + 10);
  h.Update();

  const GameAction* inhibit = h.LastAction(GameActionType::SetTiltInhibit, kInhibitSwitch);
  REQUIRE(inhibit != nullptr);
  CHECK(inhibit->value == 0);

  // GI comes back on, not left at the tilt level.
  const GameAction* gi = h.LastAction(GameActionType::SetGi, 1);
  REQUIRE(gi != nullptr);
  CHECK(gi->value == h.config().giOnLevel);
}

TEST_CASE("endsBallOnly false keeps the player tilted for the rest of the game")
{
  GameHarness h;
  h.config().freePlay = true;
  h.config().tilt.endsBallOnly = false;
  h.Start();
  h.StartGameAndServe();

  TiltNow(h);
  REQUIRE(h.core().IsTilted(0));

  h.DrainAndFinishBall();
  CHECK(h.core().GetCurrentBall() == 2);
  CHECK(h.core().IsTilted(0));
}

TEST_CASE("a tilted ball that never drains is force-ended")
{
  GameHarness h;
  h.config().freePlay = true;
  h.config().tilt.recoverTimeoutMs = 30000;
  h.Start();
  h.StartGameAndServe();
  TiltNow(h);
  REQUIRE(h.core().IsTilted(0));
  h.ClearRecorded();

  // The ball is trapped: the trough never refills.
  h.AdvanceAndUpdate(h.config().tilt.recoverTimeoutMs + 10);
  h.Update();

  CHECK(h.Saw(GameEventType::BallEnd));
  const GameAction* inhibit = h.LastAction(GameActionType::SetTiltInhibit, kInhibitSwitch);
  REQUIRE(inhibit != nullptr);
  CHECK(inhibit->value == 0);
}

TEST_CASE("an extra ball is forfeited by a tilt unless configured otherwise")
{
  GameHarness h;
  h.config().freePlay = true;
  h.config().tilt.extraBallSurvivesTilt = false;
  h.Start();
  h.StartGameAndServe();

  h.core().AwardExtraBall();
  REQUIRE(h.core().GetExtraBalls(0) == 1);
  TiltNow(h);

  h.DrainAndFinishBall();
  CHECK(h.core().GetCurrentBall() == 2);  // moved on rather than shooting again
}

TEST_CASE("extraBallSurvivesTilt keeps the shoot-again")
{
  GameHarness h;
  h.config().freePlay = true;
  h.config().tilt.extraBallSurvivesTilt = true;
  h.config().tilt.endsBallOnly = true;
  h.Start();
  h.StartGameAndServe();

  h.core().AwardExtraBall();
  TiltNow(h);

  h.DrainAndFinishBall();
  CHECK(h.core().GetCurrentBall() == 1);
}

TEST_CASE("slam tilt ends the game for everyone with no bonus and no match")
{
  GameHarness h;
  h.config().freePlay = true;
  h.config().match.enabled = true;
  h.Start();
  h.StartGameAndServe();
  h.PressStart();
  REQUIRE(h.core().GetPlayerCount() == 2);
  h.ClearRecorded();

  h.core().SlamTilt();
  h.Update();

  CHECK(h.Saw(GameEventType::SlamTilt));
  CHECK_FALSE(h.Saw(GameEventType::BonusCount));
  CHECK(h.Saw(GameEventType::GameEnd));

  // Slam tilt IS the case where killing everything is right.
  const GameAction* gameOn = h.LastAction(GameActionType::SetCoil, h.config().gameOnCoil);
  REQUIRE(gameOn != nullptr);
  CHECK(gameOn->value == 0);
}

TEST_CASE("Tilt() is ignored outside play")
{
  GameHarness h;
  h.config().freePlay = true;
  h.Start();
  h.ClearRecorded();

  TiltNow(h);
  CHECK_FALSE(h.Saw(GameEventType::Tilt));
}
