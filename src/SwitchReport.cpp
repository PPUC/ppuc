#include "SwitchReport.h"

#include <algorithm>
#include <cstdlib>

void SwitchReport::SetThresholdGames(uint32_t games)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_thresholdGames = games;
}

uint32_t SwitchReport::GetThresholdGames() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_thresholdGames;
}

void SwitchReport::SetMinimumGameMs(uint64_t ms)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_minimumGameMs = ms;
}

void SwitchReport::Track(int number, const std::string& description)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_descriptions[number] = description;
}

void SwitchReport::Ignore(int number)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_ignored.insert(number);
}

bool SwitchReport::IsIgnored(int number) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_ignored.count(number) != 0;
}

void SwitchReport::NoteChange(int number)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_descriptions.count(number) == 0) return;
  SwitchCount& count = m_counts[number];
  if (!count.seen || count.idleGames != 0) m_dirty = true;
  count.seen = true;
  count.idleGames = 0;
  if (m_gameRunning) m_changedThisGame.insert(number);
}

void SwitchReport::NoteGameStarted(uint64_t nowMs)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_gameRunning = true;
  m_gameStartedMs = nowMs;
  m_changedThisGame.clear();
}

bool SwitchReport::IsReportedLocked(int number, const SwitchCount& count) const
{
  return m_thresholdGames != 0 && count.seen && count.idleGames >= m_thresholdGames && m_ignored.count(number) == 0 &&
         m_descriptions.count(number) != 0;
}

std::vector<SwitchReport::Flagged> SwitchReport::NoteGameEnded(uint64_t nowMs)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  std::vector<Flagged> newlyFlagged;
  if (!m_gameRunning) return newlyFlagged;
  m_gameRunning = false;
  if (nowMs < m_gameStartedMs || nowMs - m_gameStartedMs < m_minimumGameMs)
  {
    return newlyFlagged;
  }

  ++m_gamesCounted;
  m_dirty = true;
  for (auto& [number, count] : m_counts)
  {
    if (!count.seen || m_changedThisGame.count(number) != 0) continue;
    const bool wasReported = IsReportedLocked(number, count);
    ++count.idleGames;
    if (!wasReported && IsReportedLocked(number, count))
    {
      newlyFlagged.push_back(Flagged{number, m_descriptions[number], count.idleGames});
    }
  }
  m_changedThisGame.clear();
  std::sort(newlyFlagged.begin(), newlyFlagged.end(),
            [](const Flagged& a, const Flagged& b) { return a.number < b.number; });
  return newlyFlagged;
}

std::vector<SwitchReport::Flagged> SwitchReport::FlaggedLocked() const
{
  std::vector<Flagged> flagged;
  for (const auto& [number, count] : m_counts)
  {
    if (!IsReportedLocked(number, count)) continue;
    flagged.push_back(Flagged{number, m_descriptions.at(number), count.idleGames});
  }
  std::sort(flagged.begin(), flagged.end(), [](const Flagged& a, const Flagged& b)
            { return a.idleGames != b.idleGames ? a.idleGames > b.idleGames : a.number < b.number; });
  return flagged;
}

std::vector<SwitchReport::Flagged> SwitchReport::GetFlagged() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return FlaggedLocked();
}

uint32_t SwitchReport::GetGamesCounted() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_gamesCounted;
}

void SwitchReport::Reset()
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_counts.clear();
  m_changedThisGame.clear();
  m_gamesCounted = 0;
  m_dirty = true;
}

// One line per switch seen working: "switch.47 = 3", its idle game count.
// A switch never seen has no line, which is the same as "not seen".
void SwitchReport::Load(const StateEntries& entries)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  for (const auto& [name, value] : entries)
  {
    if (value.type != StateValue::Type::Number || value.number < 0) continue;
    if (name == "games")
    {
      m_gamesCounted = static_cast<uint32_t>(value.number);
      continue;
    }
    static const std::string kPrefix = "switch.";
    if (name.compare(0, kPrefix.size(), kPrefix) != 0) continue;
    char* end = nullptr;
    const long number = std::strtol(name.c_str() + kPrefix.size(), &end, 10);
    if (end == nullptr || *end != '\0' || end == name.c_str() + kPrefix.size()) continue;
    SwitchCount& count = m_counts[static_cast<int>(number)];
    count.seen = true;
    count.idleGames = static_cast<uint32_t>(value.number);
  }
}

StateEntries SwitchReport::Save() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  StateEntries entries;
  entries.emplace_back("games", StateValue::Number(m_gamesCounted));
  std::vector<int> numbers;
  for (const auto& [number, count] : m_counts)
  {
    if (count.seen) numbers.push_back(number);
  }
  std::sort(numbers.begin(), numbers.end());
  for (const int number : numbers)
  {
    entries.emplace_back("switch." + std::to_string(number), StateValue::Number(m_counts.at(number).idleGames));
  }
  return entries;
}

bool SwitchReport::TakeDirty()
{
  std::lock_guard<std::mutex> lock(m_mutex);
  const bool dirty = m_dirty;
  m_dirty = false;
  return dirty;
}
