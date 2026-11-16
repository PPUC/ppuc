// Health snapshots: when they are taken, and that they reach the file.
//
// A pinball machine is switched off, not shut down, so the bus health and the
// boards' counters printed at exit never reached a log. These are taken while
// the machine runs instead.

#include <filesystem>
#include <fstream>
#include <string>

#include "HealthLog.h"
#include "doctest.h"

using Reason = HealthSchedule::Reason;

TEST_CASE("an attract snapshot every three minutes, never in a game")
{
  HealthSchedule s(20);
  CHECK(s.Poll(0, false, 0) == Reason::None);
  CHECK(s.Poll(HealthSchedule::kAttractIntervalMs - 1, false, 0) == Reason::None);
  CHECK(s.Poll(HealthSchedule::kAttractIntervalMs, false, 0) == Reason::Attract);
  CHECK(s.Poll(HealthSchedule::kAttractIntervalMs + 1, false, 0) == Reason::None);

  // A game, long enough for two intervals: nothing.
  const uint64_t start = HealthSchedule::kAttractIntervalMs + 1000;
  CHECK(s.Poll(start, true, 0) == Reason::None);
  CHECK(s.Poll(start + 3 * HealthSchedule::kAttractIntervalMs, true, 0) == Reason::None);
}

TEST_CASE("game over is not a snapshot, it starts the attract interval")
{
  HealthSchedule s(20);
  s.Poll(0, true, 0);
  const uint64_t over = 10 * HealthSchedule::kAttractIntervalMs;
  CHECK(s.Poll(over, false, 0) == Reason::None);
  CHECK(s.Poll(over + HealthSchedule::kAttractIntervalMs - 1, false, 0) == Reason::None);
  CHECK(s.Poll(over + HealthSchedule::kAttractIntervalMs, false, 0) == Reason::Attract);
}

TEST_CASE("the coin door opening takes one, also in a game, but not twice in a row")
{
  HealthSchedule s(20);
  s.Poll(0, true, 0);
  s.NoteCoinDoorOpened();
  CHECK(s.Poll(10, true, 0) == Reason::CoinDoorOpened);
  s.NoteCoinDoorOpened();
  CHECK(s.Poll(20, true, 0) == Reason::None);
  s.NoteCoinDoorOpened();
  CHECK(s.Poll(10 + HealthSchedule::kDoorMinGapMs, true, 0) == Reason::CoinDoorOpened);
}

TEST_CASE("too many errors in a minute force one, once per gap")
{
  HealthSchedule s(20);
  uint64_t t = 0;
  uint64_t errors = 0;
  s.Poll(t, true, errors);
  // 19 in a minute: below the threshold.
  for (int i = 0; i < 19; ++i)
  {
    t += 1000;
    errors += 1;
    CHECK(s.Poll(t, true, errors) == Reason::None);
  }
  t += 1000;
  errors += 1;
  CHECK(s.Poll(t, true, errors) == Reason::TooManyErrors);
  CHECK(s.ErrorsInWindow() >= 20);

  // Still bad: no second one until the gap has passed.
  const uint64_t first = t;
  Reason r = Reason::None;
  while (r == Reason::None)
  {
    t += 1000;
    errors += 5;
    r = s.Poll(t, true, errors);
  }
  CHECK(r == Reason::TooManyErrors);
  CHECK(t - first == HealthSchedule::kErrorMinGapMs);
}

TEST_CASE("errors spread over more than a minute do not count together")
{
  HealthSchedule s(20);
  uint64_t t = 0;
  uint64_t errors = 0;
  s.Poll(t, true, errors);
  for (int i = 0; i < 40; ++i)
  {
    t += 5000;  // one every five seconds: 12 a minute
    errors += 1;
    CHECK(s.Poll(t, true, errors) == Reason::None);
  }
}

TEST_CASE("a threshold of 0 never forces one")
{
  HealthSchedule s(0);
  s.Poll(0, true, 0);
  CHECK(s.Poll(1000, true, 100000) == Reason::None);
}

TEST_CASE("the log appends, and starts a new file past its size")
{
  const auto dir = std::filesystem::temp_directory_path() / "ppuc_health_log_test";
  std::filesystem::remove_all(dir);
  const auto path = dir / "logs" / "health.log";
  HealthLog log(path, 64);

  REQUIRE(log.Write({"first", "second"}));
  REQUIRE(log.Write({"third"}));
  {
    std::ifstream in(path);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(text == "first\nsecond\nthird\n");
  }

  REQUIRE(log.Write({std::string(80, 'x')}));
  // Past 64 bytes now: the next write moves it aside.
  REQUIRE(log.Write({"fresh"}));
  std::filesystem::path previous = path;
  previous += ".1";
  CHECK(std::filesystem::exists(previous));
  std::ifstream in(path);
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  CHECK(text == "fresh\n");
  std::filesystem::remove_all(dir);
}
