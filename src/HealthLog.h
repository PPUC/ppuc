#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// The machine's health, written to the log ppuc already has.
//
// A pinball machine is switched off, not shut down, so nothing printed at exit
// - the bus health, the boards' own counters - ever reached the log. These are
// snapshots of the same information, taken while the machine runs.
//
// Where stdout leads, for deciding how a snapshot is made to survive.
enum class HealthLogSink
{
  File,   // a regular file: fsync it
  Pipe,   // a pipe or socket - tee, the journal: sync once the reader has it
  Other,  // a terminal or a device: nothing to sync
};

HealthLogSink ClassifyHealthLog(int fd);

// How long after a snapshot into a pipe the whole system is synced. The
// reader - zedmdos runs ppuc through `tee -a` into the USB stick's log - has
// to have written the lines first, and it holds the file open, so the
// stick's `flush` mount option, which acts on close, never applies. Without
// this a snapshot sat in the page cache for up to half a minute, long enough
// to be lost to the power switch.
constexpr int64_t kHealthPipeSyncDelayMs = 1000;

// Writes the lines to `out`, each with a newline, and flushes. When `out` is
// a regular file it is also fsynced; when it is a pipe or socket, sync() runs
// kHealthPipeSyncDelayMs later on a thread of its own, so the main loop never
// waits for the card or the stick. A terminal is only flushed. Returns false
// if writing failed.
bool WriteHealthLines(std::FILE* out, const std::vector<std::string>& lines);

// When to take a snapshot. No clock or I/O of its own, so it can be tested.
//
// - The coin door opening: high power is off anyway, and somebody opening it is
//   usually looking for whatever just went wrong. At most every kDoorMinGapMs.
// - Every kAttractIntervalMs without a game, counted from the last snapshot or
//   from the end of the last game. Never on game over itself: the ROM may be
//   taking initials, offering a buy-in or playing its end-of-game show.
// - Too many bus errors in a minute, game or not: errorsPerMinute or more
//   recovery events in the last kErrorWindowMs. At most every
//   kErrorMinGapMs, so a bus that stays bad does not stop the game every
//   minute. 0 turns this off.
class HealthSchedule
{
 public:
  enum class Reason
  {
    None,
    CoinDoorOpened,
    Attract,
    TooManyErrors,
  };

  static constexpr uint64_t kAttractIntervalMs = 3 * 60 * 1000;
  static constexpr uint64_t kDoorMinGapMs = 30 * 1000;
  static constexpr uint64_t kErrorWindowMs = 60 * 1000;
  static constexpr uint64_t kErrorMinGapMs = 5 * 60 * 1000;

  explicit HealthSchedule(uint32_t errorsPerMinute) : m_errorsPerMinute(errorsPerMinute) {}

  void NoteCoinDoorOpened() { m_doorOpened = true; }

  // errorCount: every bus recovery event since the start, cumulative. Called
  // often; the error window is sampled once a second.
  Reason Poll(uint64_t nowMs, bool gameRunning, uint64_t errorCount);

  // The errors counted in the last window, as of the last Poll().
  uint64_t ErrorsInWindow() const { return m_errorsInWindow; }

 private:
  struct Sample
  {
    uint64_t ms;
    uint64_t count;
  };

  uint32_t m_errorsPerMinute;
  bool m_started = false;
  bool m_doorOpened = false;
  bool m_wasRunning = false;
  uint64_t m_lastSnapshotMs = 0;
  uint64_t m_lastDoorMs = 0;
  bool m_haveDoorSnapshot = false;
  uint64_t m_lastErrorSnapshotMs = 0;
  bool m_haveErrorSnapshot = false;
  uint64_t m_nextSampleMs = 0;
  uint64_t m_errorsInWindow = 0;
  std::vector<Sample> m_samples;
};
