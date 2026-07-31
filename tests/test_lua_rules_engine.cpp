// Tests for LuaRulesEngine.
//
// Rules are the layer game authors write, so a regression here breaks other
// people's machines rather than ours. The engine is also the newest subsystem
// in the stack and the one with the least prior validation.
//
// Time-dependent behaviour (ppuc.after, setState with a duration,
// onlyOnceEvery, triggerHistory windows) is deliberately not covered yet: the
// engine reads the clock internally, so testing it would require either real
// sleeps or a clock seam in production code. That seam is worth adding, but as
// a separate, deliberate change.

#include "RulesFixture.h"

using ppuc_test::RulesHarness;

TEST_CASE("a script with no handlers loads cleanly") {
  RulesHarness harness("-- intentionally empty\n");
  CHECK(harness.Load().empty());
  CHECK_FALSE(harness.engine().HasFatalError());
}

TEST_CASE("a syntax error is reported rather than silently ignored") {
  RulesHarness harness("function ppuc.onSwitchChanged(  -- unterminated\n");
  const auto error = harness.Load();
  CHECK_FALSE(error.empty());
}

TEST_CASE("switch changes reach onSwitchChanged with number and state") {
  RulesHarness harness(R"LUA(
seen = {}
function ppuc.onSwitchChanged(number, state)
  table.insert(seen, tostring(number) .. ":" .. tostring(state))
  if number == 13 and state == 1 then
    ppuc.pupTrigger("P", 100, 1)
  end
end
)LUA");
  harness.LoadOrFail();

  harness.engine().ProcessSwitchState(13, 1);

  REQUIRE(harness.triggers.size() == 1);
  CHECK(harness.triggers[0].source == 'P');
  CHECK(harness.triggers[0].id == 100);
  CHECK(harness.triggers[0].value == 1);
}

TEST_CASE("switches are forwarded to PinMAME unless suppressed") {
  SUBCASE("not suppressed") {
    RulesHarness harness(R"LUA(
function ppuc.onSwitchChanged(number, state)
end
)LUA");
    harness.LoadOrFail();

    const auto result = harness.engine().ProcessSwitchState(9, 1);
    CHECK(result.forwardToCpu);
  }

  SUBCASE("suppressed") {
    // This is the interceptor mechanism: the rule decides the ROM must not see
    // this switch closure. Getting the polarity of this wrong would silently
    // break ball handling, so it is worth pinning.
    RulesHarness harness(R"LUA(
function ppuc.onSwitchChanged(number, state)
  if number == 9 and state == 1 then
    ppuc.suppressSwitch(9)
  end
end
)LUA");
    harness.LoadOrFail();

    const auto result = harness.engine().ProcessSwitchState(9, 1);
    CHECK_FALSE(result.forwardToCpu);
  }

  SUBCASE("suppressing one switch does not affect another") {
    RulesHarness harness(R"LUA(
function ppuc.onSwitchChanged(number, state)
  if number == 9 then
    ppuc.suppressSwitch(9)
  end
end
)LUA");
    harness.LoadOrFail();

    CHECK_FALSE(harness.engine().ProcessSwitchState(9, 1).forwardToCpu);
    CHECK(harness.engine().ProcessSwitchState(10, 1).forwardToCpu);
  }
}

TEST_CASE("state helpers reflect what the engine has been told") {
  RulesHarness harness(R"LUA(
lampWasOn = nil
function ppuc.onSwitchChanged(number, state)
  lampWasOn = ppuc.lampState(42)
  if lampWasOn then
    ppuc.speech("lamp is on")
  end
end
)LUA");
  harness.LoadOrFail();

  SUBCASE("lamp off") {
    harness.engine().OnLampState(42, 0);
    harness.engine().ProcessSwitchState(13, 1);
    CHECK(harness.speech.empty());
  }

  SUBCASE("lamp on") {
    harness.engine().OnLampState(42, 1);
    harness.engine().ProcessSwitchState(13, 1);
    REQUIRE(harness.speech.size() == 1);
    CHECK(harness.speech[0] == "lamp is on");
  }
}

TEST_CASE("ball, player and attract mode are visible to rules") {
  RulesHarness harness(R"LUA(
function ppuc.onSwitchChanged(number, state)
  ppuc.speech(tostring(ppuc.currentBall()) .. "/" ..
              tostring(ppuc.currentPlayer()) .. "/" ..
              tostring(ppuc.attractMode()))
end
)LUA");
  harness.LoadOrFail();

  harness.engine().SetCurrentBall(3);
  harness.engine().SetCurrentPlayer(2);
  harness.engine().SetAttractMode(false);
  harness.engine().ProcessSwitchState(13, 1);

  REQUIRE(harness.speech.size() == 1);
  CHECK(harness.speech[0] == "3/2/false");
}

TEST_CASE("switch groups are visible to rules") {
  RulesHarness harness(R"LUA(
function ppuc.onSwitchChanged(number, state)
  if ppuc.switchGroupState("playfield") then
    ppuc.speech("playfield active")
  end
end
)LUA");
  harness.LoadOrFail();
  harness.engine().SetSwitchGroups({{"playfield", {10, 11, 12}}});

  SUBCASE("no group member closed") {
    harness.engine().ProcessSwitchState(99, 1);
    CHECK(harness.speech.empty());
  }

  SUBCASE("a group member is closed") {
    harness.engine().ProcessSwitchState(11, 1);
    CHECK(harness.speech.size() == 1);
  }
}

TEST_CASE("named states without a duration persist until cleared") {
  RulesHarness harness(R"LUA(
function ppuc.onSwitchChanged(number, state)
  if number == 15 then
    ppuc.setState("ballSaveReady")
  elseif number == 16 then
    ppuc.clearState("ballSaveReady")
  elseif number == 17 and ppuc.stateActive("ballSaveReady") then
    ppuc.speech("armed")
  end
end
)LUA");
  harness.LoadOrFail();

  harness.engine().ProcessSwitchState(17, 1);
  CHECK(harness.speech.empty());

  harness.engine().ProcessSwitchState(15, 1);
  harness.engine().ProcessSwitchState(17, 1);
  CHECK(harness.speech.size() == 1);

  harness.engine().ProcessSwitchState(16, 1);
  harness.engine().ProcessSwitchState(17, 1);
  CHECK_MESSAGE(harness.speech.size() == 1,
                "clearState should have disarmed the named state");
}

TEST_CASE("a runtime error in a handler is fatal and reported") {
  // The documented contract is that a Lua runtime error sets a fatal state so
  // the main loop can restore outputs and exit cleanly, rather than continuing
  // to run a machine whose rules are half-broken.
  RulesHarness harness(R"LUA(
function ppuc.onSwitchChanged(number, state)
  error("deliberate failure")
end
)LUA");
  harness.LoadOrFail();

  harness.engine().ProcessSwitchState(13, 1);

  CHECK(harness.engine().HasFatalError());
  CHECK_FALSE(harness.engine().GetFatalError().empty());
}

TEST_CASE("the sandbox denies filesystem access") {
  // Rules ship inside game folders that users download from each other, so the
  // Lua environment is deliberately constrained.
  RulesHarness harness(R"LUA(
function ppuc.onSwitchChanged(number, state)
  if io ~= nil or dofile ~= nil or loadfile ~= nil then
    ppuc.speech("sandbox escape")
  end
end
)LUA");
  harness.LoadOrFail();

  harness.engine().ProcessSwitchState(13, 1);

  CHECK_MESSAGE(harness.speech.empty(),
                "io/dofile/loadfile must not be reachable from rules");
}
