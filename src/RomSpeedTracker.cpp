#include "RomSpeedTracker.h"

#include <algorithm>
#include <cstdlib>

bool RomSpeedTracker::Update(uint64_t nowMs, int position, uint8_t* pValue)
{
  if (!m_started)
  {
    m_started = true;
    m_lastPosition = position;
    m_windowStartMs = nowMs;
    return false;
  }

  const uint64_t elapsedMs = nowMs - m_windowStartMs;
  if (elapsedMs < kWindowMs)
  {
    return false;
  }

  const int moved = ((position - m_lastPosition) % kMechRange + kMechRange) % kMechRange;
  const int possible = static_cast<int>(elapsedMs * kSamplesPerSecond / 1000);
  const int duty = std::clamp((moved * 255 + possible / 2) / std::max(possible, 1), 0, 255);
  m_lastPosition = position;
  m_windowStartMs = nowMs;

  const int measured = m_before > 0 && duty > 0 ? m_middle : 0;
  const bool running = m_middle > 0 && duty > 0;
  m_before = m_middle;
  m_middle = duty;

  if (measured > 0 && (!m_sent || std::abs(measured - m_lastSent) >= kMinChange))
  {
    m_sent = true;
    m_lastSent = static_cast<uint8_t>(measured);
    m_lastSentMs = nowMs;
    *pValue = m_lastSent;
    return true;
  }
  if (running && m_sent && nowMs - m_lastSentMs >= kRepeatMs)
  {
    m_lastSentMs = nowMs;
    *pValue = m_lastSent;
    return true;
  }
  return false;
}
