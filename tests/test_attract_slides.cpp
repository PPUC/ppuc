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

// The navigation buttons -- usually the two flipper buttons, or the cursor
// keys. These are the one kind of input that must not take the show down:
// pressing them is somebody reading, not somebody walking up.

TEST_CASE("a navigation button starts the show from hidden, at the first slide")
{
  AttractSlides::Show show = ShowWithSlides();
  show.Update(true, 1000);
  REQUIRE_FALSE(show.Visible());

  // Well before the idle time is up: a player who wants the rules should not
  // have to wait out the minute.
  show.Next(2000);
  CHECK(show.Visible());
  CHECK(show.CurrentIndex() == 0);
  CHECK(show.CurrentSinceMs() == 2000);

  // And the same from the other button, rather than starting at the last
  // slide: both buttons mean "show me", and only then "which way".
  show.NoteActivity(3000);
  REQUIRE_FALSE(show.Visible());
  show.Previous(4000);
  CHECK(show.Visible());
  CHECK(show.CurrentIndex() == 0);
}

TEST_CASE("the buttons step both ways and wrap")
{
  AttractSlides::Show show = ShowWithSlides();
  show.Next(1000);
  REQUIRE(show.CurrentIndex() == 0);

  show.Next(2000);
  CHECK(show.CurrentIndex() == 1);
  CHECK(show.CurrentSinceMs() == 2000);
  // Two slides, so forward from the last wraps to the first.
  show.Next(3000);
  CHECK(show.CurrentIndex() == 0);
  // And back from the first wraps to the last.
  show.Previous(4000);
  CHECK(show.CurrentIndex() == 1);
}

TEST_CASE("stepping resets the slide's own clock")
{
  AttractSlides::Show show = ShowWithSlides();
  show.Next(0);
  REQUIRE(show.CurrentIndex() == 0);

  // Most of the way through the first slide, then stepped. The second slide
  // must get its whole duration, not the remainder of the first.
  show.Update(true, 7000);
  REQUIRE(show.CurrentIndex() == 0);
  show.Next(7000);
  REQUIRE(show.CurrentIndex() == 1);

  show.Update(true, 8000);
  CHECK(show.CurrentIndex() == 1);
  // The second slide carries 2000 of its own.
  show.Update(true, 9000);
  CHECK(show.CurrentIndex() == 0);
}

TEST_CASE("pausing holds the slide until it is released")
{
  AttractSlides::Show show = ShowWithSlides();
  show.Next(0);
  REQUIRE(show.CurrentIndex() == 0);
  CHECK_FALSE(show.Paused());

  show.TogglePause();
  CHECK(show.Paused());

  // Five minutes of somebody reading. Nothing moves.
  for (uint64_t now = 0; now < 300000; now += 1000)
  {
    show.Update(true, now);
    CHECK(show.CurrentIndex() == 0);
    CHECK(show.Visible());
  }

  // Stepping still works while held, and leaves it held: a reader moving on
  // at their own pace has not asked for the timer back.
  show.Next(300000);
  CHECK(show.CurrentIndex() == 1);
  CHECK(show.Paused());

  show.TogglePause();
  CHECK_FALSE(show.Paused());
  show.Update(true, 302000);
  CHECK(show.CurrentIndex() == 0);
}

TEST_CASE("a hold does not survive the show coming down")
{
  AttractSlides::Show show = ShowWithSlides();
  show.Next(0);
  show.TogglePause();
  REQUIRE(show.Paused());

  // Somebody walked up. The next show is for whoever comes next, and it should
  // not begin frozen on a slide the last person was reading.
  show.NoteActivity(1000);
  CHECK_FALSE(show.Paused());

  show.Update(true, 1000 + kIdleMs);
  REQUIRE(show.Visible());
  CHECK_FALSE(show.Paused());
}

TEST_CASE("pausing does nothing when no show is running")
{
  AttractSlides::Show show = ShowWithSlides();
  show.TogglePause();
  CHECK_FALSE(show.Paused());
  CHECK_FALSE(show.Visible());
}

TEST_CASE("the buttons do nothing on a game with no slides")
{
  AttractSlides::Show show(kIdleMs, kDefaultDurationMs);
  show.Next(1000);
  CHECK_FALSE(show.Visible());
  show.Previous(2000);
  CHECK_FALSE(show.Visible());
}

// The fade. Arithmetic with edge cases, and every one of them looks like a
// flicker on a real screen rather than like a failure.

TEST_CASE("a slide fades in from black and out again")
{
  constexpr uint32_t kDuration = 8000;
  constexpr uint32_t kFade = 400;

  // Full black at the instant it comes up.
  CHECK(AttractSlides::FadeAlpha(0, kDuration, kFade, false) == 255);
  // Half way in, half dimmed.
  CHECK(AttractSlides::FadeAlpha(200, kDuration, kFade, false) == 127);
  // Clear once the fade is over, and stays clear through the middle.
  CHECK(AttractSlides::FadeAlpha(400, kDuration, kFade, false) == 0);
  CHECK(AttractSlides::FadeAlpha(4000, kDuration, kFade, false) == 0);
  // And back to black as its time runs out.
  CHECK(AttractSlides::FadeAlpha(7600, kDuration, kFade, false) == 0);
  CHECK(AttractSlides::FadeAlpha(7800, kDuration, kFade, false) == 127);
  CHECK(AttractSlides::FadeAlpha(7999, kDuration, kFade, false) > 250);
}

TEST_CASE("a held slide does not dim underneath the person reading it")
{
  constexpr uint32_t kDuration = 8000;
  constexpr uint32_t kFade = 400;

  // The fade in still runs: it is what the eye expects when a slide arrives.
  CHECK(AttractSlides::FadeAlpha(0, kDuration, kFade, true) == 255);
  CHECK(AttractSlides::FadeAlpha(400, kDuration, kFade, true) == 0);

  // But nothing fades out, however long they hold it -- including well past
  // the duration it would have had.
  CHECK(AttractSlides::FadeAlpha(7900, kDuration, kFade, true) == 0);
  CHECK(AttractSlides::FadeAlpha(300000, kDuration, kFade, true) == 0);
}

TEST_CASE("the fade has no edges to fall off")
{
  // Switched off.
  CHECK(AttractSlides::FadeAlpha(0, 8000, 0, false) == 0);
  CHECK(AttractSlides::FadeAlpha(4000, 8000, 0, false) == 0);

  // Past the end, which happens on the pass where a slide is about to be
  // replaced: no underflow, no wrap to fully black.
  CHECK(AttractSlides::FadeAlpha(8000, 8000, 400, false) == 0);
  CHECK(AttractSlides::FadeAlpha(99999, 8000, 400, false) == 0);

  // A slide shorter than two fades never reaches full brightness, and the
  // honest answer is a dim slide rather than a wrong one.
  const uint8_t midway = AttractSlides::FadeAlpha(250, 500, 400, false);
  CHECK(midway > 0);
  CHECK(midway < 255);
}
