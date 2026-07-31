# Stabilization Plan — Measurement-Independent Work

**Status: proposed, awaiting approval.**

Everything in this plan can proceed **without** the RS485 bus measurement
described in [`BUS_MEASUREMENT.md`](BUS_MEASUREMENT.md). Nothing here changes
transport timing, frame layout, or the runtime protocol, so none of it can
invalidate or be invalidated by those results.

Rationale and priorities come from [`ASSESSMENT.md`](ASSESSMENT.md).

---

## Guiding constraints

1. **No transport timing changes.** Not until the measurement is in.
2. **No protocol changes on the wire.** Sequence validation, heartbeat and error
   frames all wait — they need the bandwidth picture first.
3. **Tests must not require hardware.** Everything below runs on a laptop and in
   CI. Hardware-in-the-loop is a later, separate step.
4. **Safety properties get tests before they get refactors.**

---

## Phase 1 — Test harness (do this first)

Nothing else is safe to change until there is something to catch regressions.
This phase adds no behaviour; it only creates the ability to assert.

### 1.1 Test frameworks — **approved**

- **doctest** for `libppuc` and `ppuc`. Single header, no build-system
  integration required, matching the existing pattern of staging dependencies
  into `third-party/include`.
- **Unity** for `io-boards`, run natively with `pio test -e native`. It is what
  PlatformIO ships, so no fighting the toolchain.

### 1.2 `libppuc` test target

- Add a `PPUC_BUILD_TESTS` CMake option, default `OFF`, `ON` in CI.
- `tests/` directory, one binary, `ctest` integration.
- No new external dependencies beyond the vendored header.

### 1.3 `io-boards` native test environment

This is the one place with real friction, and it should be understood before
approval.

`PwmDevices` — the code most worth testing — includes `<Arduino.h>` and depends
on `EventDispatcher`, which in turn pulls in the whole protocol and serial
stack. Testing it natively needs two test doubles:

- an **Arduino shim** providing `millis()`, `analogWrite()`, `digitalWrite()`,
  `pinMode()`, `byte` — with `millis()` driven by the test so time can be
  advanced deterministically
- an **EventDispatcher stub** implementing only what `PwmDevices` and
  `HighPowerOffAware` actually use: `addListener()` and `getSwitchState()`

This is a bounded piece of scaffolding in a `test/` support directory, **not** a
refactor of production code. No production file needs to change in this phase.
If it turns out a seam is genuinely required in production code, that becomes a
separate proposal rather than something smuggled in here.

- Add `[env:native]` to `platformio.ini`.
- `pio test -e native` runs on a laptop and in CI.

### 1.4 Wire tests into CI

Add a test job to `libppuc` and `io-boards` workflows. Keep them fast; a slow
suite gets ignored.

**Phase 1 deliverable:** `pio test -e native` and `ctest` both run green with a
handful of trivial assertions. No behaviour changed.

---

## Phase 2 — Safety invariants

The first real tests target the one thing that can damage a machine.

### 2.1 Pulse-envelope tests (`io-boards`)

The invariant to pin down, stated generally so it covers magnets and motors when
they arrive:

> **No configured solenoid may be energised beyond its declared protection,
> regardless of host behaviour.**

Cases:

- host on → off within `minPulseTime` → output stays on until the minimum
  elapses, then turns off
- host on and never off → output forced off at `maxPulseTime`
- fast switch closes → coil fires locally; switch opens during `minPulseTime` →
  coil stays on until the minimum elapses
- fast switch held closed past `maxPulseTime` → coil forced off, **and does not
  refire** until the switch has opened and closed again
- `holdPowerActivationTime` elapses → power drops to `holdPower`
- host disappears mid-pulse (no further events) → output still turns off

### 2.2 Coil configuration validator (`libppuc` + `config-tool`)

Every solenoid needs **at least one** thermal protection mechanism:

1. `maxPulseTime > 0`
2. hold power — `holdPower` plus `holdPowerActivationTime`
3. a dual-winding coil with an EOS contact

A dual-wound flipper with EOS and `maxPulseTime: 0` is correct. A single-winding
kicker with the same setting is a fire risk. **The configuration cannot
currently express the difference**, so this ordering is forced:

1. Add a YAML field declaring a coil as dual-wound with EOS.
   *Decision needed: field name. Proposal: `dualWinding: true` alongside the
   existing `fastSwitch`/`ballSearch` boolean style, optionally with
   `eosSwitch: <number>` when the contact is wired to an input.*
2. Extend `libppuc` validation to require one of the three mechanisms, failing
   with the section path and YAML line/column like existing validation.
3. Add the field to the `config-tool` PWM device form and exporter.
4. Unit-test the rule in `libppuc`; ideally a PHPUnit test on the exporter.

*Note:* this will likely reject some existing game YAML. That is the point, but
it needs a deliberate decision — hard error or warning first, and whether
`ppuc_games` configs get fixed in the same change. **Recommendation: warn
loudly for one release, then error.**

### 2.3 Fast-switch double-fire fix (`io-boards`)

`PwmDevices::handleEvent()` drives a coil from the host `EVENT_SOURCE_SOLENOID`
path with no check for whether that coil is fast-switch managed. On modern games
where the switch is also reported to the CPU for scoring, the ROM's echo fires
the coil a second time, one bus cycle after the board already fired it — and
that second pulse also bypasses the stuck-switch re-arm latch.

The fix must be **narrow**: suppress *the next* host activation after a local
fast-switch fire, not all host activations. The ROM legitimately fires these
coils on its own — its ball search, or deliberate effects such as World Poker
Tour moving a flipper finger to indicate where the ball will be served.

Design points to settle during implementation:

- one-shot per coil, **time-bounded** so a stale flag cannot swallow an unrelated
  activation minutes later
- bounded counter rather than a boolean, for rapid repeat hits
- firmware, not host: only the board knows whether it actually fired, versus
  having declined because the re-arm latch was set
- the discrimination is inherently heuristic — the time window is the only
  signal available — and should be documented as such

Tests: local fire then echo within the window → single pulse. Local fire then an
unrelated ROM activation after the window → coil fires normally. No local fire,
ROM activation → coil fires normally.

**Real-machine validation is required before this ships**, and Flash will not
exercise it — it needs a game where those coils are CPU-driven.

---

## Phase 2.4 — Untangle the switch-reply turnaround (`io-boards`)

Found while reading `EventDispatcher.cpp`. Both items are prerequisites for the
delay sweep in [`BUS_MEASUREMENT.md`](BUS_MEASUREMENT.md) to produce
interpretable results, and neither changes the wire protocol.

### One knob currently moves two independent things

```c
postTxSettleUs = switchReplyDelayUs / 4      // capped at 2000
delayMicroseconds(switchReplyDelayUs);       // (1) BEFORE asserting DE
digitalWrite(rs485Pin, HIGH);
...
digitalWrite(rs485Pin, LOW);                 // DE off
delayMicroseconds(postTxSettleUs);           // (2) AFTER releasing
```

These guard different things:

- **(1) pre-TX delay** waits for the *previous* transmitter's line release to
  settle before this board drives the bus. Functionally it is *"wait for the bus
  to settle after whoever passed me the token let go."*
- **(2) post-TX settle** stalls core 0 after this board has already gone
  high-Z. It protects nothing on the wire.

Because (2) is derived from (1), reducing `switchReplyDelayUs` from 2000 µs to
200 µs also cuts the settle from 500 µs to 50 µs. **A short pre-TX delay with a
generous post-TX settle cannot currently be tested.**

**Change:** split into two independent configuration values. Keep the existing
config topic for the pre-TX delay; add a second for the post-TX settle. This is
small, and it makes the delay sweep interpretable.

*Note the physical reading of (1):* it is compensating for line-settle time
after driver release, which is exactly what bias strength governs. That gives
the falsifiable prediction in the measurement brief — strengthen the bias and
the required pre-TX delay should drop.

### DE is dropped on an estimate, not a completion signal

```c
hwSerial->write(frame, frameBytes);              // returns after queueing
delayMicroseconds(FrameWireTimeUs(frameBytes));  // bytes*10*1e6/baud + 200 µs
digitalWrite(rs485Pin, LOW);
```

`FrameWireTimeUs()` is a computed estimate with a fixed 200 µs guard. If it is
ever short, DE drops while bits are still shifting out and the frame is
truncated — corrupt at *every* receiver, which matches the reported symptom.

The comment explains this exists because `HardwareSerial::flush()` hangs in this
path. Reasonable — but polling the PL011 status register directly is not
Arduino's `flush()` and should not share its failure mode:

```c
while (uart_get_hw(uart0)->fr & UART_UARTFR_BUSY_BITS) { /* bounded spin */ }
```

`BUSY` stays asserted until the transmit shift register empties, giving the
exact completion instant.

**Change:** replace the estimate with a bounded poll of `BUSY`, keeping a short
timeout so a hardware fault cannot wedge the loop. This removes a whole class of
uncertainty from the turnaround and allows the 200 µs guard to shrink.

**Why precision matters here:** the ADM3483 datasheet (Table 4) gives driver
disable times `t_PHZ`/`t_PLZ` of **80 ns max**. There is no grace period — if DE
drops early the driver releases almost immediately and the remaining bits are
lost. By contrast the enable times are up to 1300 ns, and
`RS485_MODE_SWITCH_DELAY` is currently 50 µs, roughly 38× the required enable
time and 600× the required disable time. That is 100 µs per reply of pure
margin: not the dominant cost, but free to reclaim once the analyzer confirms
real turnaround behaviour.

**Validation:** both changes need the logic analyzer to confirm on hardware —
DE must fall after the last stop bit, never before. Implement now, verify when
the analyzer is available.

## Phase 3 — Protocol conformance

### 3.1 Conformance suite against `PPUCProtocolV2.h`

The suite lives in **`io-boards`**, next to the header it tests, because that
header is the protocol source of truth. `libppuc`'s `external.sh` already copies
the protocol headers into `third-party/include/io-boards/`; extend it to copy
the conformance tests too, so **both repositories run the same suite**.

Coverage:

- header layout and `kHeaderBytes == 5`
- CRC-16/CCITT over header + payload, including known-answer vectors
- big-endian field encoding for `SetupFrame`, `MappingFrame`, `ConfigFrame`
- frame size calculations for every type, at several bit-count configurations
- `BitsToBytes()` boundaries
- GI nibble packing/unpacking across the 5 strings, values `0..8`
- round-trip encode → decode for every frame type
- rejection of truncated frames, bad sync, bad CRC

This is what makes the SHA pin chain safe: host/firmware protocol drift becomes
a build failure instead of a hardware debugging session.

### 3.2 `libppuc` unit tests

Pure logic, no serial port:

- YAML schema validation — accept valid configs, reject each malformed case with
  the right path and location
- mapping derivation: sparse/high logical numbers → dense indexes, sizes equal
  the count of unique configured numbers, clamping at `kMax*Bits`
- switch group construction, including the reserved built-in `buttons` group
- `coilGiMappings` parsing
- virtual-board selection from `SetSkippedBoardsCsv()`

---

## Phase 4 — Diagnostics and consistency

### 4.1 Fail loudly on unmapped device numbers

`QueueEvent()` silently ignores lamp and coil numbers absent from the derived
mapping, so a mistyped number in YAML produces a dead output with no diagnostic.
Log once per unknown number — enough to diagnose, not enough to flood.

### 4.2 Firmware version reporting

A board's firmware binary is versioned independently of the protocol headers the
host was built against, and nothing detects a mismatch. Have boards report
`FIRMWARE_VERSION_*` during startup config, and have the host log it and warn on
an unexpected value.

*This one touches the wire and should wait for the measurement, unless it can
ride inside an existing config-ack field. Flagged here so it is not forgotten;
**not** proposed for immediate implementation.*

### 4.2b Non-intrusive runtime diagnostics (`io-boards`)

**The project cannot currently observe runtime health without changing it.**
Enabling the debug DIP switch:

- **disables the watchdog** (`main.cpp` skips starting it, because it interferes
  with USB debugging) — so the high-power-output shutoff is absent exactly when
  someone is investigating a problem on a live machine
- **stalls core 1 once per second**, via `rp2040.idleOtherCore()` wrapped around
  the blocking USB writes of the `V2DBG` counter line
- **blocks at boot** waiting for a USB CDC connection

The result is that a timing-sensitive fault cannot be diagnosed with the only
built-in instrument, because the instrument perturbs timing — and does so while
removing a safety net. This is a real obstacle to the reset/restart robustness
work, which is the top unresolved transport problem.

#### Design: timestamped binary ring buffer

The costs today are not the transport. They are `rp2040.idleOtherCore()`,
`Serial.print()` blocking when the CDC buffer fills, and `vsnprintf` into a
1024-byte stack buffer. Removing those makes plain USB CDC entirely adequate.

1. **Binary records, not formatted strings.**
   `{ uint32_t timestamp_us; uint8_t event_id; uint8_t core; uint16_t arg0;
   uint32_t arg1; }` or similar — a fixed-size struct write of tens of cycles,
   versus thousands for `vsnprintf`. **Format on the host**, not on the board.
   This is the single largest saving.
2. **One ring buffer per core.** Single producer each means no locks, no
   cross-core synchronisation, and `idleOtherCore()` disappears entirely.
3. **Microsecond timestamps** from `timer_hw->timerawl`. Cheap, and without them
   the log is useless for timing work.
4. **Non-blocking drain** from the main loop: check `Serial.availableForWrite()`,
   write only what fits, return. Never wait. Overflow drops the oldest records
   and increments a dropped-count — a lost diagnostic is always preferable to a
   disturbed machine.
5. **Investigate decoupling the watchdog from USB debugging.** Establish *why* it
   interferes. A silent, non-blocking debug mode may not need the watchdog off at
   all, which removes the safety asymmetry.

#### The part that matters most: survive a reset

Place the ring buffer in a linker section **outside `.bss`** so startup does not
zero it, with a magic word and a sequence counter to detect validity. Then when a
board wedges and gets reset, the log of what it was doing *immediately before*
the wedge can be read back afterwards.

This costs nothing during the fault — no transmission at all — and it is the only
instrument that works on a board that has stopped talking. Given that
reset/restart recovery is the top unresolved transport problem, and that a wedged
board will not report anything over USB while wedged, this is likely the highest-
value diagnostic in the whole plan.

A later, optional extension: read the buffer over **SWD** using a spare Pico as a
debug probe. That works even on a board that is no longer servicing USB, and
costs the firmware nothing at all.

#### Ruled out

- **DMA to USB CDC.** Not possible in this sense: USB on the RP2040 goes through
  the USB controller's DPRAM and TinyUSB, and there is no DREQ for USB endpoints.
  DMA-to-UART is real; DMA-to-USB is not.
- **A dedicated debug UART.** UART0 is the RS485 bus. UART1 exists but there is
  no free GPIO on IO_16_8_1 — 0/1/2 are RS485, 3–24 and 26/27/29 are ports, 25 is
  the LED, 28 is the board-ID ADC. It would require sacrificing a port pin;
  acceptable on a bench board, not in production.
- **PIO-based output.** Same pin problem, plus contention: `Switches` and
  `SwitchMatrix` both claim `pio0` and WS2812FX needs PIO as well.

None of these are necessary once records are binary and the drain is
non-blocking.

The record-buffer and host-side decoder logic is pure and unit-testable
natively, so it fits the Phase 1 harness.

*Note:* `CrossLinkDebugger` — which prints per event with `idleOtherCore()` on
every call — is currently inert, because both listener registrations in
`main.cpp` are commented out and `active` is therefore never set. Anyone
re-enabling it should know it would be ruinous for runtime timing as written.
It also formats into a 1024-byte stack buffer, which is worth revisiting before
it is ever used again.

### 4.3 Schema consistency check

Firmware config topics, `libppuc` validation and the `config-tool` exporter
describe the same schema in three places with no shared artifact. Full
single-sourcing is a large change; a cheap first step is a test that round-trips
a `config-tool` export through `libppuc` validation and fails on drift.

*(This gap is not theoretical: `coilGiMappings` was missing from the stack
documentation's section list precisely because it is declared differently from
its neighbours.)*

### 4.4 Pin-chain checker in CI

`tools/check-pins.sh` exists. Run it in CI, using `GITHUB_TOKEN` to avoid the
unauthenticated API rate limit. Verify the `libdmdutil` variable names, which
were guessed and are currently unverified.

---

### 4.5 Build system robustness

Two gaps found while wiring up `build.sh --test`, both of which cost real
debugging time and neither of which produced any error.

#### A rebuild that changes nothing must compile nothing

Four separate defects were making every build redo work that could not have
changed:

- `SOURCE_DIR_CACHE_BUSTER` was a wall-clock timestamp, so any build using a
  `*_SOURCE_DIR` could never hit its dependency cache.
- flite's artifact check tested for `install/include/flite` — a *directory*
  that the staging step never creates, since flite stages its headers flat. The
  check was therefore always false and flite rebuilt on every run, even with a
  matching SHA.
- Header staging used `cp` and `cp -r`, which do not preserve mtimes. Every
  staged header looked newer than the objects that included it on every run,
  cascading into a full rebuild of the Lua sources, `ppuc.cpp` and all of the
  VPX plugins.
- `B2SLegacyPlugin` was compiled and packaged on every build although
  `MediaPluginHost` only ever loads `PUP`, `AltSound` and `B2S`.

All four are fixed. The point of this item is that **none of them produced an
error** — they were pure waste that looked like normal build output, and they
survived because nobody was watching the clock.

**Add a CI job that runs the platform `build.sh` twice and asserts the second
run compiles nothing.** Grep the second log for `Building CXX object` /
`Building C object` and fail if any appear. That converts this entire class of
defect from invisible to a red build, cheaply and permanently.

#### `external.sh` is not transactional

`ppuc_clean_runtime_lib_dir` deletes the staged runtime libraries early and
repopulates them at the end. Any failure in between — and the script runs under
`set -e` for many minutes — leaves the tree **worse than before it started**,
with headers and libraries half-removed.

This is not hypothetical: during this work an abort partway through
`external.sh` left `libyaml-cpp.dylib` and `libppuc.dylib` missing, so the
*next* run failed with a completely different and misleading error. Debugging a
long build is hard enough without the previous attempt having corrupted the
inputs.

**Stage into a temporary directory and swap at the end**, or take a marker-file
approach so an interrupted run is detected and re-staged rather than silently
half-applied. Either removes the failure mode.

## Phase 5 — Hygiene

- Delete stale feature branches (`v2*`, `lua*`, `interceptor`, …) left from the
  v2 bring-up.
- Decide the fate of the `Out_8x10` board: `config-tool` knows it,
  `platformio.ini` does not build it.
- Consider stub files at the old `INTERCEPTOR.md` / `RULES_AND_EFFECTS.md`
  paths if external links to them exist — GitHub does not redirect moved files.

---

## Sequencing and dependencies

```
Phase 1  harness ─────┬─→ Phase 2  safety invariants
                      ├─→ Phase 3  conformance + libppuc tests
                      └─→ Phase 4.1/4.3  diagnostics

Phase 2.2  needs the dual-winding YAML field decision first
Phase 2.3  needs real-machine validation on a modern title
Phase 4.2  deferred until after the bus measurement
Phase 5    independent, any time
```

Phase 1 gates everything else and is the only hard ordering constraint. Within
Phases 2–4 the items are largely independent and can be reordered freely.

---

## Decisions needed before starting

1. ~~**Test frameworks**~~ — **decided: doctest + Unity.**
2. **Dual-winding/EOS field name** — `dualWinding: true` (+ optional
   `eosSwitch`)?
3. **Validator severity** — warn for one release, then error? Or error
   immediately?
4. **Existing game configs** — fix `ppuc_games` YAML in the same change, or
   separately?
5. **Scope of first delivery** — Phase 1 alone, so the harness can be reviewed
   before tests are written against it? Or Phase 1 + 2.1 together, so the first
   real safety test lands with it?
6. **Priority of 4.2b (non-intrusive diagnostics)** — listed in Phase 4, but
   there is now a strong argument for Phase 2. Three reasons: the watchdog being
   disabled in debug mode is a safety asymmetry; the inability to observe runtime
   health without perturbing it will obstruct the reset/restart work; and the
   reset-surviving buffer is the *only* instrument that can see what a wedged
   board was doing before it stopped. **Recommendation: raise to Phase 2.**

*Recommendation for (5): Phase 1 plus 2.1. A harness with no tests in it is hard
to evaluate, and the pulse-envelope tests are the ones most worth having.*

---

## Explicitly not in scope

- Any change to transport timing or `switchReplyDelayUs`
- The 250 kbps baud change
- Sequence validation, heartbeat frames, error frames
- Reset/restart robustness work — needs hardware
- Hardware-in-the-loop testing
- Multiball, motors, magnets
