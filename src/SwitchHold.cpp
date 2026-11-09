#include "SwitchHold.h"

#include <vector>

void SwitchHold::SetMinHoldMs(uint32_t minHoldMs)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_minHoldMs = minHoldMs;
}

uint32_t SwitchHold::GetMinHoldMs() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_minHoldMs;
}

void SwitchHold::Submit(int number, uint8_t state, uint64_t nowMs, const Sink& sink)
{
  const uint8_t value = state == 0 ? 0 : 1;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_entries.find(number);
    if (it == m_entries.end())
    {
      // Nothing forwarded yet, so nothing to hold against.
      m_entries.emplace(number, Entry{value, nowMs, false, 0});
    }
    else
    {
      Entry& entry = it->second;
      if (value == entry.forwarded)
      {
        if (entry.pending)
        {
          // Back where the ROM already is before the reversal went out: it never
          // happened as far as the ROM can tell.
          entry.pending = false;
          return;
        }
        // A repeat of the state already forwarded. libppuc repeats a state when
        // the boards refresh their switches; pass it on as before, but it is no
        // new edge, so the hold keeps counting from the real one.
      }
      else if (m_minHoldMs != 0 && nowMs - entry.forwardedAtMs < m_minHoldMs)
      {
        entry.pending = true;
        entry.pendingState = value;
        return;
      }
      else
      {
        entry.forwarded = value;
        entry.forwardedAtMs = nowMs;
        entry.pending = false;
      }
    }
  }
  // Outside the lock: the sink reaches the engine and must not hold this up.
  sink(number, value);
}

void SwitchHold::Service(uint64_t nowMs, const Sink& sink)
{
  std::vector<std::pair<int, uint8_t>> due;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& [number, entry] : m_entries)
    {
      if (!entry.pending || nowMs - entry.forwardedAtMs < m_minHoldMs)
      {
        continue;
      }
      entry.pending = false;
      entry.forwarded = entry.pendingState;
      // The released state is held from now, not from when it was due, so it
      // gets its full time too.
      entry.forwardedAtMs = nowMs;
      due.emplace_back(number, entry.pendingState);
    }
  }
  for (const auto& [number, state] : due)
  {
    sink(number, state);
  }
}

void SwitchHold::Reset()
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_entries.clear();
}

bool SwitchHold::IsHolding(int number) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  auto it = m_entries.find(number);
  return it != m_entries.end() && it->second.pending;
}
