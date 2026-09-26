#include "BallTroughWatch.h"

namespace BallTroughWatch
{

void NoteSwitch(State& state, bool closed, uint64_t nowMs)
{
    state.closedSinceMs = closed ? nowMs : 0;
    state.representAtMs = 0;
}

Action Update(State& state, bool gameRunning, uint64_t nowMs, uint32_t graceMs, uint64_t gapMs)
{
    // Finish what was started, even if the game ended in between: leaving the
    // ROM with a switch we opened and never closed would be worse than either
    // acting or not acting.
    if (state.representAtMs != 0)
    {
        if (nowMs >= state.representAtMs)
        {
            state.representAtMs = 0;
            return Action::SendClose;
        }
        return Action::None;
    }

    if (!gameRunning || state.closedSinceMs == 0)
    {
        return Action::None;
    }
    if (nowMs - state.closedSinceMs < graceMs)
    {
        return Action::None;
    }

    // The clock restarts rather than latching, so a ball the ROM still will not
    // serve is tried again a grace period later instead of on every pass. A ball
    // it does serve leaves the trough, and NoteSwitch clears this outright.
    state.closedSinceMs = nowMs;
    state.representAtMs = nowMs + gapMs;
    return Action::SendOpen;
}

}  // namespace BallTroughWatch
