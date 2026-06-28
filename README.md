# PPUC - Pinball Power-Up Controllers

The *Pinball Power-Up Controllers* are a set of hard- and software designed to repair and enhance the capabilities of
classic pinball machines of the 80s and 90s and to drive the hardware of home brew pinball machines.
The project is in ongoing development. Visit the [PPUC Page](http://ppuc.org) for further information.
This directory contains the PinMAME related parts, mainly the building blocks to emulate a pinball CPU that drives
PPUC I/O boards.

## Motivation

We want to enable people to be creative and to modernize old pinball machines using today's technology. Our goal is to
establish an open and affordable platform for that. Ideally people will publish their game-specific PPUs so others could
leverage and potentially improve them. We want to see a growing library of so-called *Pinball Power-Ups* (PPUs) and a
vital homebrew pinball community.

## Licences

The code in this directory and all sub-directories is licenced under GPLv3, except if a different license is mentioned
in a file's header or in a sub-directory. Be aware of the fact that your own enhancements of ppuc need to be licenced
under a compatible licence.

PPUC uses
* [libpinmame](https://github.com/vpinball/pinmame)
* [libdmdutil](https://github.com/vpinball/libdmdutil)
* [cargs](https://github.com/likle/cargs)
* [yaml-cpp](https://github.com/jbeder/yaml-cpp)
* [openal-soft](https://github.com/kcat/openal-soft/)
* [libppuc](https://github.com/PPUC/libppuc)

## Documentation

These components are still in an early development stage and the documentation will grow.

### Command Line Options

* -c path
    * path to config file
    * required
* --ini-file path
    * path to a ppuc runtime ini file
    * optional
    * values from dedicated command line options override values from the ini file at runtime
* -r rom name
    * rom to use, overwrites *rom* setting in config file
    * optional
* -s serial device
    * serial device path to use, overwrites *serialPort* setting in config file
    * optional
* -d
    * enable debug mode, overwrites *debug* setting in config file
    * optional
* --debug-effects
    * enable effect trigger debug output
    * optional
* -u
    * enable Serum colorization
    * optional
* -t VALUE
    * Serum timeout in milliseconds to ignore unknown frames
    * optional
* -p VALUE
    * Serum ignore number of unknown frames
    * optional
* --rules path
    * path to one Lua rules file or a directory containing Lua rule files
    * optional
* --music-files csv
    * comma-separated MP3 playlist for gameplay background music
    * plays only while the game is not in attract mode
    * ducks while PinMAME or speech audio is active
    * optional
* --switch-refresh-idle-ms VALUE
    * re-read all IO board switches after this many milliseconds without non-button switch updates
    * default: `15000`
    * always active; the value must be greater than zero
* --ball-search
    * enable host-side ball search for coils marked `ballSearch: true` in the game YAML
    * optional and disabled by default because newer ROMs often implement their own ball search
* --ball-search-delay-ms VALUE
    * first ball-search delay after no non-button switch activity while the game is running
    * default: `15000`
* --ball-search-round-delay-ms VALUE
    * delay between complete ball-search rounds
    * default: `5000`
* --speech-backend value
    * speech backend to use: `auto`, `flite`, `espeak-ng`
    * optional
* --speech-voice value
    * speech voice name, mainly for `espeak-ng`
    * optional
* --speech-rate value
    * speech rate in words per minute, mainly for `espeak-ng`
    * optional
* --speech-pitch value
    * speech pitch `0-100`, mainly for `espeak-ng`
    * optional
* -i
    * render display in console
    * optional
* -h
    * help

An example runtime ini file is available at `examples/ppuc-pinmame.ini`.

### Switch Refresh And Ball Search

`ppuc-pinmame` always runs a switch-refresh safety net. If no non-button switch
update arrives for `Runtime.SwitchRefreshIdleMs`, the host sends a v2 switch
refresh command. IO boards re-read their switch inputs, restart their local
switch readers, and return full switch bitmaps through the normal switch chain.
The normal runtime output/switch-poll cadence is controlled by
`Runtime.OutputFrameIntervalMs` or `--output-frame-interval-ms`; the default is
`4`.

Switches can be marked as cabinet/player buttons in the game YAML:

```yaml
switches:
  -
    description: 'LEFT FLIPPER BUTTON'
    number: 63
    board: 1
    port: 17
    debounce: 3
    debounceMode: fastFlip
    button: true
```

Button switch activity does not postpone switch refresh or ball search. A held
button does suppress ball search, so a player can hold a ball on a raised
flipper without the host firing search coils.

Host-side ball search is separate from switch refresh and is disabled by
default. Enable it only for older ROMs that do not perform their own ball
search:

```ini
[Runtime]
OutputFrameIntervalMs = 4
BallSearch = true
BallSearchDelayMs = 15000
BallSearchRoundDelayMs = 5000
```

Only coils marked `ballSearch: true` are fired. They are pulsed one after
another with the same short pulse style as coil test, then the host waits
`BallSearchRoundDelayMs` before starting the next round if the machine is still
quiet.

```yaml
pwmOutput:
  -
    description: 'Outhole Kicker'
    number: 7
    board: 4
    port: 3
    type: solenoid
    ballSearch: true
```

### Lua Rules

Use `--rules <path>` to run Lua rules. The path can point to one `.lua` file or
to a directory. Directory loading is non-recursive, loads top-level `*.lua`
files in filename order, and fails on the first load or runtime error. Rules
are independent from `--pup`, and can also drive speech callouts, board-local
PPUC effects, and host-side interceptor behavior.

Rules define handlers on the `ppuc` namespace:

```lua
function ppuc.onSwitchChanged(number, state)
  if number == 13 and state == 1 and ppuc.lampState(42) then
    ppuc.pupTrigger("P", 100, 1)
  end

  if ppuc.stateActive("ballSave") and number == 9 and state == 1 then
    ppuc.suppressSwitch(9)
    ppuc.pulseCoil(7, 120)
  end
end

function ppuc.onLampChanged(number, state)
  if number == 23 and state == 1 and not ppuc.attractMode() then
    ppuc.speech("New highscore!")
  end
end
```

When multiple rule files define the same handler, all handlers run in load
order. Rule files share one Lua state, so use `local` helper functions and
variables unless cross-file globals are intentional.

Supported handlers:
* `ppuc.onSwitchChanged(number, state)`
* `ppuc.onLampChanged(number, state)`
* `ppuc.onCoilChanged(number, state)`
* `ppuc.onBallChanged(ball)`
* `ppuc.onPlayerChanged(player)`
* `ppuc.onRulesUpdate()`

State helpers and handler values:
* `ppuc.switchState(number)`, `ppuc.lampState(number)`, `ppuc.coilState(number)`
* `ppuc.currentBall()`, `ppuc.currentPlayer()`, `ppuc.attractMode()`
* Changed handlers receive `number` and `state`; use `state == 1` for active/closed/on and `state == 0` for inactive/open/off.

Named states, history, and switch groups:
* `ppuc.setState(name)` and `ppuc.setState(name, durationMs)`
* `ppuc.clearState(name)` and `ppuc.stateActive(name)`
* `ppuc.triggerHistory(id)` and `ppuc.triggerHistory(id, windowMs)`
* `ppuc.triggerSequence(windowMs, id1, id2, id3)`
* `ppuc.onlyOnceEvery(name, durationMs)` returns true only once per named time window
* `ppuc.switchGroupState(name)`, `ppuc.switchGroupClosing(name)`, `ppuc.switchGroupOpening(name)`

Switch groups can be declared in the game YAML:

```yaml
switchGroups:
  playfield:
    switches: [10, 11, 12, 13]
```

The group `buttons` is built in from switches marked `button: true` and cannot
be overridden in YAML.

Outputs and integrations:
* `ppuc.after(delayMs, function() ... end)` schedules non-blocking delayed Lua work
* `ppuc.pupTrigger(source, id, value)`
* `ppuc.speech(text)`
* `ppuc.effectTrigger(id, value)` or `ppuc.effectTrigger(name, value)`
* `ppuc.suppressSwitch(number)`
* `ppuc.sendSwitchToCpu(number, state)`
* `ppuc.pulseCoil(number, durationMs)`
* `ppuc.blinkLamp(number, onMs, offMs)` and `ppuc.stopBlinkLamp(number)`

`ppuc.after(...)` does not sleep inside the PinMAME loop. It stores the callback
and runs it from the normal rules update tick after the requested delay:

```lua
function ppuc.onSwitchChanged(number, state)
  if number == 16 and state == 1 then
    ppuc.after(500, function()
      ppuc.speech("Test")
    end)
  end
end
```

A ready-to-use sample file is available at `examples/rules.lua`.
Interceptor-specific behavior is documented in `INTERCEPTOR.md`.

Board effect trigger source:
* `F`
  * board-local effect trigger
  * forwarded to `libppuc` as a runtime event with source `EVENT_SOURCE_EFFECT`
  * use matching `trigger.source: F` plus `trigger.name` or `trigger.number` in the game YAML effect block

Speech callouts use the configured speech backend directly from Lua:

```lua
ppuc.speech("New highscore!")
```

Speech backends:
* `auto`
  * prefer `espeak-ng` when available, otherwise `flite`
* `flite`
  * lightweight default backend
* `espeak-ng`
  * second backend with broader voice/language support when staged in `third-party`
  * default voice is now a more distinct `en-us+f3`

Examples:
* `--speech-backend espeak-ng --speech-voice en-us+m3`
* `--speech-backend espeak-ng --speech-rate 210 --speech-pitch 35`
* `--speech-backend flite --speech-voice kal`

Ready-to-use samples are available at:
* `examples/rules.lua`


### Compiling

The platform build scripts stage pinned third-party dependencies into
`third-party`. For local development, they automatically prefer sibling
checkouts named `../libppuc` and `../libsdldmd` when those directories exist,
then fall back to the pinned GitHub archives from `platforms/config.sh`.

Set `PPUC_USE_LOCAL_DEPS=0` to force the pinned archive path, or set
`PPUC_LOCAL_DEPS_ROOT=/path/to/workspace` to look for local dependency
checkouts somewhere other than the parent directory.

#### Windows (x64)

```shell
platforms/win/x64/build.sh
```

#### Windows (x86)

```shell
platforms/win/x86/build.sh
```

#### Linux (x64)
```shell
platforms/linux/x64/build.sh
```

##### Ubuntu 23.10 Example
```shell
sudo apt install git autoconf libtool libudev-dev libpipewire-0.3-dev libwayland-dev libdecor-0-dev liburing-dev libasound2-dev libpulse-dev libaudio-dev libjack-dev libsndio-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev libxtst-dev libxkbcommon-dev libdrm-dev libgbm-dev libgl1-mesa-dev libgles2-mesa-dev libegl1-mesa-dev libdbus-1-dev libibus-1.0-dev
git clone https://github.com/PPUC/ppuc.git
cd ppuc
platforms/linux/x64/build.sh
ppuc/ppuc-pinmame -c examples/t2.yml -n -i
```

### Menu launcher

`ppuc-menu` is an SDL3 selector that reads a plain-text menu file with repeated `title`, `image`, `selected-image`, and `command` fields. It uses the same speech backend options as `ppuc-pinmame`.

```shell
ppuc/ppuc-menu \
  --menu-file examples/menu.txt \
  --logo assets/ppuc-logo.png \
  --slogan "Choose a machine and press Enter" \
  --speech --greeting
```

#### Linux (aarch64)
```shell
platforms/linux/aarch64/build.sh
```

#### GNOME Autostart on Debian/Linux

After building, install a per-user GNOME/XDG autostart entry with:

```shell
platforms/linux/install-gnome-autostart.sh -- -c examples/t2.yml -n -i
```

This writes `~/.config/autostart/ppuc-pinmame.desktop` with absolute paths to the
generated launcher and starts `ppuc-pinmame` in a terminal window so the process
stays attached to the foreground session. Pass the same arguments you normally
use on the command line after `--`.

By default the installer prefers `gnome-terminal`, then `kgx`, then
`x-terminal-emulator`. Override that with:

```shell
platforms/linux/install-gnome-autostart.sh --terminal gnome-terminal -- -c examples/t2.yml -n -i
```

The installer also sets a GNOME autostart delay of 10 seconds by default so the
terminal is launched after the session settles. Override that with:

```shell
platforms/linux/install-gnome-autostart.sh --delay 15 -- -c examples/t2.yml -n -i
```

On GNOME, the generated launcher also tries to dismiss the Activities overview a
couple of seconds after startup so the terminal and game are shown on the normal
workspace instead of remaining visible only in the workspace selector. Disable
that behavior with `--no-dismiss-overview` if needed.

Remove the autostart entry with:

```shell
platforms/linux/uninstall-gnome-autostart.sh
```

#### MacOS (arm64)
```shell
platforms/macos/arm64/build.sh
```

#### MacOS (x64)
```shell
platforms/macos/x64/build.sh
```
