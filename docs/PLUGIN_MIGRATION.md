# Run PinMAME as a plugin instead of libpinmame

## Context

PPUC runs the game ROM through **libpinmame** and translates its C callbacks into
PPUC's world at the `GameEngine` seam. VPX media plugins (PUP, B2S, AltSound)
are integrated and working for video and backglass.

Sound is where the design breaks down. AltSound today is all-or-nothing,
switched *inside PinMAME* by `PinmameSetSoundMode(PINMAME_SOUND_MODE_ALTSOUND)`
(`PinmameEngine.cpp:301-303`), which silences the ROM stream entirely. AltSound
"mode 1" — play the pack's sample for a command, fall back to the ROM sound
when the pack has none — cannot work, because ROM audio never reaches the bus
as a stream anything could mix against. It goes straight from
`HandleAudioUpdated` into `AudioOutput::gameQueue_`.

The VPX plugin maintainer identified the root cause: the plugin API's override
chain is how one plugin replaces another's output, and PinMAME is meant to be
*a plugin* that streams ROM sound onto the bus. With libpinmame linked as a
library, PPUC reimplements discovery, overriding and resource location by hand
— and nothing publishes the ROM audio source AltSound is designed to override.

A second motivation is **kiki** (`../PPUC/kiki-vpx-plugin`), which runs Stern
Spike games and already publishes on the same CTLPI contract. Once PPUC
consumes CTLPI generically rather than libpinmame specifically, Spike support
becomes "load a different plugin", not "write a second engine".

**The goal that must not regress: real-time coil and switch latency.** PPUC
fires coils itself with its own PWM, strength and max-on-time protection; it
only needs the "on" edge as fast as it gets it today.

### Two premises from the chat turn out to be wrong, both in PPUC's favour

1. **PPUC has no message broker of its own.** It already compiles VPX's
   `MsgPI::MsgPluginManager` into `ppuc-pinmame` (a source file of the
   executable in `CMakeLists.txt`), registers as a static plugin at endpoint 1,
   and implements `MsgModuleLoader`, `LoggingPluginAPI`, `ScriptablePluginAPI`
   and `VPXPluginAPI`. **There is nothing to throw away and restart.**

2. **Coil latency will not regress, and today's baseline is lower than it
   looks.** On WPC, `OnSolenoid()` is only reached from `core_updateSw()`, once
   per `wpc_interface_update` vblank with `WPC_INTERFACE_UPD_PER_FRAME == 1`
   (`wpc/wpc.c:31,367,602`). **The solenoid state itself only changes at 60 Hz.**
   The libpinmame callback was zero-latency relative to a 16.7 ms
   quantization. A 1 kHz poll adds ≤1 ms on top — a 6 % worst case — for
   ~6.4 µs per tick, 0.6 % of one core.

The real gap is narrower and more precise: PPUC is a bus **consumer** that is
not a **provider**, so B2S is driven through a fake COM object and PUP by
pushing a PPUC-invented message; and ROM audio bypasses the bus entirely, so
the override chain has nothing to act on.

## Decisions taken

| Decision | Choice |
|---|---|
| Migration shape | **Replace `PinmameEngine` outright.** No fallback engine. `ScriptEngine` stays. |
| Coil semantics | **`PMPI_GROUP_VPM_SOLENOID`** (uint8, VPinMAME-compatible) — same semantics as today, so any playfield difference is a bug rather than a design change. |
| Fork strategy | Upstream work happens on **`ppuc_plugin_cleanup`**, branched off master `8ddf00417` in `../PPUC/vpinball` (confirmed: 792-line `ControllerPlugin.h`). The old `ppuc_uses_plugins` delta is re-applied selectively, not merged. Bump `VPINBALL_SHA`, `PINMAME_SHA`, `LIBDMDUTIL_SHA`. |
| `b2slegacy` | **Out of scope.** Do not touch beyond the minimum needed to keep it building. No deprecation PR, no feature-parity work — that is a VPX conversation, not PPUC's. |
| Provider scope | **Become a real provider.** Publish `ControllerDef`, `DisplaySrcId`, `SegSrcId`, `StateSrcId`. |
| DMD | **Go bus-native.** Load `serum`, `alphadmd`, `upscaledmd`; author `hardware-dmd` (replacing `plugin-dmdutil`) and `ppuc-display`; retire PPUC's direct `DMDUtil::DMD` usage. |
| Upstream | **Contribute PRs** rather than route around missing API — fix broken plugins, add PPUC's missing features, retire obsolete plugins, add optimized ones. All maintainers are reachable. |
| libdmdutil | **Decompose.** Most of it is now plugin territory; what remains is device drivers + frame pacing → a lean `hardware-dmd` plugin. |
| `ppuc-backbox` | **Deferred.** Largely obsoleted by the B2S plugin. The remaining need — offloading to a second machine — is better served by a **bus network-transport plugin**, with backbox becoming a bus consumer later. |

## Key API facts this plan is built on

The plugin API **changed shape** since the version PPUC pins; that is the source
of the merge conflicts on `ppuc_uses_plugins`.

- PPUC pins `ControllerPlugin.h` at **356 lines** (old `GetInputs`/`GetDevices`
  split). vpinball master has **792 lines** with a *unified state API*:
  `CTLPI_STATE_GET_SRC_MSG "GetStateSrc:1"`. kiki already targets the new API.
- `StateDef::GetState`/`SetState` are **thread safe**; **the change-callback
  hook was removed**. Polling is now the only way to observe machine state.
- `StateSrcId` has **no `overrideId`** (only `DisplaySrcId` and `AudioSrcId` do).
  State overriding is not expressible.
- **All `CTLPI_EVT_ON_*` events are gone** (`ON_GAME_START`, `ON_GAME_END`,
  `ON_SOUND_COMMAND`). Lifecycle is `VPXPI_EVT_ON_GAME_START/END` plus
  `CTLPI_CONTROLLERS_ON_CHG_MSG`; sound commands are `PMPI_EVT_ON_AUDIO_CMD`.
- **`PinmameSetSoundMode` no longer exists.** ROM audio always streams. *This is
  the single change that makes AltSound mode 1 possible.*
- `PMPI_GET_MACHINE_STATE` gives game/rom/hardwareGen; `PMPI_READ_MEMORY`
  replaces `PinmameReadMainCPUByte`, so the NVRAM ball/player tracking survives.
- PinMAME's state groups are the `StateSrcId::id.resId`; each `StateDef`'s
  `mappingId` is the PinMAME number — solenoids 1-based, GI/lamps 0-based,
  switches **signed** (cabinet switches negative, matching PPUC's `243 -> -3`).
- **There is no table script**, so PPUC starts a ROM by instantiating the
  plugin's scriptable `VPinMAME.Controller` (`GameName`, `Run()`, `Stop()`,
  `Running`, `Dip`) — the trick PPUC already uses for `B2S.Server`.
- `PINMAME_SHA=3a5ce504` has **no plugin support whatsoever**. Bump to
  `d2f97f50` (tip of `mkalkbrenner/pinmame` master, already the download URL).

### The B2S COM shim cannot be deleted

Both B2S and PinMAME are entered *only* through the scriptable/COM API —
`SetCOMObjectOverride("B2S.Server", ...)` and
`SetCOMObjectOverride("VPinMAME.Controller", ...)`. No bus message instantiates
either. So "delete the COM shim" becomes **"promote the COM shim"**: extract
`EnsureB2SServer`/`FindScriptMember`/`CallB2SMember` into a reusable
`ScriptObject`, used by both the engine and the media host.

### Provider scope: PPUC must become a provider

> **Revised.** This section was written before it was established that
> libpinmame publishes its own `ControllerDef`. That makes PPUC's
> `ControllerDef` correct **only for ROM-less games** — with a ROM, publishing a
> second one for the same game id is a coin flip for every plugin that binds
> with `items.front()`. See "Two things the plan had wrong" under the audio
> section. The `DisplaySrcId` / `SegSrcId` / `StateSrcId` rows below are
> unaffected and still stand.

PPUC publishes no `ControllerDef` today. Master's `serum.cpp::SelectController`
and `vni.cpp` both bind through `CTLPI_CONTROLLERS_GET_MSG`, so **serum and vni
cannot activate in PPUC at all** until it does. This is not optional polish;
it is the gate on the whole display chain.

`hostPlugin_` (already registered in `MediaPluginHost.cpp:705`) gains four
providers:

| Provider | Content | Unlocks |
|---|---|---|
| `CtrlItemProvider<ControllerDef>` | `gameId = "pinmame::" + rom` — see below | serum, vni, pup, dof, altsound, b2s binding |
| `CtrlItemProvider<DisplaySrcId>` | `identifyFormat = depth==2 ? BITPLANE2 : BITPLANE4` + `GetIdentifyFrame` (raw indexed), plus `LUM32F` `GetRenderFrame` | serum, vni, upscaledmd, PUPDMD matching, dmdutil |
| `CtrlItemProvider<SegSrcId>` | segment state | `alphadmd` |
| `CtrlItemProvider<StateSrcId>` | switches, lamps, coils, GI | B2S/PUP/DOF; later the hardware plugin |

The display provider is cheap because **there is already exactly one funnel**:
`GameEngineHost::OnDmdFrame(pData, depth, w, h)` (`ppuc.cpp:2559`), which both
the PinMAME path and the Lua `DmdCanvas` path already go through. One provider
on that single seam unlocks serum, vni, upscaledmd, pup-dmd and dmdutil
simultaneously.

**Declare `pinmame::<rom>`, not `ppuc::<rom>`.** The header settles it: `ControllerDef.gameId` "must be unique and
allow to identify **what** is emulated and how it is exposed, **not who** is the
controller emulating it" (`ControllerPlugin.h:73`), and `CtrlGetGameKey`
(`:477`) just splits `layout :: gameid`. So the prefix is an **asset-namespace
declaration**, not a provider identity. PPUC emulates the same games, and its
B2S / PUP / AltSound / DOF assets are keyed by the same ROM names — so
`pinmame::t2_l8` is true, not an impersonation. Four plugins filter on it
(`b2s/B2SRenderer.cpp:32`, `dof/DOFPlugin.cpp:243`,
`altsound/AltSoundPlugin.cpp:201`, `pup/PUPPlugin.cpp:219`) and all four are
really asking "can I resolve assets for this namespace?". Declaring it costs
nothing upstream and unblocks all of them immediately.

The genuine abuse is elsewhere: `vni.cpp:46` subscribes to
`PMPI_EVT_ON_CONSOLE_DATA`, a hard runtime dependency on the PinMAME plugin
rather than an asset namespace. That is what a capability bit is for — PR #22.

This also serves two longer-term goals directly:

- **PUP packs reacting to Lua / interceptor triggers** — publishing
  `StateSrcId` gets it natively. (`B2SPluginEventStream::OnB2SStateChange`
  subscribes unconditionally to `"B2S"/"OnStateChange:1"` with
  `{uint8_t type; int32_t index; int32_t value}`, which remains a useful
  injection point for author-written trigger letters and for board-local
  switches 200–241 that no controller sees.)
- **libppuc as a plugin, VPX scripts driving a real machine** — see below; the
  state API is already bidirectional and needs **no upstream additions**.

## Architecture

```
main() owns, in this construction order:

  PluginBus          ← NEW. MsgPluginManager, endpoint id, module loader,
                        settings handler, Logging/Scriptable/VPX APIs,
                        COM-override registry, ProcessAsyncCallbacks

    hostPlugin_ (in-process) provides:            [NEW providers]
      ControllerDef "pinmame::<rom>" ─┐  gates serum/vni/pup/dof/altsound/b2s
      DisplaySrcId  (identify+render) ┤  off the single OnDmdFrame funnel
      SegSrcId, StateSrcId, AudioSrcId ┘

    frame sources    ├── plugin-pinmame   states, displays, segments, ROM audio
                     └── plugin-alphadmd  SegSrcId → 128x32 DMD
    colorizers       ├── plugin-serum     + TriggerScene input  [patch 1]
                     └── plugin-vni
    cosmetic         └── plugin-upscaledmd
    sinks            ├── hardware-dmd     ZeDMD/PIN2DMD/Pixelcade [replaces plugin-dmdutil]
                     └── ppuc-display     SDL/KMS virtual DMD     [PPUC-authored]
    media            ├── plugin-altsound  audio source overriding PinMAME's
                     ├── plugin-pup       self-driving via B2SPluginEventStream
                     └── plugin-b2s       self-driving via CTLPI state/seg binding
    dev              ├── plugin-inspector live web view of every DisplaySrcId
                     └── plugin-scoreview segment preview without hardware
    later            ├── bus-bridge       CTLPI over TCP → a second machine/Pi
                     └── ppuc-hardware    libppuc RS485: consumes states,
                                          provides physical switches

  PluginEngine       ← NEW, replaces PinmameEngine. GameEngine impl.
  MediaPluginHost    ← demoted: backglass window/renderer, audio sink, B2S server
```

PPUC does **not** load: `pinmame` (PPUC is the controller), `dof` (PPUC drives
real hardware), `flexdmd`, `wmp`, `b2slegacy`, `dmdutil`, the samples.
`plugin-inspector`'s `displays.html` is a live web view of every `DisplaySrcId`
on the bus — the debugging tool for this entire migration.

Display chain: **PinMAME | alphadmd | PPUC → serum | vni → upscaledmd →
dmdutil | ppuc-display.**

The `GameEngine` seam is **unchanged**. `GameEngine.h` already permits an
engine-owned thread to push `OnCoilChanged`, so `PluginEngine` is a drop-in
sibling of `PinmameEngine` and `ppuc.cpp`'s main loop barely moves.

### The polling design (the crux)

`CtrlItemConsumer::With()` takes a mutex; it must never be called from the hot
loop. Neither a seqlock nor RCU is sufficient, because the borrowed data is
*provider-owned* — `callContext` points into libpinmame's `msgLocals` and
`GetState` points into `plugin-pinmame.dylib`, both destroyed at
`ReleaseMsgApi`/plugin unload. A seqlock detects the tear *after* jumping
through a dangling pointer; RCU keeps our vector alive but not what it points
at. The `CtrlItemConsumer` contract demands a **quiesce handshake**.

Design: **a narrow gate around the fetch phase only.**

```cpp
std::unique_ptr<CoilPlan> m_plan;      // msg thread writes, RT thread reads under gate
std::atomic<bool>     m_gateOpen{false};
std::atomic<unsigned> m_gateActive{0};
```

RT tick = *enter gate → read all solenoid states into a thread-local buffer →
leave gate → diff and dispatch to host sinks touching only PPUC-owned memory*.
`onItemsAboutToChange` closes the gate and spins until `m_gateActive == 0`.
The spin is bounded by one fetch phase (~6 µs), **not** by a host sink —
critical, because `OnCoilChanged` takes the interceptor mutex and writes the
serial bus, which would block the main thread for milliseconds if it were
inside the gate.

| Consumer | Thread | Rate | Cost/tick |
|---|---|---|---|
| `PMPI_GROUP_VPM_SOLENOID` | dedicated RT thread | **1000 Hz** | ~64 × ~100 ns ≈ 6.4 µs (0.6 % of a core) |
| `VPM_LAMP` / `VPM_GI` | main, in `Update()` | 120 Hz | ~10 µs |
| DMD identify frame | main | 120 Hz | 1 call + `frameId` compare |
| Segment frames | main | 60 Hz | 1 call + `nElements×16` float→bit |
| Switches | never polled | — | 0 |
| Ball/player NVRAM | main | 2 Hz | 2 `SendMsg` |

**Coils get their own thread; nothing else does.** Beyond latency, there is a
correctness reason: `core_update_pwm_outputs` mutates
`coreGlobals.physicOutputState[index]` with **no lock** — the comment at
`wpc/core.c:3296` says the load is deliberately moved to the caller thread. Two
PPUC threads polling overlapping indices would corrupt
`lastIntegrationFlipPos`. Giving the RT thread the solenoid range exclusively
and the main thread everything else keeps them disjoint by construction.
**State this invariant in the header** — "GetState is thread safe" means safe
against the *emulation* thread, not against two concurrent readers of the same
index.

`Update()` must throttle: `MAIN_LOOP_SLEEP_US == 20`, so an unthrottled lamp
sample would run at 50 kHz, and each sample now costs a PWM integration.

RT hygiene: `sleep_until(deadline += period)`, not `sleep_for`; best-effort
`SCHED_FIFO` on Linux. The tail, not the mean, is the risk — 1 ms `sleep_for`
on a loaded Pi can overshoot to 5–10 ms.

### Sink mapping

| Sink | Source |
|---|---|
| `OnCoilChanged` | `VPM_SOLENOID` `GetState`, RT thread, booleanised `!= 0` |
| `OnGameRunningChanged` | same sweep, emitted **before** the coil (order is load-bearing) |
| `OnRunStateChanged` / `IsReady` | `CtrlItemConsumer<ControllerDef>` + a 250 ms `Controller.Running` watchdog |
| `TryGetIdentity` | `PMPI_GET_MACHINE_STATE`; **copy `game`/`rom` immediately** — they point at emulator globals |
| `OnDmdFrame` | `GetIdentifyFrame`, **not** `GetRenderFrame` — identify returns raw bitplane 2/4, a byte-for-byte match for what `DMDUtil::UpdateData` wants today; render returns LUM32F floats which would change every ROM's look and break Serum keying |
| `OnSegmentDigit`/`OnPlayerScore` | `SegSrcId::GetState`, 60 Hz |
| `OnSoundCommand` | `PMPI_EVT_ON_AUDIO_CMD` — replaces both the callback *and* the `PinmameGetNewSoundCommands` polling |
| `OnLogMessage` | `PluginBus::SetLogSink` — the va_list-vs-`char*` portability hack dies with the callback |

**Segment decoding** — **done**. `SegDisplayFrame` gives 16 floats per element,
and `GetSegDisplay` fills them from the **same bit index** the old 16-bit word
used — so the existing 7-segment table is correct; only the input encoding
changed. It now lives in a dependency-free `src/SegmentDigitDecode.{h,cpp}` with
a float wrapper that rebuilds the mask with hysteresis.

Two corrections to what this section originally said:

- The thresholds given here were inverted (an "on" threshold *below* the "off"
  one describes no reachable state). The on threshold is now **0.5, matching
  `alphadmd.cpp:189`** — that is what builds the identify frame Serum keys an
  alphanumeric colorization on, and a display whose digits PPUC and the
  colorizer disagree about is worse than either choice alone. Segments hold
  until they fall below 0.3.
- The rebuild must read only the **first `nSegments[elementType]`** floats of
  each element. The provider leaves the rest of the sixteen untouched, so they
  are stale rather than zero; folding them in produces a mask the table has no
  digit for, and the display renders blank.

On System 6 `coreGlobals.nAlphaSegs` is unset and the provider writes strictly
0.0 or 1.0, so the hysteresis band never comes into play there. **It is untested
against a game that does set it** — if a modulated game renders blank, that pair
of thresholds is the first thing to suspect.

Digit bases are assigned by walking displays in **`resId` order with an
`nElements` stride**, replacing PinmameEngine's first-seen numbering. libpinmame
assigns `resId` in sorted layout order (top, then left), so the numbering the
backglass sees is stable across runs and matches reading order.

Segments are polled from `Update()` on the main thread at 60 Hz and need **no
quiesce gate**, unlike the solenoids: libpinmame publishes its sources from
`OnGameStart` via `RunOnMainThread`, so a source change and a poll cannot
overlap. That invariant is what makes the borrowed accessors safe, and it is
stated in `PluginEngine.h` — segment polling must not move off the main thread
without a gate of its own.

### Audio: how mode 1 actually works — **done**

Topology is now: every audio producer is a bus source; PPUC is purely a sink.
`AudioOutput` carries a two-level **lane (source) / stream** model, and the
decision of which lane is heard lives in `src/AudioLanes.{h,cpp}` with no SDL in
it. `gameQueue_` survives but is ScriptEngine-only: with a ROM, PinMAME's audio
arrives over the bus like any other plugin's.

On `CTLPI_AUDIO_ON_SRC_CHG_MSG` the host enumerates sources and marks any lane
named by another lane's `overrideId` as `overridden`. Then:

- **Mode 0 (`Replace`, default)** — overridden lanes are hard-muted. Today's
  behaviour, and what `PinmameSetSoundMode` used to do inside the emulator.
- **Mode 1 (`Fallback`, `--altsound-mode 1`)** — the overridden lane is unmuted
  while everything above it in the override chain has been silent for longer
  than `kFallbackHoldMs` (250 ms). `AudioMixer::Mix` already returns "had
  audible samples" on a 512-LSB threshold, so the signal needed no new plumbing.

Overridden lanes are **drained silently** via `AudioMixer::Discard`, not
stalled: PinMAME produces at 60 Hz whether or not anyone is listening, and a
stalled deque would hit the overflow cap and then dump stale audio the moment
the override lifted.

Honest limits: the hold window is a heuristic; packs with long near-silent
ambience will leak ROM music; there is up to `kFallbackHoldMs` of delay before
ROM sound returns. A fresh overrider is given one hold window before the lane
under it is let through, so a game start does not leak a burst of ROM audio
while a pack is still loading. Default stays mode 0.

#### Verified on a real pack

The whole design rests on ROM audio reaching the bus as a stream something can
override, so it is worth recording what a real AltSound pack does. Terminator 2,
616 samples, 120 seconds of attract, counting one-second windows:

| mode | ROM muted | ROM heard |
|---|---|---|
| 0 (`Replace`, default) | 120 | 0 |
| 1 (`Fallback`) | 113 | **7** |

Mode 0 is the old all-or-nothing behaviour, and it is exact: the ROM never gets
through. Mode 1 let the ROM through in seven of those windows -- the moments the
pack had nothing to play. That is the thing `PinmameSetSoundMode` made
impossible.

Note what mode 0 shows in `--debug-audio` while it mutes:

```
AltSound  heard,              signal, 1 stream(s), 228 ms buffered
PinMAME   [overridden] muted, signal, 1 stream(s), 238 ms buffered
```

The ROM lane still carries signal and its queue stays flat, which is the point:
the stream is arriving and being drained rather than stalled, so mode 1 has
something to unmute.

#### Resampling must be stateful

Running all three producers at once -- ROM, AltSound and a PUP pack -- showed
AltSound's queue growing without bound: 0 to 805 ms over 170 seconds, about
0.47% faster than real time. It did the same alone, so it was not contention.

That number is a fingerprint. `SDL_ConvertAudioSamples` is a one-shot with no
memory of the previous buffer, so every buffer's fractional remainder is
rounded away independently:

| producer | buffer | x 48000/44100 | error |
|---|---|---|---|
| PinMAME | 735 frames (one 60 Hz frame) | 800.0 exactly | none |
| AltSound | 128 frames (`BUFFER_SIZE_FRAMES`) | 139.32 | +0.49% per buffer |

PinMAME never showed it because its buffer size divides evenly. AltSound's does
not, and half a percent of surplus audio accumulates until the overflow cap
starts discarding sound -- heard as latency that grows the longer a game runs.

Each producer now keeps an `SDL_AudioStream`, which carries the resampler's
fractional position across calls. A stream is also kept alive while its
resampler still holds a partial frame, or draining it on a momentary gap would
reintroduce the same rounding.

Measured over 170 seconds with all three producers running:

| lane | before | after |
|---|---|---|
| AltSound | 0 to 805 ms | 0 to 69 ms |
| PUP | 208 to 556 ms | oscillates 234-554 ms |
| PinMAME | flat ~240 ms | flat ~283 ms |

The residual 69 ms is a different problem: AltSound's clock and the audio
device's clock are independent, and no fixed-ratio resampler can track that.
Correcting it needs adaptive rate matching, and at 0.4 ms/s it is far from
urgent.

#### A stall must not become permanent latency

Loading a PUP pack blocks the main loop long enough for PinMAME's audio to pile
up, and because production and consumption then match exactly, that backlog
never shrinks again. Measured on Terminator 2, steady-state depth of the ROM
lane:

| configuration | before | after |
|---|---|---|
| no plugins | ~35 ms | ~35 ms |
| + AltSound | ~37 ms | ~37 ms |
| + AltSound + PUP | **~480 ms** | **~220 ms** |

The queue is now trimmed back to `kTargetBufferedMs` once it passes
`kHighWaterBufferedMs`. The gap between the two matters: trimming at the target
alone clipped every peak of the ~20 ms sawtooth a lane naturally oscillates
through -- two dozen discontinuities in forty seconds, each an audible click, to
save milliseconds that were about to drain anyway. With the high-water mark it
fires once, after the stall that caused it.

#### The cut itself is spliced, not butted

A trim that lands on an arbitrary pair of samples is a step, and a step is a
click -- on a Williams *Flash* a Serum scene leaves the ROM lane ~150 ms behind
and the recovery was audible every time. The cut is now made the way audio is
normally spliced:

* it may move up to `kSpliceSearchMs` either side of where the arithmetic
  points, landing where the audio after the cut best matches the audio before it
  (sum of absolute differences over the fade window -- no normalisation, no
  floating point, and on periodic material it finds the same phase);
* the two sides are then blended across `kSpliceCrossfadeMs` rather than butted
  together, so whatever step survives is spread over hundreds of samples.

Both are free: the trim only runs once a lane is 150 ms past its target, so
moving the cut five milliseconds either way changes nothing it was trying to
achieve. The number of samples dropped is also kept a multiple of the channel
count, because dropping an odd number from an interleaved stereo lane swaps left
and right for the rest of the session.

#### Steering the rate closes both

Trimming bounds a backlog by discarding audio; it cannot hold a depth, and it
cannot answer clock drift at all. Each lane's resampler now has its ratio nudged
toward the target instead, via `SDL_SetAudioStreamFrequencyRatio`. Greater than
1.0 consumes input faster, yielding fewer output samples and draining a lane
that is running ahead.

The correction is clamped to **0.3%** -- roughly five cents of pitch, inaudible
on programme material, and more than an order of magnitude above the 0.04% drift
it has to cancel. A stream is now created even when the producer already matches
the device rate, because such a producer still drifts against the device's clock
and needs the same steering.

Measured on Terminator 2 with ROM, AltSound and a PUP pack, 170 seconds, ms
buffered sampled every 15 s:

| lane | before any of this | trim only | with steering |
|---|---|---|---|
| PinMAME | ~480 flat | ~220 flat | **84 94 77 79 82 84 103 106 88 77 82 102** |
| AltSound | 0 → 805 climbing | 0 → 69 climbing | **0 51 70 90 98 95 98 100 93 92 101 103** |

AltSound is the clearest result: it used to climb without bound, and now rises
to the target once and holds there. Trimming fired once in the whole run, for
the startup stall, and never again -- steering keeps the lanes off the
high-water mark by itself.

PUP reads about 200 ms because it runs two streams and the debug line sums them;
each is on target.

#### Latency

Every plugin audio buffer is marshalled through `ProcessAsyncCallbacks` once per
main-loop iteration, and the queue settles at **180-240 ms** on both Time Warp
and Terminator 2. That is not a drain problem -- the depth is stable, see below
-- but it *is* the audio latency, and it is high enough to be worth looking at
before this runs on a real playfield where a sound effect follows a coil hit.
`--debug-audio` prints it per lane, along with whether the lane is unmuted
("heard"/"muted") and whether it actually carries signal.

#### What the measurements said

The plan's top risk was that `ProcessAsyncCallbacks` — through which every
plugin audio buffer is marshalled, once per main-loop iteration — could not
drain as fast as PinMAME fills. `--debug-audio` prints per-lane buffered depth
once a second; on Time Warp it holds at **130–200 ms and does not grow**. No
dedicated drain thread is needed. That depth is also the audio latency, which is
worth revisiting on hardware but is not a correctness problem.

#### Two things the plan had wrong

**libpinmame is already a full CTLPI provider.** It publishes its own
`AudioSrcId` `{endpointId, 0}` named "PinMAME", streams ROM audio on
`AudioUpdate:1`, and publishes its own `ControllerDef` with the identical
`pinmame::<rom>` game id (`libpinmame.cpp:2683`, `:2779`). The PinMAME *plugin*
leaves `cb_OnAudioAvailable`/`cb_OnAudioUpdated` null precisely because
libpinmame broadcasts directly. So there was nothing to add upstream for ROM
audio to reach the bus.

**PPUC publishing its own `ControllerDef` was therefore actively harmful.** With
a ROM running, two endpoints claimed `pinmame::tmwrp_l2` — observed as
`count=2` from the fakectl plugin. AltSound, PUP, DOF and B2S all bind with
`items.front()` and no tie-break, and AltSound derives the audio source it
overrides from whichever it got (`overrideId = {controller.endpointId, 0}`). Pick
PPUC's and the override resolves to nothing, so the pack and the ROM both play —
a failure that is audible only on hardware. `Options::provideController` is now
false whenever a ROM controller is on the bus; PPUC declares itself the
controller only for ROM-less games. PPUC's own audio source also moved to
`resId 0`, the convention an overrider names, so it is overridable at all.

### Teardown

`PinmameStop()`'s thread join is what makes teardown safe today. Now there are
three producers to silence, in this order: **join the RT thread first** (that
is what replaces the libpinmame join, and it is what makes the later
`Unsubscribe` safe), then unsubscribe audio/sound-command, then
`Controller.Stop()`, then drain `ReleaseMsgApi` while the consumers are still
alive, then `Unsubscribe()` the consumers (mandatory — `CtrlItemConsumer`'s
destructor asserts `!m_subscribed`), then release msg ids, then release the
script object. `PluginBus` must be declared **before** the engine and media
host in `main()` so reverse destruction order is right even on an early return.

Debug builds are unforgiving here: every libpinmame getter opens with
`assert(_isRunning == 1)`, so one stray poll after `Controller.Stop()` aborts
the process.

## The DMD chain: go bus-native

**On master the plugins are cleanly separated**, and `plugins/dmdutil/` is a
pure hardware sink. It uses exactly three parts of libdmdutil — `Config::*`
(ZeDMD device/WiFi/SPI/brightness, Pixelcade, PIN2DMD, `SetDMDServer*`),
`FindDisplays`/`DumpDMDTxt`/`DumpDMDRaw`, and
`UpdateRGB24Data`/`UpdateRGB16Data`. The entire `Mode::Data` / `SerumThread` /
`VniThread` / `PupThread` half of `DMD.cpp` is dead code there. The rest is
*duplicated by other plugins*, not lost:

| libdmdutil feature | Bus replacement | Status |
|---|---|---|
| Serum colorization | `plugins/serum/` | complete, incl. V2 32P/64P and scene rotation |
| VNI / PAL | `plugins/vni/` | complete |
| PUPDMD capture + triggers | `PUPManager::ProcessDmdFrame` + `B2SPluginEventStream::SetDMDHandler` | complete, incl. the 128×16 pad and 256×64 downscale hacks |
| DMDServer **client** | `DMDServer`/`Addr`/`Port` settings | present |
| `CreateConsoleDMD` | none | 3-line patch, or fold into `ppuc-display` |
| `CreateSDLDMD`/`CreateKMSDMD` | none | → PPUC-authored `ppuc-display` plugin |

Chain: **PinMAME / alphadmd / PPUC → serum \| vni → upscaledmd → dmdutil.**

### The Flash Serum triggers are not blocked

`vpinball/libdmdutil` **already vendors `PPUC/libserum`** — not
`zesinger/libserum`. `plugins/serum/plugin.cfg` says
`link = "https://github.com/ppuc/libserum"`, vpinball's
`platforms/macos-arm64/external.sh:438` copies that `libserum.dylib` into the
SerumPlugin link line, and the staged `serum-decode.h:196` already declares
`Serum_Scene_Trigger`. Master's `serum.cpp:186` even tests
`FLAG_RETURNED_V2_SCENE`, a flag that exists **only in the PPUC fork**. Upstream is
already tracking libserum_concentrate.

So the Flash case needs one **small upstream patch**, not a fork:

```cpp
// SerumPluginLoad
onTriggerScene = msgApi->GetMsgID("Serum", "TriggerScene:1");
msgApi->SubscribeMsg(endpointId, onTriggerScene, OnTriggerScene, nullptr);

// ColorizeThread, inside the existing m_stateMutex block, before the identify read:
while (auto sceneId = m_pendingScenes.pop()) {
   const uint32_t r = Serum_Scene_Trigger(*sceneId);
   if (r != IDENTIFY_NO_FRAME && r != IDENTIFY_SAME_FRAME) updated = true;
   const uint32_t d = r & 0xffff;
   hasAnimation = (r > 0) && d && (d < SERUM_MAX_ROTATION_DELAY_MS);
   if (hasAnimation) { animationTick = now; animationNextTick = now + ms(d); }
}
```

Everything `libdmdutil/src/DMD.cpp:1480-1508` does around the call already has a
plugin equivalent: the re-emit is just `m_colorizedframeId++` (the bus is
pull-based, so bumping the id *is* the re-render), and the rotation timer reuses
the `SERUM_MAX_ROTATION_DELAY_MS = 2048` guard the plugin **already declares**
at `serum.cpp:134`. The 50000–62000 range and the `'D'`/value==1 filter move
into the subscriber.

The one real care is threading: `Serum_Scene_Trigger` mutates libserum globals
(`sceneFrameCount`, `sceneCurrentFrame`, `mySerum.rotationtimer`), so it must
run on the colorize thread under `m_stateMutex`, not on the message thread —
hence the queue. ~40 lines, symmetric with the existing
`"Serum"/"OnDmdTrigger:1"` output.

### The one genuine API gap: sink size negotiation

libdmdutil sizes Serum from the actual panel —
`(m_pZeDMD->GetHeight() == 64) ? FLAG_REQUEST_64P_FRAMES : FLAG_REQUEST_32P_FRAMES`
(`DMD.cpp:1560-1620`). The serum plugin instead hardcodes **both** flags and
publishes two sources — 32P as `resId 1`, 64P as `resId 2`, both with the same
`overrideId`. Two consumers then disagree: `DMDUtilPlugin::SelectSource` takes
"largest colour" (always 64P), while `ResURIResolver` walks the chain with
`break` on the *first* match (32P). Its own comment admits it:
`// TODO ... handle situations where a source has multiple overrides`.

Result on a 128×32 ZeDMD: it receives a 256×64 frame and downscales an upscaled
colorization. There is no back-channel — `DisplaySrcId` has no field for what
the sink wants, and `DMDUtil::DMD` has no public panel-size accessor
(`m_pZeDMD` is private).

- **Cheap fix, do first:** add `DMD::GetPreferredWidth()/GetPreferredHeight()`
  to libdmdutil from the same logic, and make `DMDUtilPlugin` construct its
  dispatcher at *load* time (not inside the consumer's onChange lambda) so
  `SelectSource` can prefer the height that matches the device. ~80 lines.
- **Right fix, raise upstream:** a `CTLPI_DISPLAY_GET_SINK_MSG` /
  `DisplaySinkId { width, height, frameFormat }` so colorizers size output to
  real devices. This is **the only thing in this plan that genuinely cannot
  work on the bus today**, and it also closes the `ResURIResolver` TODO.

### The alphadmd override bug — and its correct fix

`alphadmd.cpp:124` sets `overrideId = {sourceEndpointId, 0xFFFF}` as a sentinel,
commented *"we do not override a DMD but we want to identify the source
endpointId for colorization purposes"*. `CtlResId` compares as strict 64-bit
equality and serum/vni look for a **real** item with that id, so the lookup
always fails and alphanumeric displays are never colorized. `vni.cpp:72` has the
identical bug.

The comment names the real requirement: not *"which display do I replace"* but
*"which controller do I belong to"* — and on a segment-only machine there is no
parent `DisplaySrcId` at all, so `overrideId` is structurally the wrong field.
Correct fix is a new field:

```c
typedef struct DisplaySrcId {
   CtlResId id;
   CtlResId overrideId;     // strictly: the display I replace, 0 if none
   uint32_t controllerId;   // NEW: endpoint of the controller I derive from; 0 == id.endpointId
   ...
```

alphadmd then sets `overrideId = {0,0}`, `controllerId = sourceEndpointId`, and
`isFromController` checks the owner first. ~20 lines across five files. Making
alphadmd merely "selectable" instead would break `serum.cpp::SelectController`,
which requires the controller to *have* a colorizable DMD before considering it.

### `hardware-dmd`: what actually survives from libdmdutil

`DMD.cpp` is 3852 lines, but only **~44% is the hardware sink**:

| Bucket | Lines | Fate |
|---|---|---|
| Device drivers + output plumbing (ZeDMD/PIN2DMD/Pixelcade/RGB24/Level/Console threads, `FindDisplays`, ring buffer, pacing) | ~1,700 in `DMD.cpp`, **~2,930 lib-wide** | **keep** → `hardware-dmd` |
| Colorization orchestration (`SerumThread`, `VniThread`, `PupDMDThread`, `QueueSerumFrames`, alt-color paths, `SetRomName`) | ~1,000 | plugins own it |
| Segment rendering (`AlphaNumeric.cpp` + `Mode::AlphaNumeric` branches) | ~870 | `alphadmd` owns it |
| `DMDServer` network transport | ~630 | → `bus-bridge` |
| Dump / dev tooling (`playDump` 2767, `compareJsonDumps`, `convertSerum`, the four `DumpDMD*Thread`s) | ~890 in `DMD.cpp`, ~3,590 outside | stays in libdmdutil |

**Recommendation: link `libzedmd`/PIN2DMD/Pixelcade directly; VPX stops linking
libdmdutil.** Not a wrapper, because everything libdmdutil offers above the
drivers is expressed in the `DMD::Update`/`DMD::Mode` model — a fixed **82 KB**
struct with a 12-value mode enum (`SerumV1 … SerumV2_64_32, Vni, SerumCommand`)
that exists *because* libdmdutil owns colorization. Once serum/vni own it, every
producer of those modes disappears, leaving a permanent translation of bus
semantics into a vocabulary with no speakers. Concretely, wrapping inherits:

- the 82 KB `Update` `memcpy`'d into a 128-slot ring — **10.5 MB of buffers per
  `DMD` instance**, nearly all always zero;
- `DMDUtilPlugin.cpp:200-213`'s documented workaround skipping any source over
  256x64 because "libdmdutil's update buffers are fixed at 256x64 and overflowed
  by larger frames (vpinball/libdmdutil#65)";
- `Config` as a process-global singleton — fine for one DMD, wrong for a plugin
  that should drive two.

**~1,200-1,400 lines**, about 4x the current `DMDUtilPlugin.cpp`.

**Keep libdmdutil alive as a product.** It still ships `dmdserver` (which
dmd-extensions and dmdreader depend on) and owns `playDump`/`compareJsonDumps`/
`convertSerum` — the Serum regression harness. The PR is *"VPX stops linking
libdmdutil"*, not *"delete libdmdutil"*.

#### The non-obvious value — five things a naive sink gets wrong

Worth writing into the PR, because `DMDUtilPlugin.cpp` **is** the naive sink:

1. **Frame skipping targets bounded lag, never head.** `DMD.cpp:1163-1191`
   deliberately stays `DMDUTIL_MIN_FRAMES_BEHIND` (4) behind rather than
   jumping to head, because jumping breaks the next frame's inter-frame delta
   on devices doing temporal dithering — which ZeDMD HD does — and produces a
   visible stutter-then-snap. It also handles the uint16 wrap explicitly.
2. **Per-consumer cursors.** Each device thread holds its own `bufferPosition`.
   A ZeDMD blocking 200 ms over WiFi costs that thread 200 ms and nothing else.
   A single dispatch thread serializes every device behind the slowest.
3. **Dedup is layered, not single-level**: `frameId` at the source, content
   `memcmp` for segment data before re-rendering (`DMD.cpp:1375`), and the
   device's own detection (`// Note: libzedmd has its own update detection.`).
4. **Device-rate throttling is implicit and correct** — the threads never sleep
   on a timer; they block on the CV, then on the device write, so the device's
   throughput *is* the pacing signal. `DMDUtilPlugin.cpp:120-122` instead
   hardcodes `sleep_for(16666)` with the comment *"TODO the dispatch should be
   done at the refresh rate of the target display"*. `serum.cpp:149` hardcodes
   the same 16666 — so today there are **two independent 60 Hz pollers in
   series**, up to ~33 ms of phase error, and Serum rotations faster than 60 Hz
   are silently dropped (rotation delay is any value 1..2047 ms).
5. **HD arbitration is already solved and thrown away.** `DMD.cpp:1292-1297`
   picks between Serum V2's 32p and 64p output by asking the device its width.
   In the plugin chain serum publishes both and nothing downstream knows which
   to take, so `SelectSource` takes the widest and a 128x32 panel gets the
   256x64 stream. That *is* the ZeDMD HD bug; `DisplaySinkId` is the fix.

Two more in the same file: `DMDUtilPlugin.cpp:140` does `new uint8_t[w*h*3]`
/ `delete[]` **per frame** — 60 heap round-trips a second for 48 KB — and
`:124` `items.front()` means it can drive exactly one display, ever.

### PPUC's own display plugin

`libsdldmd`'s `SDLDMD` is already `Update(uint8_t* rgb24, w, h)` with ten
rendering modes and four rotations — nothing bus-hostile. Either fold it into
`hardware-dmd` as another sink, or keep a separate `ppuc-display` plugin driven
from a `CtrlItemConsumer<DisplaySrcId>`. The small refactor either way is making
the `DMDUtil::RGB24DMD` base and the `DMD&` parameter optional so it need not
link libdmdutil. PPUC already has the right pacing seam in
`src/dmd/DmdRenderer.h::DmdSink`.

### libppuc as a plugin — the long game, and it fits

`libppuc/src/PPUC.h:65-70` maps almost 1:1 onto the state API:

| `PPUC.h` | `StateDef` |
|---|---|
| `SetSolenoidState`, `SetLampState`, `SetGIState` | consume `StateSrcId` from whatever controller is present, drive RS485 |
| `SetSwitchState` | provide a `StateSrcId` of physical switches with `SetState` wired in |

`StateDef` already carries `mappingId`, `dataFormat`, `semanticType`
(`CTLPI_STATE_TYPE_SWITCH`, `_RELATIVE_BRIGHTNESS`) and **both** `GetState` and
`SetState`. So "a VPX table script drives a real machine" needs **no upstream
API additions at all** — this is the cleanest plugin candidate in the stack. The
open design question is matching: `StateDef.mappingId` is a "user friendly
mapping id" while PPUC's YAML numbers are ROM-specific, so it wants a mapping
file keyed on `ControllerDef.gameId`.

### `bus-bridge`: a new plugin, not a generalization of `remote-control`

`ppuc-backbox` stops being a program and becomes *"a second machine running
`bus-bridge` in importer mode, plus `b2s` + `pup` + `hardware-dmd`."*

**`remote-control` is not the base to build on**, despite the surface
similarity. It carries exactly one struct — `StateMsg { uint16_t version;
VPXInputState state; }` — raw `memcpy` over **UDP**, no length prefix, no type
tag, and a version field that is inert (sender always writes 0, receiver errors
on non-0, so a mismatch can never be detected). It is entirely `VPXPluginAPI`
and carries **zero CTLPI**. It also pushes *inputs from a thin client to a fat
renderer* — the opposite direction from bridging frames to a thin sink. And on
the far side of a bus bridge there is no VPX at all, so `vpxApi` is null there.

**Do reuse its socket shim**: lines 36-219 are a vendored portable-socket
wrapper with nothing remote-control-specific in them. Lifting that to
`plugins/plugins/PortableSocket.h` plus a TCP variant is a small, obviously-good
PR both plugins consume. (While there: `hasTimedOut()` at `:145` returns `false`
unconditionally on non-Windows with a `// TODO implement`, so a benign
`SO_RCVTIMEO` expiry is treated as a hard error and permanently kills the link
on Linux and macOS.)

**The forced topology.** `GetRenderFrame`, `GetState` and `SetState` are
function pointers plus an opaque `callContext` into the provider's address
space, so they cannot cross a socket:

- **Near side (exporter)** is a pure CTLPI *consumer*: subscribe to the
  `On*Changed` events, poll the selected sources, dedupe on `frameId`, push
  `{remappedSrcId, frameId, w, h, format, payload}`. `AudioUpdate:1` is the one
  part of CTLPI that marshals naturally — already a push message carrying its
  own buffer, so just forward the broadcast.
- **Far side (importer)** is a CTLPI *provider*: `AddItem` with
  `callContext = &itsOwnCacheEntry` and a trampoline `GetRenderFrame` serving
  the last received buffer — a **synthetic provider**. `SetState` sends back
  upstream; that direction is naturally cheap.
- **Discovery is the descriptor channel**: on every `On*Changed`, serialize the
  array minus function pointers and `callContext`, resend, rebuild, republish.

**The id-remapping trap, and why `controllerId` gates this.** A bridge cannot
publish sources claiming another endpoint's id, so it must remap every `id` into
its own space and remap `overrideId` through the same map or the override chain
silently breaks. But serum (`serum.cpp:89`) and vni (`vni.cpp:70`) identify
their input by `src.id.endpointId == controllerEndpoint` — meaningless after
remapping. An explicit `DisplaySrcId.controllerId` survives the bridge.

**Bandwidth — already far better than `DMDServer`.** DMDServer sends a
`#pragma pack(1)` `Update` of **82,198 bytes regardless of resolution**, plus a
534-byte `PathsHeader` **resent every frame**:

| Stream | Bytes/frame | @60 fps |
|---|---|---|
| DMDServer `Update` (any resolution) | 82,754 | **39.7 Mbit/s** |
| 256x64 SRGB565 | 32,768 | 15.7 Mbit/s |
| 128x32 LUM32F | 16,384 | 7.9 Mbit/s |
| 128x32 SRGB888 | 12,288 | 5.9 Mbit/s |
| 128x32 BITPLANE2 (identify) | 1,024 | **0.5 Mbit/s** |

So correctly-sized is already 2.5-5x cheaper than today; the apparent
"bandwidth regression" of moving off `Mode::Data` compares against the wrong
baseline. XOR-delta + LZ4 gets another 5-20x on typical DMD content. **Carry the
identify frame and colorize on the far side** — that is the 0.5 Mbit/s row.

**Latency**: LAN TCP adds ~0.3-1 ms, irrelevant. The near-side *poll* is the
real cost — fine for a DMD, not for `StateSrcId` driving solenoids. Poll
displays at the display's rate; poll state scalars at ~1 kHz (1-8 bytes, free)
and push on change.

**Transport: TCP, length-prefixed, versioned.** UDP is right for input and wrong
for frames — a 32 KB datagram is ~22 IP fragments and losing one drops the frame.

**Size:** ~900-1,300 lines.

## Colorization runs in the Serum plugin

**Done for ROM games.** `--serum` now loads vpinball's Serum plugin and turns
libdmdutil's own colorizer off. The plugin consumes the controller's display
straight off the bus and publishes its colorized output as an override;
`PluginEngine` renders that instead of the raw frame, through
`OnDmdRgb16Frame`/`OnDmdRgb24Frame` into `DMDUtil::UpdateRGB16Data`/
`UpdateRGB24Data`. libdmdutil still drives the panels; it just no longer colorizes.

Selection follows the override chain. `DmdSourceSelect` prefers a display that
nothing else overrides, because a colorizer's output and the frame it colorized
are the same size and only the chain can tell them apart. That also resolves a
two-step chain — alphadmd renders segments, serum colorizes, upscaledmd could
scale — to its far end. Verified on `afm_113b` (controller → serum) and
`flash_l1` (segments → alphadmd → serum).

Both colorizers move, not just Serum. Serum and VNI are different formats rather
than alternatives -- a game has one or the other, and libdmdutil applied
whichever it found -- so switching off its colorizer means loading `VNIPlugin`
alongside `SerumPlugin` or every `.vni`/`.pal` game silently loses colour.
Verified on `t2_l8`, which is VNI: the plugin loads the PAL and VNI and publishes
an SRGB888 output, where Serum publishes SRGB565.

A ROM-less Lua game keeps libdmdutil's colorizer: its frames come from
`DmdCanvas` through `OnDmdFrame` and exist only inside PPUC, so there is no
controller display for the plugin to consume.

### The sizing gap this exposes, and the honest state of it

libdmdutil asked the ZeDMD its height and requested only that size from Serum
(`DMD.cpp:1561`). The plugin cannot: it publishes 32 and 64 row outputs and
leaves consumers to choose, and PPUC — like `DMDUtilPlugin` — takes the largest.
**So a 128x32 panel now receives the 256x64 colorization unless told otherwise.**

`--serum-resolution 32` (or `64`) is the answer for a host that knows its panel:
only that size is computed and only it is published, so nothing is left to
choose and the other half is never computed. On a Raspberry Pi that is also the
point — it halves the colorization work.

It is a knob rather than a negotiation, and that is the gap. The automatic
version is `DisplaySinkId`: the colorizer asks the sink its geometry instead of
the host being told twice. Until that exists, an SD panel needs the flag.

A cheaper interim fix, if the wait is long: give libdmdutil a public
`GetPreferredHeight()` and have PPUC set `Serum:Resolution` from it. That moves
the same knowledge libdmdutil already has into the plugin's hands without any
bus API change.

## TODO: render a machine's auxiliary displays

PPUC renders exactly one display per machine. `DmdSourceSelect::SelectMainDisplay`
picks the largest and silently discards the rest, which is right for the main
DMD and wrong for every machine that has more than one physical display.

**These are real playfield hardware, not internal state.** The `CORE_NODISP`
flag they carry means "PinMAME's own renderer does not draw this", not "this is
not a display" — PinMAME has no window layout for them, so it skips them and
leaves them to a host that does. `sam.c:982-1000` submits them frame by frame
from the emulated LED latches, and even rotates rows into columns first, which
is not work anyone does for a debug artefact.

Known cases, all currently dropped:

| Game | Layout | Geometry |
|---|---|---|
| World Poker Tour | `sammini1_dmd128x32` | 14 displays of 5x7, two rows of seven characters (`top` 34 and 43, `left` 10..52 step 7) |
| Wheel of Fortune | `sammini2_dmd128x32` | one 35x5 strip |
| Various Sega/Stern | `segames.c:669,1041,1206,1908` | a 15x7 strip; a 21x5 strip; a green and a red 14x10 pair; three 5x7 characters |

So the feature is "auxiliary displays", not "the WPT displays". A design that
only fits fourteen 5x7 cells will not fit Wheel of Fortune's single strip.

### What is already in place

- Each auxiliary display is published as its own `DisplaySrcId` with a working
  `GetIdentifyFrame`, on the same endpoint as the main DMD. `wpt_140a` publishes
  fifteen. Nothing new is needed from the bus to *read* them.
- `PluginEngine::SampleDmd` already polls one display at 120 Hz and dispatches
  through `GameEngineHost::OnDmdFrame`. Polling several is a loop, not a redesign.
- `libsdldmd`'s `SDLDMD` takes a window, geometry, a rendering mode and a
  rotation, so a second window is a second `CreateSDLDMD` call.

### What has to be decided

1. **`OnDmdFrame` carries no display identity.** It is `(data, depth, width,
   height)`, which was enough when there was only ever one. Auxiliary displays
   need either an id parameter or a separate sink; the first is less churn but
   touches `ScriptEngine` and `DmdCanvas` too.

2. **Grouping.** WPT's fourteen cells are one logical 2x7 panel and should be
   composited into a single 35x14 surface (plus gaps) rather than opened as
   fourteen windows. The layout to composite by is in `left`/`top`, which CTLPI
   does **not** carry — `DisplaySrcId` has width, height and hardware, and no
   position. Either upstream gains a position hint, or PPUC groups by observed
   geometry and orders by `resId`, which happens to follow layout order.

3. **Output routing.** One extra window, one extra screen, or a region of the
   existing DMD window. This wants a config surface — probably an `[AuxDisplay]`
   INI section mirroring `[VirtualDMD]` — rather than a flag.

4. **Whether they should be colorized.** Almost certainly not: they are LED
   matrices with a fixed colour, and pushing them through Serum would key
   against a colorization built for the main DMD.

5. **Cost.** Fourteen more `GetIdentifyFrame` calls per poll at 120 Hz. Each is
   an indirect call plus a `frameId` compare when nothing changed, so this is
   small, but it is not free and belongs in the same budget as the coil poll.

### Suggested first step

Extend `DmdSourceSelect` to return *all* renderable displays ranked, rather than
one index, and have `PluginEngine` keep the main one on today's path while
publishing the rest through a new sink that nothing consumes yet. That separates
the selection change from the rendering work and keeps the main DMD's behaviour
provably unchanged — `test_dmd_source_select.cpp` already pins it.

## Alphanumeric games

> **Fixed.** PPUC now builds and loads `AlphaDMDPlugin`, and `PluginEngine`
> falls back to a non-controller display when the controller publishes none.
> Verified end to end on `flash_l1`: alphadmd matches the layout
> (`4x6+2x2`), publishes a 128x32 BITPLANE2 display, PPUC renders it, and Serum
> loads `flash_l1.cROMc (Serum v2, concentrate v8)` with "28 frames and 2972
> rotation scene frames" — the scene-trigger case. A machine with a real DMD is
> unaffected: `t2_l8` and `wpt_140a` still pick the controller's own display.
>
> Time Warp's own `tmwrp_l2.cROMc` is **version 6** and libserum rejects it as
> too old, with no `.cROM`/`.cRZ` beside it to regenerate from. That is a stale
> asset, not a code path: the chain is proven by Flash.
> libpinmame's plugin path publishes
> only `CORE_DMD` and `CORE_VIDEO` layouts; it no longer synthesizes the
> `PINMAME_DISPLAY_TYPE_DMD | DMDSEG` frame that `PinmameEngine` rendered. PPUC
> has no segment-to-DMD renderer of its own, so **an alphanumeric game
> configured with a physical DMD now shows nothing** — including Flash, whose
> `flash_l1.cROMc` colorization is the Serum scene-trigger case. Loading
> `alphadmd` and consuming its `DisplaySrcId` is the fix; PPUC currently
> consumes displays only from the controller's own endpoint.
>
> Note also that the paragraph below is out of date on one point: PPUC does not
> need to publish a `SegSrcId` provider for this. libpinmame publishes its own,
> and `alphadmd` binds to that.

libpinmame used to synthesize a DMD frame for segment ROMs; upstream now splits
`SegSrcId` from `DisplaySrcId` and renders segments in `alphadmd`. Loading that
plugin covers it — but note it only works once PPUC publishes a `SegSrcId`
provider, and once the `controllerId` fix above lands, or those displays render
uncolorized.

The layout table is the real value: 13 entries with Black Hole, Medusa,
Hyperball, Taxi, Police Force and Riverboat Gambler special-cased. Two known
defects to carry: `Layout_4x6_2x2_1x6` (Black Hole) has no identify-frame
support at all, and `Layout_4x7_5x2` (Medusa) has a live uninitialised
`m_seg_data2` FIXME.

### A present bug in the Flash trigger path

Independent of this migration, `ppuc.cpp:4018-4026` dispatches with `else if`:

```cpp
if (pMediaPluginHost) { pMediaPluginHost->QueueEvent(source, id, value); }   // PUP only
else if (pDmd)        { pDmd->SetPUPTrigger(source, id, value); }            // Serum scenes
```

`QueueEvent` only reaches the PUP plugin, so **whenever `--pup`, `--altsound`
or `--b2s` is on, the Flash Serum scene triggers are silently dropped.** Flash
ships both `flash_l1.pup.csv` and `flash_l1.cROMc`, so this likely bites in the
real configuration. It wants to be a fan-out, not a branch — worth fixing
before the migration, as a standalone commit.

## Files

| File | Change |
|---|---|
| `ppuc/src/PluginEngine.{h,cpp}` | **new** — `GameEngine` impl, RT poll thread, quiesce gate |
| `ppuc/src/PluginBus.{h,cpp}` | **new** — extracted from `MediaPluginHost::Impl` |
| `ppuc/src/PluginScriptObject.{h,cpp}` | **new** — generalised from `EnsureB2SServer`/`FindScriptMember`/`CallB2SMember` |
| `ppuc/src/SegmentDigitDecode.{h,cpp}` | **new** — `DecodeB2SSegmentDigit` table + float/hysteresis wrapper |
| `ppuc/src/PluginSwitchMap.{h,cpp}` | **new** — `243 -> -3`, 200–241 exclusion, signed `mappingId` round-trip |
| `ppuc/src/PinmameEngine.{h,cpp}` | **delete** (923 lines). Preserve `ResolveVpmPath` (move to `PinmameNvramMapLoader`), the segment table, digit-base assignment, score accumulation, NVRAM tracking |
| `ppuc/src/MediaPluginHost.{h,cpp}` | ~1954 → ~900 lines; loses the bus, gains the lane-aware audio sink; `QueueEvent`/`QueueDmdTrigger` → `EmitEvent`/`EmitSerumTrigger` |
| `ppuc/src/AudioOutput.{h,cpp}` | lane/stream model; extract `AudioMixer` for testability |
| `ppuc/src/ppuc.cpp` | bus construction before the engine, `PpucEngineHost` audio sinks, main-loop `bus->Process()`, teardown order |
| `ppuc/src/GameEngine.h` | correct the `OnAudioFormat` comment — `AudioUpdateMsg` has no `samplesPerFrame`, so the return value is meaningless |
| `ppuc/CMakeLists.txt` | new sources; **drop `pinmame` from the link line**; rewrite the plugin-SDK gate |
| `ppuc/src/PluginDisplayProvider.{h,cpp}` | **new** — `ControllerDef` + `DisplaySrcId` + `SegSrcId` + `StateSrcId` providers, fed off the existing `OnDmdFrame` funnel |
| `ppuc/plugins/ppuc-display/` | **new PPUC-authored plugin** — `SDLDMD`/`KMSDMD` virtual DMD driven from a `CtrlItemConsumer<DisplaySrcId>` |
| `libsdldmd` | make the `DMDUtil::RGB24DMD` base and the `DMD&` parameter optional so the plugin need not link libdmdutil |
| `ppuc/platforms/config.sh` | bump `PINMAME_SHA`, `VPINBALL_SHA` and `LIBDMDUTIL_SHA`; add `PinMAMEPlugin AlphaDMDPlugin SerumPlugin VNIPlugin UpscaleDMDPlugin DMDUtilPlugin` to the build loop; stage libpinmame + `PinMAMEPlugin.h` |

**Dropping libpinmame from `ppuc-pinmame`'s link line matters for correctness,
not just hygiene**: otherwise the executable and `plugin-pinmame` each carry a
copy of libpinmame, and only one MAME global state can exist per process.
`PinmameNvramMapLoader.cpp` keeps `#include "pinmame/libpinmame.h"` header-only for
`PINMAME_HARDWARE_GEN_*` — a compile dependency, not a link dependency.

## Ordering

**Step 1: land this document in the repo** as
`ppuc/docs/PLUGIN_MIGRATION.md`, cross-linked from `ppuc/docs/STACK.md` and
`ppuc/AGENTS.md`. Per `AGENTS.md`, stack-wide documentation is versioned in
`ppuc` because it is the root of the build graph. Note while doing so that
`STACK.md` and `AGENTS.md` are **already stale** — both still describe
`src/ppuc.cpp` as containing the PinMAME callbacks and neither mentions
`GameEngine.h`, `PinmameEngine` or `ScriptEngine`.

**Step 2: open the `#12` `DisplaySinkId` design issue**, so the design clock
runs while the port happens.

**Step 3: begin Phase 0.**

**The critical path to "PPUC runs bus-native" is `#0 → #1 → #4 → #12 → #14`.**
Everything else is parallel or optional. The long pole is `#12`, and it is a
conversation rather than a patch — so open that design issue on day one, in
parallel with the port.

### Phase 0 — nothing is testable until this lands

`#0`: port `MediaPluginHost.cpp` to master's CTLPI. It uses
`CTLPI_EVT_ON_GAME_START`, `CTLPI_AUDIO_SRC_BACKGLASS_STEREO` and `msg.format`,
none of which exist on master; `ControllerPlugin.h` itself is `676 +++----`
between the two. 300-500 lines of churn. **Start here.**

Also in this phase, PPUC-side and free: **declare `pinmame::<rom>`**, which
immediately unblocks `b2s`, `pup` and `altsound`.

### Phase 1 — PPUC-side prep, against the current pin, each independently shippable

1. Fix the Flash trigger `else if` fan-out bug (below).
2. Extract `ScriptObject` from the B2S shim, with tests.
3. Extract `AudioMixer` from `AudioOutput`, keeping today's queue model.
4. Extract `PluginBus` from `MediaPluginHost::Impl`; replace `kHostEndpointId = 1`
   with the captured `m_endpointId`.
5. Rebase the fork delta onto vpinball master on a `pinmame_plugin` branch.
6. Build a `plugin-fakectl` stub against the new headers.

### Phase 1u — upstream, land immediately, zero coupling

`#1` is **the cheapest unblocker on the whole list** — ~10 lines, and without it
Serum cannot load in *any* non-VPX host. `#5` is 3 lines. Then `#2`, `#3`, `#18`,
`#9`, and `#11` after `#10`.

Sequence `#18` (de-duplicate `common.h`) or `#1` as the **opening PR** — small,
obviously correct, establishes the relationship before the design-heavy asks.

### Phase 2 — one bug fix that needs review attention

`#4` (`controllerId`) touches the shared header and six plugins, but it fixes a
**real user-visible bug** — Serum never colorizes alphanumeric games — and that
carries it. Hard prerequisite for `#12` and `#16`, so get it in early. Ship
`#6`/`#7`/`#8` as one "alphadmd identify-frame correctness" PR alongside it;
`#7` and full `#6` need Medusa and Black Hole ROMs, so ship the safe variant of
`#6` when those are unavailable.

### Phase 3 — design issues, not PRs

`#12`, `#13`, `#21`, `#22`. Expect round trips measured in weeks of calendar
time but low personal effort. `#15` lands in parallel — small, independent, and
in a repo PPUC already maintains.

### Phase 4 — the big builds, gated on Phase 3

`#14` (`hardware-dmd`) is gated on `#12`, because building it against today's
API means hardcoding the very HD-sizing decision it is meant to fix. `#16`
(`bus-bridge`) is gated on `#4` + `#10` + `#12`. Then `#17`. `#19` and `#20`
whenever.

### The PPUC engine work, in parallel

The pin bump lands as its own commit — `MediaPluginHost.cpp` will not compile
against the new header, so bump + port + ship, *then* build `PluginEngine`,
which must land together with the audio lane rework and the link-line change.

**Effort, honestly:** Phase 1u is ~150 lines across six PRs, a couple of days.
Phase 2 is ~150 lines but two items need ROMs. Phase 3 is mostly writing and
waiting. Phase 4 is ~2,400 lines of new plugin plus deleting `plugins/dmdutil` —
the real work, correctly sequenced last.

## Verification

- **Unit tests** (`ppuc_tests`, which must link without libppuc/libpinmame/SDL/
  DMDUtil — after dropping the link line, `MsgPluginManager.cpp` qualifies):
  segment decode over both encodings incl. hysteresis ramp; switch-map
  `243→-3`, `241→-1`, 200–241 exclusion, negative `mappingId` round-trip;
  audio lane override + fallback + stream lifecycle.
- **`plugin-fakectl`** — a ~250-line stub CTLPI provider advertising a
  `pinmame::fake` controller, state groups, a seg source and two audio sources
  where one overrides the other. The only way to test the *integration*
  headless.
- **The gating measurement, before anything ships:** `--exit-after-ms 20000
  --no-display` with a new `--debug-audio` printing per-lane queue depth and
  oldest-buffer age. **A monotonically growing age means
  `ProcessAsyncCallbacks` is draining slower than PinMAME produces** — every
  audio buffer is marshalled through it, and it currently runs once per main
  loop iteration. If that fails, the drain must move to its own thread. Test
  this on a desk with no boards attached.
- **Parity diff:** same ROM through old and new binaries with
  `--debug-coils --debug-lamps --debug-switches --debug-sound-commands`. These
  print from the *host* side, so they are engine-agnostic and the diff is a
  genuine check on the `GameEngine` seam.
- **Needs hardware:** end-to-end coil latency (`docs/BUS_MEASUREMENT.md`, whose
  method is unchanged — nothing on the `OnCoilChanged` → `SetSolenoidState`
  path moves), playfield assist under a real drain, ZeDMD timing, and judging
  AltSound mode 1 by ear.

## Top risks

| Risk | Mitigation |
|---|---|
| **`ProcessAsyncCallbacks` slower than PinMAME's audio rate** → unbounded latency | Measure first; fallback is a dedicated drain thread |
| Alphanumeric DMD goes blank | Load `alphadmd` **and** publish a `SegSrcId`; verify on a real alphanumeric game incl. a special-cased layout |
| ZeDMD HD receives a downscaled 256x64 colorization | Sizing fix (cheap) or `DisplaySinkId` (right); measure on a 128x32 panel |
| `#12` `DisplaySinkId` is a design negotiation on the critical path | Open the issue on day one, in parallel with the `#0` port, so the design clock runs |
| `#0` port is 300-500 lines and blocks all testing | Do it first; nothing upstream unblocks PPUC until it lands |
| serum/vni never activate because PPUC publishes no `ControllerDef` | This is the gating provider — do it first, before loading either plugin |
| Two threads polling overlapping PWM indices corrupts emulator state | RT thread owns solenoids exclusively; document the invariant |
| `assert(_isRunning == 1)` in every getter aborts debug builds | Join the RT thread strictly before `Controller.Stop()` |
| Segment digit renumbering shifts B2S digit indices | New base rule is `resId`-sorted with `nElements` stride; needs a per-game pass |
| Segment threshold/hysteresis wrong → flickering or blank scores | Genuinely new signal processing; no numeric baseline to diff against |
| `MsgEntry` `vector` reallocation invalidates msg ids | Carry the fork's `deque` patch through the rebase and upstream it |
| Double PUP DMD triggers (PPUC's DMDUtil match + PUP's own) | Remove `SetPUPTriggerCallback` when PUP is loaded |
| PWM'd flashers sampled at 1 kHz catch off-phase where 60 Hz push never did | Expect visual difference on physout ROMs; fall back to the float group if it bites |
| `--pinmame-path` silently ignored — `CreateObject` prefers `<tabledir>/pinmame` | Return an empty `GetTableInfo().path`, or document the precedence |

## Upstream PR set

`R`: V=vpinball, D=libdmdutil, Z=libzedmd, P=PPUC.
Type: **B**ug / **F**eature PPUC needs / **N**ew plugin / **D**esign negotiation.

| # | PR | R | Size | Type | Needs |
|---|---|---|---|---|---|
| **0** | **Port `MediaPluginHost.cpp` to master CTLPI** — uses `CTLPI_EVT_ON_GAME_START`, `CTLPI_AUDIO_SRC_BACKGLASS_STEREO`, `msg.format`, none of which exist on master | P | 300-500 | F | — |
| **1** | **serum: don't bail when `vpxApi == nullptr`** — `serum.cpp:338` returns an empty path in any non-VPX host, so `SelectController` rejects every controller and **Serum never loads at all**. Skip the table-relative probes, fall through to `SerumPath`. | V | ~10 | B | — |
| 2 | serum: `IgnoreUnknownFramesTimeout` / `MaxUnknownFramesToSkip` settings (PPUC's `--serum-timeout`/`--serum-skip-frames`) | V | ~15 | F | — |
| 3 | serum: `"Serum"/"TriggerScene:1"` input message — the Flash case | V | ~40 | F | — |
| **4** | **`DisplaySrcId.controllerId`; kill the `0xFFFF` sentinel** — fixes Serum never colorizing alphanumeric games | V | ~90 | B | — |
| 5 | `MsgPluginManager`: `vector<MsgEntry>` → `deque` — `:141` does `emplace_back()` then `&m_msgs.back()`, and `:131` holds a reference across iteration; msg ids are indices, so reallocation invalidates both | V | ~3 | B | — |
| 6 | alphadmd: Black Hole advertises identify support it lacks — `:308` returns before the identify write, so Serum colorizes a permanently blank frame. Safe fix: null the identify hooks. | V | 8-40 | B | — |
| 7 | alphadmd: Medusa `m_seg_data2` never written — 10 displays blank in the identify frame | V | ~20 | B | ROM |
| 8 | alphadmd: Riverboat Gambler reversed display order (`:329` FIXME) | V | ~5 | B | ROM |
| 9 | serum: use-after-free on V1 resize (`:226` FIXME) — `m_colorFrameV1.resize()` can free a buffer a consumer is reading | V | ~25 | B | — |
| 10 | Lift `remote-control`'s socket shim to `plugins/plugins/PortableSocket.h`, add TCP | V | ~340 | F | — |
| 11 | remote-control: `hasTimedOut()` is a non-Windows stub; dead version handshake | V | ~15 | B | 10 |
| **12** | **`DisplaySinkId` + `CTLPI_DISPLAY_GET_SINK_MSG`** — the principled ZeDMD HD fix | V | ~250 | **D** | 4 |
| 13 | `ResURIResolver`: multiple-override selection heuristic (`:357` TODO) — land *with* 12, which gives it a principled input | V | ~80 | D | 12 |
| **14** | **`hardware-dmd` plugin; delete `plugins/dmdutil`; VPX stops linking libdmdutil** | V | ~1,300 | **N** | 12 |
| 15 | libzedmd: stable native-geometry accessor that survives `SetFrameSize` | Z | ~40 | F | — |
| **16** | **`bus-bridge` plugin** | V | ~1,100 | **N** | 4, 10, 12 |
| 17 | libdmdutil: keep `dmdserver` + dump tooling as the product; gate the VPX surface | D | ~100 | F | 14 |
| 18 | `plugins/plugins/common.h` — de-duplicate **11 copies** of `common.cpp/h` | V | net −200 | F | — |
| 19 | Fold `wmp` into `altsound` and remove it — 1,015 lines + a vendored `miniaudio_private.c` for **two tables** | V | −1,015 | D | — |

**`b2slegacy` is deliberately absent from this table.** At 11,858 lines it is
44% of all plugin source and the single biggest consolidation target, but `b2s`
(3,010) does not yet cover its animation / reel / Dream7 surface, so a removal
PR is not credible today — and it is a VPX-internal conversation, not PPUC's.
**Constraint: don't break it.** If `#4` (`DisplaySrcId.controllerId`) or `#18`
(`common.h`) touch it, apply the minimum mechanical change to keep it compiling
and nothing more.
| 20 | `ConsoleDMD` setting — moot if 14 lands | V | ~10 | F | — |
| 21 | `StateSrcId.overrideId` (+ optional push event) | V | ~90 | D | — |
| 22 | `ControllerDef.capabilities` — so `vni` stops using the `pinmame::` prefix as a proxy for `PMPI_EVT_ON_CONSOLE_DATA` | V | ~30 | D | — |

### Do not expose Serum's scaling algorithm as a bus message

libserum 2.6.2 upscales internally and never downscales, and
`Serum_GetScalingAlgorithm()` reports the authored choice so a caller scaling
the output further can match it. libdmdutil already does this at
`DMD.cpp:1634`, which works because it links libserum directly.

Mirroring that onto the bus so a sink can ask looks like the obvious next step.
Don't, for now:

- `serum.cpp:45` holds a single file-static `colorizer`, so any global query is
  implicitly single-colorization. Scaling is a property *of a source*, so if it
  ever belongs on the bus it belongs as a `DisplaySrcId` field.
- Nothing would consume it. `dmdutil` hands RGB to libdmdutil without scaling,
  and `upscaledmd` deliberately applies a user-chosen filter rather than the
  authored one. The only future consumer is `hardware-dmd` (#14).
- **`DisplaySinkId` (#12) largely dissolves the need.** Once the colorizer
  produces the plane matching the panel, nothing downstream scales and the
  algorithm never has to leave the plugin. Shipping the workaround first takes
  the pressure off the real fix.

Revisit as part of #12, not before.

### `DisplaySinkId` — the strawman for #12

```c
#define CTLPI_DISPLAY_GET_SINK_MSG      "GetDisplaySinks:1"
#define CTLPI_DISPLAY_ON_SINK_CHG_MSG   "OnDisplaySinksChanged:1"

typedef struct DisplaySinkId {
   CtlResId id;
   const char* name;                 // "ZeDMD HD (serial)"
   unsigned int width, height;       // native device geometry
   uint32_t hardware;                // of the *physical* device
   unsigned int frameFormats;        // bitmask of natively accepted formats
   unsigned int refreshRateMilliHz;  // 0 = free running
   unsigned int flags;               // CAN_UPSCALE / CAN_DOWNSCALE / WANTS_EXACT
   CtlResId boundSrcId;              // source routed here, 0 if unbound
} DisplaySinkId;
```

Published by `hardware-dmd` (one per device — that is the point), `scoreview`,
the VPX DMD window, `b2s`'s overlay region. Consumed by `serum` (pick 32p vs
64p), `vni`, `upscaledmd` (pick the scale instead of a fixed factor), `alphadmd`
(render 128x32 vs 256x64 instead of its hardcoded `.width = 128`).

A consumer must *not* pick a sink directly — a sink only means something
relative to the source routed to it. Two phases: sinks publish with
`boundSrcId = 0`; the resolver walks each sink's configured URI to the tail of
the override chain and republishes with `boundSrcId` set; a colorizer in that
chain then reads the geometry.

**Breaking the chicken-and-egg** (output size → which sources exist → chain
resolution → sink binding → output size): rule that **a colorizer may only
*select among* sizes it natively produces, never resize arbitrarily**. Serum
produces exactly two, so picking one is a selection and the graph converges in
one iteration. This is exactly what `DMD.cpp:1292-1297` already does with
`GetWidth() == 256` — the PR moves that decision from libdmdutil's private
knowledge into a public message.

**This is a design negotiation, not a patch.** `ControllerPlugin.h:16-20` still
carries the *"will evolve likely a lot … Do not use it"* banner — good for you,
but the maintainer will want to shape the struct. **Open it as a design issue on
day one**, in parallel with #0, so the design clock runs during the port.

## Further API gaps, not yet in the PR set

These need no immediate action but are worth raising while the
maintainers' attention is on this area — the first is the one with real teeth for PPUC.

- **No batched state read.** `GetState` is one indirect call per state. A
  `GetStates(ctx, first, count, void*)` would cut the RT coil sweep from 64
  calls to one. Highest-value addition for PPUC's use case specifically.
- **No source *rate hint*.** Consumers hardcode `sleep_for(16666)` —
  `DMDUtilPlugin.cpp:120` and `serum.cpp:149` both do, each with a TODO — which
  gives two 60 Hz pollers in series and aliases Serum rotations.
- **No push event on `StateSrcId`**, so bridges and `dof` must poll.
- **Audio override is binary.** Mode 1 needs either an `overrideMode` field or,
  better, an explicit "not handling this command" signal from the overrider, so
  the host stops guessing from a silence timer.
- **No `PMPI_SET_MEMMAP`** — a bus client cannot supply the NVRAM map, so
  `PMPI_GROUP_GAMESTATE` availability is unknowable to PPUC.
- **No NODISP hint on `DisplaySrcId`**, and no way to identify the main DMD.
  Hit in practice. libpinmame publishes every `CORE_DMD` layout, including the
  fourteen 5x7 `CORE_NODISP` mini-displays a Stern SAM game carries, and nothing
  on the wire says which is the score display. `src/DmdSourceSelect.cpp` works
  around it by taking the largest, tie-broken by `resId`. Both plausible rules
  agree on `sam.c` — the 128x32 is imported first *and* the extras are 5x7 — but
  that is luck, not a contract. Confirmed against a real ROM: `wpt_140a`
  publishes **15 displays** and the right one is chosen.

- **The PinMAME plugin ignores an explicitly configured `PinMAMEPath`** whenever
  `<tabledir>/pinmame/roms` exists — `PinMAMEPlugin.cpp`'s `CreateObject` probes
  table-relative first and only falls back to the setting PPUC hands it. Right
  for VPX, wrong for an embedding host that sets the path deliberately. Listed
  in the risk table as a guess; now observed. Low severity for PPUC, whose game
  folders normally carry their own ROMs, but it makes `--pinmame-path` a silent
  no-op in exactly the case someone would reach for it.
- **May a bridge publish sources whose `id.endpointId` is not its own?** Today
  no — which forces id remapping, which forces `controllerId` (PR #4).
