# Rules And Effects

PPUC rules are Lua scripts loaded by `ppuc-pinmame` with:

```text
--rules <path>
```

The path can be one `.lua` file or a directory containing `.lua` files. When a
directory is used, `ppuc-pinmame` loads top-level `.lua` files in filename order.

The old trigger-rule file format and separate speech text file are no longer
supported. Speech callouts are written directly in Lua with `ppuc.speech(...)`.

## What Rules Can Do

Lua rules observe PinMAME and physical-machine state, then trigger extra PPUC
behavior:

- send DMD/PUP triggers with `ppuc.pupTrigger(...)`
- trigger board-local effects with `ppuc.effectTrigger(...)`
- speak text with `ppuc.speech(...)`
- delay work without blocking with `ppuc.after(...)`
- suppress or later send physical switches to PinMAME
- pulse coils and blink lamps as host-side interceptor output overrides

The original ROM still owns scoring, original lamp logic, solenoid timing,
switch matrix behavior, ball flow, and attract/game mode. PPUC adds extra output
and presentation behavior around that baseline.

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

Outputs and integrations:

```lua
ppuc.after(delayMs, function() ... end)
ppuc.pupTrigger(source, id, value)
ppuc.speech(text)
ppuc.effectTrigger(id, value)
ppuc.effectTrigger(name, value)
ppuc.suppressSwitch(number)
ppuc.sendSwitchToCpu(number, state)
ppuc.pulseCoil(number, durationMs)
ppuc.blinkLamp(number, onMs, offMs)
ppuc.stopBlinkLamp(number)
```

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

See `INTERCEPTOR.md` for a focused interceptor reference.

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
