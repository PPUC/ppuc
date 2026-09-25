# The PPUC Handbook

For the person who owns, sets up or operates a machine running PPUC. It covers
what the machine does and how to make it do it: the game folder, sound, the
service tools on the backbox screen, the how-to-play slideshow, and what to do
when something looks wrong.

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
  translite-on.png   the backglass image while a game is running
  translite-off.png  the backglass image in attract mode
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

## 3. `ppuc.ini`

Sections and the keys worth knowing. Anything not listed has a sensible default
and can be left out.

```ini
[Game]
Rom=flash_l1
Engine=pinmame          ; or `script` for a game with no ROM

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
Debug=false
```

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

The levels in `ppuc.ini` are what the machine starts with. The **Volume** tool
in the service menu changes them for the session only and never writes them
back: the game folder is the record of what this machine should sound like, and
a menu that silently rewrote it would lose that. To change the defaults, change
`ppuc.ini` — or the game in the config tool, and export again.

## 5. The service tools

A cabinet has no keyboard in normal use. That is the point: plug one in and the
machine grows a service menu, unplug it and nothing has changed.

| key | what it does |
|---|---|
| **SPACE** | open the menu |
| **cursor keys** | move |
| **ENTER** | select |
| **ESC** | back — tool to menu, menu to game |
| **Q** | quit PPUC, only when no menu is open |

Q is not ESC on purpose. A key that closes a menu in one place and kills the
machine mid-game in another is the kind of difference nobody remembers with a
ball in play.

The menu has five entries:

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
| **A** | walk through every device in turn; A again stops |
| **ESC** | leave, which turns everything off and lets the game go |

The switch test needs no cursor: press switches on the machine and watch them
change.

Coil and flasher tests ask *"A game is in progress"* before opening if a ball
is in play, since they can throw one. The other three open straight away.

High power is raised for the coil and flasher tests when no game is running,
and lowered again on the way out — but only if the test raised it, so leaving a
test during a game never cuts power to the game.

### Volume

The four levels and the music duck, for this session. See section 4.

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
scenes, and hold off the ball search.

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
4. **The game**: B2S, PUP video, or the translite image.

Only one of them draws at a time, and only one presents a frame. On a machine
with no desktop compositor, two windows presenting means the panel alternates
between them as fast as they draw — which looks exactly like a hardware fault
and is not one.

## 9. When something looks wrong

**A board is being flashed and the machine looks dead.** It is not. A firmware
transfer takes minutes, the screen says so, and a board interrupted mid-write
has to be recovered over USB. Wait.

**A ball drained and the game did not notice.** The ROM missed a switch. Open
the switch monitor and look at the trough: if it reads closed there, the machine
saw it and the ROM did not. PPUC re-checks what it has told the ROM and corrects
it, so this should heal within a second — if it does not, the switch is being
suppressed by a rule, or it is mechanically marginal and never closed long
enough to be reported at all.

**A coil never fires.** Select it in the monitor and press ENTER. If it fires,
the wiring and the board are fine and the problem is upstream — the ROM, or a
rule suppressing it. If it does not, it is the coil, its fuse or its connector.

**The screen flickers between two things.** Something is presenting twice. On a
machine with a B2S or PUP pack, check you did not also configure a translite.

**No sound from one source.** Check the four levels, in the Volume tool rather
than in the file — the file is only what the machine started with, and somebody
may have turned something down during the last show.

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
