# Interceptor Lua Rules

The interceptor feature lets `ppuc-pinmame` react to physical machine events
before they are forwarded to PinMAME. Interceptor logic lives in the Lua rules
file loaded with:

```text
--rules <file>
```

The same Lua rules file can emit DMD/PUP triggers, speech callouts, board effect
triggers, and host-side interceptor actions.

## Runtime API

Interceptor rules use the `ppuc` Lua namespace:

```lua
function ppuc.onSwitchChanged(number, state)
  if ppuc.stateActive("ballSave") and ppuc.switchClosing(9) then
    ppuc.suppressSwitch(9)
    ppuc.pulseCoil(7, 120)
  end
end
```

Useful functions:

- `ppuc.switchState(number)`
- `ppuc.switchClosing(number)`
- `ppuc.switchOpening(number)`
- `ppuc.switchGroupState(name)`
- `ppuc.switchGroupClosing(name)`
- `ppuc.switchGroupOpening(name)`
- `ppuc.setState(name)` and `ppuc.setState(name, durationMs)`
- `ppuc.clearState(name)`
- `ppuc.stateActive(name)`
- `ppuc.suppressSwitch(number)`
- `ppuc.pulseCoil(number, durationMs)`
- `ppuc.blinkLamp(number, onMs, offMs)`
- `ppuc.stopBlinkLamp(number)`

## Switch Groups

Switch groups are declared in the game YAML and loaded by `libppuc`:

```yaml
switchGroups:
  playfield:
    switches: [10, 11, 12, 13]
```

The group name `buttons` is reserved. It is built automatically from switches
marked with `button: true` in `switches` or `switchMatrix.switches`.

## Runtime Output Overrides

PinMAME remains the normal owner of lamps and coils. Interceptor output actions
temporarily override a single output number:

- `ppuc.pulseCoil(...)` forces the coil on until the pulse timer expires, then
  restores the latest PinMAME coil state for that coil.
- `ppuc.blinkLamp(...)` forces a lamp blink until `ppuc.stopBlinkLamp(...)`,
  then restores the latest PinMAME lamp state for that lamp.

If PinMAME changes the same output while the override is active, the new PinMAME
state is remembered and restored when the override ends.

## Ball Save Example

This example arms ball save on switch 15, starts a 5 second save window when any
playfield switch closes, blinks lamp 8 while ball save is ready or active, and
suppresses the outhole switch while pulsing coil 7.

```lua
function ppuc.onSwitchChanged(number, state)
  if ppuc.switchClosing(15) then
    ppuc.setState("ballSaveReady")
  end

  if ppuc.stateActive("ballSaveReady") and ppuc.switchGroupClosing("playfield") then
    ppuc.clearState("ballSaveReady")
    ppuc.setState("ballSave", 5000)
  end

  if ppuc.stateActive("ballSave") and ppuc.switchClosing(9) then
    ppuc.suppressSwitch(9)
    ppuc.pulseCoil(7, 120)
  end
end

function ppuc.onRulesUpdate()
  if ppuc.stateActive("ballSaveReady") or ppuc.stateActive("ballSave") then
    ppuc.blinkLamp(8, 250, 250)
  else
    ppuc.stopBlinkLamp(8)
  end
end
```

The feature is intentionally host-side. It does not change the RS485 wire
protocol or move rules into the IO-board firmware.
