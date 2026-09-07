-- Drives Time Warp (tmwrp_l2) from power-on to a ball kicking out, with no
-- hardware attached, and reports a single pass/fail verdict.
--
-- The point is the coil path. PluginEngine polls solenoids on its own thread at
-- 1 kHz and pushes edges from there, and nothing about that is exercised by a
-- machine sitting in attract: Time Warp fires no solenoid until a game starts.
-- Coil silence looks identical whether the poll thread is working or wedged,
-- so this makes the ROM produce an edge on demand.
--
-- Presenting a start button alone is not enough. The ROM will not start a game
-- unless the machine looks sane: a ball in the outhole to serve, the coin door
-- shut, no tilt. Getting that wrong looks exactly like a broken engine, which
-- is why the state is set up explicitly rather than assumed.
--
-- Driven by tests/integration/tmwrp_coldstart.sh, which greps the verdict line.

local SW_TILT          = 1    -- Bomb Tilt, must stay open
local SW_START         = 3    -- Credit (Start) Button
local SW_COIN_RIGHT    = 4    -- Right Coin Switch
local SW_SLAM_TILT     = 7    -- SLAM TILT, must stay open
local SW_OUTHOLE       = 9    -- ball resting in the outhole

local COIL_BALL_RELEASE = 1   -- what a successful start must energize

local VERDICT_AT_MS     = 14000

local sawBallRelease = false
local coilsSeen      = 0

local function press(sw, holdMs, atMs)
  ppuc.after(atMs, function() ppuc.sendSwitchToCpu(sw, 1) end)
  ppuc.after(atMs + holdMs, function() ppuc.sendSwitchToCpu(sw, 0) end)
end

-- Let the ROM finish its own power-on self test before presenting anything.
-- Switches asserted during boot are ignored or, worse, read as a stuck-switch
-- fault.
ppuc.after(3000, function()
  -- A ball is sitting in the outhole. Held closed, not pulsed: this is a
  -- resting ball, and the ROM checks it when deciding whether it can serve.
  ppuc.sendSwitchToCpu(SW_OUTHOLE, 1)
  -- Tilt switches idle open. Stated explicitly so a future change to the
  -- default switch state cannot silently invalidate the test.
  ppuc.sendSwitchToCpu(SW_TILT, 0)
  ppuc.sendSwitchToCpu(SW_SLAM_TILT, 0)
end)

-- A coin, unless the NVRAM says free play. Harmless when it is: the ROM just
-- banks a credit it does not need.
press(SW_COIN_RIGHT, 120, 4000)

-- Then start. Two attempts: the first may land while the ROM is still counting
-- the coin.
press(SW_START, 120, 5500)
press(SW_START, 120, 8000)

-- Handlers hang off the ppuc table, not the global namespace.
function ppuc.onCoilChanged(number, state)
  if state ~= 1 then return end
  coilsSeen = coilsSeen + 1
  sawBallRelease = sawBallRelease or number == COIL_BALL_RELEASE
end

-- One verdict line, whatever happened. Distinguishing "no coil edges at all"
-- from "edges, but the game never started" matters: the first means the poll
-- thread or the quiesce gate is broken, the second means the switch injection
-- never convinced the ROM.
ppuc.after(VERDICT_AT_MS, function()
  if sawBallRelease then
    print("TEST-RESULT: PASS ball release energized after start")
  elseif coilsSeen > 0 then
    print("TEST-RESULT: FAIL coil edges arrived but ball release never fired; "
          .. "the ROM did not start a game")
  else
    print("TEST-RESULT: FAIL no coil edges at all; the solenoid poll path is dead")
  end
end)
