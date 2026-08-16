// Tests for DMD frame pacing.
//
// The main loop sleeps 20 microseconds. Without pacing, the renderer would
// offer roughly 50,000 frames a second into libdmdutil's 128-entry ring with a
// dozen consumer threads behind it. The last test in this file is the one that
// protects against that, and it is the reason DmdRenderer exists at all.

#include <cstdint>
#include <vector>

#include "dmd/DmdRenderer.h"
#include "doctest.h"

namespace
{
class RecordingSink final : public DmdSink
{
 public:
  void Push(const uint8_t* data, uint16_t width, uint16_t height, uint8_t r, uint8_t g, uint8_t b) override
  {
    ++pushes;
    lastWidth = width;
    lastHeight = height;
    lastR = r;
    lastG = g;
    lastB = b;
    lastFrame.assign(data, data + static_cast<size_t>(width) * height);
  }

  int pushes = 0;
  uint16_t lastWidth = 0;
  uint16_t lastHeight = 0;
  uint8_t lastR = 0;
  uint8_t lastG = 0;
  uint8_t lastB = 0;
  std::vector<uint8_t> lastFrame;
};

struct Harness
{
  Harness()
  {
    renderer.SetSink(&sink);
    renderer.SetClock([this]() { return nowMs; });
  }

  void Service() { renderer.Service(canvas); }
  void Advance(uint64_t ms) { nowMs += ms; }

  uint64_t nowMs = 1000;
  RecordingSink sink;
  DmdCanvas canvas{128, 32};
  DmdRenderer renderer;
};
}  // namespace

TEST_CASE("the first service pushes a frame")
{
  Harness h;
  h.Service();
  CHECK(h.sink.pushes == 1);
  CHECK(h.sink.lastWidth == 128);
  CHECK(h.sink.lastHeight == 32);
}

TEST_CASE("an unchanged canvas is not pushed again")
{
  Harness h;
  h.Service();
  REQUIRE(h.sink.pushes == 1);

  for (int i = 0; i < 100; ++i)
  {
    h.Advance(50);
    h.Service();
  }
  // Only keep-alive frames, one per second, not one per service.
  CHECK(h.sink.pushes <= 7);
}

TEST_CASE("a change inside the minimum frame interval waits")
{
  Harness h;
  h.Service();
  REQUIRE(h.sink.pushes == 1);

  h.canvas.Pixel(1, 1, 15);
  h.Advance(10);
  h.Service();
  CHECK(h.sink.pushes == 1);

  h.Advance(30);
  h.Service();
  CHECK(h.sink.pushes == 2);
}

TEST_CASE("redrawing identical content does not push")
{
  Harness h;
  h.canvas.Clear(0);
  h.canvas.Text(0, 0, "SCORE");
  h.Service();
  REQUIRE(h.sink.pushes == 1);

  // A static score screen is redrawn from scratch every frame. That is the
  // normal case, and it must not generate traffic.
  for (int i = 0; i < 20; ++i)
  {
    h.Advance(40);
    h.canvas.Clear(0);
    h.canvas.Text(0, 0, "SCORE");
    h.Service();
  }
  CHECK(h.sink.pushes <= 2);  // at most one keep-alive
}

TEST_CASE("genuinely new content is pushed")
{
  Harness h;
  h.Service();

  h.Advance(40);
  h.canvas.Clear(0);
  h.canvas.Text(0, 0, "ONE");
  h.Service();
  CHECK(h.sink.pushes == 2);

  h.Advance(40);
  h.canvas.Clear(0);
  h.canvas.Text(0, 0, "TWO");
  h.Service();
  CHECK(h.sink.pushes == 3);
}

TEST_CASE("a static screen still emits a keep-alive")
{
  Harness h;
  h.renderer.SetKeepAliveMs(1000);
  h.Service();
  REQUIRE(h.sink.pushes == 1);

  h.Advance(1100);
  h.Service();
  CHECK(h.sink.pushes == 2);
}

TEST_CASE("keep-alive can be switched off")
{
  Harness h;
  h.renderer.SetKeepAliveMs(0);
  h.Service();
  REQUIRE(h.sink.pushes == 1);

  for (int i = 0; i < 50; ++i)
  {
    h.Advance(100);
    h.Service();
  }
  CHECK(h.sink.pushes == 1);
}

TEST_CASE("the draw callback runs once per flush, not once per service")
{
  Harness h;
  int draws = 0;
  h.renderer.SetDrawCallback([&draws]() { ++draws; });

  for (int i = 0; i < 500; ++i)
  {
    h.Advance(1);
    h.Service();
  }

  // 500ms of 1ms ticks at ~30fps is about 15 eligible frames, not 500.
  CHECK(draws >= 10);
  CHECK(draws <= 20);
  // The callback drew nothing, so every frame after the first is identical and
  // the hash suppresses it: many draws, almost no pushes.
  CHECK(h.sink.pushes <= 2);
}

TEST_CASE("the tint reaches the sink")
{
  Harness h;
  h.canvas.SetColor(255, 120, 0);
  h.Service();
  CHECK(h.sink.lastR == 255);
  CHECK(h.sink.lastG == 120);
  CHECK(h.sink.lastB == 0);
}

TEST_CASE("ten thousand services across one simulated second push at most 31 frames")
{
  // This is the test that protects libdmdutil from the 20-microsecond main
  // loop. If it ever fails, the pacing has been removed or bypassed.
  Harness h;
  int drawn = 0;
  h.renderer.SetDrawCallback(
      [&h, &drawn]()
      {
        // Content changes every frame, so nothing is suppressed by the hash:
        // this measures the frame-rate cap alone.
        h.canvas.Clear(0);
        h.canvas.Number(0, 0, ++drawn);
      });

  for (int i = 0; i < 10000; ++i)
  {
    h.Advance(0);
    if (i % 10 == 0) h.Advance(1);
    h.Service();
  }

  CHECK(h.sink.pushes <= 31);
  CHECK(h.sink.pushes >= 20);
}

TEST_CASE("a renderer with no sink does nothing and does not crash")
{
  DmdRenderer renderer;
  DmdCanvas canvas(128, 32);
  renderer.Service(canvas);
  CHECK(renderer.GetPushCount() == 0);
}
