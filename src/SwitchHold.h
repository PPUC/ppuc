#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>

// Keeps every switch state the ROM is told about in place long enough for the
// ROM to accept it.
//
// A ROM does not take a switch on a single reading. It scans the matrix in its
// interrupt and accepts a state only once it has read it several times in a
// row; on real hardware that filters contact bounce. A ball glancing off a
// standup target closes it for a few milliseconds, and by the time that closure
// has crossed the bus it is however long lay between two switch polls, 4 ms by
// default. That is right at the edge of what a ROM accepts, so some hits count
// and some do not.
//
// So a state that reaches the engine stays there for at least the hold time
// before the next change follows. The first edge always goes out at once:
// nothing about how quickly the ROM sees a closure changes, which matters for an
// opto in front of a diverter. Only a reversal that comes back sooner is held
// back until the hold time has passed. It works the same in both directions,
// because for a normally closed switch -- some optos -- the short event is the
// open.
//
// A change that comes back to the state already forwarded while a reversal is
// still held cancels that reversal: the ROM sees one longer closure instead of
// two it could not tell apart. Two hits on one switch inside the hold time are
// therefore one hit. Keep the hold below half the period of the fastest switch
// the machine has -- a spinner -- or its turns merge.
//
// Free of the engine and of SDL so it can be tested on its own. Thread safe: the
// rules may inject switches from the coil poll thread while the main loop
// forwards the boards' own.
class SwitchHold
{
 public:
  using Sink = std::function<void(int number, uint8_t state)>;

  // 0 turns the hold off: every change is forwarded as it comes.
  void SetMinHoldMs(uint32_t minHoldMs);
  uint32_t GetMinHoldMs() const;

  // Forwards the change through `sink` now, or holds it until Service() finds
  // it due.
  void Submit(int number, uint8_t state, uint64_t nowMs, const Sink& sink);

  // Forwards every held change whose time has come. Call every main-loop pass.
  void Service(uint64_t nowMs, const Sink& sink);

  // Forgets everything, for a new engine session.
  void Reset();

  // Whether a change of this switch is being held back.
  bool IsHolding(int number) const;

 private:
  struct Entry
  {
    uint8_t forwarded = 0;
    uint64_t forwardedAtMs = 0;
    bool pending = false;
    uint8_t pendingState = 0;
  };

  mutable std::mutex m_mutex;
  uint32_t m_minHoldMs = 0;
  std::unordered_map<int, Entry> m_entries;
};
