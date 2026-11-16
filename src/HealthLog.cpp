#include "HealthLog.h"

#include <cstdio>
#include <system_error>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

HealthLog::HealthLog(std::filesystem::path path, uint64_t maxBytes) : m_path(std::move(path)), m_maxBytes(maxBytes) {}

bool HealthLog::Write(const std::vector<std::string>& lines)
{
  std::error_code ec;
  if (m_path.has_parent_path())
  {
    std::filesystem::create_directories(m_path.parent_path(), ec);
  }

  const auto size = std::filesystem::file_size(m_path, ec);
  if (!ec && size > m_maxBytes)
  {
    std::filesystem::path previous = m_path;
    previous += ".1";
    std::filesystem::rename(m_path, previous, ec);
  }

  FILE* file = std::fopen(m_path.string().c_str(), "a");
  if (file == nullptr)
  {
    return false;
  }
  for (const std::string& line : lines)
  {
    std::fputs(line.c_str(), file);
    std::fputc('\n', file);
  }
  std::fflush(file);
  // The point of the file: the machine is switched off, not shut down, and a
  // line in the page cache does not survive that.
#ifdef _WIN32
  _commit(_fileno(file));
#else
  fsync(fileno(file));
#endif
  return std::fclose(file) == 0;
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
