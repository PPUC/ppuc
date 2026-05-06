# Flash Rules And Effects

This document describes the behavior added by the PPUC layer on top of the original ROM when the game is run through PinMAME.

The original ROM still owns the game rules, scoring, lamp logic, solenoid timing, switch matrix behavior, ball flow, and attract/game modes.
PPUC adds extra outputs and trigger-driven presentation around that baseline.

## What PPUC Adds

PPUC currently adds four kinds of enhancements:

1. Extra addressable LEDs that do not exist in the original machine.
2. A shaker motor effect layer.
3. Trigger rules that emit DMD or PUP-style events from ROM state changes.
4. Optional speech callouts driven by those trigger rules.

PPUC does not replace the ROM logic here.
It observes ROM-driven switch, lamp, and coil activity and uses that activity to drive modern extras.

## Flash Example
### Cabinet LED String

The Flash game config defines an addressable LED string named `Cabinet` on IO board `1`, port `29`.

Source:
[Flash_877888bc-06a9-428a-91ab-6d821e104107.yml](../ppuc_games/flash/Flash_877888bc-06a9-428a-91ab-6d821e104107.yml)

#### Physical Layout

- LEDs `0-7` are assigned individually.
- LEDs `8-93` are grouped into segment `1`.

#### LEDs 0-7

These are configured as GI-linked cabinet/button/status lights:

- LED `0`: left flipper button bottom, `FF00FF`
- LED `1`: left flipper button bottom, `004AFF`
- LED `2`: start button, `FF7F00`
- LED `3`: right flipper button top, `FF00FF`
- LED `4`: right flipper button bottom, `004AFF`
- LED `5`: coin 1, `FFFFFF`
- LED `6`: coin 2, `FFFFFF`
- LED `7`: coin 3, `FFFFFF`

All of these map to GI string `1`, so they follow ROM-driven GI state rather than acting as independent PinMAME lamps.

#### LEDs 8-93

This range is configured as `segment 1`.
The intent of this segment is effect playback, not direct PinMAME lamp ownership.

Current configured effect:

- Description: `Exprerssion Lights`
- Segment: `1`
- Color: `0000FF`
- WS2812FX effect: `22`
- Duration: `0` (unlimited)
- Speed: `0`
- Priority: `1`
- Repeat: `-1`

Under the new model, this segment should be defined in YAML only as an effect target.
The logic deciding when it starts should live only in the rule file.

### Shaker Motor

The Flash game config also defines a shaker on IO board `1`, port `19`.

It is declared as:

- Type: `shaker`
- Number: `100`
- Power limit: `128`

This is not ROM-native hardware.
PPUC adds it as a modern feedback device.

### Configured Shaker Effects

Two PWM effects should be defined in the Flash YAML.

#### Jet Bumper Shaker

- Description: `Jet Bumper Shaker`
- PWM effect: `3`
- Frequency: `4`
- Max intensity: `128`
- Min intensity: `0`
- Duration: `0`
- Priority: `1`
- Repeat: `0`

Under the new model, this effect remains defined in YAML, but its trigger logic belongs only in the rule file.

#### Flash Shaker

- Description: `Flash Shaker`
- PWM effect: `1`
- Frequency: `4`
- Max intensity: `192`
- Min intensity: `0`
- Duration: `1000 ms`
- Priority: `2`
- Repeat: `0`

This is a second shaker pattern that should be triggered from the rule file instead of from YAML-generated trigger entries.

## Trigger Rules From `flash.rules`

Rules file:
[flash.rules](/Volumes/data/workspace/PPUC/ppuc_combined/ppuc/examples/flash.rules)

These rules watch ROM-visible switch, lamp, and coil changes and emit additional actions.

### Terminology

The word `source` comes from the older effect-trigger vocabulary inherited from DOF, the Direct Output Framework.

That old name is still visible in some code and protocol constants, but in the first field of a rule line it is better understood as a target channel, not as the source of the condition.

Example:

```text
F cabinet-flash-attract 1 : lamp_rising(5) && attract
```

Here:

- `F` is the target channel for the effect event that will be emitted. It means effect.
- `cabinet-flash-attract` is the effect trigger ID or name
- `lamp_rising(5) && attract` is the condition

There is also a silent channel:

- `S` means evaluate the rule and apply state/history side effects, but emit no external trigger.

Inside the rule expression itself we no longer use those old single-character codes.
The conditions are written with readable words:

- `ball(...)`
- `player(...)`
- `history(...)`
- `sequence(...)`
- `switch(...)`
- `lamp(...)`
- `coil(...)`
- `switch_rising(...)`
- `lamp_rising(...)`
- `coil_rising(...)`
- `attract`

Current limitation:
`ball(...)` is currently populated from PinMAME CPU RAM only for Williams System 3, System 4, and System 6 style games.
`player(...)` can now be supplied either by runtime state updates or by rules that use `set_player=<n>`.
Trigger history is retained per player for a rolling time window and is cleared when the table returns to attract mode.
Rules can also use `clear_player_history=<n>` to drop stored history for a specific player before the new trigger is recorded.

So the intended reading is:

- rule header: target/output channel plus trigger ID
- rule body: readable boolean logic over ball, switch, lamp, coil, and attract state

### DMD / PUP Trigger Rules

Target channel `D` emits extra trigger IDs for display/video/DMD-side integrations.

Current `D` triggers:

- `60001`: Ball 1
- `60002`: Ball 2
- `60003`: Ball 3
- `60005`: Bonus x2
- `60006`: Bonus x3
- `60007`: Extra Ball
- `60008`: Flash attract event
- `60009`: Game Over
- `60010`: Highscore
- `60012`: progress-1.mp4 trigger
- `60013`: progress-1.mp4 trigger
- `60014`: progress-1.mp4 trigger
- `60015`: progress-1.mp4 trigger
- `60016`: Shoot Again off
- `60017`: Special
- `60018`: Tilt

These are not original ROM outputs.
They are interpretation layers built from ROM state transitions.

### Speech Trigger Rules

Target channel `O` is used for speech.

Current speech-triggered rule:

- `60010`: Highscore

The speech text is defined in:
[flash.speech](/Volumes/data/workspace/PPUC/ppuc_combined/ppuc/examples/flash.speech)

Current speech lines:

- `60010`: `New highscore!`
- `60009`: `Game over.`
- `60007`: `Extra ball!`

Only IDs emitted by an `O` rule are spoken.
Right now the rules file explicitly emits `O 60010`, so `New highscore!` is the currently wired speech callout from the sample rule set.

## New Rule-Driven Board Effects

PPUC now also supports board-local effect triggering directly from the trigger-rule engine.

This is the new path intended for:

- named WS2812FX segment effects
- named shaker / WavePWM effects
- any effect that should run on the IO board and not be treated like a normal PinMAME lamp or coil

### Rule Source

Target channel `F` in a rules file means:

- evaluate the rule in `ppuc`
- send a runtime `EVENT_SOURCE_EFFECT`
- let the firmware match that event against configured LED or PWM effects

### Named Trigger IDs

Rules and named YAML effect definitions can now use names instead of only numeric IDs.

Example rule:

```text
F cabinet-flash-attract 1 : lamp_rising(5) && attract
```

Matching YAML effect definition:

```yaml
- name: cabinet-flash-attract
  effect: running_random
  segment: 1
  color: 0000FF
  speed: 0
  duration: 0
  mode: 0
  priority: 1
  repeat: -1
```

The name is hashed deterministically on both sides, so the rule file and YAML can refer to the same effect without a hand-maintained numeric ID table.
No YAML `trigger:` block is required for named effects anymore.

### Sample `F` Rules Added In The Repo

The sample `flash.rules` now also contains:

- `F cabinet-flash-attract 1 cooldown=20000 : lamp_rising(5) && attract`
- `F shaker-flash-hit 1 : coil_rising(6) && !attract`

Rules can also use `delay=<milliseconds>` when an effect should fire after a hold time instead of immediately.

These demonstrate the intended future direction:

- cabinet segment effects triggered by game state
- shaker effects triggered by ROM activity

Important current status:

- the runtime `F` transport path is implemented
- the sample `F` rules exist
- named YAML effects can now auto-register themselves as `F` targets

So the intended setup is now:

- YAML defines effect targets and parameters
- `flash.rules` defines all boolean logic and trigger conditions
- YAML effect `trigger:` blocks are no longer needed for named effects

## Config Tool Status

`../config-tool` now needs one explicit machine effect name per LED or PWM effect.

- That field is exported as YAML `name:`.
- The old effect-level `trigger:` field has been removed from LED and PWM effect content types.
- Generated YAML now emits only static effect definitions plus the machine effect name.
- All trigger logic now belongs exclusively in the `*.rules` file.

## Effect Names

PPUC now accepts named effect values in addition to numeric IDs.

### WS2812FX Names

Examples of accepted LED effect names:

- `static`
- `blink`
- `breath`
- `color_wipe`
- `scan`
- `running_lights`
- `sparkle`
- `strobe`
- `chase_rainbow`
- `running_color`
- `running_random`
- `larson_scanner`
- `comet`
- `fireworks`
- `fire_flicker`
- `tricolor_chase`
- `twinklefox`
- `rain`
- `heartbeat`
- `multi_comet`
- `popcorn`
- `oscillator`

### PWM / WavePWM Names

Accepted PWM effect names:

- `sine`
- `ramp_down_stop`
- `impulse`

These names make the Flash YAML easier to read than raw numeric effect IDs.

## Summary

For Flash, the PPUC layer currently adds:

- cabinet button/coin/start lighting on an external addressable LED string
- one large cabinet LED segment reserved for effect playback
- shaker motor feedback effects
- extra DMD/PUP trigger events derived from ROM activity
- optional speech callouts

And it now supports a cleaner next step:

- rule-driven named board effects via target channel `F`
- named trigger IDs
- named LED and PWM effect modes

That combination is the intended mechanism for cabinet animations and shaker patterns that should exist beside the original ROM rather than inside it.
