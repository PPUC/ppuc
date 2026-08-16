#include "DmdRenderer.h"

#include <chrono>

namespace
{
uint64_t SteadyNowMs()
{
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}
}  // namespace

DmdRenderer::DmdRenderer() : m_clock(SteadyNowMs) {}

void DmdRenderer::SetClock(ClockFn clock) { m_clock = clock ? clock : ClockFn(SteadyNowMs); }

uint32_t DmdRenderer::Hash(const uint8_t* data, size_t size)
{
  // FNV-1a. Not a checksum with adversaries in mind -- just a cheap way to
  // notice that this frame is byte-identical to the last one.
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < size; ++i)
  {
    hash ^= data[i];
    hash *= 16777619u;
  }
  return hash;
}

void DmdRenderer::Service(DmdCanvas& canvas)
{
  if (m_sink == nullptr)
  {
    return;
  }

  const uint64_t now = m_clock();
  const bool keepAliveDue = m_pushedOnce && m_keepAliveMs != 0 && (now - m_lastPushMs) >= m_keepAliveMs;

  // The frame-rate cap comes first and applies unconditionally. This is the
  // filter that protects libdmdutil from a 20-microsecond main loop.
  //
  // It is keyed off the last frame CONSIDERED, not the last frame pushed. A
  // suppressed frame does not advance the push timestamp, so keying off that
  // would make every tick eligible again as soon as a static screen went one
  // interval without traffic -- and the draw callback would run 500 times a
  // second while pushing nothing.
  if (m_consideredOnce && now - m_lastFrameMs < m_minFrameIntervalMs)
  {
    return;
  }
  m_lastFrameMs = now;
  m_consideredOnce = true;

  if (m_draw)
  {
    // With a draw callback installed, the callback owns the frame contents, so
    // the dirty flag cannot gate it: the callback is what would set the flag.
    // Running it on every eligible tick and letting the content hash suppress
    // identical frames is what keeps a static screen off the wire.
    m_draw();
  }
  else if (m_pushedOnce && !canvas.IsDirty() && !keepAliveDue)
  {
    return;
  }

  const uint32_t hash = Hash(canvas.Data(), canvas.Size());
  if (m_pushedOnce && hash == m_lastHash && !keepAliveDue)
  {
    // Identical content. Clearing the dirty flag matters: without it the canvas
    // stays dirty forever and the no-callback path never short-circuits.
    canvas.ClearDirty();
    return;
  }

  m_sink->Push(canvas.Data(), canvas.Width(), canvas.Height(), canvas.ColorR(), canvas.ColorG(), canvas.ColorB());
  canvas.ClearDirty();
  m_lastHash = hash;
  m_lastPushMs = now;
  m_pushedOnce = true;
  ++m_pushes;
}
