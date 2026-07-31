// Tests for the time-dependent parts of LuaRulesEngine.
//
// These are the features a rules author reaches for when writing real game
// logic - ball save windows, combo sequences, rate-limited callouts, delayed
// coil pulses - and a bug in any of them shows up as "the ball save did not
// fire" rather than as a crash. They were previously untestable because the
// engine read the clock internally; LuaRulesEngine::SetClock() now makes them
// deterministic.
//
// The harness starts its clock at 10000 ms and only moves it when a test says
// so, so every assertion below is exact rather than timing-dependent.

#include "RulesFixture.h"

using ppuc_test::RulesHarness;

TEST_CASE("a named state with a duration expires on time") {
  RulesHarness harness(R"LUA(
function ppuc.onSwitchChanged(number, state)
  if number == 15 then
    ppuc.setState("ballSave", 5000)
  elseif number == 17 and ppuc.stateActive("ballSave") then
    ppuc.speech("saved")
  end
end
)LUA");
  harness.LoadOrFail();

  harness.engine().ProcessSwitchState(15, 1);

  SUBCASE("active just before the deadline") {
    harness.Advance(4999);
    harness.engine().ProcessSwitchState(17, 1);
    CHECK(harness.speech.size() == 1);
  }

  SUBCASE("expired just after the deadline") {
    harness.Advance(5001);
    harness.engine().ProcessSwitchState(17, 1);
    CHECK(harness.speech.empty());
  }
}

TEST_CASE("a named state without a duration does not expire") {
  RulesHarness harness(R"LUA(
function ppuc.onSwitchChanged(number, state)
  if number == 15 then
    ppuc.setState("armed")
  elseif number == 17 and ppuc.stateActive("armed") then
    ppuc.speech("still armed")
  end
end
)LUA");
  harness.LoadOrFail();

  harness.engine().ProcessSwitchState(15, 1);
  harness.Advance(60'000);
  harness.engine().ProcessSwitchState(17, 1);

  CHECK_MESSAGE(harness.speech.size() == 1,
                "setState without a duration must never time out");
}

TEST_CASE("onlyOnceEvery rate-limits within its window") {
  RulesHarness harness(R"LUA(
function ppuc.onSwitchChanged(number, state)
  if ppuc.onlyOnceEvery("callout", 10000) then
    ppuc.speech("bonus")
  end
end
)LUA");
  harness.LoadOrFail();

  harness.engine().ProcessSwitchState(13, 1);
  CHECK(harness.speech.size() == 1);

  SUBCASE("suppressed inside the window") {
    harness.Advance(9999);
    harness.engine().ProcessSwitchState(13, 1);
    CHECK(harness.speech.size() == 1);
  }

  SUBCASE("allowed again after the window") {
    harness.Advance(10'001);
    harness.engine().ProcessSwitchState(13, 1);
    CHECK(harness.speech.size() == 2);
  }
}

TEST_CASE("ppuc.after runs the callback from the update tick") {
  RulesHarness harness(R"LUA(
function ppuc.onSwitchChanged(number, state)
  if number == 16 and state == 1 then
    ppuc.after(500, function()
      ppuc.speech("delayed")
    end)
  end
end
)LUA");
  harness.LoadOrFail();

  harness.engine().ProcessSwitchState(16, 1);
  CHECK_MESSAGE(harness.speech.empty(),
                "ppuc.after must not run its callback synchronously");

  SUBCASE("not yet due") {
    harness.AdvanceAndUpdate(499);
    CHECK(harness.speech.empty());
  }

  SUBCASE("due") {
    harness.AdvanceAndUpdate(501);
    REQUIRE(harness.speech.size() == 1);
    CHECK(harness.speech[0] == "delayed");
  }

  SUBCASE("runs exactly once") {
    harness.AdvanceAndUpdate(501);
    harness.AdvanceAndUpdate(5000);
    CHECK(harness.speech.size() == 1);
  }
}

TEST_CASE("several ppuc.after callbacks fire in due order") {
  RulesHarness harness(R"LUA(
function ppuc.onSwitchChanged(number, state)
  ppuc.after(300, function() ppuc.speech("third") end)
  ppuc.after(100, function() ppuc.speech("first") end)
  ppuc.after(200, function() ppuc.speech("second") end)
end
)LUA");
  harness.LoadOrFail();

  harness.engine().ProcessSwitchState(16, 1);
  harness.AdvanceAndUpdate(1000);

  REQUIRE(harness.speech.size() == 3);
  CHECK(harness.speech[0] == "first");
  CHECK(harness.speech[1] == "second");
  CHECK(harness.speech[2] == "third");
}

TEST_CASE("triggerHistory only sees triggers inside its window") {
  RulesHarness harness(R"LUA(
function ppuc.onSwitchChanged(number, state)
  if number == 20 then
    ppuc.pupTrigger("P", 42, 1)
  elseif number == 21 and ppuc.triggerHistory(42, 5000) then
    ppuc.speech("recent")
  end
end
)LUA");
  harness.LoadOrFail();

  harness.engine().ProcessSwitchState(20, 1);

  SUBCASE("inside the window") {
    harness.Advance(4999);
    harness.engine().ProcessSwitchState(21, 1);
    CHECK(harness.speech.size() == 1);
  }

  SUBCASE("outside the window") {
    harness.Advance(5001);
    harness.engine().ProcessSwitchState(21, 1);
    CHECK(harness.speech.empty());
  }
}

TEST_CASE("triggerSequence requires the right order inside the window") {
  RulesHarness harness(R"LUA(
function ppuc.onSwitchChanged(number, state)
  if number == 20 then
    ppuc.pupTrigger("P", 1, 1)
  elseif number == 21 then
    ppuc.pupTrigger("P", 2, 1)
  elseif number == 22 then
    ppuc.pupTrigger("P", 3, 1)
  elseif number == 30 and ppuc.triggerSequence(5000, 1, 2, 3) then
    ppuc.speech("combo")
  end
end
)LUA");
  harness.LoadOrFail();

  SUBCASE("correct order within the window") {
    harness.engine().ProcessSwitchState(20, 1);
    harness.Advance(100);
    harness.engine().ProcessSwitchState(21, 1);
    harness.Advance(100);
    harness.engine().ProcessSwitchState(22, 1);
    harness.engine().ProcessSwitchState(30, 1);
    CHECK(harness.speech.size() == 1);
  }

  SUBCASE("wrong order") {
    harness.engine().ProcessSwitchState(22, 1);
    harness.Advance(100);
    harness.engine().ProcessSwitchState(21, 1);
    harness.Advance(100);
    harness.engine().ProcessSwitchState(20, 1);
    harness.engine().ProcessSwitchState(30, 1);
    CHECK(harness.speech.empty());
  }

  SUBCASE("correct order but spread beyond the window") {
    harness.engine().ProcessSwitchState(20, 1);
    harness.Advance(3000);
    harness.engine().ProcessSwitchState(21, 1);
    harness.Advance(3000);
    harness.engine().ProcessSwitchState(22, 1);
    harness.engine().ProcessSwitchState(30, 1);
    CHECK(harness.speech.empty());
  }
}

TEST_CASE("the default clock is used when none is injected") {
  // Guards the production path: SetClock is an override, and an engine that
  // never gets one must still see time advance.
  LuaRulesEngine engine;
  std::string error;

  ppuc_test::TempLua script(R"LUA(
function ppuc.onSwitchChanged(number, state)
  if ppuc.onlyOnceEvery("x", 1) then
    ppuc.speech("tick")
  end
end
)LUA");

  int calls = 0;
  engine.SetSpeechCallback([&calls](const std::string&) { ++calls; });
  REQUIRE(engine.LoadScript(script.path(), error));

  engine.ProcessSwitchState(13, 1);
  CHECK(calls == 1);
}
