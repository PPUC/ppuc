#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "MachineState.h"

// Which switches have stopped working, by counting the games they sat through
// without changing.
//
// What a WPC ROM shows as its test report ("check switch 47") when the coin door
// is opened. A switch that has not moved in many games is almost always broken:
// an opto with a dead LED reads closed forever, a rollover with a bent wire reads
// open forever. Neither stops a game, so nobody notices until a mode cannot be
// lit any more.
//
// Engine-neutral and vendor-neutral: it is told about switch changes and about
// games, and knows nothing about either beyond that.
//
// - A game counts once it has lasted `minimumGameMs`, so a game started and
//   abandoned straight away is not a game nobody hit the targets in.
// - Only switches seen working at least once are reported. A switch that never
//   changed since the report started is far more often one nobody uses -- a coin
//   switch on a free-play machine, a spare input -- than a broken one, and a
//   switch broken from the start is what the switch test at setup is for.
// - Ignored switches are never reported: the coin door, its buttons, the tilt
//   bob and the slam tilt, coin switches, and whatever the rules add. They can
//   legitimately sit still for a hundred games.
// - A reported switch drops off the report the moment it changes: a repaired
//   switch reports itself repaired.
//
// The counters are kept in the game folder's state/ (see MachineState.h), so the
// report covers games across power cycles, which is the only way it can ever
// reach a useful number of games.
//
// Thread-safe; in practice everything arrives on the main thread.
class SwitchReport
{
 public:
  struct Flagged
  {
    int number = 0;
    std::string description;
    uint32_t idleGames = 0;
  };

  // 0 turns the report off. It still counts, so turning it back on reports
  // immediately what it would have reported all along.
  void SetThresholdGames(uint32_t games);
  uint32_t GetThresholdGames() const;
  void SetMinimumGameMs(uint64_t ms);

  // The switches there are, with what they are called.
  void Track(int number, const std::string& description);
  void Ignore(int number);
  bool IsIgnored(int number) const;

  void NoteChange(int number);
  void NoteGameStarted(uint64_t nowMs);
  // The switches that reached the threshold with this game, for the log.
  std::vector<Flagged> NoteGameEnded(uint64_t nowMs);

  // Worst first.
  std::vector<Flagged> GetFlagged() const;
  uint32_t GetGamesCounted() const;

  // Forgets every count: after a repair session, a fresh start.
  void Reset();

  void Load(const StateEntries& entries);
  StateEntries Save() const;
  bool TakeDirty();

 private:
  struct SwitchCount
  {
    bool seen = false;
    uint32_t idleGames = 0;
  };

  std::vector<Flagged> FlaggedLocked() const;
  bool IsReportedLocked(int number, const SwitchCount& count) const;

  mutable std::mutex m_mutex;
  std::unordered_map<int, std::string> m_descriptions;
  std::unordered_set<int> m_ignored;
  std::unordered_map<int, SwitchCount> m_counts;
  std::unordered_set<int> m_changedThisGame;
  uint32_t m_thresholdGames = 10;
  uint64_t m_minimumGameMs = 30000;
  uint64_t m_gameStartedMs = 0;
  bool m_gameRunning = false;
  uint32_t m_gamesCounted = 0;
  bool m_dirty = false;
};
