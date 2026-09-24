// Tests for the attract-slideshow timing.
//
// The drawing can be judged by looking at it; the timing cannot. Every failure
// mode here is silent on a bench and embarrassing on a machine in a bar: a show
// that starts the instant the machine boots, a show that never starts, or one
// that stays up while somebody is trying to play. So this covers the state
// machine and nothing else -- AttractSlides.cpp deliberately links without
// yaml-cpp or SDL so it can be driven by a fake clock.

#include <vector>

#include "AttractSlides.h"
#include "doctest.h"

namespace
{

constexpr uint32_t kIdleMs = 60000;
constexpr uint32_t kDefaultDurationMs = 8000;

std::vector<AttractSlides::Slide> TwoSlides()
{
  AttractSlides::Slide first;
  first.title = "Drop targets";
  AttractSlides::Slide second;
  second.title = "Skill shot";
  second.durationMs = 2000;
  return {first, second};
}

AttractSlides::Show ShowWithSlides()
{
  AttractSlides::Show show(kIdleMs, kDefaultDurationMs);
  show.SetSlides(TwoSlides());
  return show;
}

}  // namespace

TEST_CASE("nothing is shown before the idle time has passed")
{
  AttractSlides::Show show = ShowWithSlides();

  // The clock does not start at zero on a real machine, and the show must count
  // from when it first ran rather than from the epoch -- otherwise a machine
  // whose uptime is already over a minute shows slides the moment PPUC starts.
  show.Update(true, 500000);
  CHECK_FALSE(show.Visible());

  show.Update(true, 500000 + kIdleMs - 1);
  CHECK_FALSE(show.Visible());

  show.Update(true, 500000 + kIdleMs);
  CHECK(show.Visible());
  CHECK(show.CurrentIndex() == 0);
}

TEST_CASE("a game running keeps the slides away and resets the countdown")
{
  AttractSlides::Show show = ShowWithSlides();
  show.Update(true, 1000);
  show.Update(true, 1000 + kIdleMs);
  REQUIRE(show.Visible());

  // Ball one. Down immediately, without waiting to hear about a switch.
  show.Update(false, 1000 + kIdleMs);
  CHECK_FALSE(show.Visible());

  // Five minutes of play, ticked the way the main loop ticks it.
  uint64_t gameOver = 1000 + kIdleMs + 300000;
  for (uint64_t now = 1000 + kIdleMs; now <= gameOver; now += 100)
  {
    show.Update(false, now);
    CHECK_FALSE(show.Visible());
  }

  // Game over, back to attract. The minute starts again here: the slides must
  // not appear the moment the last ball drains, however long the game was.
  show.Update(true, gameOver);
  CHECK_FALSE(show.Visible());
  show.Update(true, gameOver + kIdleMs - 1);
  CHECK_FALSE(show.Visible());
  show.Update(true, gameOver + kIdleMs);
  CHECK(show.Visible());
}

TEST_CASE("any activity takes the show down and restarts it from the first slide")
{
  AttractSlides::Show show = ShowWithSlides();
  show.Update(true, 0);
  show.Update(true, kIdleMs);
  REQUIRE(show.Visible());

  // Run on to the second slide, so that coming back to the first is a
  // distinguishable outcome.
  show.Update(true, kIdleMs + kDefaultDurationMs);
  REQUIRE(show.Visible());
  REQUIRE(show.CurrentIndex() == 1);

  // Somebody pressed a flipper button.
  show.NoteActivity(kIdleMs + kDefaultDurationMs + 10);
  CHECK_FALSE(show.Visible());

  show.Update(true, kIdleMs + kDefaultDurationMs + 10);
  CHECK_FALSE(show.Visible());

  // And a full minute of nothing brings it back at the beginning.
  show.Update(true, kIdleMs + kDefaultDurationMs + 10 + kIdleMs);
  CHECK(show.Visible());
  CHECK(show.CurrentIndex() == 0);
}

TEST_CASE("slides advance on their own duration and wrap round")
{
  AttractSlides::Show show = ShowWithSlides();
  show.Update(true, 0);
  show.Update(true, kIdleMs);
  REQUIRE(show.Visible());
  CHECK(show.CurrentIndex() == 0);
  CHECK(show.CurrentSinceMs() == kIdleMs);

  // The first slide carries no duration of its own, so it runs for the default.
  show.Update(true, kIdleMs + kDefaultDurationMs - 1);
  CHECK(show.CurrentIndex() == 0);
  show.Update(true, kIdleMs + kDefaultDurationMs);
  CHECK(show.CurrentIndex() == 1);
  CHECK(show.CurrentSinceMs() == kIdleMs + kDefaultDurationMs);

  // The second carries 2000, which must be honoured rather than the default.
  uint64_t second = kIdleMs + kDefaultDurationMs;
  show.Update(true, second + 1999);
  CHECK(show.CurrentIndex() == 1);
  show.Update(true, second + 2000);
  CHECK(show.CurrentIndex() == 0);
}

TEST_CASE("a single slide stays up rather than flickering")
{
  AttractSlides::Show show(kIdleMs, kDefaultDurationMs);
  AttractSlides::Slide only;
  only.title = "Skill shot";
  show.SetSlides({only});

  show.Update(true, 0);
  show.Update(true, kIdleMs);
  REQUIRE(show.Visible());

  for (uint64_t now = kIdleMs; now < kIdleMs + 60000; now += 1000)
  {
    show.Update(true, now);
    CHECK(show.Visible());
    CHECK(show.CurrentIndex() == 0);
  }
}

TEST_CASE("a game with no slides never shows anything")
{
  AttractSlides::Show show(kIdleMs, kDefaultDurationMs);
  CHECK(show.Empty());

  show.Update(true, 0);
  show.Update(true, kIdleMs * 10);
  CHECK_FALSE(show.Visible());
}

TEST_CASE("loading new slides while a show runs takes it down")
{
  AttractSlides::Show show = ShowWithSlides();
  show.Update(true, 0);
  show.Update(true, kIdleMs);
  REQUIRE(show.Visible());

  // Nothing does this today, but a Show holding an index into a vector it no
  // longer owns would read out of bounds, and that is worth one line to rule
  // out.
  show.SetSlides({});
  CHECK_FALSE(show.Visible());
  show.Update(true, kIdleMs * 4);
  CHECK_FALSE(show.Visible());
}
