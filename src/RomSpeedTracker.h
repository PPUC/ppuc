#pragma once

#include <cstdint>

// Works out how fast a ROM is running a motor, from a PinMAME mech that counts.
//
// A ROM sets a motor's speed by pulsing its driver, and the on/off coil state
// hides that. A mech with no switches does see it: in fast mode it samples the
// raw driver state 480 times a second and moves one position for each sample
// that finds it on, so the rate its position changes at is the duty.
//
// No dependency on the engine, so it can be tested without one.
class RomSpeedTracker
{
 public:
  // Position * length * 60 has to fit an int inside PinMAME's mech code, which
  // is what limits this. It wraps long before any window does.
  static constexpr int kMechRange = 5000;
  static constexpr int kSamplesPerSecond = 480;
  static constexpr uint64_t kWindowMs = 250;
  // About 3%, more than a steady ROM wanders by, so a steady speed sends nothing.
  static constexpr int kMinChange = 8;
  // A trigger frame is not corrected by the next one the way output state is.
  static constexpr uint64_t kRepeatMs = 1000;

  // Feed it the mech's position. Returns true when the board should be told
  // *pValue, a scale from 1 to 255.
  bool Update(uint64_t nowMs, int position, uint8_t* pValue);

 private:
  bool m_started = false;
  bool m_sent = false;
  int m_lastPosition = 0;
  uint64_t m_windowStartMs = 0;
  uint64_t m_lastSentMs = 0;
  uint8_t m_lastSent = 0;
  // The last two windows' duty. A run starts and ends part way through a window,
  // so a window only counts once both of its neighbours saw the motor running.
  int m_before = 0;
  int m_middle = 0;
};
