# Rules And Effects

PPUC rules are Lua scripts loaded by `ppuc-pinmame` with:

```text
--rules <path>
```

The path is a directory containing `.lua` files. `ppuc-pinmame` loads top-level
`.lua` files in filename order.

## What Rules Can Do

Lua rules observe PinMAME and physical-machine state, then trigger extra PPUC
behavior:

- send PUP pack triggers with `ppuc.pupTrigger(...)`
- play a Serum colorization scene with `ppuc.serumScene(...)`
- play AltSound pack sounds with `ppuc.soundCommand(...)`
- trigger board-local effects with `ppuc.effectTrigger(...)`
- color the GI strings with `ppuc.setGiProgram(...)`
- speak text with `ppuc.speech(...)`
- play an audio file from the game folder with `ppuc.playSound(...)`
- delay work without blocking with `ppuc.after(...)`
- suppress or later send physical switches to PinMAME
- pulse coils and blink lamps as host-side interceptor output overrides

In the default `pinmame` engine the original ROM still owns scoring, original
lamp logic, solenoid timing, switch matrix behavior, ball flow, and attract/game
mode. PPUC adds extra output and presentation behavior around that baseline.

With `engine: script` there is no ROM: a C++ game core owns players, ball flow,
scoring, tilt and attract instead. Rules see the same `ppuc.currentBall()`,
`ppuc.currentPlayer()` and `ppuc.attractMode()` with the same meanings — only the
thing writing them changes — so a rules file written for a ROM game keeps working.
See [`EM_GAMES.md`](EM_GAMES.md).

## Lua API

Common state helpers:

```lua
ppuc.switchState(number)
ppuc.lampState(number)
ppuc.coilState(number)
ppuc.currentBall()
ppuc.currentPlayer()
ppuc.attractMode()
```

`ppuc.attractMode()` is true while no game is in progress. Under PinMAME it
comes from the ROM's own `game_over` flag in its
[pinball-memory-maps](https://github.com/tomlogic/pinball-memory-maps) map, so a
drained ball, the bonus count and a tilt are all still part of the game. A ROM
whose map has no usable `game_over` falls back to the configured game-on
solenoid; on a Fliptronic or later WPC that is the flipper enable, which drops
between balls, so attract mode then flickers on at every drain. ppuc prints
which of the two it uses at startup (`Attract mode: ...`).

Named states, timing, and switch groups:

```lua
ppuc.setState(name)
ppuc.setState(name, durationMs)
ppuc.clearState(name)
ppuc.stateActive(name)
ppuc.onlyOnceEvery(name, durationMs)
ppuc.switchGroupState(name)
ppuc.switchGroupClosing(name)
ppuc.switchGroupOpening(name)
```

Trigger history and sequences:

```lua
ppuc.triggerHistory(id)
ppuc.triggerHistory(id, windowMs)
ppuc.triggerSequence(windowMs, id1, id2, id3)
```

Outputs and integrations:

```lua
ppuc.after(delayMs, function() ... end)
ppuc.pupTrigger(source, id, value)
ppuc.serumScene(id)
ppuc.soundCommand(cmd [, board])
ppuc.speech(text)
ppuc.playSound(file)
ppuc.effectTrigger(id, value)
ppuc.effectTrigger(name, value)
ppuc.suppressSwitch(number)
ppuc.sendSwitchToCpu(number, state)
ppuc.pulseCoil(number, durationMs)
ppuc.setCoil(number, on)
ppuc.blinkLamp(number, onMs, offMs)
ppuc.stopBlinkLamp(number)
ppuc.ballSave(durationMs)
ppuc.setGiProgram(string, colors, effect, periodMs)
```

`ppuc.setCoil(number, true)` holds a coil on until `ppuc.setCoil(number,
false)`: a diverter, a gate, a motor. `on` may also be a number (0 = off).
Releasing ends a running `ppuc.pulseCoil` on the same coil too, and the coil
returns to what the engine wants (off under `engine: script` unless GameCore
drives it). The rule only says on and off: power, hold power and the maximum
on-time stay the coil's configuration (config-tool), which the board
enforces, so a rule that forgets to release cannot cook a coil beyond what
its configuration allows.

`ppuc.ballSave` works under both engines: ball save lives in the switch path
ahead of the engine, not inside it. So do `ppuc.setGiProgram`, see
[GI Colors](#gi-colors), and `ppuc.soundCommand`, see
[AltSound Commands](#altsound-commands). Under `engine: script` a further
`ppuc.game.*` and `ppuc.dmd.*` API is available; see [`EM_GAMES.md`](EM_GAMES.md).

Handlers:

```lua
function ppuc.onSwitchChanged(number, state)
end

function ppuc.onLampChanged(number, state)
end

function ppuc.onCoilChanged(number, state)
end

function ppuc.onBallChanged(ball)
end

function ppuc.onPlayerChanged(player)
end

function ppuc.onRulesUpdate()
end
```

## Playing Sounds

`ppuc.playSound(file)` plays an audio file once, mixed over the game's own
sound at the master `Volume`. `file` is relative to the game folder, for
example `"sounds/intro.mp3"`; an absolute path or one leaving the folder with
`..` is refused. MP3 is the format to use: it is the one SDL_mixer is built
with. One sound plays at a time, and a second call replaces one still
playing. It plays in attract mode too, where the music is off. It returns
whether the sound started: `false` without sound output, or when the file is
missing or unreadable, which is also printed.

Once per attract mode, 10 seconds after it began:

```lua
local wasAttract = false
local pending = false

function ppuc.onRulesUpdate()
  if ppuc.attractMode() then
    if not wasAttract then
      wasAttract = true
      pending = true
      ppuc.setState("intro wait", 10000)
    end
    if pending and not ppuc.stateActive("intro wait") then
      pending = false
      ppuc.playSound("sounds/intro.mp3")
    end
  else
    wasAttract = false
    pending = false
    ppuc.clearState("intro wait")
  end
end
```

## Interceptor Rules

Interceptor behavior is part of the Lua rules engine. It lets `ppuc-pinmame`
react to physical machine events before they are forwarded to PinMAME.

```lua
function ppuc.onSwitchChanged(number, state)
  if ppuc.stateActive("ballSave") and number == 9 and state == 1 then
    ppuc.suppressSwitch(9)
    ppuc.pulseCoil(7, 120)
  end
end
```

Suppressed switches can be sent to PinMAME later:

```lua
function ppuc.onSwitchChanged(number, state)
  if number == 16 and state == 1 then
    ppuc.suppressSwitch(16)
    ppuc.after(500, function()
      ppuc.sendSwitchToCpu(16, 1)
    end)
    ppuc.after(650, function()
      ppuc.sendSwitchToCpu(16, 0)
    end)
  end
end
```

`ppuc.after(...)` schedules work on the rules update tick. It does not sleep and
does not block `ppuc-pinmame`.

See [`INTERCEPTOR.md`](INTERCEPTOR.md) for a focused interceptor reference.

## Switch Groups

Switch groups are declared in game YAML and loaded by `libppuc`:

```yaml
switchGroups:
  playfield:
    switches: [10, 11, 12, 13]
```

The `buttons` group name is reserved. It is built automatically from switches
marked with `button: true` in `switches` or `switchMatrix.switches`.

In the config-tool, switch group names are entered on the game node as one name
per line:

```text
playfield
standups
```

Switches are added to those groups with the checkboxes on each switch edit page.

## SYS11 Coil-To-GI Mapping

Some Williams System 11 games use one or more coils to control GI strings or
parts of GI. PPUC supports this with `coilGiMappings` in game YAML:

```yaml
coilGiMappings:
  - coil: 12
    gi: 1
    onBrightness: 8
    offBrightness: 0
  - coil: 12
    gi: 2
    onBrightness: 8
    offBrightness: 0
```

When PinMAME reports the mapped coil as active, `ppuc-pinmame` sets the mapped
GI string to `onBrightness`. When the coil becomes inactive, it sets the GI
string to `offBrightness`.

In the config-tool, coil/GI mappings are entered on the game node as one mapping
per line:

```text
12: 1,2 = 8/0
13: 3 = 0/8
```

The format is:

```text
coil: gi[,gi...] = onBrightness/offBrightness
```

Brightness values are clamped to the PPUC GI range `0..8`.

## AltSound Commands

```lua
ppuc.soundCommand(cmd [, board])
```

Hands sound command `cmd` to the AltSound plugin exactly as if the CPU board had
sent it to sound board `board` (default `0`). `cmd` is the number the pack's
CSV is keyed by -- on WPC DCS / WPC-S / WPC95 the 16-bit DCS command, e.g.
`0x03e6`. Returns whether the command was handed over: `false` when AltSound
is not running through the plugin host, or when the hardware generation cannot
express the number (the log says why). A command outside `0x0000..0xFFFF` or a
board outside `0..255` is a script error.

Under the `pinmame` engine the commands come on top of the ROM's own. Under
`engine: script` they are the only ones; this is how a ROM-less
reimplementation of a game plays the original sounds, by sending the commands
the ROM sends at the same moments. A custom game adds sounds as new numbers
(e.g. `0x1000` and up) with rows of their own in the pack.

**Why the hardware generation matters.** libaltsound is not given commands; it
is given the bytes the CPU board wrote to the sound board, one at a time, and
reassembles commands from them by rules that differ per generation. A DCS ROM
writes every command as two bytes, high byte first, so `ppuc.soundCommand`
splits `0x03e6` into `0x03`, `0xe6`. With a ROM, PinMAME reports the
generation. Without one, `[Game] AltSoundHardware` names it (`wpcs`, `wpcdcs`,
`wpc95`, `wpcdmd`, `s11c`, `de`, `ws`, ...); left at `none`, every command
reaches the pack whole, which plays plain commands too but skips DCS's own
volume sequences (`0x55aa` and the like), which a script may send on purpose.

Per generation:

| Generation | Accepted `cmd` | Sent as |
|---|---|---|
| `wpcdcs`, `wpcs`, `wpc95dcs`, `wpc95` | `0x0000..0xFFFF` | high byte, low byte |
| `wpcalpha2`, `wpcdmd`, `wpcfliptronic` | one byte except `0x7A`, or `0x7Axx` | one byte, or `0x7A` and the low byte |
| `wpcalpha1`, `s11`, `s11x`, `s11b2`, `s11c` | `0x00..0xFF`; a second board's `0x01xx` is its low byte with `board = 1` | one byte |
| `de`, `dedmd16/32/64` | `0x01..0xFE` | one byte |
| `ws`, `ws1`, `ws2` | `0xFC00..0xFFFF` | high byte, low byte |
| `gts80` | anything but `0` | whole |
| `none`, `by17`, `by35` | anything | whole |

**Timing against the ROM.** The ROM's bytes reach AltSound one by one through
the main loop, so a ROM's pair can be half delivered when a rule's command is
sent. A rule's pair waits until the ROM's pair is complete -- at most 100 ms,
which is how long the DCS board itself keeps a half-received command.

`DebugSoundCommands` (or `--debug-sound-commands`) logs every rule command as
`Lua sound command: board=0 hex=0x03E6`, next to the ROM's own
`PinMAME sound command` lines.

## GI Colors

On a playfield whose GI is RGB LEDs (WS2812 on the IO boards), a rule can give a
GI string another color than the one its LEDs are configured with. Custom games
use it for mode colors: while a mode runs, the GI takes its color, and with
several colored modes it cycles or cross-fades between them.

```lua
ppuc.setGiProgram(string, colors, effect, periodMs)
```

- `string`: the GI string, `1`-`5`.
- `colors`: a list of `"RRGGBB"` (a leading `#` is fine, numbers like
  `0xFF3000` too), highest priority first, or a single color. The boards hold
  four; further colors are dropped.
- `effect`: `"static"` (the default) shows the first color, `"cycle"` switches
  hard to the next color every `periodMs`, `"fade"` cross-fades to the next one
  over each `periodMs`, the last back into the first. `"default"` puts back the
  colors the LEDs are configured with.
- `periodMs`: time per color for `cycle` and `fade`, up to 65535.

Brightness is not part of it. GI dimming keeps working as before, from the ROM
or from the game, and the color follows it, through afterglow and heat-up too.

Returns `true` when the boards will show the program, `false` when they cannot:
no IO boards, or a board on firmware older than 0.5.9 (logged once at the first
call). A string number, color or effect it cannot take is an error naming the
line.

```lua
-- Car chase is red on strings 1 and 2; back to normal when it ends.
ppuc.setGiProgram(1, {"FF3000"})
ppuc.setGiProgram(2, {"FF3000"})
...
ppuc.setGiProgram(1, nil, "default")

-- Two colored modes at once.
ppuc.setGiProgram(1, {"FF3000", "0040FF"}, "fade", 2000)
```

Calling it with the color a string is configured with and `"static"` is the
same as `"default"`: that is what a game says when no colored mode runs, and the
board then restores every LED's own color, so a string with a differently
colored accent LED gets its accent back. On a string of mixed colors any of its
configured colors counts. A program the boards already show is not sent again,
so a rule may set the same program on every update.

How it works, and what it costs: the boards run the program themselves. The
host sends one 25-byte frame per change of program, not a frame per color step,
and sends it again after a bus resync. A GI string is meant to be wired as one
LED segment on one board. Every LED assigned to the string takes the color
wherever it is, since the frame goes to all boards, but a fade split across two
boards runs on two clocks and is only roughly in step.

## Board Effect Triggers

Board-local PWM and LED effects are defined in YAML as effect targets. Lua rules
start those effects with `ppuc.effectTrigger(...)`.

```lua
function ppuc.onSwitchChanged(number, state)
  if number == 18 and state == 1 then
    ppuc.effectTrigger("jet-bumper", 1)
  end
end
```

The board effect trigger source is `F` internally. In YAML effect definitions,
use matching `trigger.source: F` plus `trigger.name` or `trigger.number` when a
simple board-side trigger is needed. For new rule-driven behavior, prefer Lua
logic and named effects.

### Triggering from lamp state

A board effect can also be triggered by a lamp rather than by a rule, with
`trigger.source: L`. No frame is sent for this. The board compares each output
frame's lamp bitmap against the previous one and raises a light event for every
bit that changed, which the effects controller matches against the triggers
configured at startup.

Two things about that matching are easy to get wrong:

- The number a trigger matches is the **logical lamp number**, the one in the
  YAML — not the bitmap position. The board translates via the mapping it was
  sent during configuration.
- The match is exact on source, number *and* value. `value: 1` fires when the
  lamp turns on and never when it turns off; a trigger that should do both
  needs two entries.

Effects are stacked per device, and for an LED strip the segment is part of
that key. Effects on different segments of one strip therefore run
independently and do not terminate or suspend each other, while two effects on
the same segment resolve by priority.
