# RS485 Bus Timing Measurement Brief

**For:** hardware/measurement work — no firmware or host source knowledge assumed.
**Purpose:** determine what the `switchReplyDelayUs` setting is actually
compensating for, and establish the real runtime cycle time on the RS485 bus.
**Status of the premise:** unverified. Everything motivating this brief is
derived from reading source code. The measurement decides whether it is true.

---

## 1. Why this matters

The IO boards use an **ADM3483** RS485 transceiver. It is slew-rate limited —
the right choice for a cabinet full of solenoid switching noise — and it is
capped at **250 kbps**. The bus currently runs at 115200 baud, so the entire
system has about 2.17× of headroom left, permanently, unless the transceiver
changes. It should not change; the noise immunity is worth more than the speed.

That makes it important to know how the existing bandwidth is spent.

Each IO board that reads switches executes a **blocking delay** before it
transmits its reply to the host, configured by `switchReplyDelayUs` and
currently set to 2000–4000 µs, plus a further settle period of one quarter of
that value. Reading the code suggests this fixed delay may consume **60–75 % of
every runtime cycle**, and — critically — it does not shrink when the baud rate
is raised. If that is right, raising the baud rate to 250 kbps would buy only
about 1.3×, while reducing the delay would be worth roughly 2.4× at the current
baud and about 5× combined with 250 kbps.

It is almost certainly **not** compensating for the ADM3483 itself: that part's
driver enable/disable propagation is in the hundreds of nanoseconds, four orders
of magnitude below 2000 µs. So it is compensating for something else, and the
three candidates are testable:

1. **Host-side USB adapter latency.** If the value was originally tuned on a Mac
   or PC using a USB-to-RS485 adapter, it may be unnecessary on a Raspberry Pi
   using the kernel RS485 driver on a real UART. USB serial adapters commonly add
   1–16 ms of latency per transaction.
2. **Board-side scheduling** — how long after the token arrives the RP2040 is
   actually ready to drive the line.
3. **Host-side scheduling jitter** — how reliably the host software is back in
   its read window.

---

## 1b. Candidate causes — check these first

**Symptom:** switch replies become unreliable — at the host *and at other
boards* — unless a delay is inserted between one board finishing its reply and
the next being polled.

On a half-duplex multi-drop bus, that symptom has a classic electrical cause,
and it is worth ruling in or out before any software timing work.

### Why the turnaround window is suspect

Between one board deasserting DE and the next asserting it, **no driver is
active and the bus is undriven**. An RS485 bus that is terminated but not
*biased* sits at roughly 0 V differential in that window — precisely the
receiver's decision threshold. The receiver output then chatters, every
listening UART sees spurious start bits, and garbage enters the RX FIFO of every
board on the bus.

That mechanism fits the symptom exactly:

- corruption occurs during turnaround, so it scales with how quickly boards
  follow one another
- a long delay "fixes" it because the garbage is received and discarded *before*
  the real frame begins; shorten the delay and the garbage collides with the
  real frame
- it degrades reception at the host **and at other boards equally**, because it
  is a property of the shared bus rather than of any one board's firmware
- it is largely insensitive to what the firmware does, which is why it resists
  being pinned to a code path

### Known bus configuration

The bus is deliberately configured as follows, and this is correct practice:

- **Termination (`R4`, `JP2`)** — active at the two physical ends: the last board
  on the bus, and the USB-RS485 adapter or the Raspberry Pi breakout board.
  Never on boards in between.
- **Bias (`R3`/`R5`, `JP1`/`JP3`)** — active at **exactly one** end, either the
  last board (when using a USB adapter) or the breakout board.

So the questions below are not about setup errors. They are about whether the
*values* are right for this load.

### Idle bias: in spec, but with thin worst-case margin

The ADM3483 datasheet (Rev. E, Table 2) specifies:

> Differential Input Threshold Voltage (V_TH): **Min −0.2 V, Max +0.2 V**,
> for −7 V < V_CM < +12 V. Input hysteresis 50 mV.

This is the classic **symmetric ±200 mV** window — the part has *no* shifted
threshold, so a logic HIGH is guaranteed only above +200 mV.

Bias is taken from the **5 V** rail (`R3` → 5 V, `R5` → GND, 620 Ω each), with
120 Ω termination at both ends (60 Ω effective):

```
5.0 V rail:  5.0 × 60 / (60 + 620 + 620) = 231 mV
−5 % rail:   4.75 × 60 / 1300            = 219 mV
−10 % rail:  4.50 × 60 / 1300            = 208 mV
```

**All above the 200 mV threshold — the configuration is compliant.** But the
margin is only 8–31 mV against the worst-case limit.

Two mitigating factors, and one aggravating one:

- The ±200 mV figure is the **worst case** over the full common-mode and
  temperature range. A typical part switches far closer to 0 mV, so typical
  devices have generous headroom. A worst-case device has almost none.
- The **50 mV hysteresis** works in the helpful direction at idle: once the
  receiver has settled HIGH, noise must drag the line below roughly 150 mV to
  flip it — about 81 mV of practical margin.
- Coming *out* of a driven-LOW state, however, the line must climb past +200 mV
  to be read HIGH again, and there the raw ~31 mV is what applies. That is
  precisely the turnaround transition of interest.

**Note on the "fail-safe" in the general description:** Table 9 specifies
fail-safe for **"Inputs Open"** only — open-circuit fail-safe, for genuinely
disconnected inputs. On a terminated bus the 120 Ω resistors hold A and B at a
defined differential, which is not an open circuit. The idle state therefore
depends entirely on the bias network, not on that feature.

### Cheap ways to increase the margin

| Change | R_t | Idle differential (5 V) | Driver load |
|---|---|---|---|
| Current configuration | 60 Ω | 231 mV — in spec, 31 mV margin | 57 Ω |
| **Open one termination jumper (`JP2`)** | 120 Ω | **441 mV** | 109 Ω |
| `R3`/`R5` → 330 Ω, keep both terminators | 60 Ω | 417 mV | 55 Ω |

**Try the jumper first** — it costs nothing, is instantly reversible, and nearly
doubles the margin. The datasheet explicitly supports single-point termination
for this part:

> *"The slew rate limited ADM3483/ADM3488 are more tolerant of imperfect
> termination."*

With driver transition times up to 1200 ns (Table 4) against a few metres of
cable, reflections settle three orders of magnitude faster than the edge itself.

**Falsifiable prediction:** if marginal bias is a contributor, this change should
allow `switchReplyDelayUs` to be reduced measurably. If the required delay does
not move, bias is exonerated and the cause lies elsewhere — see the firmware
items in `STABILIZATION_PLAN.md` §2.4, which are defects regardless of the
outcome here.

### Honest weighting

This started as the leading hypothesis on the assumption of a 3.3 V bias rail,
which would have placed the idle level *below* the guaranteed threshold. With
the actual 5 V rail the configuration is compliant, and this drops to **one
plausible contributor among several** rather than a confirmed fault. It remains
worth testing first only because the test is free.

### Reference: how the jumpers interact

For completeness, since the settings are not independent — idle differential and
driver load at 3.3 V with nominal values:

| Configuration | Bias pts | Term pts | Idle differential | Driver load |
|---|---|---|---|---|
| **Current setup** | 1 | 2 | **152 mV** | 57 Ω |
| Bias 620 Ω ×2, term 2 | 2 | 2 | 291 mV | 55 Ω |
| **Bias 330 Ω ×1, term 2** | 1 | 2 | **275 mV** | **55 Ω** |
| Bias 1, term 1 | 1 | 1 | 291 mV | 109 Ω |
| *(all jumpers, 5 boards)* | 5 | 5 | 291 mV | 22 Ω — driver overloaded |

The last row is included only to note why "close everything" is not an option:
five terminators in parallel is 24 Ω against a 54 Ω design load, and the driver
cannot develop full amplitude.

*Assumptions to confirm from the schematic:* that `R3` pulls A toward 3V3 and
`R5` pulls B toward ground, and that the bias rail is 3.3 V rather than 5 V. A
5 V rail raises every idle figure by roughly half and would make the present
620 Ω configuration comfortable.

### Also worth confirming

- Does the bus cable carry a **signal ground / reference conductor** alongside
  A/B between boards? RS-485 tolerates only about −7 V to +12 V of common-mode
  difference, and a cabinet with high-current solenoid returns can exceed that
  transiently. This would show up as errors correlated with coil activity —
  worse during gameplay than during attract.
- Daisy-chain topology throughout, or any star/stub sections?

### The measurement that confirms or kills it

Capture **CH1 (RX)** through a complete turnaround: from the last stop bit of
one board's reply, across the gap, into the start of the next frame.

- **Garbage bytes, framing errors, or edges during the gap** → confirmed. This
  is electrical, the fix is bias and termination, and no amount of firmware
  work will properly solve it.
- **A clean idle line through the whole gap** → the hypothesis is wrong, and the
  cause is in firmware or host timing. Proceed with the measurements in §5.

Do this at a **short** `switchReplyDelayUs` (200 µs or 0) where the problem
actually manifests. At 2000–4000 µs the gap is long enough to hide it.

A scope on A/B differential across the same window is even more direct if one is
available: watch whether the differential voltage collapses to near zero between
frames, and by how much.

### Suggested order of work

0. **Open one termination jumper and retry.** Before any instrumentation:
   open `JP2` at one end of the bus, raising the idle differential from 231 mV
   to 441 mV, then try reducing `switchReplyDelayUs`. This costs minutes and is
   instantly reversible. A clear improvement implicates bias margin; no change
   points at the firmware turnaround items instead.
1. **Bench, two boards, short cable, solenoid power off.** Does
   `switchReplyDelayUs = 0` work, with and without the jumpers set? If yes, the
   problem is environmental or scales with board count — both consistent with
   the hypothesis above.
2. **Add boards one at a time.** Find the count at which it breaks. Degrading as
   boards are added implicates bus loading; being fixed by bias implicates the
   floating idle line.
3. **In-machine, solenoid power off, then on.** Isolates ground shift and
   switching noise. RS485 tolerates only about ±7 V of common-mode difference,
   and a cabinet with high-current solenoid returns can exceed that if boards
   share only the power ground.
4. **Core 1 / WS2812 disabled versus enabled.** Cheap to try and worth doing,
   though rank it below the electrical checks: a bus-level symptom affecting all
   receivers does not fit a core-1 workload cause. It does test whether WS2812
   switching couples into the RS485 pair, which is a real possibility if they
   run near each other.

Bench-first is deliberate. It is faster and unambiguous, and a result there
tells you whether to keep chasing electrical or switch to firmware. Realism can
be added back in steps 3 and 4 once the mechanism is known.

## 2. Equipment

**Logic analyzer:** 4 channels minimum, 2 MS/s or better. At 115200 baud one bit
is 8.68 µs, so even 1 MS/s gives ~9 samples per bit. This is an undemanding
capture — almost any analyzer will do.

Options, cheapest first:

- **A spare Raspberry Pi Pico running sigrok-pico.** Free if one is on the
  shelf, and more than fast enough. Streams over USB, so keep captures to a few
  seconds.
- Any `fx2lafw`-compatible USB analyzer (the common inexpensive 8-channel ones).
- Saleae Logic or equivalent.

**Software:** PulseView (sigrok) or Saleae Logic 2. Both have a **UART decoder**,
which is essential — configure it for 115200 8N1 so frames appear as bytes
rather than edges.

**Also needed:** a USB cable to one board for the debug console (§6), and a way
to edit the game YAML / runtime INI between runs.

### Safety

A pinball machine contains high voltage. The solenoid supply is typically
25–50 V DC and the AC section is line voltage.

- Connect analyzer ground to **board logic ground only**, never to chassis or
  anything in the AC section.
- The three signals below are 3.3 V logic and safe to probe.
- Prefer probing with the high-power supply off where the test allows it. Switch
  tests and attract mode do not need solenoid power.
- If the analyzer is USB-connected to a laptop, be aware that its ground is now
  tied to the machine's logic ground.

---

## 3. Probe points

All three signals are RP2040 pins on the IO board. Assignments are from the
firmware source (`io-boards/src/main.cpp`, `io-boards/src/PPUC.h`); please
confirm against the schematic and identify the most accessible physical test
points.

| Channel | Signal | RP2040 pin | What it shows |
|---------|--------|-----------|---------------|
| CH0 | RS485 **TX** (MCU → transceiver DI) | GPIO 0 | when *this* board transmits |
| CH1 | RS485 **RX** (transceiver RO → MCU) | GPIO 1 | **all** bus traffic — host frames and every board's replies |
| CH2 | RS485 **DE** (driver enable) | GPIO 2 | exactly when this board drives the line |
| CH3 | *optional* | — | DE of a **second** board, to see token handoff directly |
| GND | ground reference | board GND | required |

**CH1 is the important one.** Because RS485 is a shared bus, one board's RX line
carries the entire conversation. A single board's RX plus its DE is enough to
reconstruct a complete cycle.

CH3 on a second board's DE is a nice-to-have that makes the token chain visible
at a glance, but is not required.

---

## 4. What the traffic looks like

So you know what you are looking at in the decoder.

Every frame starts with sync byte **`0xA5`**, followed by a 5-byte header, a
payload, and a 2-byte CRC. The second header byte encodes the frame type in its
low nibble. The types you will see during normal running:

| Byte 2 (low nibble) | Frame | Direction |
|---|---|---|
| `0x01` | OutputState — full coil/lamp/GI snapshot | host → all boards |
| `0x02` | SwitchState — a board's switch bitmap | board → host |
| `0x09` | SwitchNoChange — nothing changed | board → host |
| `0x0D` | SwitchRefresh | host → boards |

The steady-state cycle is:

1. Host sends one **OutputState** frame. Header byte 3 (`nextBoard`) names which
   board should answer.
2. That board replies once, with **SwitchState** or **SwitchNoChange**. Its own
   `nextBoard` byte names the next board to answer.
3. Chain continues until a reply carries `nextBoard = 0xFF` (`kNoBoard`).
4. Host sends the next OutputState frame, and the cycle repeats.

Only boards configured with `pollEvents: true` participate in the chain.
Output-only boards listen but never transmit.

A typical frame is 15–25 bytes, so about 1.3–2.2 ms of wire time at 115200 baud.

---

## 5. Measurements

### M1 — Actual cycle time

Measure the interval between the **starts of consecutive OutputState frames** on
CH1. Take at least 50 consecutive cycles; report min, max, mean.

*Predicted from source: ~17 ms for a 4-polling-board machine. If it comes back
near 4 ms, the analysis behind this brief is wrong and everything downstream
should be reconsidered.*

### M2 — Token-to-reply latency (the key number)

For this board's own reply, decompose the turnaround:

| Interval | From | To |
|---|---|---|
| **t_a** | last stop bit of the frame carrying this board's token (CH1) | DE rising edge (CH2) |
| **t_b** | DE rising edge (CH2) | first start bit of the reply (CH0) |
| **t_c** | last stop bit of the reply (CH0) | DE falling edge (CH2) |

`t_a` is firmware reaction time. `t_b` and `t_c` are where the configured delay
and settle should appear. Report min/max/mean over ≥50 replies.

### M3 — Delay sweep (the decisive test)

Repeat **M1 and M2** with `switchReplyDelayUs` set to each of:

```
4000, 2000, 1000, 500, 200, 0
```

This value is set in the runtime INI as `Runtime.SwitchReplyDelayUs`, or on the
command line as `--switch-reply-delay-us`. It is sent to the boards at startup,
so it takes effect on a restart of the host application — no reflashing needed.

**What the result means:**

- If `t_b` tracks the configured value 1:1 all the way down to 0, and the reply
  still arrives intact → the delay is pure padding and can be cut drastically.
- If reply latency hits a **floor** above zero (say it will not go below 1–2 ms
  no matter what is configured) → that floor is genuine board-side turnaround,
  and firmware work is needed rather than a config change.
- Note the lowest value at which the machine still runs cleanly (see M5).

### M4 — Host turnaround, Pi versus USB adapter

Measure the gap from the **end of the last reply in the chain** to the **start
of the next OutputState frame** on CH1. That interval is entirely host-side:
processing plus operating-system scheduling.

Run this in two configurations with everything else identical:

- **Config A:** Raspberry Pi, kernel RS485 driver on the real UART
  (`/dev/ttyAMA0` or `/dev/serial0`)
- **Config B:** PC or Mac with a USB-to-RS485 adapter

**This is the hypothesis test.** If Config B shows multi-millisecond gaps and
Config A does not, then the delay was compensating for USB adapter latency and
the Pi does not need it. That single comparison may explain the whole thing.

### M5 — Does it still work at reduced delay

For each delay value in M3, run the machine for a few minutes in attract mode
and record:

- any communication errors reported by the host
- the firmware debug counters (§6)
- subjective quality: does lamp animation look right, do switches respond

Timing structure comes from the analyzer; **error rates over long periods should
come from the firmware counters**, not from the analyzer, which cannot capture
for hours.

### M6 — 250 kbps *(only if a firmware and host rebuild is available)*

Repeat M1 and M2 at 250000 baud. Expected: wire time roughly halves, the fixed
delay does not change at all. This directly demonstrates why the delay matters
more than the baud rate.

---

## 6. Firmware debug counters

One board can print diagnostics over USB when its debug DIP switch (switch 3) is
on. Connect a USB cable to that board and open a serial terminal at 115200 baud.
Once per second it prints a line like:

```
V2DBG board=1 rx=1234 rx_crc_fail=0 raw=... raw_a5=... raw_ff=... tx=567 tx_nochange=560 xcore_drop=0
```

| Field | Meaning |
|---|---|
| `rx` | valid frames received |
| `rx_crc_fail` | frames that failed CRC — **should stay 0** |
| `tx` | frames this board transmitted |
| `tx_nochange` | of those, SwitchNoChange replies |
| `xcore_drop` | events lost between the two CPU cores — should stay 0 |

Record these before and after each timed run so the delta over a known interval
is available.

### Read this before enabling the debug DIP switch

Debug mode is **not** a passive observer. Enabling it changes three things:

1. **The watchdog is disabled.** `main.cpp` deliberately does not start the
   watchdog when USB debugging is active, because it interferes. The watchdog is
   what shuts off high-power outputs if polling stalls — so in debug mode that
   safety net is gone. **Do not run debug mode on a machine with solenoid power
   connected unless someone is watching it and can cut power.** Prefer switch
   tests and attract mode with high power off.
2. **Core 1 is stalled once per second.** The counter print wraps
   `rp2040.idleOtherCore()` around a series of blocking USB writes, halting the
   effects core for the duration.
3. **The board waits for a USB connection at boot** before continuing startup.

Consequences for this measurement:

- **All timing measurements (M1–M4, M6) must be taken with debug OFF.** Numbers
  captured with debug on are not representative of production behaviour.
- **Error-rate runs (M5) with debug on are a separate exercise** and should be
  reported as such. They tell you whether frames are being lost, but under
  conditions that differ from normal running.
- If a delay setting looks stable in debug mode, that is *not* evidence it is
  stable in production. The reverse is also possible.

This limitation is a known gap — the project currently has no way to observe
runtime health without perturbing it. Improving that is on the stabilization
plan; it is not a prerequisite for this measurement, because the analyzer
provides timing independently of the firmware.

---

## 7. What to record per run

| Field | Example |
|---|---|
| Host type and OS | Raspberry Pi 4 / Debian 12 |
| Serial interface | kernel RS485 on `/dev/ttyAMA0`, or FTDI USB adapter model |
| Baud | 115200 |
| `switchReplyDelayUs` | 2000 |
| Number of boards / of which polling | 5 / 4 |
| Game config used | Flash |
| Capture file | `flash-pi-2000us.sr` |
| M1 cycle time | min / max / mean |
| M2 t_a, t_b, t_c | min / max / mean each |
| M4 host gap | min / max / mean |
| V2DBG counters | over a stated interval |
| Subjective notes | "lamp attract animation smooth", "switch felt laggy" |

Keep the raw capture files — they can be re-analyzed later for things nobody
thought to measure the first time.

---

## 8. Deliverable

**If only one thing gets done, do step 0 of §1b:** open one termination jumper
and see how far `switchReplyDelayUs` can be reduced. That takes minutes, needs
no instruments, and may answer the whole question.

If instruments are available, add one capture of RX through a turnaround gap at
a short reply delay, with the jumpers open and then closed. The before/after is
the proof.

After that, the filled table from §7 for each configuration, plus the raw
captures. The most valuable timing comparison is M2 and M4 on the Raspberry Pi
versus a USB adapter at the same delay setting.

Send results to the maintainer; the outcome determines whether the next step is
a configuration change, a firmware change, or neither.

---

## 9. Open questions this should settle

In priority order:

1. **Is the bus electrically correct?** How many 120 Ω terminators are fitted,
   and are fail-safe bias resistors present anywhere? (§1b)
2. **Is there garbage on RX during the turnaround gap** at a short reply delay?
   This single capture confirms or kills the leading hypothesis.
3. What is the lowest `switchReplyDelayUs` at which the machine runs cleanly —
   before, and then after, any electrical fix?
4. Is the reply delay compensating for host USB latency, board-side scheduling,
   or bus turnaround? (§5, M2–M4)
5. What is the real runtime cycle time, and does it match the ~17 ms predicted
   from source?
6. Would moving to 250 kbps deliver a worthwhile gain before the delay is
   addressed? *(Predicted: no.)*

Questions 1 and 2 come first because if the bus is unbiased or over-terminated,
every timing number measured beforehand describes a system that is compensating
for an electrical fault — and would change completely once that is fixed.
