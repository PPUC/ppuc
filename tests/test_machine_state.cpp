// Tests for what a machine keeps on its USB stick and for the coin door buttons:
// the state file format, the operator's adjustments, the switch report, the
// rules' machine memory and who the four service buttons belong to.

#include <cstdio>
#include <filesystem>
#include <string>

#include "Adjustments.h"
#include "CoinDoor.h"
#include "LuaRulesEngine.h"
#include "MachineState.h"
#include "RulesFixture.h"
#include "SwitchReport.h"
#include "doctest.h"

// ---- State files -----------------------------------------------------------

TEST_CASE("State files round-trip numbers, booleans and strings")
{
  StateEntries entries;
  entries.emplace_back("balls", StateValue::Number(3));
  entries.emplace_back("ratio", StateValue::Number(0.5));
  entries.emplace_back("free_play", StateValue::Boolean(true));
  entries.emplace_back("champion", StateValue::String("Callahan \"Dirty\" Harry\\44\nline"));

  const std::string text = MachineState::Format(entries, "PPUC test\nsecond line");
  CHECK(text.find("# PPUC test\n# second line\n") == 0);
  CHECK(text.find("balls = 3\n") != std::string::npos);

  int bad = -1;
  const StateEntries parsed = MachineState::Parse(text, &bad);
  CHECK(bad == 0);
  REQUIRE(parsed.size() == entries.size());
  for (size_t i = 0; i < entries.size(); ++i)
  {
    CHECK(parsed[i].first == entries[i].first);
    CHECK(parsed[i].second == entries[i].second);
  }
}

TEST_CASE("A mangled line costs that line, not the file")
{
  int bad = 0;
  const StateEntries parsed = MachineState::Parse(
      "a = 1\n"
      "no equals sign\n"
      "bad name! = 2\n"
      "b = \"unterminated\n"
      "c = 12abc\n"
      "  d=  true  \n"
      "a = 5\n",
      &bad);
  CHECK(bad == 4);
  REQUIRE(parsed.size() == 2);
  CHECK(parsed[0].first == "a");
  CHECK(parsed[0].second == StateValue::Number(5));
  CHECK(parsed[1].first == "d");
  CHECK(parsed[1].second == StateValue::Boolean(true));
}

TEST_CASE("Writing a state file replaces it whole and creates the folder")
{
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / ("ppuc_state_test_" + std::to_string(std::rand()));
  const std::string path = (dir / "state" / "adjustments.txt").string();
  std::string error;
  REQUIRE(MachineState::WriteFileAtomically(path, "a = 1\n", &error));
  REQUIRE(MachineState::WriteFileAtomically(path, "a = 2\n", &error));
  std::string content;
  REQUIRE(MachineState::ReadFile(path, &content, &error));
  CHECK(content == "a = 2\n");
  CHECK_FALSE(std::filesystem::exists(path + ".tmp"));
  std::filesystem::remove_all(dir);

  CHECK(MachineState::StateDirectory("") == "");
  CHECK(MachineState::StateDirectory("/games/dh") == (std::filesystem::path("/games/dh") / "state").string());
}

// ---- Adjustments -----------------------------------------------------------

static Adjustments::Definition NumberAdjustment(const std::string& name, double def, double min, double max,
                                                double step = 1)
{
  Adjustments::Definition d;
  d.name = name;
  d.label = name;
  d.kind = Adjustments::Kind::Number;
  d.min = min;
  d.max = max;
  d.step = step;
  d.defaultValue = StateValue::Number(def);
  return d;
}

TEST_CASE("Adjustments clamp to their range and their step")
{
  Adjustments adjustments;
  std::string error;
  REQUIRE(adjustments.Define(NumberAdjustment("ball_save", 6, 0, 15), &error));
  StateValue applied;
  REQUIRE(adjustments.Set("ball_save", StateValue::Number(40), &applied, &error));
  CHECK(applied == StateValue::Number(15));
  REQUIRE(adjustments.Set("ball_save", StateValue::Number(4.4), &applied, &error));
  CHECK(applied == StateValue::Number(4));
  CHECK_FALSE(adjustments.Set("ball_save", StateValue::String("six"), &applied, &error));
  CHECK_FALSE(adjustments.Set("nothing", StateValue::Number(1), &applied, &error));

  CHECK(adjustments.Stepped("ball_save", +1) == StateValue::Number(5));
  REQUIRE(adjustments.Set("ball_save", StateValue::Number(15), &applied, &error));
  CHECK(adjustments.Stepped("ball_save", +1) == StateValue::Number(15));
}

TEST_CASE("Choices and booleans step round")
{
  Adjustments adjustments;
  std::string error;
  Adjustments::Definition choice;
  choice.name = "difficulty";
  choice.label = "Difficulty";
  choice.kind = Adjustments::Kind::Choice;
  choice.choices = {"Easy", "Normal", "Hard"};
  choice.defaultValue = StateValue::String("Normal");
  REQUIRE(adjustments.Define(choice, &error));
  CHECK(adjustments.Stepped("difficulty", +1) == StateValue::String("Hard"));
  REQUIRE(adjustments.Set("difficulty", StateValue::String("Hard"), nullptr, &error));
  CHECK(adjustments.Stepped("difficulty", +1) == StateValue::String("Easy"));
  CHECK_FALSE(adjustments.Set("difficulty", StateValue::String("Insane"), nullptr, &error));

  Adjustments::Definition flag;
  flag.name = "gun_broken";
  flag.label = "Gun out of order";
  flag.kind = Adjustments::Kind::Boolean;
  flag.defaultValue = StateValue::Boolean(false);
  REQUIRE(adjustments.Define(flag, &error));
  CHECK(adjustments.Stepped("gun_broken", -1) == StateValue::Boolean(true));
  CHECK(adjustments.Display("gun_broken") == "No");
}

TEST_CASE("Invalid adjustment definitions are refused with a reason")
{
  Adjustments adjustments;
  std::string error;
  CHECK_FALSE(adjustments.Define(NumberAdjustment("bad name", 1, 0, 2), &error));
  CHECK_FALSE(adjustments.Define(NumberAdjustment("x", 5, 0, 2), &error));
  CHECK(error.find("out of range") != std::string::npos);
  CHECK_FALSE(adjustments.Define(NumberAdjustment("y", 1, 0, 2, 0), &error));
}

TEST_CASE("Stored adjustments wait for their definition and survive a redefinition")
{
  Adjustments adjustments;
  std::string error;
  adjustments.Load(MachineState::Parse("ball_save = 9\nlater = 3\nunused = \"x\"\n"));

  REQUIRE(adjustments.Define(NumberAdjustment("ball_save", 6, 0, 15), &error));
  CHECK(adjustments.Get("ball_save") == StateValue::Number(9));

  // The rules loaded again with the same definition: the operator's value stays.
  REQUIRE(adjustments.Define(NumberAdjustment("ball_save", 6, 0, 15), &error));
  CHECK(adjustments.Get("ball_save") == StateValue::Number(9));
  // A narrower range: clamped, not lost.
  REQUIRE(adjustments.Define(NumberAdjustment("ball_save", 6, 0, 8), &error));
  CHECK(adjustments.Get("ball_save") == StateValue::Number(8));

  // A stored value that no longer fits at all goes back to the default.
  Adjustments::Definition later;
  later.name = "later";
  later.label = "Later";
  later.kind = Adjustments::Kind::Boolean;
  later.defaultValue = StateValue::Boolean(false);
  REQUIRE(adjustments.Define(later, &error));
  CHECK(adjustments.Get("later") == StateValue::Boolean(false));

  // Saved: what differs from the default, and what nobody defined yet.
  const StateEntries saved = adjustments.Save();
  REQUIRE(saved.size() == 2);
  CHECK(saved[0].first == "ball_save");
  CHECK(saved[1].first == "unused");
}

TEST_CASE("Only a real change marks the adjustments for writing")
{
  Adjustments adjustments;
  std::string error;
  REQUIRE(adjustments.Define(NumberAdjustment("a", 1, 0, 5), &error));
  CHECK_FALSE(adjustments.TakeDirty());
  REQUIRE(adjustments.Set("a", StateValue::Number(1), nullptr, &error));
  CHECK_FALSE(adjustments.TakeDirty());
  REQUIRE(adjustments.Set("a", StateValue::Number(2), nullptr, &error));
  CHECK(adjustments.TakeDirty());
  CHECK_FALSE(adjustments.TakeDirty());
  REQUIRE(adjustments.ResetToDefault("a"));
  CHECK(adjustments.TakeDirty());
}

// ---- Switch report ---------------------------------------------------------

static void PlayGame(SwitchReport& report, uint64_t& nowMs, std::initializer_list<int> hits)
{
  report.NoteGameStarted(nowMs);
  for (const int number : hits) report.NoteChange(number);
  nowMs += 120000;
  report.NoteGameEnded(nowMs);
}

TEST_CASE("A switch that sits through enough games is reported, and repairs itself")
{
  SwitchReport report;
  report.SetThresholdGames(3);
  report.Track(47, "HQ saucer");
  report.Track(48, "Left ramp");
  report.Track(5, "Center coin");

  uint64_t nowMs = 1000;
  // Seen working once, then dead.
  PlayGame(report, nowMs, {47, 48});
  PlayGame(report, nowMs, {48});
  PlayGame(report, nowMs, {48});
  CHECK(report.GetFlagged().empty());

  report.NoteGameStarted(nowMs);
  report.NoteChange(48);
  nowMs += 120000;
  const auto newly = report.NoteGameEnded(nowMs);
  REQUIRE(newly.size() == 1);
  CHECK(newly[0].number == 47);
  CHECK(newly[0].description == "HQ saucer");
  CHECK(newly[0].idleGames == 3);
  REQUIRE(report.GetFlagged().size() == 1);

  // The coin switch never moved: never reported.
  CHECK(report.GetFlagged()[0].number == 47);

  report.NoteChange(47);
  CHECK(report.GetFlagged().empty());
  CHECK(report.GetGamesCounted() == 4);
}

TEST_CASE("Short games, ignored switches and a threshold of 0 report nothing")
{
  SwitchReport report;
  report.SetThresholdGames(1);
  report.Track(14, "Plumb bob");
  report.Track(30, "Target");
  report.Ignore(14);

  uint64_t nowMs = 1000;
  report.NoteChange(14);
  report.NoteChange(30);

  // Abandoned after five seconds: not a game.
  report.NoteGameStarted(nowMs);
  nowMs += 5000;
  CHECK(report.NoteGameEnded(nowMs).empty());
  CHECK(report.GetGamesCounted() == 0);

  PlayGame(report, nowMs, {});
  REQUIRE(report.GetFlagged().size() == 1);
  CHECK(report.GetFlagged()[0].number == 30);

  report.SetThresholdGames(0);
  CHECK(report.GetFlagged().empty());
  report.SetThresholdGames(1);
  CHECK(report.GetFlagged().size() == 1);
}

TEST_CASE("The switch report survives a power cycle")
{
  SwitchReport report;
  report.SetThresholdGames(2);
  report.Track(-3, "Dedicated");
  report.Track(47, "HQ saucer");
  uint64_t nowMs = 0;
  report.NoteChange(47);
  report.NoteChange(-3);
  PlayGame(report, nowMs, {-3});
  PlayGame(report, nowMs, {-3});
  REQUIRE(report.GetFlagged().size() == 1);
  CHECK(report.TakeDirty());

  const std::string text = MachineState::Format(report.Save());
  SwitchReport restored;
  restored.SetThresholdGames(2);
  restored.Track(-3, "Dedicated");
  restored.Track(47, "HQ saucer");
  restored.Load(MachineState::Parse(text));
  REQUIRE(restored.GetFlagged().size() == 1);
  CHECK(restored.GetFlagged()[0].idleGames == 2);
  CHECK(restored.GetGamesCounted() == 2);

  restored.Reset();
  CHECK(restored.GetFlagged().empty());
}

// ---- Machine memory --------------------------------------------------------

TEST_CASE("Machine memory keeps values and refuses what it cannot write")
{
  MachineMemory memory;
  std::string error;
  REQUIRE(memory.Set("eb_level", StateValue::Number(3), &error));
  CHECK(memory.TakeDirty());
  REQUIRE(memory.Set("eb_level", StateValue::Number(3), &error));
  CHECK_FALSE(memory.TakeDirty());
  CHECK_FALSE(memory.Set("bad name", StateValue::Number(1), &error));
  CHECK_FALSE(memory.Set("long", StateValue::String(std::string(2000, 'x')), &error));

  MachineMemory restored;
  restored.Load(MachineState::Parse(MachineState::Format(memory.Save())));
  StateValue value;
  REQUIRE(restored.Get("eb_level", &value));
  CHECK(value == StateValue::Number(3));
  CHECK(restored.Remove("eb_level"));
  CHECK_FALSE(restored.Get("eb_level", &value));
}

// ---- Coin door -------------------------------------------------------------

TEST_CASE("Coin door buttons: WPC has them, a config can name them")
{
  CHECK(CoinDoorButtons::ForPlatform(1).numbers == std::array<int, 4>{5, 6, 7, 8});
  CHECK_FALSE(CoinDoorButtons::ForPlatform(11).Any());

  CoinDoorButtons buttons;
  std::string error;
  REQUIRE(CoinDoorButtons::Parse(" 245, 246 ,0,247", &buttons, &error));
  CHECK(buttons.numbers == std::array<int, 4>{245, 246, 0, 247});
  CHECK(buttons.ButtonFor(247) == CoinDoorButtons::Enter);
  CHECK(buttons.ButtonFor(0) == -1);
  CHECK_FALSE(CoinDoorButtons::Parse("1,2,3", &buttons, &error));
  CHECK_FALSE(CoinDoorButtons::Parse("1,2,x,4", &buttons, &error));
}

TEST_CASE("ENTER opens the tools only with the door open and a screen to show them")
{
  CoinDoorRouter router;
  router.Configure(CoinDoorButtons::ForPlatform(1), 22);
  router.SetToolsAvailable(true);
  using Route = CoinDoorRouter::Route;

  // Door closed: everything is the game's.
  CHECK(router.OnSwitch(22, 1) == Route::Game);
  CHECK(router.OnSwitch(8, 1) == Route::Game);
  CHECK(router.OnSwitch(8, 0) == Route::Game);

  // Door open: ENTER is the tools', UP is still the game's (WPC volume).
  CHECK(router.OnSwitch(22, 0) == Route::Game);
  CHECK(router.OnSwitch(7, 1) == Route::Game);
  CHECK(router.OnSwitch(7, 0) == Route::Game);
  CHECK(router.OnSwitch(8, 1) == Route::Tools);
  // The tools open while ENTER is still held: the release follows the press.
  router.SetToolsOpen(true);
  CHECK(router.OnSwitch(8, 0) == Route::Tools);
  CHECK(router.OnSwitch(7, 1) == Route::Tools);
  CHECK(router.OnSwitch(7, 0) == Route::Tools);
  // Other switches are never the tools'.
  CHECK(router.OnSwitch(47, 1) == Route::Game);

  // No screen: ENTER stays the game's.
  router.SetToolsOpen(false);
  router.SetToolsAvailable(false);
  CHECK(router.OnSwitch(8, 1) == Route::Game);
  CHECK(router.OnSwitch(8, 0) == Route::Game);
}

TEST_CASE("A release goes where its press went, even when the tools close in between")
{
  CoinDoorRouter router;
  router.Configure(CoinDoorButtons::ForPlatform(1), 22);
  router.SetToolsAvailable(true);
  router.SetToolsOpen(true);
  using Route = CoinDoorRouter::Route;
  CHECK(router.OnSwitch(5, 1) == Route::Tools);  // ESCAPE closes them
  router.SetToolsOpen(false);
  CHECK(router.OnSwitch(5, 0) == Route::Tools);
  CHECK(router.OnSwitch(5, 1) == Route::Game);
  CHECK(router.OnSwitch(5, 0) == Route::Game);
}

TEST_CASE("The ROM's service menu keeps the buttons until the door closes")
{
  CoinDoorRouter router;
  router.Configure(CoinDoorButtons::ForPlatform(1), 22);
  router.SetToolsAvailable(true);
  using Route = CoinDoorRouter::Route;
  router.SetPassthrough(true);
  CHECK(router.OnSwitch(8, 1) == Route::Game);
  CHECK(router.OnSwitch(8, 0) == Route::Game);
  CHECK(router.OnSwitch(22, 1) == Route::Game);
  CHECK_FALSE(router.IsPassthrough());
  CHECK(router.OnSwitch(22, 0) == Route::Game);
  CHECK(router.OnSwitch(8, 1) == Route::Tools);
}

TEST_CASE("Without a door switch the door counts as open")
{
  CoinDoorRouter router;
  CoinDoorButtons buttons;
  buttons.numbers = {0, 0, 0, 9};
  router.Configure(buttons, 0);
  router.SetToolsAvailable(true);
  CHECK(router.IsDoorOpen());
  CHECK(router.OnSwitch(9, 1) == CoinDoorRouter::Route::Tools);
}

// ---- Lua -------------------------------------------------------------------

TEST_CASE("Rules define adjustments, read them and keep machine memory")
{
  Adjustments adjustments;
  adjustments.Load(MachineState::Parse("gun_difficulty = \"Hard\"\n"));
  MachineMemory memory;
  SwitchReport report;
  report.SetThresholdGames(1);
  report.Track(5, "Coin");

  ppuc_test::RulesHarness rules(R"(
    difficulty = ppuc.defineAdjustment{name = "gun_difficulty", label = "Gun difficulty",
                                       default = "Normal", choices = {"Easy", "Normal", "Hard"}}
    save = ppuc.defineAdjustment{name = "save", label = "Ball save", default = 6, min = 0, max = 15, unit = "s"}
    broken = ppuc.defineAdjustment{name = "gun_broken", label = "Gun out of order", default = false}
    ppuc.excludeFromSwitchReport(5)
    function ppuc.onAdjustmentChanged(name, value)
      changed = name .. "=" .. tostring(value)
      ppuc.nvSet("last_change", changed)
    end
    function ppuc.onSwitchChanged(number, state)
      if number == 1 and state == 1 then
        ppuc.nvSet("eb_level", (ppuc.nvGet("eb_level") or 2) + 1)
      end
    end
  )");
  rules.engine().SetAdjustments(&adjustments);
  rules.engine().SetMachineMemory(&memory);
  rules.engine().SetSwitchReport(&report);
  REQUIRE(rules.Load() == "");

  CHECK(adjustments.Get("gun_difficulty") == StateValue::String("Hard"));
  CHECK(adjustments.Get("save") == StateValue::Number(6));
  CHECK(report.IsIgnored(5));
  REQUIRE(adjustments.List().size() == 3);
  CHECK(adjustments.List()[0].definition.group == "Game");
  CHECK(adjustments.Display("save") == "6 s");

  rules.engine().ProcessSwitchState(1, 1);
  rules.engine().ProcessSwitchState(1, 0);
  rules.engine().ProcessSwitchState(1, 1);
  StateValue value;
  REQUIRE(memory.Get("eb_level", &value));
  CHECK(value == StateValue::Number(4));

  rules.engine().CallGameHandler("onAdjustmentChanged", {"save", 8});
  REQUIRE(memory.Get("last_change", &value));
  CHECK(value == StateValue::String("save=8"));
}

TEST_CASE("A bad adjustment definition names itself")
{
  ppuc_test::RulesHarness rules(R"(
    ppuc.defineAdjustment{name = "x", default = "Medium", choices = {"Low", "High"}}
  )");
  const std::string error = rules.Load();
  CHECK(error.find("default must be one of the choices") != std::string::npos);
}

TEST_CASE("Without a store, adjustments read as their default and memory lasts the session")
{
  ppuc_test::RulesHarness rules(R"(
    v = ppuc.defineAdjustment{name = "level", default = 2, min = 1, max = 5}
    assert(ppuc.adjustment("level") == 2)
    assert(ppuc.adjustment("nothing") == nil)
    ppuc.nvSet("k", "v")
    assert(ppuc.nvGet("k") == "v")
    ppuc.nvSet("k", nil)
    assert(ppuc.nvGet("k") == nil)
  )");
  CHECK(rules.Load() == "");
}

TEST_CASE("A game's adjustment with a machine adjustment's name is the machine's")
{
  Adjustments adjustments;
  Adjustments::Definition machine;
  machine.name = "balls_per_game";
  machine.label = "Balls per game";
  machine.group = "Machine";
  machine.kind = Adjustments::Kind::Number;
  machine.min = 1;
  machine.max = 10;
  machine.defaultValue = StateValue::Number(3);
  std::string error;
  REQUIRE(adjustments.Define(machine, &error));
  REQUIRE(adjustments.Set("balls_per_game", StateValue::Number(5), nullptr, &error));

  ppuc_test::RulesHarness rules(R"(
    balls = ppuc.defineAdjustment{name = "balls_per_game", label = "Balls", default = 3, min = 1, max = 5}
    assert(balls == 5)
  )");
  rules.engine().SetAdjustments(&adjustments);
  REQUIRE(rules.Load() == "");
  CHECK(adjustments.GroupOf("balls_per_game") == "Machine");
  CHECK(adjustments.List().size() == 1);
}
