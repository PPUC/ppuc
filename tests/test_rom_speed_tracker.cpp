#include <vector>

#include "RomSpeedTracker.h"
#include "doctest.h"

namespace
{
// Drives a tracker the way the engine does, and records what it sends.
struct Run
{
  RomSpeedTracker tracker;
  uint64_t nowMs = 0;
  double position = 0;
  std::vector<int> sent;

  void Advance(uint64_t ms, double duty)
  {
    for (uint64_t i = 0; i < ms; i += 50)
    {
      nowMs += 50;
      position += duty * RomSpeedTracker::kSamplesPerSecond * 50 / 1000;
      uint8_t value = 0;
      if (tracker.Update(nowMs, static_cast<int>(position) % RomSpeedTracker::kMechRange, &value))
      {
        sent.push_back(value);
      }
    }
  }
};
}  // namespace

TEST_CASE("a stopped motor sends nothing")
{
  Run run;
  run.Advance(5000, 0.0);
  CHECK(run.sent.empty());
}

TEST_CASE("a steady speed is measured and sent once a second")
{
  Run run;
  run.Advance(900, 0.5);
  REQUIRE(run.sent.size() == 1);
  CHECK(run.sent[0] == doctest::Approx(128).epsilon(0.03));

  run.Advance(2000, 0.5);
  CHECK(run.sent.size() == 3);
  CHECK(run.sent[2] == run.sent[0]);
}

TEST_CASE("a lower speed is sent straight away")
{
  Run run;
  run.Advance(1000, 0.5);
  const size_t before = run.sent.size();
  run.Advance(750, 0.07);
  REQUIRE(run.sent.size() > before);
  CHECK(run.sent.back() == doctest::Approx(18).epsilon(0.2));
}

TEST_CASE("the position wrapping is not a speed")
{
  Run run;
  run.position = RomSpeedTracker::kMechRange - 100;
  run.Advance(2000, 0.5);
  REQUIRE_FALSE(run.sent.empty());
  for (int value : run.sent)
  {
    CHECK(value == doctest::Approx(128).epsilon(0.03));
  }
}

TEST_CASE("a window a run only partly covers is not a speed")
{
  // Starting and stopping halfway through a window measures half the real duty.
  Run run;
  run.Advance(400, 0.0);
  run.Advance(1050, 0.5);
  run.Advance(1000, 0.0);
  REQUIRE_FALSE(run.sent.empty());
  for (int value : run.sent)
  {
    CHECK(value == doctest::Approx(128).epsilon(0.05));
  }
}

TEST_CASE("nothing repeats once the motor has stopped")
{
  Run run;
  run.Advance(1000, 0.5);
  run.Advance(1000, 0.0);
  const size_t stopped = run.sent.size();
  run.Advance(5000, 0.0);
  CHECK(run.sent.size() == stopped);
}
