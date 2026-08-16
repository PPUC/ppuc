// Tests for the ROM-less game state machine.
//
// The failures these are meant to catch are the ones a player notices and a log
// does not: a machine that eats a ball, that skips a player's turn, that awards
// the same replay twice, or that collects bonus in the wrong order. Everything
// here runs on an injected clock with no hardware.

#include "GameFixture.h"
#include "doctest.h"

using Order = std::vector<std::pair<int, int>>;

TEST_CASE("a game does not start without credits")
{
  GameHarness h;
  h.Start();

  h.PressStart();
  CHECK(h.core().GetState() == GameState::Attract);
  const GameEvent* rejected = h.Last(GameEventType::StartRejected);
  REQUIRE(rejected != nullptr);
  CHECK(rejected->reason == StartRejectReason::NoCredits);
}

TEST_CASE("free play starts without a coin")
{
  GameHarness h;
  h.config().freePlay = true;
  h.Start();

  h.PressStart();
  h.Update();
  CHECK(h.Saw(GameEventType::GameStart));
  CHECK(h.core().GetCurrentPlayer() == 1);
  CHECK(h.core().GetCurrentBall() == 1);
}

TEST_CASE("a coin buys a credit and starting spends it")
{
  GameHarness h;
  h.Start();

  h.InsertCoin();
  CHECK(h.core().GetCredits() == 1);

  h.PressStart();
  h.Update();
  CHECK(h.core().GetCredits() == 0);
  CHECK(h.Saw(GameEventType::GameStart));
}

TEST_CASE("a start with an empty trough is refused and asks for a ball search")
{
  GameHarness h;
  h.Start();
  h.InsertCoin();

  // Take the ball out of the trough without a game running.
  h.core().OnSwitch(11, 0);
  h.Settle();
  h.ClearRecorded();

  h.PressStart();
  const GameEvent* rejected = h.Last(GameEventType::StartRejected);
  REQUIRE(rejected != nullptr);
  CHECK(rejected->reason == StartRejectReason::NotEnoughBalls);
  CHECK(h.CountAction(GameActionType::BallSearch, 0) == 1);
  CHECK(h.core().GetCredits() == 1);  // not spent on a refused start
}

TEST_CASE("game start asserts game-on and lights ball-in-play")
{
  GameHarness h;
  h.config().freePlay = true;
  h.Start();
  h.ClearRecorded();

  h.PressStart();
  h.Update();

  const GameAction* gameOn = h.LastAction(GameActionType::SetCoil, 10);
  REQUIRE(gameOn != nullptr);
  CHECK(gameOn->value == 1);
  const GameAction* ballInPlay = h.LastAction(GameActionType::SetLamp, 42);
  REQUIRE(ballInPlay != nullptr);
  CHECK(ballInPlay->value == 1);
  const GameAction* gameOver = h.LastAction(GameActionType::SetLamp, 40);
  REQUIRE(gameOver != nullptr);
  CHECK(gameOver->value == 0);
}

TEST_CASE("a player can be added during ball one but not after")
{
  GameHarness h;
  h.config().freePlay = true;
  h.Start();
  h.StartGameAndServe();
  CHECK(h.core().GetPlayerCount() == 1);

  h.PressStart();
  CHECK(h.core().GetPlayerCount() == 2);
  h.PressStart();
  CHECK(h.core().GetPlayerCount() == 3);

  // Play out ball one for all three players.
  for (int i = 0; i < 3; ++i)
  {
    h.DrainAndFinishBall();
  }
  REQUIRE(h.core().GetCurrentBall() == 2);
  h.ClearRecorded();

  h.PressStart();
  CHECK(h.core().GetPlayerCount() == 3);
  const GameEvent* rejected = h.Last(GameEventType::StartRejected);
  REQUIRE(rejected != nullptr);
  CHECK(rejected->reason == StartRejectReason::TooLate);
}

TEST_CASE("a fifth player is refused")
{
  GameHarness h;
  h.config().freePlay = true;
  h.config().maxPlayers = 4;
  h.Start();
  h.StartGameAndServe();

  for (int i = 0; i < 3; ++i) h.PressStart();
  REQUIRE(h.core().GetPlayerCount() == 4);
  h.ClearRecorded();

  h.PressStart();
  CHECK(h.core().GetPlayerCount() == 4);
  const GameEvent* rejected = h.Last(GameEventType::StartRejected);
  REQUIRE(rejected != nullptr);
  CHECK(rejected->reason == StartRejectReason::TooManyPlayers);
}

TEST_CASE("one player plays exactly ballsPerGame balls in order")
{
  GameHarness h;
  h.config().freePlay = true;
  h.config().ballsPerGame = 3;
  h.Start();
  h.StartGameAndServe();

  h.DrainAndFinishBall();
  h.DrainAndFinishBall();
  h.Drain();
  h.AdvanceAndUpdate(h.config().bonusTimeoutMs + 10);
  h.Update();

  CHECK(h.BallOrder() == Order{{1, 1}, {1, 2}, {1, 3}});
  CHECK(h.Saw(GameEventType::GameEnd));
}

TEST_CASE("two players alternate balls")
{
  GameHarness h;
  h.config().freePlay = true;
  h.config().ballsPerGame = 2;
  h.Start();
  h.StartGameAndServe();
  h.PressStart();
  REQUIRE(h.core().GetPlayerCount() == 2);

  for (int i = 0; i < 3; ++i) h.DrainAndFinishBall();
  h.Drain();
  h.AdvanceAndUpdate(h.config().bonusTimeoutMs + 10);
  h.Update();

  CHECK(h.BallOrder() == Order{{1, 1}, {2, 1}, {1, 2}, {2, 2}});
  CHECK(h.Saw(GameEventType::GameEnd));
}

TEST_CASE("a player added during ball one still gets a full game")
{
  GameHarness h;
  h.config().freePlay = true;
  h.config().ballsPerGame = 2;
  h.Start();
  h.StartGameAndServe();
  h.PressStart();

  for (int i = 0; i < 3; ++i) h.DrainAndFinishBall();
  h.Drain();
  h.AdvanceAndUpdate(h.config().bonusTimeoutMs + 10);
  h.Update();

  const Order order = h.BallOrder();
  const int player2Balls = static_cast<int>(
      std::count_if(order.begin(), order.end(), [](const auto& p) { return p.first == 2; }));
  CHECK(player2Balls == 2);
}

TEST_CASE("bonus is collected at the end of every ball, before any extra ball")
{
  GameHarness h;
  h.config().freePlay = true;
  h.Start();
  h.StartGameAndServe();

  h.core().AwardExtraBall();
  REQUIRE(h.core().GetExtraBalls(0) == 1);
  h.ClearRecorded();

  h.Drain();
  h.Update();

  // BallEnd, then BonusCount, and only then ExtraBall.
  int ballEndIndex = -1;
  int bonusIndex = -1;
  int extraBallIndex = -1;
  for (size_t i = 0; i < h.events.size(); ++i)
  {
    if (h.events[i].type == GameEventType::BallEnd && ballEndIndex < 0) ballEndIndex = static_cast<int>(i);
    if (h.events[i].type == GameEventType::BonusCount && bonusIndex < 0) bonusIndex = static_cast<int>(i);
  }
  REQUIRE(ballEndIndex >= 0);
  REQUIRE(bonusIndex >= 0);
  CHECK(ballEndIndex < bonusIndex);

  h.core().BonusDone();
  h.Update();
  for (size_t i = 0; i < h.events.size(); ++i)
  {
    if (h.events[i].type == GameEventType::ExtraBall && extraBallIndex < 0) extraBallIndex = static_cast<int>(i);
  }
  REQUIRE(extraBallIndex >= 0);
  CHECK(bonusIndex < extraBallIndex);
}

TEST_CASE("an extra ball replays the same ball number")
{
  GameHarness h;
  h.config().freePlay = true;
  h.Start();
  h.StartGameAndServe();
  REQUIRE(h.core().GetCurrentBall() == 1);

  h.core().AwardExtraBall();
  h.DrainAndFinishBall();

  CHECK(h.core().GetCurrentBall() == 1);
  CHECK(h.core().GetCurrentPlayer() == 1);
  CHECK(h.core().GetExtraBalls(0) == 0);
  CHECK(h.Saw(GameEventType::ExtraBall));
}

TEST_CASE("extra balls stack")
{
  GameHarness h;
  h.config().freePlay = true;
  h.Start();
  h.StartGameAndServe();

  h.core().AwardExtraBall();
  h.core().AwardExtraBall();
  REQUIRE(h.core().GetExtraBalls(0) == 2);

  h.DrainAndFinishBall();
  CHECK(h.core().GetCurrentBall() == 1);
  CHECK(h.core().GetExtraBalls(0) == 1);

  h.DrainAndFinishBall();
  CHECK(h.core().GetCurrentBall() == 1);
  CHECK(h.core().GetExtraBalls(0) == 0);

  h.DrainAndFinishBall();
  CHECK(h.core().GetCurrentBall() == 2);
}

TEST_CASE("per-ball variables survive an extra ball but not a new ball")
{
  GameHarness h;
  h.config().freePlay = true;
  h.Start();
  h.StartGameAndServe();

  h.core().SetBallVar("ramps", 3);
  h.core().AwardExtraBall();
  h.DrainAndFinishBall();
  CHECK(h.core().GetBallVar("ramps") == 3);  // shoot again continues the ball

  h.DrainAndFinishBall();
  CHECK(h.core().GetBallVar("ramps") == 0);  // a genuinely new ball
}

TEST_CASE("a serve that never happens retries then asks for a ball search")
{
  GameHarness h;
  h.config().freePlay = true;
  h.config().trough.kickRetries = 3;
  h.Start();
  h.InsertCoin();
  h.PressStart();
  h.Update();
  h.ClearRecorded();

  // Never take the ball out of the trough.
  for (int i = 0; i < 5; ++i)
  {
    h.AdvanceAndUpdate(h.config().trough.kickRetryMs + 10);
  }

  CHECK(h.CountAction(GameActionType::PulseCoil, 1) >= 3);
  CHECK(h.Count(GameEventType::BallServeFailed) >= 1);
  CHECK(h.CountAction(GameActionType::BallSearch, 0) >= 1);
}

TEST_CASE("a trough closure shorter than settleMs does not end the ball")
{
  GameHarness h;
  h.config().freePlay = true;
  h.Start();
  h.StartGameAndServe();
  REQUIRE(h.core().GetState() == GameState::BallInPlay);
  h.ClearRecorded();

  // A chattering outhole switch: closed, then open again well inside the
  // settle window. This is the case that would otherwise eat a ball.
  h.core().OnSwitch(11, 1);
  h.AdvanceAndUpdate(h.config().trough.settleMs / 4);
  h.core().OnSwitch(11, 0);
  h.AdvanceAndUpdate(h.config().trough.settleMs / 4);
  h.Update();

  CHECK_FALSE(h.Saw(GameEventType::BallEnd));
  CHECK(h.core().GetState() == GameState::BallInPlay);
}

TEST_CASE("scores accumulate per player and never leak across them")
{
  GameHarness h;
  h.config().freePlay = true;
  h.config().ballsPerGame = 2;
  h.Start();
  h.StartGameAndServe();
  h.PressStart();

  h.core().AddScore(1000);
  CHECK(h.core().GetScore(1) == 1000);
  CHECK(h.core().GetScore(2) == 0);

  h.DrainAndFinishBall();
  REQUIRE(h.core().GetCurrentPlayer() == 2);
  h.core().AddScore(500);

  CHECK(h.core().GetScore(1) == 1000);
  CHECK(h.core().GetScore(2) == 500);
}

TEST_CASE("score rolls over at the configured digit count")
{
  GameHarness h;
  h.config().freePlay = true;
  h.config().scoreDigits = 6;
  h.Start();
  h.StartGameAndServe();

  h.core().AddScore(999000);
  h.core().AddScore(2000);
  CHECK(h.core().GetScore(1) == 1000);  // 1,001,000 wraps on a 6-digit machine
}

TEST_CASE("a replay threshold is awarded once per game")
{
  GameHarness h;
  h.config().freePlay = true;
  h.config().replay.thresholds = {50000};
  h.config().replay.awardCredit = true;
  h.Start();
  h.StartGameAndServe();
  h.ClearRecorded();

  h.core().AddScore(60000);
  h.Update();
  CHECK(h.Count(GameEventType::Replay) == 1);
  CHECK(h.core().GetCredits() == 1);
  CHECK(h.CountAction(GameActionType::PulseCoil, 9) == 1);  // knocker

  h.core().AddScore(10000);
  h.Update();
  CHECK(h.Count(GameEventType::Replay) == 1);  // not again
}

TEST_CASE("crossing two replay thresholds at once awards both")
{
  GameHarness h;
  h.config().freePlay = true;
  h.config().replay.thresholds = {10000, 20000};
  h.Start();
  h.StartGameAndServe();
  h.ClearRecorded();

  h.core().AddScore(25000);
  h.Update();
  CHECK(h.Count(GameEventType::Replay) == 2);
}

TEST_CASE("game over drops game-on and lights the game-over lamp")
{
  GameHarness h;
  h.config().freePlay = true;
  h.config().ballsPerGame = 1;
  h.Start();
  h.StartGameAndServe();
  h.ClearRecorded();

  h.Drain();
  h.AdvanceAndUpdate(h.config().bonusTimeoutMs + 10);
  h.Update();

  REQUIRE(h.Saw(GameEventType::GameEnd));
  const GameAction* gameOn = h.LastAction(GameActionType::SetCoil, 10);
  REQUIRE(gameOn != nullptr);
  CHECK(gameOn->value == 0);
  const GameAction* gameOverLamp = h.LastAction(GameActionType::SetLamp, 40);
  REQUIRE(gameOverLamp != nullptr);
  CHECK(gameOverLamp->value == 1);
}

TEST_CASE("the machine returns to attract after the game-over hold")
{
  GameHarness h;
  h.config().freePlay = true;
  h.config().ballsPerGame = 1;
  h.Start();
  h.StartGameAndServe();

  h.Drain();
  h.AdvanceAndUpdate(h.config().bonusTimeoutMs + 10);
  h.Update();
  REQUIRE(h.core().GetState() == GameState::GameOver);

  h.AdvanceAndUpdate(h.config().gameOverHoldMs + 10);
  CHECK(h.core().GetState() == GameState::Attract);
  CHECK(h.Saw(GameEventType::AttractStart));
}

TEST_CASE("a script veto blocks the start and says why")
{
  GameHarness h;
  h.config().freePlay = true;
  h.Start();
  h.core().SetAllowCallback([](const char* what, int) { return std::string(what) != "canStartGame"; });
  h.ClearRecorded();

  h.PressStart();
  CHECK(h.core().GetState() == GameState::Attract);
  const GameEvent* rejected = h.Last(GameEventType::StartRejected);
  REQUIRE(rejected != nullptr);
  CHECK(rejected->reason == StartRejectReason::ScriptVetoed);
}

TEST_CASE("an absent allow callback permits everything")
{
  GameHarness h;
  h.config().freePlay = true;
  h.Start();
  h.PressStart();
  h.Update();
  CHECK(h.Saw(GameEventType::GameStart));
}

TEST_CASE("match awards a credit when the last two digits agree")
{
  GameHarness h;
  h.config().freePlay = true;
  h.config().ballsPerGame = 1;
  h.config().match.enabled = true;
  h.SetRandomValue(4);  // digits become 40
  h.Start();
  h.StartGameAndServe();
  h.core().SetScore(1240, 1);
  h.ClearRecorded();

  h.Drain();
  h.AdvanceAndUpdate(h.config().bonusTimeoutMs + 10);
  h.Update();

  const GameEvent* match = h.Last(GameEventType::Match);
  REQUIRE(match != nullptr);
  CHECK(match->value == 40);
  CHECK(match->total == 0b1);  // player 1 matched
  CHECK(h.core().GetCredits() == 1);
}

TEST_CASE("bonusDone leaves the bonus state without waiting for the timeout")
{
  GameHarness h;
  h.config().freePlay = true;
  h.Start();
  h.StartGameAndServe();

  h.Drain();
  REQUIRE(h.core().GetState() == GameState::BonusCount);

  h.core().BonusDone();
  h.Update();
  CHECK(h.core().GetState() != GameState::BonusCount);
}

TEST_CASE("a bonus that never completes times out and the game continues")
{
  GameHarness h;
  h.config().freePlay = true;
  h.Start();
  h.StartGameAndServe();

  h.Drain();
  REQUIRE(h.core().GetState() == GameState::BonusCount);

  h.AdvanceAndUpdate(h.config().bonusTimeoutMs + 10);
  h.Update();
  CHECK(h.core().GetState() != GameState::BonusCount);
  CHECK(h.core().GetCurrentBall() == 2);
}

TEST_CASE("scoring is ignored outside play")
{
  GameHarness h;
  h.config().freePlay = true;
  h.Start();

  h.core().AddScore(1000);
  CHECK(h.core().GetScore(1) == 0);

  h.StartGameAndServe();
  h.core().AddScore(1000);
  CHECK(h.core().GetScore(1) == 1000);
}
