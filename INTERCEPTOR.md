# Interceptor Rules

The interceptor feature lets `ppuc-pinmame` react to physical machine events
before they are forwarded to PinMAME. It is implemented as an extension of the
existing `--pup-triggers` rule engine, so the same rules file can now emit DMD,
PUP, speech, and board effect triggers, and can also apply host-side game logic.

Compared with `main`, the interceptor branch adds:

- named transient rule state with `set_state`, `clear_state`, and `state(...)`
- switch groups from game YAML with `switch_group(...)` and group edge checks
- multi-switch edge expressions such as `switch_rising(10, 11, 12)`
- switch suppression before the event reaches PinMAME
- host-side coil pulses and lamp blinking that temporarily override PinMAME
  output for the same output number

## Rule File

Interceptor rules live in the normal trigger file loaded with:

```text
--pup-triggers <file>
```

The general rule format remains:

```text
<target> <id-or-name> [value] [options...] : <expression>
```

Use target `S` for interceptor-only rules. `S` rules are silent: they can set
state, suppress switches, pulse coils, and blink lamps, but they do not emit an
external trigger.

## Added Options

- `set_state=<name>` sets a named state when the rule matches. Names may
  contain letters, numbers, `_`, `-`, and `.`.
- `state_ms=<milliseconds>` sets an optional lifetime for `set_state`. If
  omitted or zero, the state remains set until a matching `clear_state` rule
  runs.
- `clear_state=<name>` clears a named state when the rule matches. If a rule has
  both `clear_state` and `set_state`, the clear happens first.
- `suppress_switch=<number>` prevents a matching physical switch close from
  being sent to PinMAME. The later open edge for the same switch is also
  suppressed, so PinMAME sees no switch edge at all. Rules still see the
  physical switch state.
- `pulse_coil=<number>` pulses a coil from the host when the rule matches.
- `pulse_ms=<milliseconds>` sets an optional pulse duration for `pulse_coil`.
  The default is 120 ms.
- `blink_lamp=<number>` blinks a lamp while the rule expression is true.
- `blink_on_ms=<milliseconds>` and `blink_off_ms=<milliseconds>` set optional
  blink timing. Both default to 250 ms.

## Added Expressions

```text
state(<name>)
switch_group(<name>)
switch_rising_group(<name>)
switch_falling_group(<name>)
switch_rising(<n1>, <n2>, ...)
switch_falling(<n1>, <n2>, ...)
```

`state(<name>)` is true while the named state is active.

`switch_group(<name>)` is true when any switch in the group is closed.

`switch_rising_group(<name>)` and `switch_falling_group(<name>)` match edge
events from any switch in the group.

`switch_rising(...)` and `switch_falling(...)` still accept one switch number,
and now also accept a comma-separated list.

## Switch Groups

Switch groups are declared in the game YAML and are loaded by `libppuc`:

```yaml
switchGroups:
  playfield:
    switches: [10, 11, 12, 13]
```

The group name `buttons` is reserved. It is built automatically from switches
marked with `button: true` in `switches` or `switchMatrix.switches`.

```yaml
switches:
  -
    description: 'Start Button'
    number: 13
    board: 1
    port: 1
    debounce: 50
    button: true
```

Group switch numbers are sorted and de-duplicated when the YAML is loaded.

## Runtime Output Overrides

PinMAME remains the normal owner of lamps and coils. Interceptor output actions
temporarily override a single output number:

- `pulse_coil` forces the coil on until the pulse timer expires, then restores
  the latest PinMAME coil state for that coil.
- `blink_lamp` forces a lamp blink while the expression is true, then restores
  the latest PinMAME lamp state for that lamp.

If PinMAME changes the same output while the override is active, the new
PinMAME state is remembered and restored when the override ends.

## Ball Save Example

This example arms ball save on switch 15, starts a 5 second save window when
any playfield switch closes, blinks lamp 8 while ball save is ready or active,
and suppresses the outhole switch while pulsing coil 7.

```text
S ball-save-ready set_state=ball_save_ready : switch_rising(15)
S ball-save-active set_state=ball_save state_ms=5000 clear_state=ball_save_ready : state(ball_save_ready) && switch_rising_group(playfield)
S shoot-again-blink blink_lamp=8 : state(ball_save_ready) || state(ball_save)
S outhole-save suppress_switch=9 pulse_coil=7 pulse_ms=120 : state(ball_save) && switch_rising(9)
```

Required YAML group:

```yaml
switchGroups:
  playfield:
    switches: [10, 11, 12, 13]
```

## Implementation Notes

The branch wires switch processing through `PUPTriggerEngine::ProcessSwitchState`
so the engine can return whether a switch should continue to PinMAME. Lamp and
coil updates from PinMAME go through a small output override layer that preserves
the last PinMAME state while host-side pulses and blinks are active.

The feature is intentionally host-side. It does not change the RS485 wire
protocol or move rule logic into the IO-board firmware.
