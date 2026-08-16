#pragma once

#include <cstdint>
#include <functional>

#include "DmdCanvas.h"

// Where a finished frame goes. Implemented by DmdUtilSink against
// DMDUtil::DMD, and by a recording double in the tests.
class DmdSink
{
 public:
  virtual ~DmdSink() = default;
  virtual void Push(const uint8_t* data, uint16_t width, uint16_t height, uint8_t r, uint8_t g, uint8_t b) = 0;
};

// Decides when a frame is worth pushing.
//
// This exists because of one number: the main loop sleeps MAIN_LOOP_SLEEP_US ==
// 20, so an unpaced renderer would offer ~50,000 frames a second into
// libdmdutil's 128-entry ring with a dozen consumer threads behind it. Three
// filters, all of which earn their place:
//
//   * a minimum frame interval, because 30fps is more than a score display
//     needs and matches what real DMD ROMs emit;
//   * a content hash, because a static score screen is redrawn identically
//     every single frame and that is the NORMAL case, not the exception;
//   * a keep-alive, because ZeDMD and the DMD server connection benefit from
//     traffic even when nothing changes.
class DmdRenderer
{
 public:
  using ClockFn = std::function<uint64_t()>;
  // Invoked on ticks that will actually flush, after the built-in screen is
  // drawn and before the frame is hashed. Lua DMD code therefore runs at most
  // once per pushed frame, no matter what the rules author writes.
  using DrawFn = std::function<void()>;

  DmdRenderer();

  void SetSink(DmdSink* sink) { m_sink = sink; }
  void SetClock(ClockFn clock);
  void SetDrawCallback(DrawFn draw) { m_draw = std::move(draw); }
  void SetMinFrameIntervalMs(uint32_t ms) { m_minFrameIntervalMs = ms; }
  void SetKeepAliveMs(uint32_t ms) { m_keepAliveMs = ms; }

  // Called once per main-loop tick. Cheap, and usually a no-op.
  void Service(DmdCanvas& canvas);

  uint64_t GetPushCount() const { return m_pushes; }

 private:
  static uint32_t Hash(const uint8_t* data, size_t size);

  DmdSink* m_sink = nullptr;
  ClockFn m_clock;
  DrawFn m_draw;
  uint32_t m_minFrameIntervalMs = 33;  // ~30fps
  uint32_t m_keepAliveMs = 1000;
  uint32_t m_lastHash = 0;
  uint64_t m_lastPushMs = 0;
  uint64_t m_lastFrameMs = 0;
  bool m_consideredOnce = false;
  uint64_t m_pushes = 0;
  bool m_pushedOnce = false;
};
