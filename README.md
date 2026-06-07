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
* --pup-triggers path
    * path to a lightweight PUP trigger rules file
    * optional
* --speech-file path
    * path to a speech trigger text file used with speech trigger rules
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

### PUP Trigger Rules

Use `--pup-triggers <file>` to map switch/lamp/coil conditions to calls of `SetPUPTrigger(source, id, value)`.
This trigger feature is independent from `--pup`, and can also drive speech callouts and board-local PPUC effects.

Rule syntax:

```text
<source> <id-or-name> [value] [cooldown=<milliseconds>] [delay=<milliseconds>] : <expression>
```

Expression functions:
* `switch(<number>)`
* `lamp(<number>)`
* `coil(<number>)`
* `attract` or `attract()`
* `switch_rising(<number>)`
* `switch_falling(<number>)`
* `lamp_rising(<number>)`
* `lamp_falling(<number>)`
* `coil_rising(<number>)`
* `coil_falling(<number>)`

Operators:
* `!`
* `&&`
* `||`
* parentheses `(...)`

Rule options:
* `cooldown=<milliseconds>`
  * suppress retriggering until the cooldown window has elapsed
* `delay=<milliseconds>`
  * wait before firing after the expression matches
  * for state-based expressions, the condition must still be true when the delay expires
  * for edge expressions like `switch_rising(...)`, the matching edge arms the delayed trigger once

Example:

```text
P 100 1 : switch_rising(13) && lamp(42)
P 101 1 cooldown=500 : switch_rising(13) && attract
P 102 1 delay=750 : switch(13) && attract
O 60010 1 : lamp_rising(23) && !attract
F cabinet-attract 1 : lamp_rising(5) && attract
```

A ready-to-use sample file is available at `examples/pup-triggers.rules`.

Speech trigger source:
* `O`
  * spoken callout target
  * the trigger `id` is looked up in a `--speech-file`
* `F`
  * board-local effect trigger
  * forwarded to `libppuc` as a runtime event with source `EVENT_SOURCE_EFFECT`
  * use matching `trigger.source: F` plus `trigger.name` or `trigger.number` in the game YAML effect block

### Speech Trigger Text Files

Use `--speech-file <file>` together with `--speech` and `--pup-triggers` to map speech trigger IDs to spoken text.

Syntax:

```text
<trigger-id> : <text to speak>
```

Example:

```text
60010: New highscore!
```

If a trigger rule emits source `O` with id `60010`, the speech backend will speak that text.

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
* `examples/flash.rules`
* `examples/flash.speech`


### Compiling

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
