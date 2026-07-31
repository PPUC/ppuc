# PPUC — Outside Assessment

**Date:** 2026-07-30
**Author:** Claude (Opus 5), commissioned by Markus Kalkbrenner as an
independent review before the stabilization phase.

**Basis:** Reading the source of `ppuc`, `libppuc`, `io-boards`, `libsdldmd`,
`config-tool`, the T2 and Flash game configs, the build system, and the
IO_16_8_1 BOM. **Nothing was run or measured on hardware.** All timing figures
below are derived from code and configuration and should be verified with a
scope or logic analyzer before being acted on.

---

## TL;DR

**PPUC works, and it works because of deliberate engineering rather than luck.**

The evidence is concrete. Flash ran at public events across multiple venues for
roughly 12 days at 8–12 hours a day — on the order of 100–144 hours of
uninterrupted public operation — without serious issues, and experienced
tournament players reported it plays like the original, or better. That last
part is a technical result, not a courtesy: it is what happens when flipper,
slingshot and bumper response moves from 40-year-old discrete driver circuits to
a deterministic board-local fast path. And it was not a soft test case. Flash's
configuration puts essentially the same load on the bus as the T2 example: four
switch-polling boards, 56 switches, 17 PWM outputs, 126 LED entries.

Several core decisions are genuinely good, and some were not obvious:

- **Using PinMAME as the game engine** instead of reimplementing rules. P-ROC,
  FAST and MPF all require rewriting the game logic. PPUC is the only approach
  that can tell an owner *"your machine plays exactly as it did in 1991, and now
  it also has video and LEDs"* — and it inherits every title PinMAME supports.
- **Snapshot-based output with dense bitmap mapping.** Constant-size frames,
  self-correcting after a lost frame, and multiball costs no more bandwidth than
  an idle playfield.
- **Board-owned pulse envelopes.** `minPulseTime` / `maxPulseTime` / hold power
  are enforced on the board for *every* output, so a hung host, an OS hiccup or
  a stalled bus cannot cook a coil. This is the right safety property in the
  right place.
- **The transceiver choice.** The ADM3483 is slew-rate-limited, trading raw
  speed for signal integrity — exactly the correct call for a cabinet full of
  solenoid switching noise.
- **A real configuration frontend.** Most firmware projects hand you a YAML file
  and wish you luck.

**What is missing is not architecture. It is the ability to change safely.**
There are no automated tests anywhere in the stack, no coil-safety validators,
and one confirmed latent gap (§4.2) that modern titles will expose. The
project's quality currently rests on one person's domain experience. That is
precisely why it works — and it is also the thing that does not scale to more
contributors.

That is the normal gap of a successful prototype, and the correct time to close
it is now: while users can still absorb breaking changes, and before the
project gets more popular. The remaining work is specific and addressable, not
foundational.

One note on how to read the rest of this document: the analysis went through
several rounds of correction with the maintainer, and **the architecture held up
better than the first pass suggested.** Two of the concerns raised initially
were narrowed substantially or withdrawn once domain context was applied, while
two more important issues surfaced that the first reading had missed. The
criticisms that survived that process are the ones worth acting on.

---

## 1. What PPUC is

A PC or Raspberry Pi runs the machine's original ROM under PinMAME and acts as
the CPU. RP2040-based IO boards in the cabinet drive the real coils, lamps, GI
and switches over an RS485 bus. The original game plays authentically, and a
layer of new capability — addressable LEDs, board-local effects, Lua rules, PUP
video, B2S, speech — is added on top without touching the ROM's logic.

The strategic bet is using PinMAME as the game engine instead of reimplementing
rules. This is the project's single biggest asset and it does not get enough
credit. P-ROC, FAST and MPF all require rewriting the game logic. PPUC is the
only approach that can tell an owner *"your machine plays exactly as it did in
1991, and now it also has video and LEDs"* — and it scales for free to every
title PinMAME supports.

Crucially, PPUC does not drive PinMAME directly — it uses **libpinmame**, which
is an embedding API with an event and delta model throughout:

- solenoids, display, sound commands, mechs and state changes arrive as
  **callbacks** (`OnSolenoidUpdated`, `OnDisplayUpdated`, `OnSoundCommand`, …)
- lamps and GI are drained as **deltas** (`PinmameGetChangedLamps`,
  `PinmameGetChangedGIs`)
- physical switches are **pushed in** with `PinmameSetSwitch`

This pairs well with the wire protocol: libpinmame emits deltas, `libppuc`
folds them into a snapshot, and the snapshot goes out at constant cost. Event
driven upstream, snapshot downstream, with the snapshot absorbing bursts for
free. It is an impedance *match* rather than a mismatch, and it is the reason
multiball costs nothing on the bus — a property of the two layers together, not
of the protocol alone.

What remains is a **quantization boundary** rather than a mismatch: PinMAME
produces instantaneous edges, the bus is periodic. Everything in §2 is about
how coarse that period has become, and §2.1 covers the one place where the
coarseness leaks into physical machine behavior.

## 2. The measurement to take first

> **Status: hypothesis, not a finding.** Everything in this section is derived
> from reading source and configuration. Nothing here has been measured on
> hardware. The arithmetic below is presented as *a reason to measure* and *what
> to measure*, not as a result. Please do not quote the cycle-time figures as
> established until someone has put an analyzer on the bus.


The IO_16_8_1 board uses an **ADM3483** RS485 transceiver (U1). It is
slew-rate-limited — exactly what you want in a cabinet full of solenoid
switching noise — and its 1/8 unit load allows many nodes on the bus. Choosing
signal integrity over raw speed in this environment is the right trade, and the
part choice is correct.

But it has a consequence that should be stated plainly:

> **250 kbps is not a milestone to work toward. It is a permanent ceiling in
> silicon.** Total remaining headroom in the entire system, forever, is 2.17×
> — unless the transceiver is changed, which would mean giving up the noise
> robustness that makes the system work at all. That is a bad trade. Treat the
> ceiling as fixed.

Which makes it worth knowing exactly how those bits are currently spent.

One code fact is not in doubt: `switchReplyDelayUs` is a blocking
`delayMicroseconds()` executed on each board before it transmits its reply
(`io-boards/src/EventDispatcher/EventDispatcher.cpp:768`), followed by a settle
period of delay/4. It is a fixed cost per polling board, and — this is the part
that matters — **it does not shrink when the baud rate is raised.**

If one works through the frame sizes for a Flash/T2 class configuration
(4 switch-polling boards, ~55 switches, ~17 outputs) at the documented 2000 µs
setting, the predicted cycle looks like this:

| Component | at 115200 | at 250000 |
|---|---|---|
| `OutputStateFrame` (~21 bytes) | 1.8 ms | 0.84 ms |
| 4 × switch reply (~15 bytes) | 5.2 ms | 2.4 ms |
| 4 × `switchReplyDelayUs` @ 2000 µs | **8.0 ms** | **8.0 ms** |
| 4 × derived post-TX settle (delay/4) | **2.0 ms** | **2.0 ms** |
| **Predicted total** | **~17 ms** | **~13 ms** |

*Again: predicted, not observed.* If it holds, the fixed delay is consuming
roughly 60–75% of every runtime cycle, and raising the baud rate to 250 kbps
would buy only about 1.3× while spending the last hardware headroom available.
Reducing the delay instead would be worth ~2.4× at the current baud and ~5×
combined with 250 kbps.

If it does not hold, the conclusions in this section fall with it — which is
exactly why measuring comes before acting.

The delay is certainly **not** compensating for the ADM3483 — its driver
enable/disable propagation is in the hundreds of nanoseconds, four orders of
magnitude below 2000 µs. It is compensating for something else, and the
candidates are testable:

1. **Host-side USB adapter latency.** If this value was tuned on a Mac with a
   USB-RS485 adapter, it may be largely unnecessary on a Raspberry Pi with the
   kernel RS485 driver. USB-serial adapters commonly add 1–16 ms of latency per
   transaction, which is enormous relative to the cycle budget. First and
   cheapest hypothesis to test.
2. **Core-0 scheduling on the RP2040** — how long after the token arrives the
   board is actually ready to drive the line.
3. **Host userspace scheduling jitter** — how reliably `libppuc` is back in its
   read window.

A logic analyzer on A/B and DE, capturing token-arrival to reply-start on both
the Pi and the Mac, would separate all three in an afternoon. Whatever the
answer, the parameter should end up **derived from measured turnaround** rather
than hand-tuned per cabinet. That also removes the "retune for every machine"
burden the current documentation describes.

### What the cycle time does and does not affect

Scoping this correctly matters, because raw millisecond figures invite
overstatement:

- **It does not affect coil pulse duration or machine feel.** The IO boards own
  the pulse envelope (§3), so the runtime cadence does not change how hard a
  kicker kicks or how long any coil is energised.
- **It does not affect the latency-critical playfield reactions** — flippers,
  slingshots, bumpers — which are handled board-locally and never traverse the
  bus.
- **It does not meaningfully affect lamp or GI presentation.** Incandescent
  lamps have thermal time constants in the tens of milliseconds and cannot show
  meaningful contrast much above 10–15 Hz. Game-level lamp logic is slower
  still: typical animation steps are 30–250 ms, and even a "fast flash" is
  roughly 4–8 Hz. Above ~20 Hz a blinking lamp reads as solidly on. (The
  ~125 Hz strobing of an original lamp matrix is *multiplexing*, not logical
  state change; the bulb integrates it.) Even at the slowest cycle predicted in
  §2, the snapshot rate is generously above what the ROM demands. PPUC's own
  effects are unaffected in any case, because they run board-locally on the
  RP2040. **This conclusion does not depend on the §2 measurement** — it holds
  at any plausible cycle time.
- **It does affect how quickly a switch change reaches PinMAME** — but for
  non-latency-critical switches the requirement is *reliable delivery*, not
  speed. Whether an eject hole fires now or a fraction of a second later is
  imperceptible; a *missed* switch is not.
- **It does affect headroom for larger machines.** Each additional polling board
  **Only boards that read switches affect the round trip.** Boards marked
  `pollEvents: true` join the token chain and each add a reply plus a fixed
  `switchReplyDelayUs` + settle. Output-only boards — additional lamps, coils,
  LED strips — simply consume the shared output snapshot and cost **nothing** in
  cycle time. So expanding a machine's output capacity is free; only adding
  switch-reading boards is not. With `kMaxBoards = 8`, the worst case is roughly
  double the current Flash/T2 configuration, and the term that grows is the
  fixed per-board delay — the one the 250 kbps ceiling cannot help with. Exact
  figures depend on the §2 measurement; the scaling behaviour does not.

So the case for reducing `switchReplyDelayUs` rests essentially on **capacity
for machines larger than Flash**, given that 250 kbps is fixed in silicon and
the delay is the only remaining lever. That is a real argument about future
scaling, not a claim that anything is currently deficient.

**One residual that is not a rate issue.** `PinmameGetChangedLamps` returns
lamps that changed *since the last call*, so a lamp that turns on and off
entirely between two drains nets to no change and is **dropped**. The risk
window equals the drain interval, whatever that interval is. On incandescent
hardware, losing a sub-frame lamp pulse is arguably faithful — it would have
been invisible. On LED replacements it would not have been. Worth being aware
of; not worth engineering around unless a real game exhibits it.

## 3. What is genuinely strong

**The v2 protocol design.** Full-snapshot output with dense bitmap indexes plus
`MappingFrame` indirection was the right call and it was not obvious. It
decouples wire size from logical numbering, keeps frames constant-size and
therefore timing-predictable, and makes the firmware idempotent — a lost frame
self-corrects on the next one, with no permanent divergence. It also has a
property worth naming explicitly: **gameplay complexity is free on the bus.**
Multiball with eight coils firing costs exactly the same bandwidth as an idle
playfield. Only additional *hardware* costs bandwidth.

**Token-ring switch polling.** For half-duplex RS485 with no collision
detection this is elegant — no arbitration, deterministic ordering,
self-terminating chain. Host-synthesized frames for absent boards mean the same
unmodified game config runs on a bench without the cabinet board. That pays off
every day of development.

**Board-owned pulse envelopes.** The best judgment call in the codebase, and
broader than it first appears. `PwmDevices::update()` applies `minPulseTime` and
`maxPulseTime` to **every** activated output, not only fast-switch ones: a host
"off" arriving before the minimum is scheduled rather than executed, and the
maximum forces the coil off regardless of what the host is doing. The host's
output bit is a *request*; the board is the authority on how long copper is
actually energised.

Three consequences worth stating explicitly:

- A hung host, an OS scheduling hiccup or a stalled bus **cannot cook a coil.**
  That is a hard safety property, enforced in the right place.
- Coil pulse fidelity is **decoupled from bus timing entirely.** How coarse the
  runtime cycle is does not change how long a kicker fires.
- The fast-flip case — stuck switch on a kicker or bumper, plus the
  must-reopen-before-refire latch after a max-pulse timeout — is then just the
  latency-critical specialisation of a general rule.

This is what separates a machine controller from a demo, and it is a large part
of why the architecture tolerates a non-realtime host at all.

*Caveat:* `maxPulseTime` is not the only valid protection, and requiring it
everywhere would be wrong — see §4.1.

**The debounce taxonomy.** `standard` / `fastFlip` / `slowStable`, with
`fastFlip` accepting the close edge immediately and debouncing only the open
edge, plus per-switch-type millisecond guidance in the README. Hard-won domain
knowledge made explicit and teachable.

**Transport stack choices.** Raspberry Pi with the kernel RS485 driver on a real
UART, rather than a USB adapter; ZeDMD over kernel SPI rather than USB. Both
avoid host-side scheduling jitter that would otherwise dominate the timing
budget. Correct calls.

**The `--game` folder** and **config-tool existing at all.** One path,
everything derived; and a web UI that exports a complete game folder including
Lua rules and Blockly. Most firmware projects hand you a YAML file and wish you
luck. This is a serious, expensive bet on non-programmer users, and the right
one given the goal of growing a homebrew community.

## 4. What is genuinely weak

**Zero automated tests.** Not sparse — zero, across six repositories, ~20k
lines of C++ and a Drupal site, for software that fires solenoids next to a
standing human. The fast-flip logic in particular — the one component
explicitly documented as preventing hardware damage — has nothing asserting
that a coil cannot refire before its switch reopens. Field operation is real
evidence, but it is evidence about *one configuration*; it cannot tell you what
the next refactor broke.

**Correctness conditions live in prose rather than in asserts.** The `AGENTS.md`
files had become session archaeology: *"confirmed on real hardware"*,
*"1h40m4s with no errors"*, *"preserve these fixes"*. Honest, and better than
nothing — but the proof of the failure mode is that they described a DMA
cutover path that had been deleted, and documented `kMaxCoilBits` as 256 when it
is 64. A new contributor would have lost hours to both. Every "preserve this"
note is a test that wants to exist.

**Silent failure in `QueueEvent()`.** A mistyped coil number in YAML yields a
dead coil and no diagnostic anywhere. That is a brutal experience for exactly
the non-expert users config-tool exists to serve, and it is close to a one-line
fix.

**The schema lives in three places** — the PHP exporter in `config-tool`, the
C++ validator in `libppuc`, and the firmware config topics in `io-boards` —
with no shared artifact. It will drift, and probably already has in at least one
optional field.

**Unfinished protocol surface.** `sequence` transmitted but unused,
`kFlagDelta` / `kFrameHeartbeat` / `kFrameError` defined but unimplemented, no
sender ID in frames. The missing sender ID is the one with teeth: a desynced
token chain cannot be *detected*, only inferred from silence.

**Bus factor of one.** Forks, dependency pins, domain knowledge and hardware all
in one head. The `AGENTS.md` effort is the right instinct for addressing it.

## 4.1 Missing configuration validators

Neither `config-tool` nor `libppuc` validates coil safety configuration, and
this is where the absence of validators has physical rather than cosmetic
consequences. Concrete rules that should exist:

**Every solenoid needs at least one thermal protection mechanism.** There are
three valid ones, and mandating any single one would be wrong:

1. `maxPulseTime > 0` — hard cutoff enforced by the board
2. **Hold power** — `holdPower` plus `holdPowerActivationTime` drop the duty
   cycle after activation to a current the coil can dissipate indefinitely, so
   unlimited on-time is safe by design
3. **Dual-winding coil with EOS** — the end-of-stroke contact removes the power
   winding mechanically, leaving only the high-resistance hold winding

A dual-wound flipper with EOS and `maxPulseTime: 0` is correctly configured. A
single-winding kicker with the identical setting is a fire risk. **The
configuration cannot currently express the difference** — there is no field
declaring a coil as dual-wound with EOS — so a validator cannot yet distinguish
them. Adding that field is a prerequisite for the check.

Other rules worth encoding:

- `minPulseTime` should be set for solenoids, so a coil cannot be
  under-energised by a short host request.
- Motors driven with end-of-stroke contacts should be validated like
  dual-winding coils: EOS declared, or `maxPulseTime` as the backstop for the
  case where the contact never closes.

These are also the natural first targets for unit tests: pure configuration
logic, no hardware required.

## 4.2 Confirmed gap: fast-switch coils can double-fire

In `PwmDevices::handleEvent()`, the `EVENT_SOURCE_SOLENOID` branch calls
`updateSolenoidOrFlasher()` with **no check for `fastSwitch[i] > 0`**
(`src/IODevices/PwmDevices.cpp:291`). For a coil that has a fast switch *and*
whose switch is also reported to the CPU for scoring — normal in WPC-era and
later games, where original-matrix switch numbers are forwarded to PinMAME by
`SendSwitchToCpu()` — both paths drive the same coil:

1. Switch closes → `handleFastSwitchEvent()` → coil fires locally
2. The same switch reaches PinMAME → the ROM commands the coil → the host bit
   arrives → `updateSolenoidOrFlasher()` fires it **a second time**

The guard `if (targetState && activated[i] == 0)` only helps while the first
pulse is still running. Once `minPulseTime` has elapsed and the output has been
deactivated, a host command arriving one bus cycle later starts a fresh pulse.
Additionally, `updateSolenoidOrFlasher()` sets `fastSwitchManagedActive[i] =
false` on host activation, so the second pulse also bypasses the
`fastSwitchWaitForRelease` stuck-switch re-arm protection.

**The fix must be narrow.** Ignoring *all* host activations for a fast-switch
coil would be wrong, because the ROM legitimately fires those coils without a
preceding switch closure — its own ball search, or deliberate in-game effects
such as World Poker Tour moving a flipper finger to indicate where the ball will
be served. The correct behaviour is to suppress **the next** host activation
after a local fast-switch fire, i.e. the ROM's echo of the event the board has
already handled, while leaving unrelated ROM-driven activations intact.

Design points that need deciding:

- The suppression must be **time-bounded**. If the ROM never echoes, a stale
  one-shot flag would otherwise swallow an unrelated legitimate command minutes
  later. A window of a few bus cycles is the natural bound.
- Rapid repeat hits (a bumper struck twice in quick succession) arm the
  suppression more than once; a small bounded counter is more robust than a
  boolean, though pairing can still drift if echo latency varies.
- The firmware cannot truly distinguish "the ROM's echo of the hit I just
  handled" from "the ROM deliberately firing this coil at almost the same
  moment." The time window is the only discriminator available, so the
  behaviour is heuristic by nature and should be documented as such.
- **Placement:** firmware is the better home. The board is the only party that
  knows whether it actually fired locally — it may have declined to, for
  instance because the stuck-switch re-arm latch was set. Host-side suppression
  in `libppuc` would be simpler, since the fast-switch mapping is already known
  from YAML, but it would sometimes suppress a command for a fire that never
  happened.

This does not affect older games such as Flash (System 4), where those coils are
not CPU-driven — consistent with it not having surfaced during field testing. It
sits directly in the path of supporting more modern titles.

## 5. Field evidence, honestly weighted

Flash ran at public events across multiple locations for roughly 12 days at
8–12 hours per day — on the order of 100–144 hours of public operation — without
serious issues, and experienced tournament players reported it plays like the
original or better.

This is stronger evidence than it first appears. Flash is a simple *game*, but
it is not a light *bus load*: 4 boards, all four polling switches, 56 switches,
17 PWM outputs, 126 LED entries — essentially the same transport load as the T2
example config. So this is validation at a representative configuration, not a
toy one.

"Plays like the original, if not better" is also a technical result rather than
just a compliment. It is what you would expect when flipper, slingshot and
bumper response moves from 40-year-old discrete driver circuits to a
deterministic board-local fast path. It confirms that the latency architecture —
keeping the latency-critical reactions off the host entirely — is sound.

What it does **not** validate is scaling: more boards per machine, and the
device classes still to come.

## 6. Roadmap implications

Multiball, motors and magnets interact with the above in specific ways:

- **Multiball** is nearly free on the bus, by design. The cost lands in effects
  arbitration and rules, not transport.
- **Magnets** belong in the same board-local safety category as fast-flip, and
  carry the same hazard profile: a stuck-on magnet coil is as dangerous as a
  stuck kicker. Whatever test harness gets built for fast-flip should be
  designed to cover magnets from the start.
- **Motors** typically have end-of-stroke contacts, so they fit the existing
  pattern rather than needing a new one: a local switch gating a local output,
  exactly like fast-flip, with `maxPulseTime` as the backstop if the EOS contact
  never closes. Handling them board-locally keeps the safety property intact and
  avoids inventing a closed-loop control class that the hardware does not
  require.
- All three arrive alongside **more boards per machine**, which is the one thing
  that does consume bandwidth — and the reason the `switchReplyDelayUs` question
  should be settled *before* the roadmap lands, not after.

## 7. Where the project stands

Better than a first read suggests. The architecture is sound, the hardware
choices are deliberate and correct, and there is genuine field validation at a
representative load. This is not a prototype.

What it is missing is the ability to **change safely**. There is no mechanism
that would reveal that a refactor broke fast-flip safety, or that a `libppuc`
change drifted from the firmware's expectations, short of putting it in front of
players. Since users can still absorb breaking changes today and will not be
able to after a release, that window is the thing to spend.

### Priorities

1. **Measure the switch-reply turnaround** on Pi-with-kernel-driver versus
   Mac-with-USB-adapter, and derive `switchReplyDelayUs` from measurement.
   Highest payoff-to-effort ratio in the project, and it gates the roadmap.
2. **A protocol conformance suite** generated against `PPUCProtocolV2.h`, run in
   both `libppuc` and `io-boards` CI. This is what makes the SHA pin chain safe
   and makes protocol hardening possible without a hardware round trip for every
   change.
3. **Unit tests for the pulse-envelope logic** — fast-flip, hold power, and the
   future magnet and motor cases. Pure logic, no hardware required, protecting
   the one thing that can damage a machine. The invariant worth pinning is
   general: *no configured solenoid may be energised beyond its declared
   protection, regardless of host behaviour.*
4. **Coil safety validators** (§4.1) — at minimum, refuse or warn on a solenoid
   with no thermal protection at all. Requires first adding a config field for
   dual-winding/EOS coils so the check can distinguish safe from unsafe
   `maxPulseTime: 0`.
5. **Close the fast-switch double-fire gap** (§4.2) before more modern titles
   are supported.
6. **Fail loudly on unmapped device numbers**, and generate or verify the YAML
   schema from one source instead of three.
7. **Then** protocol hardening — sequence/loss detection, heartbeat, error
   frames — carried in existing header and status fields, never as extra runtime
   frames, with before/after cycle measurements on hardware.

## 7.1 Why the testing gap is understandable — and tractable now

Testing a system that spans an emulator, a host library, a serial bus, firmware
and physical hardware is genuinely hard, and "unit tests were the only obvious
option" is a fair account of the problem. Prioritising a working, field-proven
prototype over a test harness was the right call for the phase the project was
in.

What makes it tractable now is that **the highest-value tests need no
hardware at all**:

- **Pulse-envelope logic** is pure state machine. Feed it synthetic time and
  events; assert that no configured solenoid can be energised beyond its
  declared protection. Covers fast-flip today, and magnets and motors when they
  arrive.
- **Protocol conformance** can be generated from `PPUCProtocolV2.h` and run
  against both encoder and decoder in CI. This is the piece that makes the SHA
  pin chain safe — host/firmware drift becomes a build failure instead of a
  hardware debugging session.
- **Configuration validation** is pure logic over YAML: coil protection rules,
  mapping sizes, switch-chain consistency, export/import round-trips.
- **Mapping derivation** in `libppuc` is a pure function from configured numbers
  to dense indexes.

That covers most of what can silently break, without a single board on the desk.
Hardware-in-the-loop testing is a later, optional step — valuable for timing
regression, not required to get the safety net in place.

## 8. Caveats

- All timing figures are derived from source and configuration, **not
  measured**. The cycle-time table in §2 is a hypothesis to test, not a result,
  and is marked as such in place. If it does not survive measurement, the
  conclusions drawn from it fall with it. The lamp-frequency and scaling
  conclusions in §2 do not depend on it.
- Coverage of the codebase was uneven. The protocol, host transport, build
  system and CLI were read closely. `EffectsController`, the PIO switch readers,
  `MediaPluginHost` and the config-tool exporter were only skimmed. Confidence
  varies accordingly.
- No hardware was observed. Conclusions about electrical behavior derive from
  the BOM and datasheet characteristics of the ADM3483, not from measurement.
