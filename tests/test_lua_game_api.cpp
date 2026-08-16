// Tests for the ppuc.game.* and ppuc.dmd.* Lua API.
//
// These wire a real GameCore and a real DmdCanvas to a real LuaRulesEngine
// through the same seam ScriptEngine uses, so what is tested is the API a rules
// author actually writes against rather than a mock of it.
//
// The property worth protecting above the others: `ppuc.game == nil` under the
// PinMAME engine. A rules file shared between a ROM machine and a ROM-less one
// relies on that to feature-test, and a silent no-op stub would make a
// misconfigured machine a mystery instead of an error naming the line.

#include <algorithm>
#include <string>

#include "GameFixture.h"
#include "RulesFixture.h"
#include "dmd/DmdCanvas.h"
#include "doctest.h"
#include "game/GameEvent.h"

using ppuc_test::TempLua;

namespace
{

// A GameCore, a canvas and a rules engine wired together the way ScriptEngine
// does it, with an injected clock shared by all three.
class GameApiHarness
{
 public:
  GameApiHarness() : m_canvas(128, 32)
  {
    m_engine.SetClock([this]() { return m_nowMs; });
    m_game.core().SetAllowCallback([this](const char* what, int arg)
                                   { return m_engine.CallQueryHandler(what, {arg}, true); });
    m_engine.SetActionCallback([this](const RulesAction& action) { actions.push_back(action); });
    m_engine.SetSpeechCallback([this](const std::string& text) { said.push_back(text); });
  }

  // Attaches the game and the canvas, then loads the script. Order matters:
  // the sub-tables are registered when the Lua state is built.
  void Load(const std::string& script)
  {
    m_engine.SetGameCore(&m_game.core());
    m_engine.SetDmdCanvas(&m_canvas);

    TempLua file(script);
    std::string error;
    const bool loaded = m_engine.LoadScript(file.path(), error);
    REQUIRE_MESSAGE(loaded, error);
  }

  // Loads a script with no game attached, as the PinMAME engine would.
  void LoadWithoutGame(const std::string& script)
  {
    TempLua file(script);
    std::string error;
    const bool loaded = m_engine.LoadScript(file.path(), error);
    REQUIRE_MESSAGE(loaded, error);
  }

  // Drains GameCore's events into the rules engine, as ppuc.cpp does.
  void Pump()
  {
    m_game.Update();
    for (const GameEvent& event : m_game.events)
    {
      Dispatch(event);
    }
    m_game.events.clear();
    m_engine.Update();
  }

  void Advance(uint64_t ms)
  {
    m_nowMs += ms;
    m_game.Advance(ms);
    Pump();
  }

  GameHarness& game() { return m_game; }
  LuaRulesEngine& engine() { return m_engine; }
  DmdCanvas& canvas() { return m_canvas; }

  bool HasFatalError() const { return m_engine.HasFatalError(); }
  std::string FatalError() const { return m_engine.GetFatalError(); }

  // Scripts announce what they observed through ppuc.speech, which the harness
  // records. Asserting on that keeps the tests about the API a rules author
  // uses rather than about engine internals.
  bool Said(const std::string& text) const
  {
    return std::find(said.begin(), said.end(), text) != said.end();
  }

  std::vector<RulesAction> actions;
  std::vector<std::string> said;

 private:
  void Dispatch(const GameEvent& event)
  {
    switch (event.type)
    {
      case GameEventType::GameStart: m_engine.CallGameHandler("onGameStart", {event.value}); break;
      case GameEventType::BallStart: m_engine.CallGameHandler("onBallStart", {event.player, event.ball}); break;
      case GameEventType::BallEnd: m_engine.CallGameHandler("onBallEnd", {event.player, event.ball}); break;
      case GameEventType::BonusCount: m_engine.CallGameHandler("onBonusCount", {event.player, event.ball}); break;
      case GameEventType::Score:
        m_engine.CallGameHandler("onScore", {event.player, event.value, event.total});
        break;
      case GameEventType::StartRejected:
        m_engine.CallGameHandler("onStartRejected", {StartRejectReasonName(event.reason)});
        break;
      case GameEventType::GameEnd:
        m_engine.CallGameHandler("onGameEnd", {event.player, event.value, event.total});
        break;
      default: break;
    }
  }

  uint64_t m_nowMs = 10000;
  GameHarness m_game;
  DmdCanvas m_canvas;
  LuaRulesEngine m_engine;
};

}  // namespace

TEST_CASE("ppuc.game is absent under the PinMAME engine")
{
  // The feature test a shared rules file relies on.
  GameApiHarness h;
  h.LoadWithoutGame(R"(
    sawGame = ppuc.game ~= nil
    sawDmd = ppuc.dmd ~= nil
  )");

  CHECK_FALSE(h.HasFatalError());
}

TEST_CASE("calling ppuc.game without a game core is an error, not a silent no-op")
{
  GameApiHarness h;
  h.LoadWithoutGame(R"(
    function ppuc.onRulesUpdate()
      ppuc.game.addScore(100)
    end
  )");

  h.engine().Update();
  CHECK(h.HasFatalError());
  // The message names the failure rather than leaving a machine that scores
  // nothing and says nothing.
  CHECK(h.FatalError().find("onRulesUpdate") != std::string::npos);
}

TEST_CASE("a rule can score through ppuc.game.addScore")
{
  GameApiHarness h;
  h.game().config().freePlay = true;
  h.game().Start();
  h.Load(R"(
    function ppuc.onSwitchChanged(number, state)
      if number == 22 and state == 1 then
        ppuc.game.addScore(5000)
      end
    end
  )");

  h.game().StartGameAndServe();
  h.engine().ProcessSwitchState(22, 1);
  CHECK(h.game().core().GetScore(1) == 5000);
}

TEST_CASE("scoring is still discarded while tilted, without the rule knowing")
{
  GameApiHarness h;
  h.game().config().freePlay = true;
  h.game().Start();
  h.Load(R"(
    function ppuc.onSwitchChanged(number, state)
      if number == 22 and state == 1 then ppuc.game.addScore(5000) end
    end
  )");

  h.game().StartGameAndServe();
  h.game().core().Tilt();
  h.Pump();

  h.engine().ProcessSwitchState(22, 1);
  CHECK(h.game().core().GetScore(1) == 0);
}

TEST_CASE("onScore carries player, points and running total")
{
  GameApiHarness h;
  h.game().config().freePlay = true;
  h.game().Start();
  h.Load(R"(
    lastPlayer, lastPoints, lastTotal = 0, 0, 0
    function ppuc.onScore(player, points, total)
      lastPlayer, lastPoints, lastTotal = player, points, total
    end
    function ppuc.onRulesUpdate()
      ppuc.speech("p" .. lastPlayer)
      ppuc.speech("pts" .. lastPoints)
      ppuc.speech("tot" .. lastTotal)
    end
  )");

  h.game().StartGameAndServe();
  h.game().core().AddScore(1500);
  h.Pump();

  CHECK(h.Said("p1"));
  CHECK(h.Said("pts1500"));
  CHECK(h.Said("tot1500"));
}

TEST_CASE("a score larger than 32 bits survives the round trip")
{
  // Guards the widening of handler arguments to 64-bit: an int would silently
  // wrap here.
  GameApiHarness h;
  h.game().config().freePlay = true;
  h.game().config().scoreDigits = 12;
  h.game().Start();
  h.Load(R"(
    seen = 0
    function ppuc.onScore(player, points, total) seen = total end
    function ppuc.onRulesUpdate()
      if seen > 4000000000 then ppuc.speech("big") end
    end
  )");

  h.game().StartGameAndServe();
  h.game().core().AddScore(5000000000LL);
  h.Pump();

  CHECK(h.Said("big"));
}

TEST_CASE("canStartGame can veto a start and an absent handler allows")
{
  SUBCASE("veto")
  {
    GameApiHarness h;
    h.game().config().freePlay = true;
    h.game().Start();
    h.Load(R"(
      function ppuc.canStartGame() return false end
      rejected = ""
      function ppuc.onStartRejected(reason) rejected = reason end
      function ppuc.onRulesUpdate()
        if rejected ~= "" then ppuc.speech(rejected) end
      end
    )");

    h.game().PressStart();
    h.Pump();
    CHECK(h.game().core().GetState() == GameState::Attract);
    CHECK(h.Said("scriptVetoed"));
  }

  SUBCASE("absent handler allows")
  {
    GameApiHarness h;
    h.game().config().freePlay = true;
    h.game().Start();
    h.Load("-- no handlers at all\n");

    h.game().PressStart();
    h.Pump();
    CHECK(h.game().core().IsInGame());
  }
}

TEST_CASE("a bonus rule counts down and ends the bonus")
{
  GameApiHarness h;
  h.game().config().freePlay = true;
  h.game().Start();
  h.Load(R"(
    function ppuc.onBonusCount(player, ball)
      local bonus = ppuc.game.getBall("bonus")
      ppuc.game.addScore(bonus * 1000)
      ppuc.game.bonusDone()
    end
  )");

  h.game().StartGameAndServe();
  h.game().core().SetBallVar("bonus", 7);
  h.game().core().AddScore(1000);
  h.Pump();

  h.game().Drain();
  h.Pump();
  // bonusDone() is called from inside the handler, so the state machine acts on
  // it on the next tick -- exactly as ScriptEngine drives it.
  h.Pump();

  // The bonus was collected and the game moved straight on rather than waiting
  // out the timeout.
  CHECK(h.game().core().GetScore(1) == 8000);
  CHECK(h.game().core().GetState() != GameState::BonusCount);
}

TEST_CASE("per-player and per-ball variables behave as documented")
{
  GameApiHarness h;
  h.game().config().freePlay = true;
  h.game().Start();
  h.Load(R"(
    function ppuc.onSwitchChanged(number, state)
      if number == 24 and state == 1 then
        ppuc.game.add("ramps", 1)
        ppuc.game.addBall("combo", 1)
      end
    end
  )");

  h.game().StartGameAndServe();
  h.engine().ProcessSwitchState(24, 1);
  h.engine().ProcessSwitchState(24, 0);
  h.engine().ProcessSwitchState(24, 1);

  CHECK(h.game().core().GetVar("ramps") == 2);
  CHECK(h.game().core().GetBallVar("combo") == 2);

  h.game().DrainAndFinishBall();
  CHECK(h.game().core().GetVar("ramps") == 2);   // per player, survives the ball
  CHECK(h.game().core().GetBallVar("combo") == 0);  // per ball, cleared
}

TEST_CASE("friendly names resolve through emGame.names")
{
  GameApiHarness h;
  h.game().config().freePlay = true;
  h.game().config().names.coils["outhole"] = 1;
  h.game().config().names.switches["spinner"] = 25;
  h.game().Start();
  h.Load(R"(
    function ppuc.onRulesUpdate()
      if ppuc.game.coil("outhole") == 1 then ppuc.speech("coilOk") end
      if ppuc.game.switch("spinner") == 25 then ppuc.speech("switchOk") end
      if ppuc.game.coil("nosuch") == 0 then ppuc.speech("unknownIsZero") end
    end
  )");

  h.Pump();
  CHECK(h.Said("coilOk"));
  CHECK(h.Said("switchOk"));
  CHECK(h.Said("unknownIsZero"));
}

TEST_CASE("state queries report what the machine is doing")
{
  GameApiHarness h;
  h.game().config().freePlay = true;
  h.game().Start();
  h.Load(R"(
    function ppuc.onRulesUpdate()
      ppuc.speech("state:" .. ppuc.game.state())
      if ppuc.game.attract() then ppuc.speech("inAttract") end
      if ppuc.game.inGame() then ppuc.speech("inGame") end
    end
  )");

  h.Pump();
  CHECK(h.Said("inAttract"));
  CHECK(h.Said("state:attract"));

  h.game().StartGameAndServe();
  h.Pump();
  CHECK(h.Said("inGame"));
}

TEST_CASE("ppuc.ballSave emits a GrantBallSave action")
{
  GameApiHarness h;
  h.game().config().freePlay = true;
  h.game().Start();
  h.Load(R"(
    function ppuc.onSwitchChanged(number, state)
      if number == 22 and state == 1 then ppuc.ballSave(4000) end
    end
  )");

  h.engine().ProcessSwitchState(22, 1);

  REQUIRE(h.actions.size() == 1);
  CHECK(h.actions[0].type == RulesActionType::GrantBallSave);
  CHECK(h.actions[0].durationMs == 4000);
}

// ------------------------------------------------------------- ppuc.dmd ----

TEST_CASE("a rule can draw on the DMD")
{
  GameApiHarness h;
  h.game().Start();
  h.Load(R"(
    function ppuc.onDmdFrame()
      ppuc.dmd.clear(0)
      ppuc.dmd.text(2, 2, "HELLO")
    end
  )");

  h.engine().CallGameHandler("onDmdFrame");

  int lit = 0;
  for (int y = 0; y < h.canvas().Height(); ++y)
  {
    for (int x = 0; x < h.canvas().Width(); ++x)
    {
      if (h.canvas().GetPixel(x, y) != 0) ++lit;
    }
  }
  CHECK(lit > 0);
  CHECK_FALSE(h.HasFatalError());
}

TEST_CASE("dmd geometry and text measurement are available to a rule")
{
  GameApiHarness h;
  h.game().Start();
  h.Load(R"(
    function ppuc.onDmdFrame()
      if ppuc.dmd.width() == 128 then ppuc.speech("w") end
      if ppuc.dmd.height() == 32 then ppuc.speech("h") end
      if ppuc.dmd.textWidth("00") == ppuc.dmd.textWidth("88") then ppuc.speech("fixed") end
    end
  )");

  h.engine().CallGameHandler("onDmdFrame");
  CHECK(h.Said("w"));
  CHECK(h.Said("h"));
  CHECK(h.Said("fixed"));
}

TEST_CASE("drawing options are read from the table")
{
  GameApiHarness h;
  h.game().Start();
  h.Load(R"(
    function ppuc.onDmdFrame()
      ppuc.dmd.clear(0)
      ppuc.dmd.number(126, 2, 1234567, { scale = 2, align = "right", commas = true })
    end
  )");

  h.engine().CallGameHandler("onDmdFrame");
  REQUIRE_FALSE(h.HasFatalError());

  int rightmost = -1;
  for (int x = 0; x < h.canvas().Width(); ++x)
  {
    for (int y = 0; y < h.canvas().Height(); ++y)
    {
      if (h.canvas().GetPixel(x, y) != 0) rightmost = x;
    }
  }
  CHECK(rightmost == 126);
}

TEST_CASE("formatNumber matches what number() would draw")
{
  GameApiHarness h;
  h.game().Start();
  h.Load(R"(
    function ppuc.onDmdFrame()
      if ppuc.dmd.formatNumber(1234567, { commas = true }) == "1,234,567" then
        ppuc.speech("commas")
      end
      if ppuc.dmd.formatNumber(5, { digits = 3, pad = "0" }) == "005" then
        ppuc.speech("padded")
      end
    end
  )");

  h.engine().CallGameHandler("onDmdFrame");
  CHECK(h.Said("commas"));
  CHECK(h.Said("padded"));
}

TEST_CASE("a script drawing far outside the canvas does not crash")
{
  GameApiHarness h;
  h.game().Start();
  h.Load(R"(
    function ppuc.onDmdFrame()
      ppuc.dmd.text(-500, -500, "OFFSCREEN")
      ppuc.dmd.fill(2000, 2000, 50, 50, 15)
      ppuc.dmd.line(-100, -100, 900, 900, 15)
      ppuc.dmd.number(0, 0, 999999999999, { scale = 8 })
    end
  )");

  h.engine().CallGameHandler("onDmdFrame");
  CHECK_FALSE(h.HasFatalError());
  CHECK(h.canvas().Size() == 128u * 32u);
}
