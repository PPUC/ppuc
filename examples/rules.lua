-- PPUC Lua rules example.
--
-- Define any of these handlers as needed:
-- ppuc.onSwitchChanged(number, state)
-- ppuc.onLampChanged(number, state)
-- ppuc.onCoilChanged(number, state)
-- ppuc.onBallChanged(ball)
-- ppuc.onPlayerChanged(player)
-- ppuc.onRulesUpdate()

function ppuc.onSwitchChanged(number, state)
  if ppuc.switchClosing(13) and ppuc.lampState(42) then
    ppuc.pupTrigger("P", 100, 1)
  end

  if ppuc.switchClosing(13) and ppuc.attractMode() then
    ppuc.pupTrigger("P", 101, 1)
  end

  -- Host-side ball-save interceptor example.
  if ppuc.switchClosing(15) then
    ppuc.setState("ballSaveReady")
  end

  if ppuc.stateActive("ballSaveReady") and ppuc.switchGroupClosing("playfield") then
    ppuc.clearState("ballSaveReady")
    ppuc.setState("ballSave", 5000)
  end

  if ppuc.stateActive("ballSave") and ppuc.switchClosing(9) then
    ppuc.suppressSwitch(9)
    ppuc.pulseCoil(7, 120)
  end
end

function ppuc.onLampChanged(number, state)
  if ppuc.lampRising(23) and not ppuc.attractMode() then
    ppuc.speech("New highscore!")
  end

  if ppuc.lampRising(5) and ppuc.attractMode() then
    ppuc.effectTrigger(1000, 1)
  end
end

function ppuc.onRulesUpdate()
  if ppuc.stateActive("ballSaveReady") or ppuc.stateActive("ballSave") then
    ppuc.blinkLamp(8, 250, 250)
  else
    ppuc.stopBlinkLamp(8)
  end
end
