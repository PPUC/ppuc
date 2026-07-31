# AGENTS.md

## Scope

This repository contains the PPUC host applications:

- `ppuc-pinmame`: the game runtime — PinMAME emulation, rules, media, and
  machine integration
- `ppuc-menu`: SDL3 machine/game launcher
- `ppuc-backbox`: standalone backbox/DMD display client

It sits above `../libppuc` and indirectly above `../io-boards`. It is the
application layer, **not** the protocol definition. Protocol changes belong in
`../io-boards` (wire format) and `../libppuc` (host transport).

Because `platforms/*/*/build.sh` transitively builds the whole C++ stack, this
repository is also the root of the build graph and the home of the stack-wide
documentation:

- `docs/STACK.md`: architecture, dependency/pin model, and build system across
  **all** PPUC repositories. Read this first for anything cross-repo.
- `docs/ASSESSMENT.md`: independent architecture review, current state, and the
  prioritized stabilization backlog.
- `docs/V2_RESYNC_PROPOSAL.md`: design rationale for epoch-based session resync,
  with a status block covering what shipped and what did not.
- `tools/check-pins.sh`: resolves the transitive dependency pin chain, verifies
  each pin is on its repository's `main`, and reports local checkouts that drift
  from what is pinned. Run it before concluding that a cross-repo change took
  effect.

Repository-local reference documentation:

- `README.md`: full CLI reference, game-folder layout, INI, Lua rules, build
  instructions
- `docs/RULES_AND_EFFECTS.md`: what Lua rules can do and the `ppuc.*` API
- `docs/INTERCEPTOR.md`: host-side interception of physical machine events
- `examples/ppuc-pinmame.ini`: the authoritative annotated INI reference
- `examples/rules.lua`, `examples/menu.txt`, `examples/t2.yml`

## Source Layout

- `src/ppuc.cpp` (~5k lines): main entry point. CLI parsing (`cargs`), INI and
  game-folder resolution, PinMAME callbacks, runtime loop, bench test modes,
  ball search, switch refresh, DMD/translite setup, backbox server.
- `src/LuaRulesEngine.*`: embedded Lua 5.4 rules engine exposing the `ppuc`
  namespace.
- `src/MediaPluginHost.*` (~2k lines): hosts VPX message plugins (PUP, B2S,
  AltSound) through `MsgPluginManager` from the vpinball plugin SDK.
- `src/AudioOutput.*`: SDL3_mixer background music, ducking against PinMAME and
  speech audio.
- `src/SpeechService.*`, `FliteSpeechService.*`, `ESpeakNgSpeechService.*`,
  `SpeechCliSupport.*`: speech callouts.
- `src/menu.cpp` → `ppuc-menu`; `src/backbox.cpp` → `ppuc-backbox`.
- `src/RulesAction.h`, `src/ppuc_version.h` (version macros parsed by CMake and
  CI), `src/xbrz/` (license/changelog only; the scaler itself lives in
  `libsdldmd`).

## Build And Validation

The platform scripts stage every dependency into `third-party/` and then run
CMake:

```shell
PPUC_DEPENDENCY_SOURCE=github platforms/macos/arm64/build.sh   # full, pinned
cmake --build build                                            # incremental C++
```

`build.sh` packages the result into `ppuc/`: the three executables, the runtime
libraries, `pinmame-nvram-maps`, plus the VPX media plugins into `plugins/`.
`build/` holds the raw CMake output. Binaries are linked with
`@rpath`/`$ORIGIN`, so they only run next to their staged runtime libraries.

CMake options: `PPUC_BUILD_MENU`, `PPUC_BUILD_BACKBOX` (both `ON`),
`PPUC_USE_KMSDMD` (use `libkmsdmd` instead of SDL/`libsdldmd`, for KMS-only
Linux targets), `ENABLE_SANITIZERS` (Debug, macOS/Linux).

**Feature detection is staging-driven.** Flite, espeak-ng, and SDL3_mixer are
compiled in only when their headers *and* libraries are present in
`third-party/`. Incomplete staging silently produces a reduced-feature binary
with only a CMake warning — check the configure log before assuming a feature
is broken at runtime.

Useful environment variables:

- `BUILD_TYPE=Debug`, `PPUC_VERBOSE=1`
- `PPUC_BUILD_VPINBALL_MEDIA_PLUGINS=0` to skip the expensive vpinball/FFmpeg
  plugin build
- `LIBPPUC_SOURCE_DIR=../libppuc` (and equivalents) to build against a local
  checkout instead of the pinned archive — see the workspace `AGENTS.md` for the
  full `PPUC_DEPENDENCY_SOURCE` model. `PPUC_DEPENDENCY_SOURCE=local` is not
  usable from the `PPUC_stack` workspace because several media dependencies
  live elsewhere.

Dependency pins live in `platforms/config.sh` (`PINMAME_SHA`, `VPINBALL_SHA`,
`LIBPPUC_SHA`, `LIBSDLDMD_SHA`, `LUA_VERSION`, …). Changes in `../libppuc` or
`../io-boards` do not reach a normal build until those pins are bumped.

CI is `.github/workflows/ppuc.yml`: win-mingw x64, macOS arm64, linux x64.
macOS x64 and linux aarch64 are currently commented out. There are **no
automated tests** — CI only proves that the applications compile.

Format with `.clang-format` before committing.

## Configuration Model

One game = one game folder, passed as `--game <dir>`. `ppuc-pinmame` derives
`io-boards.yaml`, `ppuc.ini`, `rules/*.lua`, `music/`, translite images,
`<rom>.directb2s`, `pup/pupvideos/<rom>/`, and `pinmame/{roms,nvram,cfg,
altsound,altcolor}` from that single path. See `README.md` for the full layout.

Precedence: built-in defaults → INI values → dedicated CLI options.

The INI is consumed twice: `ppuc.cpp` parses `[Game]`, `[Paths]`, `[Backbox]`,
`[Runtime]`, `[Speech]`, `[BenchTest]`, `[Translite]`, `[VirtualDMD]`, while the
same file is handed to `libdmdutil` (`dmdConfig->parseConfigFile()`) for
`[ZeDMD]`, `[ZeDMD-WiFi]`, `[ZeDMD-SPI]`, `[Pixelcade]`, `[PIN2DMD]`,
`[OutputFilters]`. Keep `examples/ppuc-pinmame.ini` in sync when adding keys,
and check whether `../config-tool` should export the new key.

## Working Notes

- `src/ppuc.cpp` is the main runtime entry point for machine behavior.
- Bench test modes (`--switch-test`, `--coil-test`, `--lamp-test`, `--gi-test`,
  `--flasher-test`) depend on `libppuc` behavior. If a regression appears there,
  inspect `../libppuc/src/PPUC.cpp` and `../libppuc/src/RS485Comm.cpp` first.
- For non-WPC platforms, GI is forced on from `libppuc` rather than driven by
  PinMAME GI updates — older systems such as System 6 do not report usable GI.
- Host-side ball search lives here, not in `libppuc`. It is **disabled by
  default** (`--ball-search` / `Runtime.BallSearch`) because newer ROMs
  implement their own search, and it only pulses coils marked
  `ballSearch: true` in the game YAML.
- Switch refresh is always active (`--switch-refresh-idle-ms` /
  `Runtime.SwitchRefreshIdleMs`, default 15000, must be > 0). Switches marked
  `button: true` are ignored for the idle decision, and a held button suppresses
  ball search so a player can trap a ball on a raised flipper.
- Runtime output/switch-poll cadence is `--output-frame-interval-ms` /
  `Runtime.OutputFrameIntervalMs`, default 4 ms.
- `--skip-boards <csv>` forces configured boards virtual for fast bench testing.
  Skipped boards receive no config frames and are omitted from direct
  lamp/coil/switch test walks.
- `--close-coin-door` is a **virtual-board override only**. If the configured
  coin-door switch belongs to a physically present board, `ppuc` must ignore the
  override and report that ownership stays with hardware. This keeps one game
  config usable for both cabinet and bench without turning host-side switch
  injection into a global override mechanism.
- `--hard-reset` selects `ResetFrame` instead of the default soft
  `RestartFrame` board startup.

## Lua Rules

Rules are ordinary Lua scripts defining handlers on the `ppuc` namespace, loaded
from `<game>/rules/*.lua` when `Runtime.Rules=true`, or from an explicit
`--rules <path>`. Directory loading is non-recursive, in filename order, and
fails on the first load or runtime error. All rule files share one Lua state, so
use `local` unless a global is intentional. When several files define the same
handler, all of them run in load order.

Handlers: `onSwitchChanged`, `onLampChanged`, `onCoilChanged`, `onBallChanged`,
`onPlayerChanged`, `onRulesUpdate`.

Capability groups (see `README.md` / `docs/RULES_AND_EFFECTS.md` for the full API):

- state: `switchState`, `lampState`, `coilState`, `currentBall`, `currentPlayer`,
  `attractMode`
- named states, history, sequences, switch groups: `setState`, `clearState`,
  `stateActive`, `triggerHistory`, `triggerSequence`, `onlyOnceEvery`,
  `switchGroupState/Closing/Opening`
- outputs: `pupTrigger`, `speech`, `effectTrigger`, `pulseCoil`, `blinkLamp`,
  `stopBlinkLamp`
- interceptor: `suppressSwitch`, `sendSwitchToCpu`
- scheduling: `after(delayMs, fn)` — does not sleep inside the PinMAME loop; the
  callback runs from the normal rules update tick

`ppuc.effectTrigger(...)` reaches board-local effects through `libppuc` as a
runtime event with source `EVENT_SOURCE_EFFECT`, transported as a v2
`kFrameTrigger`. The game YAML effect block must declare a matching
`trigger.source: F` plus `trigger.name` or `trigger.number`.

The original ROM still owns scoring, lamp logic, solenoid timing, switch matrix
behavior, ball flow, and attract/game mode. Rules add presentation and extra
behavior around that baseline.

This replaced the old `--pup-triggers` rule-file format and the separate
`--speech-file`; both are gone.

## Cross-Layer Findings To Remember

- `libppuc` switch-chain timing directly affects visible gameplay and output
  quality here, not only switch diagnostics. After relaxing host-side
  switch-reply timing and making session resync less aggressive, lamp
  attract-mode animation became visibly correct again.
- Practical implication: if lamps, GI, or switch tests look "mostly alive but
  wrong", do not assume the bug is in `ppuc`. Check whether `libppuc` is
  thrashing v2 session resync or timing out on switch replies.
- For games with more IO boards, treat switch-chain timing as a transport tuning
  area that can affect normal runtime presentation.

## Virtual Boards

- Virtual/missing board support lives in `../libppuc`. Presence comes from an
  explicit `ConfigAck` handshake at startup, not from later switch traffic.
- `ppuc` contributes the operator-facing overrides: `--skip-boards` and
  `--close-coin-door`.
- Keep host-side switch injection scoped to switches owned by virtualized
  boards.
