# RS485 Bus Timing Measurement Brief

**For Michael.** One question, three measurements. Should be a single session.

The previous version of this document was five times this length and led with
electrical analysis. That was the wrong shape for someone fitting the work
around their own time, and it cost a session — see below. The background is
still here, at the end, where it stays out of the way.

---

## What round 1 taught us

Two boards on the bench, stripped-down YAML, 115200 baud. Result: a frame every
~10 ms, clean 3.54 V levels, valid v2 framing (`A5 09 01 25 01 01` decodes as
sync / SwitchNoChange / nextBoard 1 / sequence 0x25 / epoch 1), and **no sign of
the 2000 µs delay**.

That was correct, and the fault was in this brief. `switchReplyDelayUs`
**defaults to 0** everywhere — firmware, libppuc and the `ppuc` binary. It is
not a YAML field, so no `.yml` can contain it. The old brief said it was
"currently set to 2000–4000 µs" and never said to set it, so a bench run on
defaults had nothing to show.

Two things follow, and both are useful:

- **Two boards with no turnaround delay at all are stable.** That is a real
  bound. The problem is not that the delay is always needed; it is that
  something about *more* boards needs it.
- Framing, CRC and sequencing are healthy on a quiet bus. Suspicion moves to
  what happens when several boards take turns.

---

## The question this round answers

> **Do two boards ever drive the bus at the same time, and how close do they
> come to it?**

Nothing else. If two drivers overlap, that is the cause. If they never overlap
even at delay 0, the cause is elsewhere and we stop looking here.

---

## Before you start: force the delay explicitly

This is the step that was missing last time. The value must be set on the
command line or in the INI — it is never in the game YAML:

```
ppuc … --switch-reply-delay-us 2000
```

or in the INI file:

```
SwitchReplyDelayUs = 2000
```

With neither, it is **0**. Please note which you used for each capture.

---

## Setup

- **Four or more boards on the bus.** The one hard requirement. Two boards
  cannot show contention between different boards' turnarounds, which is the
  entire hypothesis. No playfield or real loads needed — the boards only have to
  be addressed and reply.
- Termination and bias as you normally run them. You have said this is correct
  and I am not asking you to re-check it.
- Note the host: Raspberry Pi breakout or USB adapter.

---

## Probe points

RP2040 pins, from `io-boards/src/main.cpp` and `io-boards/src/PPUC.h`:

| Channel | Signal | Pin | On which board |
|---------|--------|-----|----------------|
| CH0 | **DE** (driver enable) | GPIO 2 | board **A** |
| CH1 | **DE** (driver enable) | GPIO 2 | board **B** |
| CH2 | RS485 **RX** | GPIO 1 | either — carries all bus traffic |
| GND | ground | — | required |

**The two DE channels are the measurement.** Last time the capture was
single-ended UART, which shows the conversation but cannot show two drivers
fighting. DE shows exactly when each board is driving the line.

Pick two boards **adjacent in the polling order**, since that is where a handoff
happens. If the order is not obvious, board 1 and board 2 are fine.

**What the DE trace should look like**, so an offset is not mistaken for a
fault. The firmware waits a fixed **50 µs** (`RS485_MODE_SWITCH_DELAY`) after
asserting DE before the first bit goes out, and again around releasing it. So
expect DE to rise, ~50 µs of idle line, then data. The configured
`switchReplyDelayUs` sits *before* DE rises, not after.

---

## M1 — DE overlap (the primary measurement)

Trigger on CH0 falling (board A releasing the bus) and capture through board B
asserting.

Record, for several handoffs:

- **Gap**: time from board A's DE falling edge to board B's DE rising edge.
- **Overlap**: does it ever go negative — both DE high at once, even briefly?
- Whether the frame that follows decodes cleanly on CH2.

Do this at `--switch-reply-delay-us 2000` first, then at `0`. The interesting
comparison is how the gap changes and whether overlap appears.

**Any overlap at all, however brief, is the answer.** A single sample with both
DE lines high is enough — no need to characterise it further.

## M2 — Delay sweep

Same four-board setup, DE channels still connected. For each value:

```
4000, 2000, 1000, 500, 200, 0
```

Run a few minutes and record: does the machine behave, do switch reports go
missing, and what is the smallest DE-to-DE gap observed.

What we want is the lowest delay that is still reliable — that determines how
much of the cycle is spent waiting rather than working.

## M3 — Where it breaks

If M2 finds a value that misbehaves, capture one failure: CH2 around the moment
a switch report goes missing or arrives corrupted, with both DE channels. One
good capture of the failure is worth more than many of it working.

---

## What to record per run

Short is fine — a line per run plus screenshots:

| Field | Example |
|-------|---------|
| Boards on bus | 4 |
| `switchReplyDelayUs` | 2000 |
| Host | Pi breakout / USB adapter |
| Min DE-to-DE gap | 82 µs |
| Any DE overlap seen | no |
| Behaviour | stable 5 min / switches dropped |

---

## What this settles

- Whether board-to-board turnaround contention causes the instability seen with
  more boards, or whether it is something else.
- What the delay can safely be reduced to, which is throughput we get back.

If M1 shows no overlap at any delay value, the turnaround hypothesis is dead and
the next suspects are the receiver's idle-bias margin (appendix) and the
firmware's handling of the reply window. Either way we will know.

---
---

# Appendix — electrical background

Kept for reference. **Not** needed to do the measurements above.

## Why the turnaround window is suspect

Each board waits `switchReplyDelayUs` before asserting DE, and a further settle
period of one quarter of that (capped at 2000 µs) after releasing it. The second
is derived from the first, so they cannot be tuned independently — a short
pre-TX delay with a generous post-TX settle is not currently expressible.
`STABILIZATION_PLAN.md` §2.4 proposes splitting them. Across the sweep values
above the cap never engages, so the settle is simply delay ÷ 4.

The pre-TX delay is the one that does work on the wire: it waits for the
previous transmitter's line release to settle before this board drives. The
post-TX settle stalls the board's own core after it has already gone high-Z, and
protects nothing on the bus.

## Idle bias: in spec, with a thin worst-case margin

The ADM3483 (Rev. E, Table 2) has a **symmetric ±200 mV** differential input
threshold with 50 mV hysteresis, and only open-circuit fail-safe — not idle
fail-safe. A logic HIGH is guaranteed only above +200 mV.

Bias is taken from the **5 V** rail (`R3` → 5 V, `R5` → GND, 620 Ω each) with
120 Ω termination at both ends, 60 Ω effective. That is compliant. An earlier
draft of this analysis assumed a 3.3 V bias rail, which would have put the idle
level below the guaranteed threshold; with 5 V it does not.

So this is **one plausible contributor among several**, not a confirmed fault.
It stays on the list only because it is free to check while the probes are
already attached: park them on A/B with the bus idle and note the DC level.
Comfortably above +200 mV closes the question.

`R3`, `R4` and `R5` are inactive unless `JP1`, `JP2` and `JP3` are closed, and
the bus is configured with termination and bias at the two ends only.

## Baud rate

The bus runs at 115200 (`kBaudRate` in `PPUCProtocolV2.h`), about 46 % of the
ADM3483's 250 kbps ceiling. There is real headroom, but **do not change it for
these measurements** — it would confound exactly the comparison we need. It is a
separate question for once the stability one is answered.
