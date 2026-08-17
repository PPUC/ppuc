# ROM-less Games (GameCore)

`ppuc-pinmame` can run a machine with no game ROM at all. Instead of PinMAME
executing original code, a C++ game core owns players, ball flow, scoring, tilt
and attract, and the scores are rendered to a DMD. This is what makes it possible
to modernize an electro-mechanical machine with PPUC.

Select it per game:

```ini
[Game]
Engine = script
```

or on the command line with `--engine script`. The default is `pinmame`, and the
engine is never inferred from whether a ROM happens to be present — a machine
with no console that silently picked the wrong engine is a miserable thing to
debug.

## What owns what

| Concern | Owner |
|---|---|
| Player count, whose turn, ball number, game over | GameCore |
| Score storage, replay thresholds, match | GameCore |
| Tilt warnings, the tilt latch, per-player tilt state | GameCore |
| Extra-ball bookkeeping | GameCore |
| Trough occupancy, serve retry, drain debounce | GameCore |
| Flipper, slingshot and pop-bumper firing | the **boards**, via `fastFlipSwitch` |
| Coil pulse length, hold power, thermal protection | the **boards** |
| What a switch scores, bonus, attract presentation | Lua rules |

GameCore never sets a coil level or times a pulse. It requests; the board decides
how long copper is energised. That property is what keeps the machine safe when
the host is busy, delayed or gone, and it is the same contract the PinMAME path
has always had.

## The `emGame:` block

Configuration lives in a new `emGame:` block in the game's `io-boards.yaml`,
alongside the wiring it refers to. libppuc ignores root keys it does not name, so
this needs no libppuc schema change — but `ppuc-pinmame` cross-checks every
number in the block against the devices declared in the same file. A role that
names a switch the machine does not have fails at startup with the number in the
message, rather than becoming "I press start and nothing happens".

```yaml
emGame:
  enabled: true
  ballsPerGame: 3
  ballCount: 1              # physical balls in the machine
  maxPlayers: 4
  addPlayerThroughBall: 1   # players may join while player 1 is on ball 1
  freePlay: false
  creditsPerCoin: 1
  scoreDigits: 6            # score rolls over here, as an EM reel does

  startSwitch: 1
  coinSwitches: [2]
  serviceCreditSwitch: 6
  gameOnCoil: 10            # 0 when the machine has no game-on relay
  knockerCoil: 4

  gameOverLamp: 40
  tiltLamp: 41
  ballInPlayLamp: 42
  shootAgainLamp: 43
  matchLamp: 44

  trough:
    switches: [11]          # a single outhole and a multi-ball trough are the
    kickCoil: 1             #   same model: what matters is how many are closed
    kickPulseMs: 80
    settleMs: 400
    kickRetryMs: 1200
    kickRetries: 3

  shooterLane:
    switch: 12              # omit when the machine has no lane switch
    serveSettleMs: 500

  tilt:                     # what happens once tilted; warnings live below
    inhibitSwitch: 250      # see "Tilt" below
    giOff: true
    endsBallOnly: true
    skipBonus: true

  replay:
    thresholds: [50000, 100000]
    awardCredit: true

  match:
    enabled: true

  names:                    # friendly names, so rules survive a rewire
    switches: { start: 1, outhole: 11, spinner: 25 }
    coils:    { outhole: 1, knocker: 4 }
    lamps:    { tilt: 41 }
```

`enabled: true` requires `startSwitch`, `trough.switches` and `trough.kickCoil`.
A machine that cannot start a game or serve a ball is a configuration error, not
a runtime surprise.

## Ball flow

```
Attract → GameStarting → BallStarting → BallInLane → BallInPlay → BallDraining
                              ↑                                        ↓
                              │                                   BallEnding
                              │                                        ↓
                              │                                   BonusCount
                              │                                        ↓
              ┌───────────────┴── extra ball ──────────────── ExtraBallCheck
              │                    (same player, same ball)            │
              └──────────────────── BallStarting ←── AdvancePlayer ←───┤
                          GameOver ← GameEnding ←──── all finished ────┘
```

**Bonus is collected at the end of every ball, including the one that awarded an
extra ball.** The order is `BallEnd → BonusCount → extra-ball check`, never the
reverse.

An extra ball replays the same ball number and keeps per-ball variables: shoot
again is a continuation of the ball, not a new one.

Trough occupancy is a **debounced count**, never a set of positions — a ball
resting between two optos makes position-derived state lie. Balls in play are
derived as `ballCount - trough`, so a ball nudged out by hand cannot desynchronise
a counter. A trough closure shorter than `settleMs` does not end the ball; that
is the one place a false positive costs the player a ball.

## Tilt warnings and ball save

These two are **not** part of `emGame:`. They live in top-level `tilt:` and
`ballSave:` blocks and work under **both** engines — giving an early-electronic
ROM warnings and a ball saver it was never written to have is much of why they
exist.

Both work the same way: by deciding whether the engine gets to see a switch at
all. A warning hit is hidden, so a ROM does not tilt on the first shove. A saved
drain is hidden and the ball is kicked back, so neither a ROM nor GameCore learns
the ball was lost.

```yaml
tilt:
  # EVERY switch that means the cabinet was moved: plumb bob, ball-roll tilt,
  # playfield tilt. A machine usually has more than one, and they all feed the
  # same warning count. A bare number is accepted where a list is expected.
  switches: [4, 5]
  # Separate in kind, not just in severity: never warned, never blanked, and it
  # ends the game rather than the ball.
  slamSwitches: [3]
  warnings: 2             # the third hit tilts
  debounceMs: 500         # one shove through the ring is one hit
  warningBlankingMs: 2000 # let the plumb settle before it can warn again
  warningLamp: 41

ballSave:
  enabled: true
  seconds: 8
  startOn: shooterLane    # troughExit | shooterLane | firstPlayfieldSwitch
  shooterLaneSwitch: 12
  drainSwitches: [11]
  kickCoil: 1
  maxSavesPerBall: 1      # 0 for unlimited within the window
  onlyOnBalls: []         # empty means every ball
  playfieldSwitches: [22, 23, 24, 25]   # only for firstPlayfieldSwitch
  lamp: 45
```

The swing filter and the blanking are shared across all tilt switches rather
than applied per switch, and deliberately so: one shove that trips both the plumb
bob and the ball-roll tilt is one warning, not two.

`warningBlankingMs` is the knob a real plumb bob needs and is deliberately
separate from `debounceMs`. The swing filter stops one shove counting as several
hits; the blanking then ignores the bob *entirely* for long enough that a
still-swinging plumb cannot burn the remaining warnings a second later.

Only the hit that actually tilts is forwarded to the engine. That is what lets a
ROM keep its own tilt handling without knowing the host counted warnings on its
behalf.

**Awarded warnings.** `PlayfieldAssist::AwardTiltWarnings(n)` grants extra
warnings to the current player for the **rest of the game**. The base allowance
resets each ball; the award does not, which is what makes it worth chasing.

**Ball save start** is configurable because not every machine has a shooter-lane
switch. `shooterLane` is the best option where one exists, since a slow plunger
does not eat the timer; it falls back to `troughExit` automatically rather than
silently never arming, which would be indistinguishable from the feature being
broken.

**Granting a save from a rule:**

```lua
ppuc.ballSave(4000)   -- four seconds, starting now
```

Works under either engine and regardless of the configured start trigger, so a
rule that has just started a mode can hand out protection with it. It extends an
active save and never shortens one. It also works when `ballSave.enabled` is
`false`, for a machine that wants no automatic saver but does want rule-driven
protection.

A tilt cancels an active save: a tilted ball is meant to be lost.

## Tilt consequences

Tilt drops **the flipper fingers and GI, and nothing else**. Every other coil —
outhole kicker, trough eject, knocker — stays live, because those are exactly what
is needed to get the balls back. A tilt that killed all high power could strand a
ball with no way to recover it.

Flippers, slingshots and pop bumpers are fired by the boards from their own switch
bitmap (`fastFlipSwitch`), with the host uninvolved. The host cannot suppress an
individual fast-flip output at runtime: `fastFlipSwitch` and `stopSwitches` are
set once during configuration, and `PPUC::SetSwitchState` only works for switches
on a board the host owns.

So tilt works like this:

1. The game YAML declares a board with `virtual: true` and puts a switch on it
   with no wiring — `inhibitSwitch: 250` in the example above.
2. libppuc sends that number to every board as `CONFIG_TOPIC_TILT_SWITCH` during
   session setup.
3. When GameCore tilts, the host asserts the switch. Every board sees it on the
   bus and inhibits its fast-flip outputs locally, so a flipper the player is
   *holding* goes dead within one board loop.
4. GI is set to 0 host-side. Scoring is discarded inside GameCore, so no rules
   file has to remember to check `tilted()`.
5. The bonus is forfeited: `BonusCount` is skipped entirely, so `onBonusCount`
   never runs and nothing is awarded. That is the usual behaviour — you lose your
   bonus when you tilt — and it is the default. `skipBonus: false` keeps the
   count-down for a machine that wants it.

An extra ball earned on the tilted ball is forfeited too, unless
`extraBallSurvivesTilt: true`. That decision is made against the tilt state of
the ball that just ended, before the tilt is cleared for the next one.

Tilt clears when the trough count reaches `ballCount` — every ball home, not
merely the one that was in play. `recoverTimeoutMs` is a safety net for a
genuinely stuck ball, not the normal path.

**This requires firmware that understands `CONFIG_TOPIC_TILT_SWITCH`.** On older
firmware, tilt still suppresses scoring and darkens GI, but the flippers stay
alive. `ppuc-pinmame` logs a warning at startup when a tilt switch is configured
but no inhibit switch is declared.

Note also that WS2812 lamps and GI are not power-gated on the boards, while
`type: lamp` PWM outputs are. A tilt indicator should therefore be an LED lamp or
the DMD — a PWM tilt lamp goes dark exactly when you want it lit.

## High power

With `gameOnCoil` set, the host asserts it for the duration of each game. With it
set to `0`, the boards hold high power on for the whole session — which is the
simpler wiring for an EM conversion, at the cost of losing the coin-door
interlock. `ppuc-pinmame` logs which case it is in at startup, because a silently
dead playfield is the single most likely first-run failure of this feature.

## The display

Scores go to the DMD. With no DMD scripting at all, a built-in screen shows the
score, ball and player, an attract cycle, a tilt banner, game over and match. It
is a pure function of GameCore state, so the display and the machine cannot
disagree.

The framebuffer is 4-bit greyscale, one byte per pixel, with a whole-frame tint —
every sink downstream (ZeDMD, the console DMD, the SDL virtual DMD) wants indexed
data, and RGB24 would triple the payload for a screen that is a single colour.

Frames are paced to roughly 30fps and suppressed when the content has not
changed. The main loop runs at 20 microseconds, so an unpaced renderer would offer
around 50,000 frames a second into libdmdutil's 128-entry ring.

## Running

```
# On a desk, no boards, no display hardware:
ppuc-pinmame --game ppuc_games/emdemo --no-serial --no-sound

# Headless, for CI:
ppuc-pinmame --game ppuc_games/emdemo --no-serial --no-sound \
             --no-display --exit-after-ms 3000

# On hardware:
ppuc-pinmame --game ppuc_games/emdemo --serial /dev/ttyUSB0
```

See [`ppuc_games/emdemo`](../../ppuc_games/emdemo/README.md) for a complete
reference machine.

## Lua API

A machine is fully playable from its `emGame:` block with no Lua at all. Rules
add what is specific to the machine: what a switch scores, how the bonus counts
down, what the display says.

`ppuc.game` and `ppuc.dmd` exist **only** under `engine: script`, so a rules file
shared between a ROM machine and a ROM-less one feature-tests on them:

```lua
if ppuc.game then ppuc.game.addScore(1000) end
```

They are deliberately not stubbed with no-ops under `engine: pinmame`. A silent
no-op turns a misconfigured machine into a mystery; an error names the line.

### `ppuc.game.*`

**State** — `state()`, `inGame()`, `attract()`, `players()`, `player()`,
`ball()`, `ballsPerGame()`, `score([player])`, `highScore()`, `highPlayer()`,
`tilted([player])`, `extraBalls([player])`, `credits()`, `ballsInTrough()`,
`ballsInPlay()`, `matchDigits()`.

**Awards** — `addScore(points [, player])`, `setScore`, `awardExtraBall`,
`addCredits`, `knock()`. `addScore` and `awardExtraBall` are silently ignored
while tilted or out of play, so no rule has to guard them.

**Control** — `startGame()`, `addPlayer()`, `tilt()`, `slamTilt()`, `endBall()`,
`endGame()`, `serveBall()`, `bonusDone()`.

**Variables** — `get/set/add(name [, player])` live for the game;
`getBall/setBall/addBall(name)` are cleared at each ball start but survive an
extra ball, because shoot again continues the ball.

**Wiring** — `coil(name)`, `switch(name)`, `lamp(name)` resolve `emGame.names`,
so a rules set survives a rewire. Zero when the name is unknown.

Omitting the player argument, or passing 0, means the current player.

### `ppuc.dmd.*`

`width()`, `height()`, `color(r,g,b)`, `clear([level])`, `pixel`, `line`,
`rect(x,y,w,h,level[,filled])`, `fill`, `text(x,y,s[,opts])`,
`number(x,y,v[,opts])`, `textWidth(s[,opts])`, `formatNumber(v[,opts])`.

`opts` is a table, every key optional: `level` (0-15), `scale`, `align`
(`left`/`center`/`right`), `spacing`, `commas`, `digits`, `pad`.

Drawing happens in `ppuc.onDmdFrame()`, on top of the built-in screen, and only
on ticks that will actually push a frame — so it runs at most ~30 times a second
regardless of what the author writes. `ppuc.dmd.textWidth` exists so a script can
right-align without knowing font metrics.

### Handlers

```lua
ppuc.onAttractStart()        ppuc.onAttractEnd()
ppuc.onGameStart(players)    ppuc.onPlayerAdded(player)
ppuc.onStartRejected(reason) -- "noCredits", "tooLate", "scriptVetoed", ...
ppuc.onBallStart(player, ball)      ppuc.onBallServed(player, ball)
ppuc.onBallServeFailed(attempts)    ppuc.onBallStuck(player, ball)
ppuc.onBallEnd(player, ball)        ppuc.onBonusCount(player, ball)
ppuc.onExtraBall(player)            ppuc.onScore(player, points, total)
ppuc.onTiltWarning(player, used, remaining)
ppuc.onTilt(player, ball)           ppuc.onSlamTilt()
ppuc.onBallSaved(player, count)
ppuc.onReplay(player, threshold)    ppuc.onMatch(digits, matchedMask)
ppuc.onGameEnd(players, highPlayer, highScore)
ppuc.onCreditsChanged(credits)      ppuc.onDmdFrame()

-- Query handlers: return a boolean. Absent means allow.
function ppuc.canStartGame() return true end
function ppuc.canAddPlayer(nextPlayer) return true end
```

`onBonusCount` **must** reach `ppuc.game.bonusDone()`, or the ball waits out
`bonusTimeoutMs` and the game continues anyway — a rules bug does not brick the
machine.

`onTiltWarning` and `onBallSaved` fire under **both** engines, because tilt
warnings and ball save live in the switch path rather than in the engine.

See `ppuc_games/emdemo/rules/` for a worked example.

## Authoring in the config-tool

A ROM-less game is an ordinary `game` node with **Engine** set to *GameCore
(ROM-less)*. There is no separate content type: every sub-structure -- boards,
switches, PWM devices, LED strings, rules -- is identical to a ROM machine.

**Roles are assigned on the device, not typed as numbers on the game.** Each
switch, matrix switch and PWM device has a *Role* select listing the closed
vocabulary. That is what makes `emGame.startSwitch references switch 11, which
is not declared` impossible to produce from the UI: the exporter collects roles
while it walks the devices, so a role can only ever name a device that was
actually exported.

Multi-valued roles -- trough, coin, tilt, playfield, player-up -- are assigned to
as many devices as needed and keep device-number order.

| Where | What |
|---|---|
| Game node | Engine, balls per game, players, credits, replay thresholds, match |
| Game node | Tilt warnings, swing filter, blanking |
| Game node | Ball save enabled, seconds, start trigger, max saves per ball |
| Switch / matrix switch | Role: start, coin, service credit, trough, shooter lane, tilt, slam tilt, tilt inhibit, playfield |
| PWM device | Role: trough kick, knocker, game-on relay, and the backbox lamps |
| I/O board | **Virtual (host-owned)** for the board carrying the tilt inhibit switch |

Tilt and ball save are exported for a **PinMAME** game too, because both work
under either engine. Only the `emGame:` block is GameCore-only, and it is
emitted solely when the engine is GameCore -- so an existing ROM game exports
exactly as it did before.

Blockly gains two toolbox categories, *Game (ROM-less)* and *Display*, for the
`ppuc.game.*` and `ppuc.dmd.*` calls.

### Keeping the two sides in step

The schema lives in two repositories that cannot see each other: the exporter in
`config-tool` writes it, and `ppuc-pinmame` reads it. A key added to one and not
the other fails silently. `ppuc/tools/check-gamecore-drift.py` compares the two:

```
python3 tools/check-gamecore-drift.py --config-tool ../config-tool --strict
```

It lives in `ppuc` rather than in `libppuc` because libppuc sits *below* ppuc in
the stack and knows nothing about either side of this schema — the parser is
ppuc's own source, and the exporter belongs to config-tool.

ppuc CI runs it **report-only**, and deliberately so. config-tool's main branch
moves independently, so a pull request here that adds a parser key is
legitimately ahead of the exporter until that side merges; a job that failed for
it would be red through every coordinated change and ignored by the time it
mattered. Use `--strict` locally, and when closing out a change that touches
both sides.

## Current limits

- Score reels, chimes and segment displays are not driven. `PWM_TYPE_MOTOR` and
  `stopSwitches` are the right primitives when that lands.
- `emGame:` is validated by `ppuc-pinmame`, not by libppuc, and config-tool
  cannot author it yet.
