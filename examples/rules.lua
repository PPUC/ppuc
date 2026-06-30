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
  if number == 13 and state == 1 and ppuc.lampState(42) then
    ppuc.pupTrigger("P", 100, 1)
  end

  if number == 13 and state == 1 and ppuc.attractMode() then
    ppuc.pupTrigger("P", 101, 1)
    ppuc.after(500, function()
      ppuc.speech("Delayed attract switch callout")
    end)
  end

  -- Host-side ball-save interceptor example.
  if number == 15 and state == 1 then
    ppuc.setState("ballSaveReady")
  end

  if ppuc.stateActive("ballSaveReady") and ppuc.switchGroupClosing("playfield") then
    ppuc.clearState("ballSaveReady")
    ppuc.setState("ballSave", 5000)
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

  if number == 5 and state == 1 and ppuc.attractMode() then
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
