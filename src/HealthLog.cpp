#include "HealthLog.h"

#include <cstdio>

#ifdef _WIN32
#include <io.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

bool WriteHealthLines(std::FILE* out, const std::vector<std::string>& lines)
{
  if (out == nullptr)
  {
    return false;
  }
  bool ok = true;
  for (const std::string& line : lines)
  {
    ok = std::fputs(line.c_str(), out) >= 0 && std::fputc('\n', out) != EOF && ok;
  }
  ok = std::fflush(out) == 0 && ok;
#ifndef _WIN32
  struct stat info;
  if (fstat(fileno(out), &info) == 0 && S_ISREG(info.st_mode))
  {
    fsync(fileno(out));
  }
#else
  _commit(_fileno(out));
#endif
  return ok;
}

HealthSchedule::Reason HealthSchedule::Poll(uint64_t nowMs, bool gameRunning, uint64_t errorCount)
{
  if (!m_started)
  {
    m_started = true;
    m_lastSnapshotMs = nowMs;
    m_nextSampleMs = nowMs;
  }

  // The error window, sampled once a second.
  if (nowMs >= m_nextSampleMs)
  {
    m_nextSampleMs = nowMs + 1000;
    m_samples.push_back({nowMs, errorCount});
    while (m_samples.size() > 1 && nowMs - m_samples.front().ms > kErrorWindowMs)
    {
      m_samples.erase(m_samples.begin());
    }
    m_errorsInWindow = errorCount >= m_samples.front().count ? errorCount - m_samples.front().count : 0;
  }

  // A game ending starts the attract interval; it is not a moment to take one.
  if (m_wasRunning && !gameRunning)
  {
    m_lastSnapshotMs = nowMs;
  }
  m_wasRunning = gameRunning;

  if (m_errorsPerMinute > 0 && m_errorsInWindow >= m_errorsPerMinute &&
      (!m_haveErrorSnapshot || nowMs - m_lastErrorSnapshotMs >= kErrorMinGapMs))
  {
    m_haveErrorSnapshot = true;
    m_lastErrorSnapshotMs = nowMs;
    m_lastSnapshotMs = nowMs;
    m_doorOpened = false;
    return Reason::TooManyErrors;
  }

  if (m_doorOpened)
  {
    m_doorOpened = false;
    if (!m_haveDoorSnapshot || nowMs - m_lastDoorMs >= kDoorMinGapMs)
    {
      m_haveDoorSnapshot = true;
      m_lastDoorMs = nowMs;
      m_lastSnapshotMs = nowMs;
      return Reason::CoinDoorOpened;
    }
  }

  if (!gameRunning && nowMs - m_lastSnapshotMs >= kAttractIntervalMs)
  {
    m_lastSnapshotMs = nowMs;
    return Reason::Attract;
  }
  return Reason::None;
}
