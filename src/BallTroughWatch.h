#pragma once

#include <cstdint>

// When to give the ROM the edge it missed on the ball trough switch.
//
// Switch state reaches the ROM as changes, never as a level. A transition the
// ROM does not act on is simply gone: the ball then rests in the trough with the
// board, the host and PinMAME all agreeing the switch is closed, and nothing
// serving it. The cure has always been to lift the ball out and drop it back,
// which is no more than presenting a fresh edge.
//
// The decision lives here, free of SDL and of the engine, because it is the part
// worth testing exhaustively: it synthesises input into a running game, and the
// conditions under which it must stay silent matter more than the ones under
// which it acts.
namespace BallTroughWatch
{

enum class Action
{
    None,
    // Send the switch open. The close follows after the gap, so that PinMAME
    // samples the matrix in between -- an open and a close in the same instant
    // is not an edge.
    SendOpen,
    SendClose,
};

struct State
{
    // When the switch was last seen closing, by the board rather than by
    // anything that may have suppressed it. 0 means open.
    uint64_t closedSinceMs = 0;
    // When the closing half of a re-presentation is due. 0 means none pending.
    uint64_t representAtMs = 0;
};

// Call every pass. `gameRunning` is false in attract and after game over, where
// a ball in the trough is exactly where it belongs and nothing should be
// synthesised.
Action Update(State& state, bool gameRunning, uint64_t nowMs, uint32_t graceMs, uint64_t gapMs);

// The switch changed on the boards. Clears anything pending: a ball that has
// left the trough needs nothing, and a ball that has just arrived starts its
// grace period now.
void NoteSwitch(State& state, bool closed, uint64_t nowMs);

}  // namespace BallTroughWatch
