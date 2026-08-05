# The PPUC Stack

Architecture, dependency model, and build system across all PPUC repositories.

This document lives in the `ppuc` repository because `ppuc` is the root of the
build graph: `platforms/<platform>/<arch>/build.sh` transitively builds every
other C++ component in the stack. Start here.

Paths are written repo-qualified — `libppuc/src/PPUC.cpp` means the file
`src/PPUC.cpp` in the `libppuc` repository.

## 1. What PPUC Is

PPUC repairs, modernizes, and extends solid-state pinball machines of the 80s
and 90s, and can drive homebrew machines. A PC or Raspberry Pi runs the original
game ROM under PinMAME and acts as the CPU, driving RP2040-based IO boards in
the cabinet over RS485.

The stack is six independent repositories:

| Repository    | Upstream                              | Stack | Version | Role |
|---------------|---------------------------------------|-------|---------|------|
| `ppuc`        | `github.com/PPUC/ppuc`                | C++20 / CMake | 0.3.0 | CLI applications: `ppuc-pinmame`, `ppuc-menu`, `ppuc-backbox`; root of the build graph |
| `libppuc`     | `github.com/PPUC/libppuc`             | C++ / CMake shared lib | 0.3.0 | Host-side machine config + RS485 v2 transport |
| `io-boards`   | `github.com/PPUC/io-boards`           | C++ / PlatformIO (RP2040) | 0.2.0 | IO board firmware; owns the wire protocol |
| `libsdldmd`   | `github.com/PPUC/libsdldmd`           | C++ / CMake shared lib | 0.1.0 | SDL3 DMD/translite rendering on top of libdmdutil |
| `config-tool` | `github.com/PPUC/config-tool`         | PHP / Drupal 11 / DDEV | – | Web frontend that authors game configuration + Lua rules |
| `ppuc_games`  | `github.com/mkalkbrenner/ppuc_games`  | data | – | Example/production game folders and config-tool exports |

Companion documents in this directory:

- [`ASSESSMENT.md`](ASSESSMENT.md) — independent architecture review, current
  state, and prioritized stabilization backlog.
- [`STABILIZATION_PLAN.md`](STABILIZATION_PLAN.md) — proposed work plan for
  tests, validators and diagnostics that does not depend on hardware
  measurement.
- [`BUS_MEASUREMENT.md`](BUS_MEASUREMENT.md) — logic-analyzer brief for
  establishing real RS485 cycle timing and what the switch-reply delay
  compensates for.
- [`V2_PROTOCOL.md`](V2_PROTOCOL.md) — reference for the V2 wire protocol:
  framing, frame types, payload layouts, session lifecycle and timing, plus a
  section recording missing features and future work.
  `io-boards/src/PPUCProtocolV2.h` is the authoritative definition.

- [`RULES_AND_EFFECTS.md`](RULES_AND_EFFECTS.md) — what Lua rules can do and the
  `ppuc.*` API.
- [`INTERCEPTOR.md`](INTERCEPTOR.md) — host-side interception of physical machine
  events before they reach PinMAME.

The authoritative CLI, game-folder and INI reference is `README.md` in the
repository root.

## 2. The Stack At A Glance

```
  config-tool (Drupal)                 ppuc_games (game folders)
        |  exports game folder / io-boards.yaml / rules.lua / ppuc.ini
        v
  ┌───────────────────────────── PC / SBC ─────────────────────────────┐
  │  ppuc-pinmame                                                      │
  │    libpinmame  ── ROM emulation, event/delta callbacks             │
  │    LuaRulesEngine ── game rules, interceptor, callouts             │
  │    MediaPluginHost ── VPX plugins: PUP, B2S, AltSound              │
  │    libdmdutil + libsdldmd ── DMD, AltColor/Serum, translite        │
  │    AudioOutput ── SDL3_mixer music, flite / espeak-ng speech       │
  │    libppuc ── YAML config, dense bitmap mapping, RS485 v2          │
  └───────────────────────────────┬────────────────────────────────────┘
                                  │ RS485, 115200 baud, v2 framing
                    ┌─────────────┴─────────────┬──────────────┐
                    v                           v              v
              IO board 0                  IO board 1     … up to 8
              (RP2040, dual core)
              core 0: IOBoardController + EventDispatcher (bus, switches, PWM)
              core 1: EffectsController (LED strips, PWM effects)
                    |
              coils, lamps, GI, WS2812 strips, switch matrix, dedicated switches

  ppuc-backbox (optional, separate machine) ← TCP :6789 DMD server ← ppuc-pinmame
```

### Layering rule

1. `ppuc` is the application layer: CLI, PinMAME, rules, media, presentation.
2. `libppuc` is the host protocol/config layer.
3. `io-boards` is the firmware layer and the **protocol source of truth**.

Keep features at the lowest layer that can own them, and no lower.

### How PinMAME is driven

`ppuc` does not drive PinMAME directly; it embeds **libpinmame**, which exposes
an event and delta model:

- solenoids, display, sound commands, mechs and state changes arrive as
  **callbacks** (`OnSolenoidUpdated`, `OnDisplayUpdated`, `OnSoundCommand`, …)
- lamps and GI are drained as **deltas** (`PinmameGetChangedLamps`,
  `PinmameGetChangedGIs`)
- physical switches are **pushed in** with `PinmameSetSwitch`; original-matrix
  switch numbers pass through directly, numbers ≥ 241 map into PinMAME's
  negative special-switch space

This pairs deliberately with the wire protocol: libpinmame emits deltas,
`libppuc` folds them into a snapshot, and the snapshot goes out at constant
cost. Gameplay complexity is therefore free on the bus — multiball costs no more
bandwidth than an idle playfield.

## 3. Repository Details

### `ppuc` — applications

- `src/ppuc.cpp` (~5k lines): main entry point. CLI parsing (`cargs`), INI and
  game-folder resolution, PinMAME callbacks, runtime loop, bench test modes,
  ball search, switch refresh, DMD/translite setup.
- `src/LuaRulesEngine.*`: embedded Lua 5.4 rules engine (`ppuc.*` namespace).
- `src/MediaPluginHost.*`: hosts VPX message plugins (PUP, B2S, AltSound) via
  `MsgPluginManager` from the vpinball plugin SDK.
- `src/AudioOutput.*`: SDL3_mixer background music and speech mixing/ducking.
- `src/SpeechService.*`, `FliteSpeechService`, `ESpeakNgSpeechService`: speech
  callouts; backends are compiled in only when staged in `third-party`.
- `src/menu.cpp` → `ppuc-menu`: SDL3 game launcher/selector.
- `src/backbox.cpp` → `ppuc-backbox`: standalone DMD/backglass client that
  connects to the `libdmdutil` DMD server exposed by `ppuc-pinmame`
  (`--backbox-address`, default port `6789`).
- Also read: `README.md` (full CLI + game folder reference),
  `docs/RULES_AND_EFFECTS.md`, `docs/INTERCEPTOR.md`.

CMake options: `PPUC_BUILD_MENU`, `PPUC_BUILD_BACKBOX` (both `ON`),
`PPUC_USE_KMSDMD` (use `libkmsdmd` instead of SDL/`libsdldmd`, for KMS-only
Linux targets), `ENABLE_SANITIZERS`.

### `libppuc` — host library

- `src/PPUC.cpp` (~1.9k lines): YAML load + schema validation, board/device
  registration, config frame generation, derivation of dense coil/lamp/switch
  mappings, virtual-board handling, public API.
- `src/RS485Comm.cpp` (~2k lines): serial transport (`libserialport`), v2 frame
  encode/decode, output snapshot loop, token-ring switch polling, epoch session
  resync, timing tuning constants.
- `src/PPUC.h` / `src/PPUC_structs.h`: the public API consumed by `ppuc`.
- Consumes shared protocol headers copied from `io-boards` into
  `third-party/include/io-boards/`.

### `io-boards` — RP2040 firmware

- `src/main.cpp`: boot staging + built-in-LED status patterns, explicit UART mux
  (RS485 TX `GPIO0`, RX `GPIO1`, DE `GPIO2`), core split, watchdog that kills
  high-power outputs on stalled polling.
- `src/EventDispatcher/*`: v2 framing/parsing, mapping tables, switch token
  chain, bridging of v2 bitmaps into internal `Event`/`ConfigEvent` objects,
  `MultiCoreCrossLink` between cores, `V2DBG` counters.
- `src/IOBoardController.*`: board address from the analog DIP/resistor ladder
  on GPIO 28, local device registration, config topic handling.
- `src/IODevices/*`: dedicated switches (PIO), switch matrix (PIO), PWM outputs
  including the pulse-envelope and fast-flip safety logic.
- `src/EffectDevices/*`, `src/Effects/*`, `EffectsController`: core-1 effect
  engine (WS2812FX segments, PWM waves, blink/impulse/ramp effects) with a
  priority stack keyed by `EffectDevice* + deviceStackScope()`.
- `src/PPUCProtocolV2.h`: **the** wire format definition.
- Root-level `*.ino` files are legacy and are not the active firmware.

The firmware is **not** built by `ppuc`'s build graph — only its protocol
headers are consumed, via `libppuc`. The binary is built and flashed separately
with `pio run`.

### `libsdldmd`

SDL3 renderer implementing `DMDUtil::RGB24DMD`: windowed/fullscreen DMD,
translite/backglass images, scaling modes including `xbrz`, rotation, multi
screen placement. Used by both `ppuc-pinmame` and `ppuc-backbox`.

### `config-tool`

Drupal 11 site (install profile `web/profiles/custom/ppuc`, module
`web/modules/custom/ppuc_games`, theme `ppuc_claro`). Games are modelled as
node types: `game`, `i_o_board`, `switch`, `switch_matrix`,
`switch_matrix_switch`, `pwm_device`, `pwm_effect`, `addressable_led(s)`,
`led_effect`, `dip_switch`, `ppuc_settings`, `rule`.

`src/Controller/GamesController.php` (~1.3k lines) is the export engine and
generates, per game node:

| Route | Output |
|-------|--------|
| `/node/{n}/yaml` | `io-boards.yaml` for `libppuc` |
| `/node/{n}/ppuc.ini` | runtime INI |
| `/node/{n}/rule.lua` | merged Lua rules |
| `/node/{n}/rules.tar.gz` | rules + Blockly workspace |
| `/node/{n}/game-folder.tar.gz` | complete `--game` folder |
| `/node/{n}/zip`, `/node/import-zip` | full project export/import |

`boards/*.php` hold port→GPIO mappings per hardware board (`IO_16_8_1`,
`Out_8x10`). Dev environment is DDEV; a prebuilt image is published to
`ghcr.io/ppuc/config-tool`. This repository is outside the `ppuc` build graph.

### `ppuc_games`

Game folders and config-tool exports (`t2`, `tmwrp`, `flash`) used as real-world
test material, and the canonical example of the `--game` folder layout.

## 4. Configuration And Data Model

One game = one **game folder**, passed as `ppuc-pinmame --game <dir>`:

```
<game>/
  io-boards.yaml        # machine hardware description, consumed by libppuc
  ppuc.ini              # runtime settings ([Game], [Runtime], DMD, translite …)
  rules/*.lua           # Lua rules, loaded in filename order
  music/*               # background music playlist
  translite-on.*        # in-game backglass image
  translite-off.*       # attract backglass image
  <rom>.directb2s       # optional B2S backglass
  pup/pupvideos/<rom>/  # optional PUP pack
  pinmame/roms|nvram|cfg|altsound|altcolor/
```

Precedence: CLI options > `ppuc.ini` > game-folder defaults.

`io-boards.yaml` top-level sections (validated in `libppuc/src/PPUC.cpp`):
`ppucVersion`, `rom`, `serialPort`, `platform`, `debug`, `boards`,
`dipSwitches`, `switches`, `switchMatrix`, `switchGroups`, `pwmOutput`,
`ledStripes`, `coilGiMappings`, `mechs`. Per-device `effects` blocks configure
board-local effects and their triggers.

`coilGiMappings` supports Williams System 11 games that drive GI strings from a
coil: when PinMAME reports the mapped coil active, `ppuc-pinmame` sets the mapped
GI string to `onBrightness`, and to `offBrightness` when it goes inactive. See
`docs/RULES_AND_EFFECTS.md`.

Optional metadata the runtime depends on: `button: true` on switches
(cabinet/flipper controls, excluded from idle detection), `ballSearch: true` on
PWM outputs, `debounce` + `debounceMode` (`standard` / `fastFlip` /
`slowStable`), `pollEvents: true` on switch-capable boards.

> **Three-way contract.** Firmware config topics
> (`io-boards/src/EventDispatcher/Event.h`), YAML validation
> (`libppuc/src/PPUC.cpp`), and the exporter
> (`config-tool` `GamesController.php`) describe the same schema in three
> places with no shared artifact. Whenever a section, field, accepted type, or
> optional key is added anywhere, all three must be updated.

## 5. RS485 v2 Protocol (Summary)

Authoritative definition: `io-boards/src/PPUCProtocolV2.h`.

- UART on `Serial1`, baud `115200` (`ppuc::v2::kBaudRate`). The transceiver is
  an ADM3483, slew-rate-limited for noise immunity and capped at **250 kbps** —
  a permanent hardware ceiling, not a milestone.
- Sync `0xA5`, 5-byte header (`sync`, `typeAndFlags`, `nextBoard`, `sequence`,
  `epoch`), CCITT-16 CRC over header + payload, big-endian multibyte fields.
- Frame types: `OutputState 0x01`, `SwitchState 0x02`, `Heartbeat 0x03`,
  `Error 0x04`, `Setup 0x05`, `Mapping 0x06`, `Reset 0x07`, `Config 0x08`,
  `SwitchNoChange 0x09`, `ConfigAck 0x0A`, `Restart 0x0B`, `Trigger 0x0C`,
  `SwitchRefresh 0x0D`.
- Limits: `kMaxBoards 8`, `kMaxCoilBits 64`, `kMaxLampBits 256`,
  `kMaxSwitchBits 256`, 5 GI strings with packed 4-bit levels (`0..8`).
- Runtime is **snapshot-driven**: one full `OutputStateFrame` per cycle
  (nominally every 4 ms), no deltas. `kFlagDelta` is defined but unused.
- Addressing is by *dense bitmap index*; `MappingFrame` binds each index to the
  logical device number. Logical numbers may be sparse and high.
- Switch polling is a **token ring**: `header.nextBoard` selects the next board;
  each selected board answers once with `SwitchState` or `SwitchNoChange` and
  carries the next token; the chain ends at `kNoBoard (0xFF)`. Frames carry no
  sender ID — chain integrity relies on token order. Only boards marked
  `pollEvents: true` participate, so output-only boards cost no cycle time.
- Session control: `RestartFrame` is the normal startup/shutdown path (clears
  board config/runtime state and outputs without rebooting). `ResetFrame` is
  the explicit hard-reboot recovery path (`ppuc-pinmame --hard-reset`).
- Presence detection is via `ConfigAck`; unacknowledged boards become host-side
  **virtual boards** whose switches the host synthesizes into the chain.

### Board-owned pulse envelopes

`PwmDevices::update()` applies `minPulseTime` and `maxPulseTime` to **every**
activated output, not only fast-switch ones. A host "off" arriving before the
minimum is scheduled rather than executed; the maximum forces the coil off
regardless of host state. The host's output bit is a *request*; the board is the
authority on how long copper is energised.

Consequences: a hung host cannot cook a coil, and coil pulse fidelity is
decoupled from bus timing entirely. Valid thermal protection is any one of
`maxPulseTime`, hold power (`holdPower` + `holdPowerActivationTime`), or a
dual-winding coil with an EOS contact.

## 6. Cross-Repo Change Guide

| Change | Repos to touch |
|--------|----------------|
| CLI option, INI key, media/presentation, rules API | `ppuc` (+ `config-tool` if it should be exported) |
| YAML schema, board/device config, mapping semantics | `libppuc` + `config-tool`, usually `io-boards` |
| Frame layout, CRC, endianness, token ring, timing | `io-boards` + `libppuc` together |
| New board-local hardware capability or config topic | `io-boards` → `libppuc` config generation → `config-tool` UI/export → optional `ppuc` exposure |
| New Lua API function | `ppuc` (`LuaRulesEngine`) + `config-tool` Blockly blocks + docs |
| DMD rendering | `libsdldmd`, possibly `libdmdutil` upstream |

When in doubt, trace one feature end to end: config-tool export → `ppuc` CLI/INI
→ `libppuc` config + transport → firmware consumption → hardware behavior.

Never treat `ppuc/external/**`, `*/third-party/**`, `ppuc/ppuc/**`,
`config-tool/vendor/**`, or `io-boards/.pio/**` as source. They are build
artifacts and `external.sh` wipes them.

## 7. Dependencies And Version Pinning

### PPUC-owned or PPUC-forked

| Dependency | Source | Used by |
|------------|--------|---------|
| `pinmame` | `mkalkbrenner/pinmame` fork | `ppuc` (libpinmame) |
| `vpinball` | `PPUC/vpinball` fork | `ppuc` (MsgPlugin SDK, PUP/B2S/AltSound plugins) |
| `libdmdutil` | `PPUC/libdmdutil` fork | `libsdldmd` |
| `libserum`, `libzedmd`, `libvni`, `libpupdmd`, `libframeutil` | vpinball org | via `libdmdutil` |
| `Adafruit_NeoPixel` | `PPUC/Adafruit_NeoPixel` fork | firmware |
| `WS2812FX` | `PPUC/WS2812FX` fork | firmware |
| `WavePWM` | `mkalkbrenner/WavePWM` | firmware |

### Third-party

`SDL3`, `SDL3_image`, `SDL3_mixer`, `libserialport` (sigrok), `yaml-cpp`,
`Lua 5.4`, `cargs`, `sockpp`, `flite`, `espeak-ng`, `libaltsound`, `FFmpeg`,
`pinmame-nvram-maps` (tomlogic), `Bounce2`, `RPI_PICO_TimerInterrupt`,
`platform-raspberrypi` (maxgerhardt) + Earle Philhower Arduino core.

### The pin chain

Every dependency is pinned by SHA or tag in a `platforms/config.sh` per repo:

- `ppuc/platforms/config.sh`: `PINMAME_SHA`, `VPINBALL_SHA` (+ its SDL/SDL_ttf/
  libaltsound/ffmpeg SHAs), `LIBPPUC_SHA`, `LIBSDLDMD_SHA`, `SDL_IMAGE_SHA`,
  `SDL_MIXER_SHA`, `FLITE_SHA`, `ESPEAK_NG_SHA`, `LUA_VERSION`,
  `PINMAME_NVRAM_MAPS_SHA`
- `libppuc/platforms/config.sh`: `IO_BOARDS_SHA`, `LIBSERIALPORT_SHA`,
  `YAML_CPP_SHA`
- `libsdldmd/platforms/config.sh`: `SDL_SHA`, `LIBDMDUTIL_SHA`

**These pins are the real inter-repo version contract**, and they are
*transitive*: `ppuc` pins `libppuc`, which pins `io-boards`; `ppuc` pins
`libsdldmd`, which pins `libdmdutil`, which pins its own dependencies.

> A single git tag on `ppuc` therefore closes over the entire dependency tree.
> One tag reproduces every version in the stack — no manifest to maintain.

The corollary: a change in `io-boards` only reaches a normal `ppuc` build after
`IO_BOARDS_SHA` is bumped in `libppuc` **and** `LIBPPUC_SHA` is bumped in
`ppuc`. Always check whether a task requires a pin bump.

Run `tools/check-pins.sh` to resolve and verify the chain.

## 8. Build System

### Model

`ppuc/platforms/<platform>/<arch>/build.sh` is the central entry point. It
transitively builds the entire C++ stack: libsdldmd → libdmdutil → libzedmd /
libserum / libvni / libpupdmd, plus SDL3, SDL3_image, SDL3_mixer, flite,
espeak-ng, Lua, pinmame, libppuc → libserialport / yaml-cpp, and the vpinball
media plugins.

Only two components sit outside that graph: the **io-boards firmware**
(`pio run`) and **config-tool** (`ddev`).

Every C++ repo follows the same two-step pattern:

1. `platforms/<platform>/<arch>/external.sh` — download/build/stage all
   dependencies into `third-party/include`, `third-party/build-libs/...`,
   `third-party/runtime-libs/...`, using `external/*/cache.txt` marker files
   for incremental reuse.
2. `platforms/<platform>/<arch>/build.sh` — run `external.sh`, configure CMake
   with `-DPLATFORM=... -DARCH=...`, build, then package.

Platforms: `macos/{arm64,x64}`, `linux/{x64,aarch64}`, `win/{x64,x86}` (MSVC),
`win-mingw/x64`. `libsdldmd` additionally has ios/tvos/android scripts.

CMake feature detection is *staging-driven*: flite, espeak-ng, SDL3_mixer and
KMSDMD support are enabled only if their artifacts are present in
`third-party`. Partial staging silently produces a reduced-feature binary with
only a CMake warning — check the configure log when a feature seems "missing at
runtime".

`build.sh` packages into `ppuc/ppuc/`: the three executables, all runtime
libraries, `pinmame-nvram-maps`, and the VPX media plugins into `ppuc/plugins/`.
`ppuc/build/` holds the raw CMake output. Binaries link against
`@rpath`/`$ORIGIN`, so they only run next to their staged runtime libraries.

### Getting a complete build

```shell
git clone https://github.com/PPUC/ppuc.git
cd ppuc
PPUC_DEPENDENCY_SOURCE=github platforms/linux/x64/build.sh
```

That is the whole onboarding path. Substitute your platform/arch directory.

### Per-repo commands

```shell
# ppuc, incremental C++ only (deps already staged)
cmake --build build

# libppuc standalone
platforms/macos/arm64/external.sh
cmake -DPLATFORM=macos -DARCH=arm64 -DCMAKE_BUILD_TYPE=Release -B build
cmake --build build

# io-boards firmware
pio run                       # default env IO_16_8_1
pio run --target upload

# config-tool
ddev start
ddev drush deploy
```

Useful environment variables: `BUILD_TYPE=Debug`, `PPUC_VERBOSE=1`,
`PPUC_BUILD_VPINBALL_MEDIA_PLUGINS=0` (skip the expensive vpinball/FFmpeg plugin
build), `MACOSX_DEPLOYMENT_TARGET`.

### CI

- `ppuc/.github/workflows/ppuc.yml` — matrix build (win-mingw x64, macOS arm64,
  linux x64), patches the git SHA into `src/ppuc_version.h`, produces release
  artifacts. macOS x64 and linux aarch64 are currently commented out.
- `libppuc/.github/workflows/libppuc.yml`, `libsdldmd/.github/workflows/…` —
  library builds.
- `io-boards/.github/workflows/io-boards.yml` — `pio run` for `IO_16_8_1`,
  nightly schedule, enforces that a `vX.Y.Z` git tag matches
  `FIRMWARE_VERSION_*` in `src/PPUC.h`.
- `config-tool/.github/workflows/docker_image.yml` — publishes the docker image.

Version numbers are single-sourced from headers (`ppuc/src/ppuc_version.h`,
`libppuc/src/PPUC.h`, `io-boards/src/PPUC.h`) and parsed by CMake and CI.

## 9. Local Development Across Repositories

For normal builds nothing needs to be checked out except `ppuc` — dependencies
come from pinned archives. To build against a local checkout of a dependency,
set its `*_SOURCE_DIR` variable, resolved relative to the `ppuc` repo root:

```shell
LIBPPUC_SOURCE_DIR=../libppuc platforms/macos/arm64/build.sh
LIBDMDUTIL_SOURCE_DIR=../../elsewhere/libdmdutil platforms/macos/arm64/build.sh
```

Override only the dependency actually being changed; everything else stays
pinned, which keeps the build reproducible.

`PPUC_DEPENDENCY_SOURCE` controls the default:

- `explicit` (default) — pinned archives unless an explicit `*_SOURCE_DIR` is
  set, which is then symlinked in.
- `github` / `sha` — force pinned archives, ignoring any `*_SOURCE_DIR`.
- `local` — auto-point **all** managed dependencies at siblings of the `ppuc`
  repo, overridable with `PPUC_LOCAL_SOURCE_ROOT`. This requires *all* of
  `libppuc`, `libsdldmd`, `io-boards`, `libdmdutil`, `libzedmd`, `libserum`,
  `libvni`, `libframeutil`, `vpinball` to be present; a missing directory
  aborts `external.sh` under `set -e`. Use `explicit` with targeted overrides
  unless you genuinely have the full tree checked out side by side.

## 10. Invariants Worth Preserving

Established through real-machine testing. Do not casually revert them while
working on unrelated features.

**Firmware safety**

- Pulse-envelope enforcement in `io-boards/src/IODevices/PwmDevices.*`:
  `minPulseTime` defers a host "off"; `maxPulseTime` forces the coil off; after
  a max-pulse timeout a fast-switch coil must not refire until the switch opened
  and closed again. A stuck kicker/bumper switch would otherwise burn a coil.
  This must stay board-local — it must hold even if the host is busy or gone.
- The watchdog in `main.cpp` shuts off high-power outputs when polling stalls.
- Explicit UART mux setup in `main.cpp` (TX `GPIO0`, RX `GPIO1`, DE `GPIO2`).
- The switch token is forwarded *before* heavier runtime fanout on core 0.
- The fallback switch-reply TX path deliberately avoids
  `HardwareSerial::flush()` and uses a bounded wire-time delay before switching
  RS485 back to RX; earlier `flush()` behavior correlated with board freezes.

**Host transport timing** (`libppuc/src/RS485Comm.*`)

Switch-chain timing affects visible lamp/GI animation quality, not only switch
diagnostics. The current known-good baseline (Time Warp attract mode ran 1h40m
without communication errors) consists of: a long-enough switch-reply receive
window, `RS485_COMM_SERIAL_READ_TIMEOUT`, stale-input flush after a missed
chain, and resync only after `RS485_COMM_SWITCH_REPLY_MISS_THRESHOLD`
consecutive misses. Treat these as *tuning parameters per cabinet*, not
protocol constants.

**Effect stack**

WS2812 strips are segmented: `WS2812FXEffect::deviceStackScope()` returns the
segment, so effect arbitration is keyed by `EffectDevice* + segment`. A
high-priority effect on one segment must never stop an effect on another segment
of the same strip. Any future device with independent subregions should override
`deviceStackScope()` rather than widening the key.

**Protocol hardening constraint**

The bus carries a full output snapshot plus a complete switch token chain every
cycle, and 250 kbps is a permanent hardware ceiling. Sequence/loss detection,
heartbeats, and error reporting must ride in existing header/status fields or in
idle time — never as extra runtime frames. Any hardening change needs a
before/after measurement of output-frame cadence and switch-reply latency on
real hardware.

## 11. Conventions

- C++: `.clang-format` in `ppuc`, `libppuc`, `io-boards` — format before
  committing.
- Lua rules API: `camelCase`, namespaced under `ppuc.`.
- Read the repo-local `AGENTS.md` in `ppuc`, `libppuc`, and `io-boards` before
  non-trivial work in those repositories.
- `main` is the baseline branch for every repository.
