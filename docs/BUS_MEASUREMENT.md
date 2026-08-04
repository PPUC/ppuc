# RS485 Bus Timing Measurement Brief

**For Michael.** One question, three measurements. Should be a single session.

> **Deutsche Fassung:** [`BUS_MEASUREMENT.de.md`](BUS_MEASUREMENT.de.md) covers
> everything up to the appendix. The appendix below is English only. If the two
> ever disagree, this file is the source — the German version is a translation
> of the working part, not a separate document.

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

### Refinement: the errors are remembered as happening on switch changes

Markus recalls the failures occurring when **dedicated switches changed state**.
That fits the code uncomfortably well, and it changes how to run the capture.

A dedicated switch change is the one thing on the board that raises an
**interrupt**. `Switches::onSwitchChanges` runs on the PIO IRQ and walks up to
16 registered switches. The RS485 transmit window runs with interrupts enabled
throughout — there is no `save_and_disable_interrupts()` anywhere in it:

```c
digitalWrite(rs485Pin, HIGH);                    // DE on
delayMicroseconds(RS485_MODE_SWITCH_DELAY);      // 50 µs
hwSerial->write(frame, frameBytes);              // queue into the UART FIFO
delayMicroseconds(FrameWireTimeUs(frameBytes));  // estimated wire time + 200 µs
digitalWrite(rs485Pin, LOW);                     // DE off
```

DE is dropped on an **estimate**, not on transmit-complete — `flush()` is
deliberately avoided because it "appears to hang in the board-to-host switch
reply path". The 200 µs guard in `FrameWireTimeUs()` is the entire margin
covering everything unmodelled.

So if the switch ISR fires in the window between that delay expiring and
`digitalWrite(rs485Pin, LOW)` executing, **this board keeps driving the bus for
the duration of the ISR** — exactly while the next board, which has just
received the frame naming it, is deciding to transmit.

That single mechanism explains every symptom we have: it only involves dedicated
switches, only fires when one actually *changes*, gets more likely with more
boards (more handoffs per cycle), and is completely masked by a large
`switchReplyDelayUs` because the next board waits it out.

**It also suggests why round 1 looked clean.** A stripped-down YAML on the bench
is a quiet bus — if no switch was toggled during the capture, the ISR never ran.
Worth confirming: were any switches actually wired and operated?

### A fix is already in, and the capture can check it

The estimate above expires roughly 200 µs *after* the frame has actually gone,
because `write()` only has to reach the 32-byte TX FIFO. So every board was
holding the bus 200 µs longer than necessary on every single frame.

The firmware now releases the driver when the UART reports the last bit is out
(`uart0`'s BUSY flag), with the old estimate kept only as a timeout, and masks
interrupts across the release itself. That gives a number worth checking on the
scope:

| | DE high, per switch reply |
|---|---|
| Before | ~1154 µs (955 µs frame + 200 µs guard) |
| After | ~955 µs |

**If you are running firmware built after this change, DE-high per reply should
be about 200 µs shorter.** That is an easy sanity check that you are on the new
build — and 200 µs per board per cycle is bus time given back.

This has **not** been tested on hardware. It compiles and the host suite passes,
but the measurement is what confirms it. If the frames stop decoding cleanly on
CH2, suspect this change first and say so — it would mean the driver is being
released too early.

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
- **At least one dedicated switch, wired and configured, that you can operate
  repeatedly.** Given the refinement above this is nearly as important as the
  board count. A microswitch, a button, or just a wire you short to ground by
  hand is fine — it needs to generate real edges on a switch input, on the board
  whose DE you are watching.
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
| CH3 | *if available:* a **switch input** | GPIO 3–18 | board **A** |
| GND | ground | — | required |

CH3 is optional but makes M1 far easier to read: it puts the switch edge, the
ISR it triggers and board A's DE release on the same screen. If you only have
three channels, drop CH2 rather than CH3 — the switch-to-DE relationship matters
more here than the decoded traffic. Which GPIO depends on which port the switch
is on; port *n* maps to GPIO *n+2* on IO_16_8_1.

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

**Run it twice, and the difference between the two runs is the experiment:**

| Run | Condition |
|-----|-----------|
| **1a — quiet** | No switch touched. This should reproduce round 1: clean, no overlap. |
| **1b — active** | Operate the dedicated switch continuously throughout the capture. |

If the gap collapses or goes negative only in 1b, the interrupt hypothesis is
confirmed and we know what to fix. If 1a and 1b look identical, it is wrong and
that is equally worth knowing.

Do both at `--switch-reply-delay-us 2000` and at `0`. The delay is expected to
mask the effect, so **1b at delay 0 is the run most likely to show it** — start
there if time is short.

**Any overlap at all, however brief, is the answer.** A single sample with both
DE lines high is enough — no need to characterise it further.

Toggling the switch by hand at a random rate is fine and arguably better than a
regular one: the ISR has to land inside a window of a couple of hundred
microseconds, so hitting it is a matter of enough attempts rather than precise
timing. Operate it steadily for the length of the capture.

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
| Switch being operated? | yes, continuously / no |
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
