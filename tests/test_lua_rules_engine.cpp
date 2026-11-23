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

TEST_CASE("coils are driven unless a rule takes one over") {
  SUBCASE("not suppressed") {
    RulesHarness harness(R"LUA(
function ppuc.onCoilChanged(number, state)
end
)LUA");
    harness.LoadOrFail();

    CHECK(harness.engine().ProcessCoilState(5, 1).forwardToBoard);
  }

  SUBCASE("suppressed") {
    // Flash's multiball: the ROM fires the eject hole the moment the ball
    // arrives, and the rule has to put the second ball into the shooter lane
    // instead. The answer has to come before the coil reaches the board.
    RulesHarness harness(R"LUA(
function ppuc.onCoilChanged(number, state)
  if number == 5 and state == 1 then
    ppuc.suppressCoil(5)
  end
end
)LUA");
    harness.LoadOrFail();

    CHECK_FALSE(harness.engine().ProcessCoilState(5, 1).forwardToBoard);
  }

  SUBCASE("suppressing one coil does not affect another, or the next activation") {
    RulesHarness harness(R"LUA(
function ppuc.onCoilChanged(number, state)
  if number == 5 and state == 1 then
    ppuc.suppressCoil(5)
  end
end
)LUA");
    harness.LoadOrFail();

    CHECK_FALSE(harness.engine().ProcessCoilState(5, 1).forwardToBoard);
    CHECK(harness.engine().ProcessCoilState(1, 1).forwardToBoard);
    // The rule answers for the activation in front of it and nothing else, so
    // a coil is never left disabled by a rule that stopped looking.
    CHECK(harness.engine().ProcessCoilState(5, 0).forwardToBoard);
  }
}

TEST_CASE("a rule holds a coil on and releases it") {
  // A diverter, a gate, a motor: on until the rule says off. The board keeps
  // applying the coil's configured power, hold power and maximum on-time.
  RulesHarness harness(R"LUA(
function ppuc.onSwitchChanged(number, state)
  if number == 38 and state == 1 then
    ppuc.setCoil(27, true)
  elseif number == 38 then
    ppuc.setCoil(27, false)
  elseif number == 42 then
    ppuc.setCoil(28, state)
  end
end
)LUA");
  harness.LoadOrFail();

  harness.engine().ProcessSwitchState(38, 1);
  harness.engine().ProcessSwitchState(38, 0);
  harness.engine().ProcessSwitchState(42, 1);

  REQUIRE(harness.actions.size() == 3);
  CHECK(harness.actions[0].type == RulesActionType::SetCoil);
  CHECK(harness.actions[0].number == 27);
  CHECK(harness.actions[0].state == 1);
  CHECK(harness.actions[1].type == RulesActionType::SetCoil);
  CHECK(harness.actions[1].state == 0);
  CHECK(harness.actions[2].number == 28);
  CHECK(harness.actions[2].state == 1);              // a number works too
}

TEST_CASE("setCoil without a state is an error naming the call") {
  RulesHarness harness(R"LUA(
function ppuc.onSwitchChanged(number, state)
  ppuc.setCoil(27)
end
)LUA");
  harness.LoadOrFail();
  harness.engine().ProcessSwitchState(1, 1);
  CHECK(harness.engine().HasFatalError());
  CHECK(harness.engine().GetFatalError().find("setCoil") != std::string::npos);
}

TEST_CASE("a rule can tell a playfield hit from a button press") {
  // The same definition the ball search uses, rather than a second list in the
  // rules that can disagree with the configuration.
  RulesHarness harness(R"LUA(
function ppuc.onSwitchChanged(number, state)
  if ppuc.isButtonSwitch(number) then
    ppuc.speech("button")
  else
    ppuc.speech("playfield")
  end
end
)LUA");
  harness.LoadOrFail();
  harness.engine().SetButtonSwitches({201, 202, 203});

  harness.engine().ProcessSwitchState(201, 1);
  harness.engine().ProcessSwitchState(27, 1);

  REQUIRE(harness.speech.size() == 2);
  CHECK(harness.speech[0] == "button");
  CHECK(harness.speech[1] == "playfield");
}

TEST_CASE("a rule can hold the ball search off and let it run again") {
  RulesHarness harness(R"LUA(
function ppuc.onSwitchChanged(number, state)
  if number == 27 then
    ppuc.holdBallSearch(true)
  elseif number == 28 then
    ppuc.holdBallSearch(false)
  end
end
)LUA");
  harness.LoadOrFail();

  harness.engine().ProcessSwitchState(27, 1);
  harness.engine().ProcessSwitchState(28, 1);

  REQUIRE(harness.actions.size() == 2);
  CHECK(harness.actions[0].type == RulesActionType::HoldBallSearch);
  CHECK(harness.actions[0].state == 1);
  CHECK(harness.actions[1].type == RulesActionType::HoldBallSearch);
  CHECK(harness.actions[1].state == 0);
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

namespace {
struct RecordedGiProgram {
  int string;
  std::vector<uint32_t> colors;
  std::string effect;
  uint16_t periodMs;
};
}  // namespace

TEST_CASE("a rule sets a GI color program and learns whether it is shown") {
  RulesHarness harness(R"LUA(
results = {}
function ppuc.onSwitchChanged(number, state)
  if number == 1 then
    results[#results + 1] = ppuc.setGiProgram(2, {"FF3000", "#00ff80"}, "fade", 2000)
  elseif number == 2 then
    results[#results + 1] = ppuc.setGiProgram(1, "FFE0B0")
  elseif number == 3 then
    results[#results + 1] = ppuc.setGiProgram(5, {0x0000FF, 0x00FF00}, "cycle", 70000)
  elseif number == 4 then
    results[#results + 1] = ppuc.setGiProgram(3, nil, "default")
  elseif number == 5 then
    if results[1] ~= true or results[4] ~= false then
      error("setGiProgram did not return what the boards answered")
    end
  end
end
)LUA");
  std::vector<RecordedGiProgram> programs;
  bool accept = true;
  harness.engine().SetGiProgramCallback(
      [&](int string, const std::vector<uint32_t>& colors, const std::string& effect, uint16_t periodMs) {
        programs.push_back(RecordedGiProgram{string, colors, effect, periodMs});
        return accept;
      });
  harness.LoadOrFail();

  harness.engine().ProcessSwitchState(1, 1);
  harness.engine().ProcessSwitchState(2, 1);
  harness.engine().ProcessSwitchState(3, 1);
  accept = false;  // the boards cannot show it: the script is told
  harness.engine().ProcessSwitchState(4, 1);

  REQUIRE(programs.size() == 4);
  CHECK(programs[0].string == 2);
  CHECK(programs[0].colors == std::vector<uint32_t>{0xFF3000, 0x00FF80});
  CHECK(programs[0].effect == "fade");
  CHECK(programs[0].periodMs == 2000);
  // A single color, and static when no effect is named.
  CHECK(programs[1].colors == std::vector<uint32_t>{0xFFE0B0});
  CHECK(programs[1].effect == "static");
  CHECK(programs[1].periodMs == 0);
  // Numbers work as colors; a period past the wire's 16 bits is capped.
  CHECK(programs[2].colors == std::vector<uint32_t>{0x0000FF, 0x00FF00});
  CHECK(programs[2].periodMs == 0xFFFF);
  CHECK(programs[3].colors.empty());
  CHECK(programs[3].effect == "default");

  harness.engine().ProcessSwitchState(5, 1);
  CHECK_FALSE(harness.engine().HasFatalError());
}

TEST_CASE("a GI program the boards cannot take is an error naming the line") {
  for (const char* call : {
           "ppuc.setGiProgram(6, 'FF0000')",
           "ppuc.setGiProgram(0, 'FF0000')",
           "ppuc.setGiProgram(1, 'FF00')",
           "ppuc.setGiProgram(1, 'GG0000')",
           "ppuc.setGiProgram(1, {'FF0000', 'red'})",
           "ppuc.setGiProgram(1, 'FF0000', 'top')",
           "ppuc.setGiProgram(1, 0x1000000)",
       }) {
    RulesHarness harness(call);
    const std::string error = harness.Load();
    CAPTURE(call);
    CHECK(error.find("ppuc.setGiProgram") != std::string::npos);
  }
}

TEST_CASE("without boards to send it to, a GI program is not shown") {
  RulesHarness harness(R"LUA(
shown = ppuc.setGiProgram(1, "FF0000")
if shown then error("shown without boards") end
)LUA");
  harness.LoadOrFail();
}

TEST_CASE("a rule sends a sound command to the pack and learns whether it went") {
  RulesHarness harness(R"LUA(
results = {}
function ppuc.onSwitchChanged(number, state)
  if number == 1 then
    results[#results + 1] = ppuc.soundCommand(0x03e6)
  elseif number == 2 then
    results[#results + 1] = ppuc.soundCommand(0x1000, 1)
  elseif number == 3 then
    if results[1] ~= true or results[2] ~= false then
      error("soundCommand did not return what the host answered")
    end
  end
end
)LUA");
  std::vector<std::pair<int, uint32_t>> sent;
  bool accept = true;
  harness.engine().SetSoundCommandCallback([&](int board, uint32_t cmd) {
    sent.emplace_back(board, cmd);
    return accept;
  });
  harness.LoadOrFail();

  harness.engine().ProcessSwitchState(1, 1);
  accept = false;  // e.g. the generation cannot express the number
  harness.engine().ProcessSwitchState(2, 1);
  harness.engine().ProcessSwitchState(3, 1);

  REQUIRE(sent.size() == 2);
  CHECK(sent[0] == std::make_pair(0, 0x03E6u));
  CHECK(sent[1] == std::make_pair(1, 0x1000u));
  CHECK_FALSE(harness.engine().HasFatalError());
}

TEST_CASE("a sound command out of range is an error naming the call") {
  for (const char* call : {
           "ppuc.soundCommand(0x10000)",
           "ppuc.soundCommand(-1)",
           "ppuc.soundCommand(1, 256)",
           "ppuc.soundCommand('loud')",
       }) {
    RulesHarness harness(call);
    const std::string error = harness.Load();
    CAPTURE(call);
    CHECK(error.find("soundCommand") != std::string::npos);
  }
}

TEST_CASE("without AltSound's host a sound command is not sent") {
  RulesHarness harness(R"LUA(
if ppuc.soundCommand(0x0015) then error("sent without a host") end
)LUA");
  harness.LoadOrFail();
}

TEST_CASE("ppuc.gameVersion names the game with a semantic version") {
  RulesHarness harness(R"LUA(
ppuc.gameVersion("Trinity", "0.1.0")
)LUA");
  harness.LoadOrFail();
  const auto identity = harness.engine().GetGameIdentity();
  CHECK(identity.name == "Trinity");
  CHECK(identity.version == "0.1.0");
}

TEST_CASE("ppuc.gameVersion refuses what is not a semantic version") {
  RulesHarness harness(R"LUA(
ppuc.gameVersion("Trinity", "0.1")
)LUA");
  const auto error = harness.Load();
  CHECK(error.find("not a semantic version") != std::string::npos);
  CHECK(harness.engine().GetGameIdentity().version.empty());
}

TEST_CASE("semantic versions as semver.org has them") {
  using E = LuaRulesEngine;
  CHECK(E::IsSemanticVersion("0.1.0"));
  CHECK(E::IsSemanticVersion("1.12.300"));
  CHECK(E::IsSemanticVersion("0.1.0-beta.2"));
  CHECK(E::IsSemanticVersion("0.1.0+20261123"));
  CHECK(E::IsSemanticVersion("0.1.0-rc.1+build.7"));
  CHECK_FALSE(E::IsSemanticVersion("0.1"));
  CHECK_FALSE(E::IsSemanticVersion("v0.1.0"));
  CHECK_FALSE(E::IsSemanticVersion("01.1.0"));
  CHECK_FALSE(E::IsSemanticVersion("0.1.0-"));
  CHECK_FALSE(E::IsSemanticVersion("0.1.0-beta..1"));
  CHECK_FALSE(E::IsSemanticVersion(""));
}
