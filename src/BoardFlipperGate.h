#pragma once

#include <cstdint>
#include <mutex>

// When board flippers are live.
//
// With boardFlippers the boards drive the flippers on their own, and the only
// thing that turns them on and off is the host, through SetFlippersEnabled().
// That used to follow "a game is running", taken from the ROM's game_over
// flag. A game goes on through a tilt and a drained ball, though, so the
// flippers stayed live after a tilt until game over, and through every bonus
// count.
//
// A WPC Fliptronic ROM says when the flippers should work with its game-on
// solenoid, which is the flipper enable: it drops at every drained ball and at
// a tilt. That is "ball in play". The point of board flippers was to take that
// solenoid off the bus, where a lost frame or a resync dropped it - this signal
// comes from PinMAME on the host and never crosses the bus. So the flippers
// follow both: on while a game is running and a ball is in play, off at game
// over at once, and off between balls or after a tilt once the solenoid has
// stayed off for kBallEndOffDelayMs. The delay keeps a brief drop of the
// solenoid from dropping a held flipper; on WPC a real end of ball keeps it off
// for seconds.
//
// Thread-safe: the engine reports on its own thread, the main loop calls
// Update() for the delayed off.
class BoardFlipperGate
{
 public:
  static constexpr uint32_t kBallEndOffDelayMs = 250;

  // What to send, if anything.
  enum class Change
  {
    None,
    On,
    Off
  };

  Change SetGameRunning(bool running, uint32_t nowMs)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_gameRunning = running;
    return EvaluateLocked(nowMs);
  }

  Change SetBallInPlay(bool inPlay, uint32_t nowMs)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_ballInPlay && !inPlay)
    {
      m_ballEndedAtMs = nowMs;
    }
    m_ballInPlay = inPlay;
    return EvaluateLocked(nowMs);
  }

  Change Update(uint32_t nowMs)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    return EvaluateLocked(nowMs);
  }

 private:
  Change EvaluateLocked(uint32_t nowMs)
  {
    bool want = m_gameRunning && m_ballInPlay;
    if (!want && m_gameRunning && m_sent == State::On &&
        nowMs - m_ballEndedAtMs < kBallEndOffDelayMs)
    {
      // A game is still running and the ball only just ended. Wait.
      want = true;
    }
    const State target = want ? State::On : State::Off;
    if (target == m_sent)
    {
      return Change::None;
    }
    m_sent = target;
    return want ? Change::On : Change::Off;
  }

  enum class State
  {
    Unknown,
    On,
    Off
  };

  std::mutex m_mutex;
  bool m_gameRunning = false;
  bool m_ballInPlay = false;
  uint32_t m_ballEndedAtMs = 0;
  // Unknown at first, so the first evaluation always says something.
  State m_sent = State::Unknown;
};
