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
* -r rom name
    * rom to use, overwrites *rom* setting in config file
    * optional
* -s serial device
    * serial device path to use, overwrites *serialPort* setting in config file
    * optional
* -d
    * enable debug mode, overwrites *debug* setting in config file
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

### PUP Trigger Rules

Use `--pup-triggers <file>` to map switch/lamp/coil conditions to calls of `SetPUPTrigger(source, id, value)`.
This trigger feature is independent from `--pup`, and can also drive speech callouts and board-local PPUC effects.

Rule syntax:

```text
<source> <id-or-name> [value] [cooldown=<milliseconds>] : <expression>
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

Example:

```text
P 100 1 : switch_rising(13) && lamp(42)
P 101 1 cooldown=500 : switch_rising(13) && attract
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
