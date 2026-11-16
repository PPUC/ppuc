#include "HealthLog.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#ifdef _WIN32
#include <io.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

HealthLogSink ClassifyHealthLog(int fd)
{
#ifndef _WIN32
  struct stat info;
  if (fstat(fd, &info) != 0)
  {
    return HealthLogSink::Other;
  }
  if (S_ISREG(info.st_mode))
  {
    return HealthLogSink::File;
  }
  if (S_ISFIFO(info.st_mode) || S_ISSOCK(info.st_mode))
  {
    return HealthLogSink::Pipe;
  }
  return HealthLogSink::Other;
#else
  (void)fd;
  return HealthLogSink::File;
#endif
}

#ifndef _WIN32
namespace {

// When the pending sync should run, in steady-clock milliseconds; 0 while none
// is pending. One thread at a time: a snapshot taken while one waits moves its
// deadline instead of starting another.
std::atomic<int64_t> g_syncDeadlineMs{0};

int64_t SteadyNowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void RequestDelayedSync()
{
  const int64_t deadline = SteadyNowMs() + kHealthPipeSyncDelayMs;
  if (g_syncDeadlineMs.exchange(deadline) != 0)
  {
    return;  // The waiting thread picks up the later deadline.
  }
  std::thread(
      []()
      {
        while (true)
        {
          const int64_t due = g_syncDeadlineMs.load();
          const int64_t now = SteadyNowMs();
          if (now >= due)
          {
            break;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(due - now));
        }
        // Cleared before syncing, so a snapshot arriving during the sync gets
        // a sync of its own rather than relying on this one.
        g_syncDeadlineMs.store(0);
        sync();
      })
      .detach();
}

}  // namespace
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
  switch (ClassifyHealthLog(fileno(out)))
  {
    case HealthLogSink::File:
      fsync(fileno(out));
      break;
    case HealthLogSink::Pipe:
      RequestDelayedSync();
      break;
    case HealthLogSink::Other:
      break;
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
