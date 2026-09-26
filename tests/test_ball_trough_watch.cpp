// Tests for the ball-trough safety net.
//
// This is the one piece of PPUC that puts input into a running game that the
// machine did not report: when a ball rests in the trough and the ROM never saw
// the edge, it presents that edge again. So the cases worth pinning are mostly
// the ones where it must stay silent. Synthesising a switch in attract, or twice
// in a row, or while a ball is simply taking its time, would each be worse than
// the fault being fixed.

#include "BallTroughWatch.h"
#include "doctest.h"

namespace
{
constexpr uint32_t kGraceMs = 5000;
constexpr uint64_t kGapMs = 80;

using BallTroughWatch::Action;
using BallTroughWatch::State;
}  // namespace

TEST_CASE("an open trough never acts")
{
  State s;
  CHECK(BallTroughWatch::Update(s, true, 100000, kGraceMs, kGapMs) == Action::None);
}

TEST_CASE("a ball being served normally is left alone")
{
  State s;
  BallTroughWatch::NoteSwitch(s, true, 1000);
  // The ROM serves well inside the grace period and the ball leaves.
  CHECK(BallTroughWatch::Update(s, true, 1500, kGraceMs, kGapMs) == Action::None);
  BallTroughWatch::NoteSwitch(s, false, 1600);
  CHECK(BallTroughWatch::Update(s, true, 90000, kGraceMs, kGapMs) == Action::None);
}

TEST_CASE("a ball stuck through the grace period gets a fresh edge")
{
  State s;
  BallTroughWatch::NoteSwitch(s, true, 1000);
  CHECK(BallTroughWatch::Update(s, true, 1000 + kGraceMs - 1, kGraceMs, kGapMs) == Action::None);
  CHECK(BallTroughWatch::Update(s, true, 1000 + kGraceMs, kGraceMs, kGapMs) == Action::SendOpen);

  // The close follows only after the gap, or it is not an edge at all.
  CHECK(BallTroughWatch::Update(s, true, 1000 + kGraceMs + kGapMs - 1, kGraceMs, kGapMs) == Action::None);
  CHECK(BallTroughWatch::Update(s, true, 1000 + kGraceMs + kGapMs, kGraceMs, kGapMs) == Action::SendClose);
}

TEST_CASE("attract mode is left alone entirely")
{
  State s;
  BallTroughWatch::NoteSwitch(s, true, 1000);
  // A ball resting in the trough between games is where it belongs.
  CHECK(BallTroughWatch::Update(s, false, 1000 + kGraceMs * 10, kGraceMs, kGapMs) == Action::None);
}

TEST_CASE("a close already begun is finished even if the game ends")
{
  State s;
  BallTroughWatch::NoteSwitch(s, true, 1000);
  REQUIRE(BallTroughWatch::Update(s, true, 6000, kGraceMs, kGapMs) == Action::SendOpen);
  // Leaving the ROM with a switch opened and never closed would be worse than
  // either acting or not acting.
  CHECK(BallTroughWatch::Update(s, false, 6000 + kGapMs, kGraceMs, kGapMs) == Action::SendClose);
}

TEST_CASE("a ball the ROM still will not serve is retried rather than hammered")
{
  State s;
  BallTroughWatch::NoteSwitch(s, true, 1000);
  REQUIRE(BallTroughWatch::Update(s, true, 6000, kGraceMs, kGapMs) == Action::SendOpen);
  REQUIRE(BallTroughWatch::Update(s, true, 6000 + kGapMs, kGraceMs, kGapMs) == Action::SendClose);

  // Nothing at all until another whole grace period has passed.
  CHECK(BallTroughWatch::Update(s, true, 6000 + kGapMs + 1, kGraceMs, kGapMs) == Action::None);
  CHECK(BallTroughWatch::Update(s, true, 6000 + kGraceMs - 1, kGraceMs, kGapMs) == Action::None);
  CHECK(BallTroughWatch::Update(s, true, 6000 + kGraceMs, kGraceMs, kGapMs) == Action::SendOpen);
}

TEST_CASE("the ball leaving cancels a pending close")
{
  State s;
  BallTroughWatch::NoteSwitch(s, true, 1000);
  REQUIRE(BallTroughWatch::Update(s, true, 6000, kGraceMs, kGapMs) == Action::SendOpen);
  // The ROM served it on the opening half; the ball is gone before the close.
  BallTroughWatch::NoteSwitch(s, false, 6010);
  CHECK(BallTroughWatch::Update(s, true, 90000, kGraceMs, kGapMs) == Action::None);
}
