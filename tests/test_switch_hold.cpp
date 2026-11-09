// Tests for the minimum hold of switch states sent to the ROM.
//
// The hold may delay the ROM, and on a playfield that is only acceptable where
// it is the point: a reversal that came back too soon. The cases worth pinning
// are therefore the ones where nothing may be delayed -- the first edge of a
// hit, a change after a quiet switch -- as much as the short pulse it exists
// for.

#include <utility>
#include <vector>

#include "SwitchHold.h"
#include "doctest.h"

namespace
{
constexpr uint32_t kHoldMs = 10;

struct Recorder
{
  std::vector<std::pair<int, uint8_t>> sent;
  SwitchHold::Sink Sink()
  {
    return [this](int number, uint8_t state) { sent.emplace_back(number, state); };
  }
};
}  // namespace

TEST_CASE("the first edge of a hit goes out at once")
{
  SwitchHold hold;
  hold.SetMinHoldMs(kHoldMs);
  Recorder r;

  hold.Submit(17, 1, 1000, r.Sink());
  REQUIRE(r.sent.size() == 1);
  CHECK(r.sent[0] == std::make_pair(17, uint8_t{1}));
}

TEST_CASE("a short closure is stretched to the hold time")
{
  SwitchHold hold;
  hold.SetMinHoldMs(kHoldMs);
  Recorder r;

  hold.Submit(17, 0, 0, r.Sink());
  hold.Submit(17, 1, 1000, r.Sink());
  hold.Submit(17, 0, 1003, r.Sink());  // the ball left after 3 ms
  CHECK(r.sent.size() == 2);
  CHECK(hold.IsHolding(17));

  hold.Service(1000 + kHoldMs - 1, r.Sink());
  CHECK(r.sent.size() == 2);

  hold.Service(1000 + kHoldMs, r.Sink());
  REQUIRE(r.sent.size() == 3);
  CHECK(r.sent[2] == std::make_pair(17, uint8_t{0}));
  CHECK_FALSE(hold.IsHolding(17));
}

TEST_CASE("a closure longer than the hold time is not delayed at all")
{
  SwitchHold hold;
  hold.SetMinHoldMs(kHoldMs);
  Recorder r;

  hold.Submit(17, 1, 1000, r.Sink());
  hold.Submit(17, 0, 1100, r.Sink());
  REQUIRE(r.sent.size() == 2);
  CHECK(r.sent[1] == std::make_pair(17, uint8_t{0}));
}

TEST_CASE("a short open of a normally closed switch is held just the same")
{
  SwitchHold hold;
  hold.SetMinHoldMs(kHoldMs);
  Recorder r;

  hold.Submit(40, 1, 0, r.Sink());
  hold.Submit(40, 0, 5000, r.Sink());  // beam briefly clear
  hold.Submit(40, 1, 5002, r.Sink());
  CHECK(r.sent.size() == 2);
  hold.Service(5000 + kHoldMs, r.Sink());
  REQUIRE(r.sent.size() == 3);
  CHECK(r.sent[2] == std::make_pair(40, uint8_t{1}));
}

TEST_CASE("a bounce back to the forwarded state cancels the held reversal")
{
  SwitchHold hold;
  hold.SetMinHoldMs(kHoldMs);
  Recorder r;

  hold.Submit(17, 1, 1000, r.Sink());
  hold.Submit(17, 0, 1002, r.Sink());
  hold.Submit(17, 1, 1004, r.Sink());
  CHECK_FALSE(hold.IsHolding(17));

  hold.Service(2000, r.Sink());
  // Only the closure: the ROM sees one longer hit, which it would also have
  // seen on real hardware.
  REQUIRE(r.sent.size() == 1);
  CHECK(r.sent[0] == std::make_pair(17, uint8_t{1}));

  // And the next real open is forwarded at once.
  hold.Submit(17, 0, 2100, r.Sink());
  REQUIRE(r.sent.size() == 2);
  CHECK(r.sent[1] == std::make_pair(17, uint8_t{0}));
}

TEST_CASE("the released state is held for its full time as well")
{
  SwitchHold hold;
  hold.SetMinHoldMs(kHoldMs);
  Recorder r;

  hold.Submit(17, 1, 1000, r.Sink());
  hold.Submit(17, 0, 1001, r.Sink());
  hold.Service(1010, r.Sink());  // the open goes out now
  REQUIRE(r.sent.size() == 2);

  // The next closure arrives 2 ms after that open reached the ROM.
  hold.Submit(17, 1, 1012, r.Sink());
  CHECK(r.sent.size() == 2);
  hold.Service(1020, r.Sink());
  REQUIRE(r.sent.size() == 3);
  CHECK(r.sent[2] == std::make_pair(17, uint8_t{1}));
}

TEST_CASE("a spinner slower than twice the hold time keeps every turn")
{
  SwitchHold hold;
  hold.SetMinHoldMs(kHoldMs);
  Recorder r;
  int closes = 0;
  auto sink = [&](int, uint8_t state) { closes += state; };

  // 25 ms per turn, closed for 3 ms of it: 40 turns a second.
  for (uint64_t t = 0; t < 1000; ++t)
  {
    const uint64_t phase = t % 25;
    if (phase == 0)
    {
      hold.Submit(30, 1, t, sink);
    }
    else if (phase == 3)
    {
      hold.Submit(30, 0, t, sink);
    }
    hold.Service(t, sink);
  }
  CHECK(closes == 40);
}

TEST_CASE("repeats of the forwarded state pass through without restarting the hold")
{
  SwitchHold hold;
  hold.SetMinHoldMs(kHoldMs);
  Recorder r;

  hold.Submit(17, 1, 1000, r.Sink());
  hold.Submit(17, 1, 1008, r.Sink());  // a switch refresh repeating the state
  REQUIRE(r.sent.size() == 2);
  hold.Submit(17, 0, 1010, r.Sink());  // 10 ms after the real edge
  REQUIRE(r.sent.size() == 3);
}

TEST_CASE("switches are held independently")
{
  SwitchHold hold;
  hold.SetMinHoldMs(kHoldMs);
  Recorder r;

  hold.Submit(17, 1, 1000, r.Sink());
  hold.Submit(17, 0, 1001, r.Sink());
  hold.Submit(18, 1, 1002, r.Sink());
  hold.Submit(18, 0, 1050, r.Sink());
  CHECK(hold.IsHolding(17));
  CHECK_FALSE(hold.IsHolding(18));
  CHECK(r.sent.size() == 3);
}

TEST_CASE("a hold of zero forwards everything as it comes")
{
  SwitchHold hold;
  hold.SetMinHoldMs(0);
  Recorder r;

  hold.Submit(17, 1, 1000, r.Sink());
  hold.Submit(17, 0, 1000, r.Sink());
  hold.Submit(17, 1, 1000, r.Sink());
  CHECK(r.sent.size() == 3);
}

TEST_CASE("reset forgets held changes")
{
  SwitchHold hold;
  hold.SetMinHoldMs(kHoldMs);
  Recorder r;

  hold.Submit(17, 1, 1000, r.Sink());
  hold.Submit(17, 0, 1001, r.Sink());
  hold.Reset();
  hold.Service(5000, r.Sink());
  CHECK(r.sent.size() == 1);
  // And the first change after a reset is an edge again.
  hold.Submit(17, 0, 5001, r.Sink());
  CHECK(r.sent.size() == 2);
}
