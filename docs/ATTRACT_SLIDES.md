# How-to-play slides in attract mode

A machine standing idle in a bar or at a show has a screen doing nothing and a
crowd who have never played it. This puts that screen to work: after a minute
of nothing happening, it shows a few slides explaining how the game is played,
and stops the instant anyone touches it.

Status: **implemented**. The config tool exports slides, and PPUC shows them.

## What a player sees

The machine sits in attract. Nobody touches it for a minute. The backbox screen
begins a loop of slides — a photograph of the playfield with the drop targets
circled and a line of text saying what they do, then the next one. Any switch
at all ends it: a coin, a flipper button, a ball being lifted out of the
trough. The backglass comes straight back.

## Where slides live

In the game folder, beside the rules:

```
ppuc/
  io-boards.yaml
  ppuc.ini
  rules/
  slides/
    slides.yaml
    0100-drop-targets.jpg
    0200-skill-shot.jpg
```

`slides.yaml` is the order and the words; the images sit beside it:

```yaml
slides:
  - image: 0100-drop-targets.jpg
    title: 'Drop targets'
    text: 'Clear both banks in one ball to light multiball. Hit the left
      standup (1), then straight into the eject hole (2).'
    durationMs: 8000
    markers:
      - x: 0.28
        y: 0.42
        number: 1
        pointer: left
      - x: 0.62
        y: 0.55
        number: 2
        pointer: right
  - image: 0200-skill-shot.jpg
    title: 'Skill shot'
    text: 'Plunge softly for the top lane.'
    durationMs: 8000
```

Filenames carry their order the way rule files do — `%04d-` from the slide's
weight, then a slug of its title — so the folder reads in the order the slides
play, and a slide can be identified in a log by name.

## In the config tool

A `slide` node bundle, one node per slide, referenced to the game:

| field | type | what it is |
|---|---|---|
| title | node title | the heading on the slide, and its filename slug |
| `field_slide_text` | long text | the line or two underneath |
| `field_image` | image | the photograph |
| `field_weight` | integer | order, and the filename prefix |
| `field_duration` | integer | milliseconds this slide is shown |
| `field_slide_markers` | long text | a small YAML list, `x` and `y` required |
| `field_game` | reference | the game it belongs to |
| published | node status | whether it is exported at all |

Publishing is the switch, exactly as it is for switches: `buildYaml` checks
`$device->isPublished()`, and the slide exporter will do the same. An
unpublished slide is not written to the folder, so a machine cannot show a
slide someone was still working on.

That also gives the seasonal version for free — a set of slides published for a
show, unpublished afterwards, with no files to move.

## In ppuc.ini

```ini
[Attract]
Slides=true
SlidesIdleMs=60000
SlideDurationMs=8000
SlideNextSwitch=0
SlidePreviousSwitch=0
SlideBorderPercent=4
SlideFadeMs=350
```

`Slides` defaults to **true when the folder has slides in it**. A game folder
with a `slides/` directory is a machine whose owner wants slides; needing a
second switch to turn them on would only be a way to have them silently not
appear. `Slides=false` suppresses them.

`SlideDurationMs` is the fallback for a slide that does not carry its own.

`Slides=true` with no slides in the folder does nothing, which is the point:
the key is a way to switch slides off on a machine that has them, not a way to
turn them on.

## How it is drawn

Into the backglass frame the media host already presents, through the same hook
the service menu uses. That is not an incidental choice: two windows both
presenting on KMSDRM means the panel alternates between them as fast as they
draw, which is what the tools menu did before it was fixed. One window, one
present, and the slide is simply the last thing drawn into it.

A slide is **cut to its own content**, not to the screen. A portrait flyer gets
a tall narrow panel with its caption in a column beside it; a landscape
photograph gets the caption underneath, exactly as wide as the picture; a line
of text on its own gets a small panel around the words. So every slide is a
different size and shape, and what is left over is backglass.

`SlideBorderPercent` is the *least* backglass that must stay visible: the panel
is never allowed closer than that to the screen edge. Most slides stay well
inside it.

The panel is also slightly see-through -- `SlideOpacityPercent`, 88 by default
-- so the backglass reads faintly through the slide and the machine stays one
thing rather than two. Not much: the caption still has to be readable over
whatever the backglass is doing underneath it.

On the own-window path — a translite, or no video at all — the translite is
drawn first and the slide goes on top of it, so the border shows the same thing
it would on a B2S machine.

Slides fade in and out of each other over `SlideFadeMs` by moving that same
opacity, which means they fade into the **backglass** rather than into black --
the right thing to fade into when the backglass is what is behind them. SDL has
no transition effects of its own; this is one rectangle filled with black at a
computed alpha, and anything fancier (a wipe, a push) would have to be written
the same way, by drawing two textures with moving rectangles. A held slide
fades in but never out: a slide somebody is reading must not dim underneath
them. `FadeAlpha` is a free function in `AttractSlides.cpp` and is tested,
because its edge cases — a slide shorter than two fades, a slide past its own
duration — look like a flicker rather than like a failure.

Consequences worth stating:

- A slide is **opaque** inside its panel — a photograph with text over a dimmed
  strip, covering whatever the backglass was showing there.
- **The service menu wins.** If someone has the tools menu open, no slides.
- On a machine with a translite and no B2S or PUP, the slideshow opens the same
  window the tools menu does, and closing it asks the translite to redraw.

Images are loaded with `IMG_Load`, which the translite already uses, so PNG and
JPEG both work and nothing new is linked in. Each slide is scaled to fit the
screen and centred, keeping its aspect ratio -- letterboxed rather than
stretched, because the markers are placed in the photograph's own coordinates
and only land on the right targets if its shape is kept.

The finished slide -- background, photograph and caption -- is rendered into a
texture once and blitted after that, and each marker's arrow and badge are
baked into small textures of their own. This is not premature: the backglass
frame is presented on **every pass of the main loop**, around 250 times a
second, and the slide has to be drawn into each one because the B2S redraws
underneath it. Laying out the text, scaling the photograph and rebuilding the
marker geometry that often cost more than everything else on the screen put
together -- measured on a Mac, a slide with four markers took the process from
47% of a core to 81%, and 57% with the textures cached. Only the pulse and the
fade are still computed per frame, because only they change, and a pulse is an
alpha and an offset, which a texture does for free.

One photograph texture is held at a time, and it is released as soon as the
show comes down. A slideshow shows one photograph for eight seconds; a cache of one is the
right size, and a machine that has just had somebody walk up to it should not
still be holding a 1920x1080 texture it is not drawing.

The caption goes in one of three places, in order of preference: beside the
picture when it is portrait and leaves a wide enough gutter, over a dimmed
strip across the bottom when it fills the frame, and in the middle of the
screen when there is no picture at all. The flyers are all portrait, so the
gutter is not a corner case -- a caption in the empty margin covers none of the
flyer. The strip is sized from the words rather than fixed, and the title wraps
like the body, because a title is something somebody typed into a node title
field.

The gutter is only used when the widest single word fits in it. SDL_ttf breaks
a word that does not fit rather than letting it overhang, and "Electrifying"
split across two lines as "Electrifyin / g" is worse than the layout it was
avoiding -- that slide drops back to the strip. A slide is written by somebody typing into a text
field, and the one thing they should not have to think about is how many lines
fit. Past a third of the screen the strip stops growing: at that point the slide
wants fewer words, and clipping says so more usefully than covering the
photograph would.

The font has **no CJK glyphs**. Japanese or Chinese in a slide's text renders as
empty boxes, so it belongs in the photograph rather than the caption.

On the own-window path the screen is repainted at 20 Hz while slides are up
rather than the 10 Hz the tools use. The marker pulse is the only animation on
these screens, and at 10 Hz it reads as a stutter rather than a pulse.

The timing -- when the show starts, which slide is up, when it comes down -- is
in `AttractSlides.cpp`, which links without SDL or yaml-cpp and is covered by
`tests/test_attract_slides.cpp`. That split is not tidiness: the drawing can be
judged by looking at it, and the timing cannot. A show that never starts, or
that will not go away when a player walks up, is invisible on a bench.

## What starts and stops it

Starting: attract mode, and no switch has changed for `SlidesIdleMs`. Or one of
the two navigation buttons, which brings the show up immediately.

Stopping: **any switch at all**, except those two buttons. This deliberately
differs from the ball search, which ignores flipper buttons and the coin door,
because those are exactly the switches a curious passer-by touches first.

A game starting stops it too, by way of leaving attract mode.

## Steering it

Two switches, `SlideNextSwitch` and `SlidePreviousSwitch`, normally the two
flipper buttons. They are the one input that is not treated as somebody walking
up: pressing them is somebody reading.

| | |
|---|---|
| next button | forward a slide, or start the show at the first slide |
| previous button | back a slide, or start the show at the first slide |
| both together | hold the current slide until pressed again — `HOLD` appears top right |
| any other switch | out, and the minute starts again |

Both buttons start the show at the *first* slide rather than the last: both
mean "show me", and only then "which way".

A keyboard stands in for them, for a machine where no switches are assigned
yet: **cursor left and right** step, **up or down** holds, and **ESC** leaves
the show and restarts the minute. These only apply while the game has slides,
attract mode is running, and no service tool is open; otherwise the keys fall
through to whatever else is bound, which matters because ENTER is Game Start on
Flash.

Both buttons act on the close, not the release. A button that acts when it is
let go feels broken to anybody used to a pinball machine.

Holding does not survive the show coming down. A hold belongs to the person who
was reading, not to the machine, and the next show should not begin frozen on
somebody else's slide.

## Open questions

**1. How are targets marked?** Settled: **as coordinates**, drawn by PPUC.

The alternative was baking the circles and arrows into the photograph in an
image editor, which costs no code at all. Coordinates won for one reason: a
slide is authored once but re-marked often. The numbering changes when the rule
changes, and a shot that needs pointing out this season does not next season --
and with coordinates that is two numbers in a text field rather than a round
trip through an image editor and a re-upload.

In the config tool a slide's markers are a small YAML list in a text field:

```yaml
- x: 0.28
  y: 0.42
  number: 1
  pointer: left
```

Only `x` and `y` are required, and a marker whose coordinates fall outside 0..1
is dropped by the exporter rather than written out -- a typo puts a marker
nowhere, not off the edge of the screen.

A marker is `x,y` in the range 0 to 1 across the *picture*, not the screen, so
the marking survives being scaled to whatever panel the machine has and stays
on target on a letterboxed portrait photograph. Optionally a number, which
draws a badge on the point, and a side for the arrow to come in from --
`left`, `right`, `above` or `below` -- so the arrow never covers what it
points at.

The markers pulse one after another rather than together. The numbers are there
to be read in order, and "the left standup (1), then the eject hole (2)" only
works if the eye is led from one to the next. Every marker keeps a faint glow
outside its own slot so none of them disappears.

Both the arrows and the badges are drawn procedurally, from spans: no new
library, no pre-rendered artwork, and the marking scales with the picture.

**2. Should a slide be able to show the DMD or a video?** Not in this design.
Text and a photograph cover "how to play"; video would pull in the media
plugins and a lot of timing.

**3. Which screen on a machine with several?** This design says the backbox
screen, the one the tools menu uses. A machine with a separate full-DMD screen
might want them there instead.
