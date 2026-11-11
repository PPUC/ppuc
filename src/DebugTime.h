#pragma once

#include <chrono>

// Milliseconds since ppuc started, for the --debug-* lines.
//
// One clock for every thread and translation unit that prints them: the coil
// lines come from the engine's poll thread, the switch and lamp lines from the
// main loop, and without a common time base an edge on one could not be put
// beside an edge on the other. Steady, so a clock set by the network in the
// middle of a session does not turn the log around. The first call fixes zero;
// main() makes it before anything else runs.
inline double DebugTimestampMs()
{
  static const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}
