# The PPUC Handbook

For the person who owns, sets up or operates a machine running PPUC. It covers
what the machine does and how to make it do it: the game folder, sound, the
service tools on the backbox screen, the how-to-play slideshow, updating the
boards' firmware, and what to do when something looks wrong.

It does not explain how any of it is built. Where that matters, each section
points at the design document that does.

---

## 1. What a machine consists of

A PPUC machine is an original playfield driven by new electronics. The original
game ROM still runs, under PinMAME, on a small computer in the cabinet — so the
game plays exactly as it always did — and PPUC sits between that ROM and the
machine, reading switches and driving coils, lamps and displays over a serial
bus to a handful of I/O boards.

Three things make up a running machine:

| | |
|---|---|
| **the host** | a Raspberry Pi or similar, running `ppuc` |
| **the boards** | the I/O boards under the playfield and in the backbox |
| **the game folder** | everything specific to this game, in one directory |

Everything you configure lives in the game folder. Move that folder to another
host and you have moved the machine.

## 2. The game folder

```
flash/
  io-boards.yaml     which board drives which switch, coil and lamp
  ppuc.ini           settings for this machine
  pinmame/           the ROM and its NVRAM
  rules/             Lua rules: ball save, multiball, anything the ROM cannot do
  slides/            how-to-play slides for attract mode
  music/             background music
  pup/               PUP video pack
  scoreboard/        the game's own screen around the videos (section 8)
  translite-on.png   the backglass image while a game is running
  translite-off.png  the backglass image in attract mode
  state/             written by PPUC: adjustments, switch report, machine memory
```

Start the machine with the folder, not with a pile of options:

```
ppuc --game /path/to/flash
```

Everything else is read from `ppuc.ini` inside it, and anything absent simply
does not happen: no `slides/`, no slideshow; no `pup/`, no video.

The folder is produced by the **config tool**, the Drupal application where a
machine is actually configured. Download it from a game's page as *Download
Game Folder* and unpack it on the host. `io-boards.yaml` is the same file you
get from *Download Game Config*, and the two are interchangeable.

`state/` is the one part the config tool does not produce: PPUC writes the
machine's adjustments, its switch report and what the rules remember there
(section 5). Unpack a new export over the folder rather than replacing the
folder, and they stay.

### Several games on one machine

One playfield can run more than one game: the original ROM, a game written in
Lua, a custom ROM build, an older ROM revision. Each is a game folder of its
own. Put them side by side in one directory and start the game selector instead
of a game:

```
games/
  tmwrp/          the original ROM
  tmwrp-lua/      the same playfield with a Lua game
  tmwrp-custom/   a custom ROM build
```

```
ppuc-menu --games-dir /path/to/games
```

Every subfolder with an `io-boards.yaml` is a card in the menu. With only one
such folder there is nothing to choose: the selector starts that game at once,
without showing anything, so it is safe to always start the selector.

On a ZeDMDOS machine nothing has to be started by hand. Put the game folders
side by side under `ppuc/` on the USB stick instead of making `ppuc/` itself the
game folder, and the machine boots into the selector:

```
<usb-stick>/
  ppuc/
    tmwrp/        each with its own ppuc.ini, io-boards.yaml, pinmame/ ...
    tmwrp-lua/
```

A `ppuc.ini` directly in `ppuc/` still means "this is the one game", as before.

To change games, switch the machine off and on again: the selector appears at
every power-on.

On the playfield:

| button | what it does |
|---|---|
| **left flipper** | previous game |
| **right flipper** | next game |
| **start** | play the highlighted game |

A keyboard works as well: cursor keys move, ENTER plays, ESC quits the
selector.

The selector appears where the game draws its backglass: it takes `Width`,
`Height`, `Screen` and `Window` from the `[Translite]` section of the game
folders (each from the first folder that sets it), and looks like the service
tools, in the same font and colours. A card shows the game's
`translite-off.png`, and the highlighted card its `translite-on.png`.

The selector finds these three buttons on its own: in the first game folder's
`io-boards.yaml` it looks for the switches described as the left and right
flipper buttons and the start button (or, for a Lua game, `emGame.startSwitch`).
It then talks to the boards with only those three switches configured, so while
it runs nothing on the playfield moves or lights up and the flippers are dead.
Pressing start hands the boards over to the game, which configures them in full.

Every game folder on the machine should wire the three buttons to the same board
inputs, since it is the same cabinet. If one does not, the selector says so in
its log at startup and keeps using the first folder's. When it cannot tell which
switch is which — no switch says "left flipper button", or two do — the log
names the candidates and the selector runs keyboard only until the game's
`ppuc.ini` settles it:

```ini
[Menu]
Title=Time Warp Deluxe  ; the card's title; the folder name when absent
Info=Lua rules edition ; a line under the title; "ROM tmwrp_l2" or "Lua game" when absent
Image=card-off.png      ; default translite-off.png
SelectedImage=card-on.png ; default translite-on.png
Order=0                 ; cards sort by Order, then by title
Hidden=false            ; true leaves the folder out of the menu
Arguments=--no-sound    ; added to the command that starts this game
LeftFlipperSwitch=0     ; switch number of the left flipper button; 0 finds it
RightFlipperSwitch=0    ; switch number of the right flipper button; 0 finds it
StartSwitch=0           ; switch number of the start button; 0 finds it
```

The buttons come from the first game in that order, so give the folder you
trust most the lowest `Order`.

The selector takes the serial port from that game's `[Paths] Serial`, or from
`--serial /dev/ttyUSB0`. With `--no-serial` it does not touch the boards and is
steered from the keyboard only. If the boards do not answer, it says so and
carries on keyboard only rather than refusing to start.

To list one game folder twice — the same ROM with and without a bench option,
say — write a menu file instead of, or as well as, `--games-dir`:

```
title: Time Warp
game: games/tmwrp

title: Time Warp, sound off
game: games/tmwrp
arguments: --no-sound
```

```
ppuc-menu --menu-file menu.txt --games-dir /path/to/games
```

Menu file entries come first, then the folders found in `--games-dir`.

## 3. `ppuc.ini`

Sections and the keys worth knowing. Anything not listed has a sensible default
and can be left out.

```ini
[Game]
Rom=flash_l1
Engine=pinmame          ; or `script` for a game with no ROM
AltSoundHardware=wpcs   ; Engine=script only: see "AltSound packs" below

[Paths]
Translite=translite-on.png
TransliteAttract=translite-off.png
MusicFiles=...
MusicGapMs=2000

[Audio]
Volume=100              ; master, in percent
RomVolume=100           ; the game's own sound
SpeechVolume=100        ; spoken callouts
MusicVolume=100         ; background music

[Attract]
Slides=true
SlidesIdleMs=60000      ; quiet time before the slideshow starts
SlideDurationMs=8000    ; per slide, unless the slide says otherwise
SlideNextSwitch=0       ; switch number of the "forward" button
SlidePreviousSwitch=0   ; switch number of the "back" button
SlideBorderPercent=4    ; how much backglass stays visible around a slide
SlideFadeMs=350         ; fade between slides

[Runtime]
NoSerial=false          ; run without boards, for testing
NoSound=false
NoTimeFence=false       ; true lets PinMAME run a whole frame at a time
SwitchMinHoldMs=10      ; shortest switch closure the ROM is shown; 0 is off
Debug=false
BallTroughSwitch=0      ; 0 is off; see the safety net below
BallTroughGraceMs=5000  ; how long a solid closure is tolerated first
```

**`BallTroughSwitch`** arms the ball-trough safety net, and takes the switch
number of the trough. Switch state reaches the ROM as changes, never as levels,
so a ball that comes to rest on the trough switch while the ROM believes it open
is a game that will not continue — and the only cure was to lift the ball out by
hand. With this set, a closure that stays solid for `BallTroughGraceMs` while a
game is in play is sent to the ROM again as an open followed by a close, giving
it a fresh edge to notice. It does nothing in attract mode, where a ball resting
in the trough is exactly where it belongs.

**`NoTimeFence`** stays `false`. PinMAME then advances in step with the machine,
a millisecond at a time, which is what lets the ROM see a ball that only grazes
a standup target and fire a diverter in time for a fast ramp shot. Set to `true`
(or `--no-time-fence`), PinMAME runs about 16 ms of game at once and then waits.
It is there to compare against if something looks different, not to be left on.

**`SwitchMinHoldMs`** is the shortest time the ROM is shown a switch in any
state. A ball that only grazes a standup target closes it for a few
milliseconds; the ROM wants to read a switch several times before it believes
it, and would miss many of those hits. With this set, the hit reaches the ROM at
once and the switch is reported open again no sooner than this many
milliseconds later. 10 is right for WPC. A different generation may want another
value: if grazing hits still do not count, raise it a few milliseconds at a time.
Do not go above half the time a fast spinner takes for one turn, or the ROM
starts counting fewer turns than it spun. 0 switches it off.

Volumes are percentages, 100 meaning unchanged. They are **levels, not
balance**: turn a loud source down rather than a quiet one up, because a source
pushed above 100 clips.

## 4. Sound

Four independent levels — master, game sound, speech, music — so that a machine
whose ROM sound is louder than its callouts can be fixed without touching
either file.

Music ducks automatically while anything else is playing, and comes back when
the other source falls quiet. How far it ducks is the fifth row in the Volume
tool, **Music in game** — 29% of its normal level by default.

That is the knob to reach for when the music cannot be heard during a game.
Turning *Music* up instead makes it loud in every quiet moment as well, which
is how a service test that silences the ROM ends up hurting somebody's ears.

### AltSound packs

An AltSound pack replaces the game's own sound with recorded samples. It lives
in `pinmame/altsound/<Rom>/` in the game folder and is switched on with
`AltSound=true` in `[Runtime]`.

A game with no ROM (`Engine=script`) plays a pack too: its rules send the sound
commands the original ROM would have sent. Two things have to be in `ppuc.ini`
for that:

- `Rom=` names the pack folder, as for every other media file of the game, e.g.
  `Rom=dh_lx2` for `pinmame/altsound/dh_lx2/`.
- `AltSoundHardware=` names the sound hardware the pack was recorded from:
  `wpcs` for WPC-S games such as Dirty Harry, `wpcdcs` for the first DCS games,
  `wpc95` for WPC-95. With the wrong value most sounds play wrong or not at all.
  With a ROM, leave it out: PinMAME knows the hardware, and the value is ignored.

If the pack is silent, run once with `DebugSoundCommands=true` in `[Runtime]`:
every command a rule sends is logged as `Lua sound command: ...`, and a command
the hardware cannot express says `ppuc.soundCommand(...) not sent` and why.

### The playlist

Music tracks live in `music/` in the game folder and play in filename order,
with `MusicGapMs` between them. A game that ends steps the playlist on, so the
next player does not get the same song again — which is what used to happen: a
machine with four tracks played the first one, and only the first one, until
somebody restarted it. Coming back from a service test is not a game ending, so
a test returns to the track that was playing.

Set `SongSelect = true` under `[Audio]` and the player is offered the playlist
when a game starts. It appears on the backbox screen over the game translite,
and it is steered by the same two buttons as the slideshow — the ones set with
`SlideNextSwitch` and `SlidePreviousSwitch`, normally the flipper buttons:

| | |
|---|---|
| One button | walk to the next or previous track, which starts playing at once |
| Both together | that one, thank you — get on with the game |
| First playfield switch | same thing, so plunging answers it too |

Each track starts the instant it is highlighted, because the only real way to
choose a song is to hear it. The flippers still work while the chooser is up.

Nothing is offered for a playlist of one, and the chooser cannot appear during
a ball — only at the start of a game. It gives up on its own after twenty
seconds in case nobody answers.

The levels in `ppuc.ini` are what the machine starts with. The **Volume** tool
in the service menu changes them for the session only and never writes them
back: the game folder is the record of what this machine should sound like, and
a menu that silently rewrote it would lose that. To change the defaults, change
`ppuc.ini` — or the game in the config tool, and export again.

## 5. The service tools

The service menu is reached from the **coin door**, as on the machine's original
ROM, or from a keyboard plugged into the host.

**From the coin door:** open the door and press **ENTER** (Begin Test). While
the menu is up, the four buttons drive it and nothing else — the game never sees
them:

| button | what it does |
|---|---|
| **ENTER** | select; on a value, start changing it, and press again to keep it |
| **UP** / **DOWN** | move; while a value is being changed, more and less |
| **ESCAPE** | back — a value being changed is left as it was, a tool goes to the menu, the menu closes |

With the door closed, or the menu closed, the buttons are the game's as always:
UP and DOWN still change the volume on a WPC with the door open.

The buttons are known for WPC (switches 5 to 8). Other machines name them in
`ppuc.ini`, as `escape, down, up, enter` — the switch numbers from
`io-boards.yaml`, 0 for a button the door does not have:

```ini
[Runtime]
CoinDoorButtons = 5, 6, 7, 8
```

Without a door switch (`coinDoorClosedSwitch: 0`) the door counts as open, so
ENTER always opens the menu. Without a screen for the menu — a machine whose only
display is a real DMD, such as a ZeDMD — ENTER stays the game's; the menu on the
DMD itself is not done yet.

**From a keyboard:** plug one in and the machine grows a service menu, unplug it
and nothing has changed.

| key | what it does |
|---|---|
| **SPACE** | open the menu |
| **cursor keys** | move; left and right change a value straight away |
| **ENTER** | select |
| **ESC** | back — tool to menu, menu to game |
| **Q** | quit PPUC, only when no menu is open |

Q is not ESC on purpose. A key that closes a menu in one place and kills the
machine mid-game in another is the kind of difference nobody remembers with a
ball in play.

The menu has these entries:

### Switch and coil monitor

Every configured switch and coil, painted once and kept painted. A scrolling
log is the wrong shape for *"is the outhole switch closed right now, and did
Ball Release ever fire"* — those are states, and the interesting case, a ball
resting on a switch the game never noticed, is the one where nothing is being
logged at all.

- **Green** means closed (a switch) or on (a coil). Grey means open or off.
- The time beside each device is **how long it has been in the state it is in**.
  Amber means it changed recently.
- The coil side has a cursor. Move to a coil and press **ENTER** to fire it
  once — enough to release a stuck ball without lifting the glass.
- A long switch list is drawn with tighter rows and a smaller font so that it
  fits on one screen. On a 1920x1080 backglass that holds 60 switches. Only
  beyond that does it page: the heading shows which ones you are looking at,
  e.g. *(1-60)*, operating a switch brings up its page for a few seconds, and
  when nothing changes the pages take turns.

The monitor shows what the *machine* is doing, before any rule suppresses
anything. That is deliberate: if a rule is eating a switch, the monitor is how
you find out.

You can also start PPUC with `--switch-monitor` to bring it up at boot. On a
machine with a B2S or a PUP pack, that flag turns them off for the session: on
a screen with no compositor, two things claiming the panel means one of them
flickers, so the monitor takes it outright.

### Tests

The switch, coil, lamp, GI and flasher tests, on the backbox screen instead of
a console. These have always existed as start-up modes — useful at a desk with
a laptop wired to the playfield, useless at a machine in a cabinet — and this
is the same set reached from the menu.

**The game freezes while a test is open**, and carries on where it left off
when you leave. A ball sitting on the playfield is still in play afterwards;
PinMAME is stopped, not starved. The screen says so in green. If it says the
game is *not* frozen, in amber, the engine could not be stopped — the test
still works, but the game is running behind it.

| | |
|---|---|
| **cursor keys** | choose a device |
| **ENTER** | fire it — a pulse for coils and flashers, on and off for lamps and GI |
| **A** | walk through every device in turn; A again stops (keyboard only) |
| **ESC** | leave, which turns everything off and lets the game go |

The switch test needs no cursor: press switches on the machine and watch them
change.

Coil and flasher tests ask *"A game is in progress"* before opening if a ball
is in play, since they can throw one. The other three open straight away.

High power is raised for the coil and flasher tests when no game is running,
and lowered again on the way out — but only if the test raised it, so leaving a
test during a game never cuts power to the game.

### Volume

The four levels and the music duck, for this session. See section 4. From the
coin door, ENTER on a level starts changing it, UP and DOWN change it, and ENTER
or ESCAPE is done.

### Adjustments

Settings an owner changes at the machine, kept in the game folder's `state/` on
the USB stick until changed again — across power cycles and a reflash of the SD
card.

PPUC's own come first, and only those that mean something on this machine:

| adjustment | what it does | there when |
|---|---|---|
| Ball save | seconds a drained ball is given back, 0 is off | ball save is configured |
| Tilt warnings | hits on the bob allowed before the ball tilts | a tilt switch is configured |
| Balls per game | from the next game on | a ROM-less game |
| Players | most players in one game | a ROM-less game |
| Free play | start without credits | a ROM-less game |
| Match | the match at the end of a game | a ROM-less game |
| Switch report after | games a switch may sit still before it is reported, 0 is off | always |

Then the game's own, marked *Game:*, which its rules define (section 7): a
difficulty, a device out of order. A machine with a ROM keeps its game
adjustments in the ROM's own menu, below.

Their defaults are what the game folder says, so a machine nobody adjusted runs
exactly as configured. A value somebody changed is drawn in blue, and the line
below shows the default. **Restore all defaults** at the end undoes everything
on the list, after asking.

`state/adjustments.txt` lists only what differs from the default, one
`name = value` per line. Deleting a line puts that adjustment back to its
default.

### Switch report

The switches that have stopped working, as the original ROM reports them when
the coin door opens. A switch that has not changed for a number of games
(*Switch report after*, 10 unless adjusted) is almost always broken — an opto
whose LED died reads closed forever, a rollover with a bent wire reads open
forever — and neither stops a game, so nobody notices until a mode cannot be lit
any more.

- Only switches seen working at least once are listed. A switch that never moved
  since the report started is far more often one nobody uses than a broken one;
  the switch test is for checking a machine after setup.
- A game counts once it has run for 30 seconds.
- The coin door and its buttons, the tilt and slam switches and the coin
  switches are never listed: they may rightly sit still for a hundred games. The
  rules can add more (section 7).
- A listed switch drops off the moment it changes, so a repaired switch reports
  itself repaired.
- **Reset the report** starts every switch from 0 games, after asking.

When the coin door is opened and the report lists anything, the backbox screen
says so for a few seconds. The menu shows the count beside the entry. The log
names each switch when it is first listed and at every start.

### ROM service menu

Only on a machine with a ROM. Closes the PPUC menu and opens the ROM's own
service menu, with its tests, audits and adjustments, on the DMD. The coin door
buttons are the ROM's until the door is closed; open it again and ENTER brings
the PPUC menu back.

### Restart PPUC / Power off

Both ask *"Are you sure?"* first, and both start on **No**. A menu that ends
the game if ENTER is pressed twice by reflex is a menu nobody should open during
a game.

**Power off** shuts the host down properly. Use it before switching the machine
off at the wall: pulling power from a running Linux host eventually corrupts the
card it boots from.

## 6. The how-to-play slideshow

A machine standing idle in a bar has a screen doing nothing and a crowd who have
never played it. After a minute of nothing happening, the backbox screen shows a
loop of slides — a photograph with the shots numbered, and a line of text — and
takes them down the instant anybody touches the machine.

It runs only in attract mode, and only if the game folder has a `slides/`
directory. Nothing needs switching on.

### Steering it

Assign two switches, normally the two flipper buttons. In the config tool they
are on the game's **PPUC Settings** page, as *Attract: SlideNextSwitch* and
*Attract: SlidePreviousSwitch*; take the numbers off the **All Slides**
page's neighbour, **All Switches**. They come out in `ppuc.ini` as:

```ini
[Attract]
SlideNextSwitch=203
SlidePreviousSwitch=201
```

Set them in the config tool rather than in the file. `ppuc.ini` is regenerated
every time you download the game folder, so a hand edit survives exactly until
the next export.

| | |
|---|---|
| next button | forward a slide — or start the show, from the first slide |
| previous button | back a slide — or start the show, from the first slide |
| both together | **hold** this slide until pressed again; `HOLD` appears top right |
| any other switch | out, and the minute starts again |
| starting a game | out, and it stays out until the game is over |

Those two buttons are the only input that does not end the show: pressing them
is somebody reading, not somebody walking up. Both start the show at the first
slide, because both mean *"show me"* and only then *"which way"*.

With a keyboard attached, **cursor left and right** step, **up or down** holds,
and **ESC** leaves the show and restarts the minute — useful before the switches
are assigned. These only apply while the slideshow has something to say; at any
other time those keys fall through to whatever else is bound.

### How it looks

A slide is cut to its own content rather than to the screen, so each one is a
different size: a portrait flyer gets a tall narrow panel with its caption
beside it, a landscape photograph gets the caption underneath, and a line of
text on its own gets a small panel around the words. Everything left over is
backglass. `SlideBorderPercent` sets the least backglass that must stay visible
round the edge, whatever the slide wants.

The panel's black background is slightly see-through — `SlideOpacityPercent`,
88 by default — so the backglass reads faintly through it and the machine stays
one thing rather than two. The photograph and the words stay solid, and a thin
amber edge runs round the panel. Slides fade into and out of the backglass
rather than into black. A held slide fades in but never out: a slide somebody is reading
must not dim underneath them.

If somebody has the service menu open, there are no slides. Somebody at the
switch monitor is diagnosing a machine, and slides appearing over that would be
a fault in its own right.

### Writing slides

In the config tool, on the game's **All Slides** tab. It lists every slide of
the game in the order they play, published or not. Tick several and use
*Publish* or *Unpublish* to put a set of slides on the machine, or away, without
opening each one — that is how a set of slides for a show is kept ready and
retired afterwards.

*Add Slide* and *Reorder Slides* are on the same page. Reordering is drag and
drop: pull the rows into the order the slides should play and save. The order
is the slide's weight, which is also the number its exported filename starts
with, so dragging a row renames a file in the game folder.

Each slide has a title, a line or two of text, an optional photograph, a weight
for its position in the loop, and an optional duration.

**Several slides can share one photograph**, and that is the cheap way to build
a how-to-play sequence: one picture of the playfield, a different caption and a
different set of markers on each slide. The machine notices they are the same
picture and decodes it once for the whole run.

**Upload whatever your camera produced.** The export scales every slide
photograph to 1100 pixels on its long edge, which is as much as a backbox panel
can show, so a picture off a phone costs the machine nothing extra. That
scaling is the `ppuc_slide` image style in the config tool — worth raising if a
machine ever gets a screen large enough to want more, and worth leaving alone
otherwise, because the machine decodes the picture every time the photograph
changes and the decode is what a slideshow actually costs.

**Publishing is the switch.** An unpublished slide is not exported and cannot
appear on the machine, which is also how you keep a set of slides for a show and
put them away afterwards without moving any files.

To point at something in the photograph, **draw on it**. The slide form shows
the slide's own photograph above the markers field: drag along a shot, from
where the ball starts to where it ends, or click to mark a spot. The field
underneath fills in as you draw, and the usual Save button saves it.

The photograph has to be uploaded and the slide saved once before there is
anything to draw on; until then the form says so.

The field is still just text, so a marker can be typed instead, and anything
typed is picked up by the drawing the moment you click out of the box:

```yaml
- x: 0.28
  y: 0.42
  number: 1
  pointer: left
```

`x` and `y` run from 0 to 1 across the *picture*, not the screen, so a marking
survives being scaled to whatever panel the machine has. `number` draws a badge
on the point — leave it out for a plain arrow. `pointer` is the side the arrow
comes in from, so it never covers what it points at: `left`, `right`, `above`,
`below`, or one of the four diagonals `above-left`, `above-right`,
`below-left`, `below-right`.

Use a diagonal to show the line of a shot rather than just the spot. An arrow
from `below-left` is a ball off the left flipper; one from `above` is a ball
draining into an outlane.

For a shot that follows something — up a lane, round a loop — give the marker a
start point instead, and the arrow runs the whole way:

```yaml
- x: 0.829
  y: 0.285
  number: 2
  fromX: 0.735
  fromY: 0.395
```

`fromX` and `fromY` are in the same 0 to 1 picture coordinates. The arrow then
lies along the line between the two points, so it can run up a lane over the
lights in it rather than sit beside the target.

The markers pulse one after another, because *"the left standup (1), then the
eject hole (2)"* only works if the eye is led from one to the next.

Two things to know when writing text: the font has **no Japanese or Chinese
glyphs** — CJK text renders as empty boxes, so it belongs in the photograph
rather than the caption — and a caption longer than a third of the screen is
clipped rather than allowed to cover the picture. If a slide is being clipped,
it wants fewer words.

### Music credits

Royalty-free music is free on a condition: that it is credited. Each music item
in the config tool has an **Attribution** field — put the credit in it exactly
as the source asks, for example:

```
Music track: In Flight by Alegend
Source: https://freetouse.com/music
Royalty Free Background Music
```

Those credits become slides automatically, at the end of the attract loop, two
tracks to a slide. They are not slides you write or can reorder: they are
generated from the tracks the game actually carries, so a track added without a
credit shows up as a gap, and a track removed takes its credit with it.

A game with music credits but no slides of its own still gets a slideshow — the
credits. That is the deal the music came with.

Design notes: [`docs/ATTRACT_SLIDES.md`](docs/ATTRACT_SLIDES.md).

## 7. Rules

Anything the original ROM cannot do is a Lua rule in `rules/`, written in the
config tool with Blockly or by hand. Ball save, playfield assists, and features
the machine never had — Flash's two-ball multiball is a rule, and the 1978 ROM
never finds out.

Rules can watch switches and coils, suppress either, send switches to the ROM
that no board reported, fire coils, trigger LED effects, speech and Serum
scenes, play sounds from the AltSound pack, hold off the ball search, and color
the GI: on a playfield with RGB GI,
a custom game gives a mode a color and the GI takes it while the mode runs,
cycling or fading when several colored modes run at once. GI dimming keeps
working on top of the color. This needs board firmware 0.5.9 or later on every
board (see [section 9](#9-updating-board-firmware)), and each GI string wired as
one LED segment.

Rules can also define the game's own **adjustments**, which then appear in the
service menu (section 5), remember values across power cycles (*machine
memory*: an extra-ball level that adjusts itself, a champion's name), and take
switches off the switch report that may rightly sit still for many games. Both
are kept in the game folder's `state/`.

A custom game can be played through on a desk, with no machine attached:
`ppuc-pinmame --game <folder> --bench --bench-play game.play` lets a small
ball model answer the coils (the trough serves, the poppers kick out) while a
script plays the shots. The rules, the scores and the scoreboard are the real
ones. The `bench:` block and the script are described in
[`docs/EM_GAMES.md`](docs/EM_GAMES.md#playing-on-a-desk).

Written up in [`docs/RULES_AND_EFFECTS.md`](docs/RULES_AND_EFFECTS.md) and
[`docs/INTERCEPTOR.md`](docs/INTERCEPTOR.md). Flash's multiball is documented
for players in `ppuc_games/flash/MULTIBALL.md` and technically in
`MULTIBALL_TECHNICAL.md`.

## 8. The backbox screen

One screen, several things that might want it, in this order:

1. **The firmware update screen**, while a board is being flashed. It says what
   is happening and what not to do, and it wins over everything: a board
   interrupted mid-write does not come back.
2. **The service menu or monitor**, when somebody has a keyboard out.
3. **The slideshow**, when the machine has been idle in attract.
4. **The game**: its scoreboard over the PUP video, a B2S, or the translite
   image.

Only one of them draws at a time, and only one presents a frame. On a machine
with no desktop compositor, two windows presenting means the panel alternates
between them as fast as they draw — which looks exactly like a hardware fault
and is not one.

### The scoreboard

A game can bring its own full screen: a frame (a ranch gate, a saloon
front, a cockpit), the scores of up to four players along it, the game's own
signs at the sides, and the scene videos in a window in the middle. It lives
in the game folder:

```
scoreboard/
  scoreboard.yaml    what goes where
  frame.png          the frame, transparent where the scene shows
  idle.png           shown in the window when no video plays (optional)
  *.png              pictures for the signs: bullets, stars, ...
  SomeFont.ttf       the font, with its licence
```

PPUC draws it on top of whatever PUP plays, in the same frame. Without a PUP
pack it opens the screen itself (the `[Translite]` screen, size and window
settings) and shows `idle.png` in the window.

It costs next to nothing, which is the point: only the video moves every
frame. The frame is one picture. Every letter is burnt into the wood once,
when the game loads, and a score is then a dozen small copies. Nothing is
redrawn while nothing changes, and no video is spent on a score.

The **PPUC logo** is always on it, burnt into one corner. The layout chooses
the corner and the size; it cannot leave it out.

#### scoreboard.yaml

```yaml
scoreboard:
  size: [1920, 1080]           # the layout's coordinates; scaled to the screen
  frame: frame.png
  idle: idle.png
  font: Rye-Regular.ttf
  window: [320, 120, 1280, 720]  # where the scene shows: x, y, width, height
  logo: {corner: top-right, size: 180, margin: 24}
  styles:                      # how text is burnt in; "burnt" and "ember" exist
    ember: {core: "ffcf48", scorch: "3a1606", glow: true}
    score:                     # a soft shadow lifts the letters off the wood
      core: "ffcf48"
      scorch: "3a1606"
      glow: true
      shadow: {opacity: 0.75, offset: 0.045, softness: 0.035, color: "140802"}
  lists:
    ranks: [Rookie, Officer, Sergeant, Detective]
  fields:
    - {type: current_score, x: 960, y: 846, size: 116, width: 720, style: score}
    - {type: other_scores, x: 330, y: 992, width: 1260, size: 44}
    - {type: text, when: game, text: "Player {player}", x: 342, y: 882, size: 40, align: left}
    - {type: text, when: game, text: "{player.rank:ranks}", x: 177, y: 714, size: 40}
    - type: icons
      source: player.bullets   # lit by a count, or with lit: mask by bit
      lit: mask
      count: 8
      on: bullet_on.png
      off: bullet_off.png
      x: 82
      y: 236
      dx: 52
      dy: 78
      columns: 4
```

- **current_score** is the player whose turn it is, large; after a game, the
  best score of it. **other_scores** spreads everybody else over its width.
  A text or score wider than its field's `width` is drawn smaller to fit,
  so a billion never runs into "Player 1" and "Ball 3" beside it.
- **text** fills in `{ball}`, `{player}`, `{players}`, `{credits}`, `{score}`,
  `{score1}`..`{score4}`, `{player.NAME}` and `{ball.NAME}` (the game's
  `ppuc.game` variables), and `{value.NAME}`, which the rules set with
  `ppuc.scoreboardSet`. `{player.rank:ranks}` turns a number into a name from
  a list. A name that means nothing shows as nothing.
- **when** is `always`, `game` or `attract`.
- **free_play** on a text: what it says instead while the machine runs free
  play, `{type: text, text: "Credits {credits}", free_play: "Free Play"}`,
  so the sign does not say "Credits 0" on a machine that needs none.
- A style's **shadow** is drawn under its letters, offset down and right:
  `opacity` 0 to 1, `offset` and `softness` as parts of the letter size,
  `color`. It is made once with the letters, like the scorch, so it costs a
  second small copy per letter. Callouts in that style get it too.
- **callouts** are the moments the rules put over the scene with
  `ppuc.scoreboardCallout`: "10,000,000" for a shot, "SILVER BULLET" for an
  award. They are made of the same burnt glyphs and move by the GPU alone,
  so a callout costs no more than the score does. A layout names where they
  go and which words to burn at load, so the first one shows at once:

  ```yaml
  callouts:
    spots:
      center: {x: 960, y: 470, size: 150, style: ember}   # the middle of the text
      low: {x: 960, y: 730, size: 80, style: ember}
    words: [SILVER BULLET, JACKPOT, EXTRA BALL]
  ```

  Without `spots`, `center` is the window's middle. A callout in the window
  is shrunk to fit it and never drawn over the frame. A word not in `words`
  still shows; it is burnt the first time, which takes a few milliseconds.
- `x` of a text is where `align` puts it; `y` is the top of the letters.

A mistake in the file is reported when the game starts, with the line, and
the game plays without the scoreboard. `--no-display` (a headless run) shows no
scoreboard either.

The board keeps the shape of its `size`. On a screen of another shape, a
21:9 monitor for example, it is drawn as large as fits, in the middle, with
black bars beside it; nothing is stretched. PUP places the scene video in
parts of the whole screen, so such a screen needs its own `CustomPos`:
`ppuc-pinmame` prints the right one when it starts drawing
("the PUP scene screen wants CustomPos ..."), and the preview prints it for
`--size 2560x1080`.

#### Making the art

`ppuc-scoreboard-preview` draws a layout into a PNG with made-up scores, so
the art can be made and checked without a machine:

```
ppuc-scoreboard-preview /path/to/game out.png --players 3 --current 2 \
    --var rank=4 --var bullets=0x93 --set "mode=Stakeout" --scene frame-of-a-video.png
```

`--callout "SILVER BULLET" --callout-at 120 --effect shake` shows a callout
as it is 120 ms after it started, to check how one moves frame by frame.

`--replay timeline.txt --frames folder` draws a whole recorded game, one PNG
per frame, callouts moving and all; ffmpeg makes a video of it. A timeline
has one change per line (`17840 value.mode Car Chase`, `28600 callout
text=PROMOTED|effect=pop`); wpc-re's scenario runner writes one from the Lua
game in its simulator, and `ppuc-pinmame --scoreboard-record game.txt`
writes one from a real game: on the machine, or on a desk with `--bench`
(see [section 7](#7-rules)). With `--no-display` the board is
then recorded without being shown.

A whole game as PNGs is tens of gigabytes. `--frames -` writes the frames
raw to stdout instead, straight into ffmpeg, with nothing on disk; with
`--transparent` the scene videos can go under it in the same command:

```
ppuc-scoreboard-preview /path/to/game last.png --replay game.txt --frames - --transparent |
  ffmpeg -f lavfi -i color=black:s=1920x1080:r=30 -f concat -safe 0 -i scenes.txt \
         -f rawvideo -pix_fmt rgba -s 1920x1080 -r 30 -i - \
         -filter_complex "[0][1]overlay=320:120[bg];[bg][2]overlay=0:0:shortest=1,format=yuv420p" game.mp4
```

(`scenes.txt` lists the clips, `file 'scene01.mp4'` a line; 320:120 is the
window's corner.)

It also prints the PUP `CustomPos` that puts a video exactly into the window
on a screen of `--size`.
Make the scene videos the window's size (1280x720 for the layout above) and
they play without being scaled, which is the cheapest thing the screen can do.

`tools/make-scenes.sh` cuts the scene clips out of a film you have the rights
to, at the window's size and as H.264 the Pi decodes in hardware:

```
tools/make-scenes.sh film.mkv pup/scenes --list scenes.txt   # name start length per line
tools/make-scenes.sh film.mkv pup/scenes --random 20 --length 6
```

A wide film is cut at the sides to fill the window, never stretched. Black
bars that are part of the picture (a wide film inside a 16:9 file, as
many copies are) are found and cut off first; `--no-crop` keeps them. For a
short clip rather than a whole film, `--skip-start 5 --skip-end 5` keeps the
random picks from leaving out minutes of it. The
sound is dropped unless `--audio` is given, since the game has its own.
`--size` matches a window other than 1280x720.

The scoreboard needs a ROM-less game for its scores; under a ROM it shows
only what the rules set.

## 9. Updating board firmware

The boards can be updated from the host over the same bus that drives the
game, so a board under the playfield never has to be unplugged. `ppuc` checks
the boards at every start, before it configures them, and flashes only when you
have explicitly allowed it.

**1. Find the images.** A PPUC build has a `firmware` folder next to `plugins/`,
with an image for every board type, and `ppuc` uses it by default. Point
`--firmware-path` at another folder only to flash something else. That folder
needs one `.uf2` file per board type, named after the type and the version, the
way the io-boards releases name them:

```
firmware/
  IO_16_8_1-0.3.4.uf2
  Opto_16-0.3.4.uf2
```

The name before the version must be one of `IO_16_8_1`, `IO_16x8_matrix`,
`Out_8x10` or `Opto_16`; any other file is ignored. Each board gets the newest
image *of its own type*, never another type's, so it is safe to keep the images
for every board in the same folder. A snapshot build may carry a build id, as in
`IO_16_8_1-0.3.4+1a2b3c4d.uf2`.

**2. Look before you flash.** Every start checks, so start the machine as usual,
without permission to update:

```
ppuc --game /path/to/flash
```

The log says what every board runs and what would happen to it, then the game
starts as usual:

```
PPUC: board 1 is IO_16_8_1, firmware 0.3.2 (admin protocol 1.0)
PPUC: board 3 is Opto_16, firmware 0.3.2 (admin protocol 1.0)
PPUC: board 1 (IO_16_8_1) will be updated: 0.3.2 -> 0.3.4
PPUC: board 3 (Opto_16) has a newer image (0.3.2 -> 0.3.4) but will not be updated: firmware for this board type has never been run on real hardware
PPUC: 1 board(s) would be updated; pass --allow-firmware-update to permit it
```

**3. Flash.** Add `--allow-firmware-update`. To include board types that are
not yet proven on hardware — currently every type except `IO_16_8_1`, the opto
board among them — also add `--allow-unvalidated-firmware-update`:

```
ppuc --game /path/to/flash --allow-firmware-update --allow-unvalidated-firmware-update
```

That second flag is a real decision, not a formality. Firmware nobody has run on
a board type drives pins whose direction has so far only been read off a
schematic, and a mistake there pushes an output into an input. If you would
rather watch it happen, leave the flag out and flash that board over USB
instead (see the io-boards README).

While the update runs, the backbox shows the firmware update screen with
*board N of M*, and the board being written shows two quick pulses on its LED,
then dark. It takes minutes. **Do not switch the machine off**: a board
interrupted mid-write has to be recovered over USB.

When every board is written, `ppuc` stops on its own: the boards are rebooting
into the new firmware and there is nothing to configure yet. The launcher starts
it again (it exits with status 75, which the launcher reads as "start me again",
not as a crash); if you started it by hand, start it again yourself. The next
start logs the new versions, finds nothing to update, and plays.

**The same in `ppuc.ini`**, for a machine you do not start from a keyboard:

```ini
[Runtime]
AllowFirmwareUpdate=true
; only if the opto board or another unproven type needs it
AllowUnvalidatedFirmwareUpdate=true
```

If you set `FirmwarePath`, give an absolute path: a relative one is read from
wherever `ppuc` was started, not from the game folder. Keep comments on lines of
their own here; the INI reader takes everything after `=` as the value.

Take the `Allow…` lines out again once the boards are updated. Installing a new
PPUC build brings new images with it, and with those lines left in it would flash
the machine at its next boot without anyone deciding to.

**Two further switches**, for development rather than for operators:

| Option / INI key | What it allows |
|---|---|
| `--allow-firmware-downgrade` / `AllowFirmwareDowngrade` | flashing a board whose firmware is *newer* than the image, to go back a version |
| `--allow-dev-firmware-update` / `AllowDevFirmwareUpdate` | reflashing a board whose version matches the image but was built from a different commit — snapshots between releases all carry the same version number |

**A board that says nothing.** `board N did not report a firmware version`
means the board is absent, or runs firmware too old to take updates over the
bus. Flash it once over USB; from then on it updates like the others.

## 10. When something looks wrong

**A board is being flashed and the machine looks dead.** It is not. A firmware
transfer takes minutes, the screen says so, and a board interrupted mid-write
has to be recovered over USB. Wait.

**A ball drained and the game did not notice.** The ROM missed a switch. Open
the switch monitor and look at the trough: if it reads closed there, the machine
saw it and the ROM did not. PPUC re-checks what it has told the ROM and corrects
it, so this should heal within a second — if it does not, the switch is being
suppressed by a rule, or it is mechanically marginal and never closed long
enough to be reported at all.

When the trough safety net is armed — `BallTroughSwitch` in `ppuc.ini` — it says
so in the log every time it acts:

```
Ball trough: switch 48 has been closed for 5000ms during a game; giving the ROM a fresh edge
```

That line is the difference between two quite different faults. If it is there,
the machine saw the stuck ball and told the ROM again, and the ROM ignored it.
If it is not, nothing ever read the switch as closed, and the problem is the
switch or the ball not reaching it.

**Reading the log.** Logging is off until you ask for it: put an empty file
named `log.txt` in the `dumps/` directory of the USB stick before booting, and
everything the machine prints lands there as well as on the screen.

It is **appended** to, so a reboot or a crash does not throw away the log of the
run that went wrong, and each launch starts with a line naming itself:

```
===== runtime starting 2026-10-05 17:41:09 (boot +34s) =====
```

The date is only right if the machine has had a network to set its clock from,
which is why the uptime is there too. Once the file passes 32 MB it becomes
`log.1.txt` and a new one starts, so a machine can be left running for days and
still have both the current and the previous stretch on the stick.

**A coil never fires.** Select it in the monitor and press ENTER. If it fires,
the wiring and the board are fine and the problem is upstream — the ROM, or a
rule suppressing it. If it does not, it is the coil, its fuse or its connector.

**An LED string goes dark after a certain LED, or some LEDs never change.**
Start PPUC with `--led-string-test` and watch the string while the console
counts along it:

```
board=4 port=29 "Lamps"  LED 27 of 0-52: lamp #54 Left Loop HQ  color=FFFFFF
```

It goes along every LED string in the order the LEDs are wired — LED 0, LED 1,
LED 2 — and lights **one LED at a time**, for a second each, even where a
flasher covers three LEDs or a GI string forty. To do that, PPUC gives the
boards a test configuration in which every LED is a lamp of its own; the next
normal start sends the game's configuration again. Each LED shows in the colour
it is configured with, and an LED nothing is assigned to lights white. Strings
on boards that are skipped or not present are left out. There is no
tools-menu version, because the boards cannot be reconfigured while a game is
running.

If the walk reaches an LED and it stays dark — and so does every LED after it —
the data line breaks at that LED. Each LED passes the signal on to the next
one, so a dead LED, a bad solder joint or a loose connector there darkens the
rest of the string. Look at that LED's data-out and the next LED's data-in. If
every LED lights correctly one at a time but the game still looks wrong, the
string is fine and the problem is in the configuration.

The test also lists, before it starts, any LED assigned beyond the end of its
string. The board drives only as many LEDs as `amount` in `io-boards.yaml`
says, so those can never light: either `amount` is too small or the LED number
is wrong.

**The GI keeps its normal color when a mode should color it.** The log says why
once, at the first color a game asks for:

```
PPUC: GI color programs need io-boards firmware 0.5.9 on every board; the GI keeps its configured colors
```

One board on older firmware is enough, because older firmware does not
understand the message and would stumble over every one. Update the boards
([section 9](#9-updating-board-firmware)). Without that line, check that the GI
LEDs are configured as GI (not as lamps) for the string the game colors. If only
part of a string changes color, the rest of its LEDs belong to another GI string
in `io-boards.yaml`.

**The screen flickers between two things.** Something is presenting twice. On a
machine with a B2S or PUP pack, check you did not also configure a translite.

**No sound from one source.** Check the four levels, in the Volume tool rather
than in the file — the file is only what the machine started with, and somebody
may have turned something down during the last show.

**A rule on the ball number never runs.** `onBallChanged` and `onPlayerChanged`
do not come from the machine — they are decoded out of the ROM's own memory
using the community NVRAM map for that ROM. A game nobody has mapped has no
ball number, so those handlers never fire, per-ball tilt warnings do not reset
and ball save does not arm. The startup log says which it is:

```
Ball and player tracking: maps/williams/system6/flash_l1.map.json
Ball and player tracking unavailable for xyz_l1: no nvram map found for ROM xyz_l1
```

**A rule on a coil never runs.** `onCoilChanged` fires for coils the *ROM*
drives. A shaker, a motor, the flipper power coils and anything else that exists
only in `io-boards.yaml` is never driven by a 1978 ROM, so nothing ever changes
for a rule to hear. Drive those from a rule, or from a switch.

**The first game after a restart behaves oddly.** Fixed — but worth knowing what
it was. The boards report their switches as soon as the bus is up, seconds before
the ROM has finished booting, and those reports used to be held until it had.
The whole backlog then arrived at once, after the ROM had already started from an
all-open switch matrix, so every switch that happened to be closed at power-on
looked like a fresh hit: a drop target left down scored itself, with the award
lit for real. The machine only agreed with itself again once the first game reset
the bank, which is why the second game was always fine.

**The slideshow never appears.** It needs attract mode, a `slides/` directory
with at least one *published* slide, and a full minute of nothing happening —
and the ball search running is not "nothing happening". Press a flipper button
if the switches are assigned; that starts it immediately.

---

## Where to read further

| | |
|---|---|
| [`docs/STACK.md`](docs/STACK.md) | how the parts fit together |
| [`docs/ATTRACT_SLIDES.md`](docs/ATTRACT_SLIDES.md) | the slideshow |
| [`docs/RULES_AND_EFFECTS.md`](docs/RULES_AND_EFFECTS.md) | rules, LED effects |
| [`docs/INTERCEPTOR.md`](docs/INTERCEPTOR.md) | lying to the ROM |
| [`docs/EM_GAMES.md`](docs/EM_GAMES.md) | electro-mechanical machines |
| [`docs/PLUGIN_MIGRATION.md`](docs/PLUGIN_MIGRATION.md) | media plugins |
| [`docs/V2_PROTOCOL.md`](docs/V2_PROTOCOL.md) | the bus |
