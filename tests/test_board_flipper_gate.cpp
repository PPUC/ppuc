// When the host turns the board flippers on and off.
//
// They used to follow "a game is running" alone, so they stayed live after a
// tilt and through every bonus count. They now also follow the ROM's flipper
// enable, the game-on solenoid, which is "ball in play".

#include "doctest.h"

#include "BoardFlipperGate.h"

using Change = BoardFlipperGate::Change;

TEST_CASE("the flippers come on once a game runs and a ball is in play")
{
  BoardFlipperGate gate;
  CHECK(gate.SetGameRunning(false, 0) == Change::Off);
  CHECK(gate.SetGameRunning(true, 10) == Change::None);
  CHECK(gate.SetBallInPlay(true, 20) == Change::On);
  CHECK(gate.Update(30) == Change::None);
}

TEST_CASE("a tilt or a drained ball turns them off after the delay")
{
  BoardFlipperGate gate;
  gate.SetGameRunning(true, 0);
  gate.SetBallInPlay(true, 0);
  CHECK(gate.SetBallInPlay(false, 1000) == Change::None);
  CHECK(gate.Update(1000 + BoardFlipperGate::kBallEndOffDelayMs - 1) == Change::None);
  CHECK(gate.Update(1000 + BoardFlipperGate::kBallEndOffDelayMs) == Change::Off);
  // The next ball.
  CHECK(gate.SetBallInPlay(true, 5000) == Change::On);
}

TEST_CASE("a brief drop of the flipper enable does not drop a held flipper")
{
  BoardFlipperGate gate;
  gate.SetGameRunning(true, 0);
  gate.SetBallInPlay(true, 0);
  CHECK(gate.SetBallInPlay(false, 1000) == Change::None);
  CHECK(gate.Update(1100) == Change::None);
  CHECK(gate.SetBallInPlay(true, 1150) == Change::None);
  CHECK(gate.Update(2000) == Change::None);
}

TEST_CASE("game over turns them off at once")
{
  BoardFlipperGate gate;
  gate.SetGameRunning(true, 0);
  gate.SetBallInPlay(true, 0);
  CHECK(gate.SetGameRunning(false, 500) == Change::Off);

  // Also when the ball ended just before, and the delay is still running.
  BoardFlipperGate other;
  other.SetGameRunning(true, 0);
  other.SetBallInPlay(true, 0);
  other.SetBallInPlay(false, 1000);
  CHECK(other.SetGameRunning(false, 1010) == Change::Off);
}

TEST_CASE("the ball in play alone does not turn them on in attract")
{
  BoardFlipperGate gate;
  gate.SetGameRunning(false, 0);
  // A ROM driving the game-on solenoid with no game running, a coil test.
  CHECK(gate.SetBallInPlay(true, 10) == Change::None);
}
