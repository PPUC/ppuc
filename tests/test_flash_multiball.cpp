// The Flash multiball rule, driven end to end.
//
// This loads the rule the machine actually runs -- ppuc_games/flash/rules --
// rather than a copy, because the thing worth testing is that file. It is the
// only rule in the stack that a ROM depends on for ordinary play: once the
// trough has two switches, the outhole the ROM sees exists only because this
// rule synthesises it, and a mistake here is a machine that never ends a ball.
//
// Skipped, not failed, when the game folder is not checked out beside ppuc.

#include <filesystem>
#include <fstream>
#include <string>

#include "RulesFixture.h"
#include "doctest.h"

namespace {

using ppuc_test::RulesHarness;

std::string RulePath() {
  // __FILE__ is absolute here, so the game folder is found without depending
  // on where the tests are run from.
  const std::filesystem::path self(__FILE__);
  return (self.parent_path().parent_path().parent_path() / "ppuc_games" /
          "flash" / "rules" / "0400-multiball.lua")
      .string();
}

std::string ReadRule() {
  std::ifstream in(RulePath());
  if (!in.is_open()) {
    return {};
  }
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

// The switches and coils the rule is written against.
constexpr int kThreeBankSeries = 31;
constexpr int kFiveBankSeries = 37;
constexpr int kTroughRest = 210;
constexpr int kTroughSecond = 211;
constexpr int kOutholeToRom = 48;
constexpr int kCoilBallRelease = 1;
constexpr int kCoilEjectHole = 5;
constexpr int kJetBumper = 18;
constexpr int kFlipperButton = 201;

// Finds the last SendSwitchToCpu for a switch, or -1 if there was none.
int LastSwitchSent(const RulesHarness& harness, int number) {
  int value = -1;
  for (const auto& action : harness.actions) {
    if (action.type == RulesActionType::SendSwitchToCpu &&
        action.number == number) {
      value = action.state;
    }
  }
  return value;
}

bool SawCoilPulse(const RulesHarness& harness, int number) {
  for (const auto& action : harness.actions) {
    if (action.type == RulesActionType::PulseCoil && action.number == number) {
      return true;
    }
  }
  return false;
}

int LastBallSearchHold(const RulesHarness& harness) {
  int value = -1;
  for (const auto& action : harness.actions) {
    if (action.type == RulesActionType::HoldBallSearch) {
      value = action.state;
    }
  }
  return value;
}

// Arms the rule the way a player does: both banks down during one ball.
// The engine starts in attract mode and the rule refuses to arm there, which
// is correct -- a game has to be in progress.
void ClearBothBanks(RulesHarness& harness) {
  harness.engine().SetAttractMode(false);
  harness.engine().ProcessSwitchState(kThreeBankSeries, 1);
  harness.engine().ProcessSwitchState(kFiveBankSeries, 1);
}

}  // namespace

TEST_CASE("Flash multiball rule") {
  const std::string script = ReadRule();
  if (script.empty()) {
    MESSAGE("skipped: no ppuc_games/flash beside ppuc (" << RulePath() << ')');
    return;
  }

  SUBCASE("the outhole the ROM sees is both trough switches, confirmed") {
    // Deliberately left in attract mode: balls rest in the trough between
    // games, and the ROM has to be told about them then too.
    RulesHarness harness(script);
    harness.LoadOrFail();
    harness.engine().SetButtonSwitches({kFlipperButton});

    // One ball resting in the trough while another is in play: the ROM must
    // not hear about it, or it ends a ball that is still being played.
    harness.engine().ProcessSwitchState(kTroughRest, 1);
    harness.AdvanceAndUpdate(500);
    CHECK(LastSwitchSent(harness, kOutholeToRom) == 0);

    // Both occupied is a drain -- but only once it is still true a moment
    // later, because a ball rolling over the second switch on its way to the
    // first must not read as two balls home.
    harness.engine().ProcessSwitchState(kTroughSecond, 1);
    CHECK(LastSwitchSent(harness, kOutholeToRom) == 0);
    harness.AdvanceAndUpdate(200);
    CHECK(LastSwitchSent(harness, kOutholeToRom) == 1);
  }

  SUBCASE("a ball rolling through the trough never reports the outhole") {
    // The multiball case: the trough is empty, one ball drains, and it touches
    // the second switch on its way to the resting one. If that ever reported a
    // drain, the ROM would end a ball while one is still in play.
    RulesHarness harness(script);
    harness.LoadOrFail();

    harness.engine().ProcessSwitchState(kTroughSecond, 1);
    harness.AdvanceAndUpdate(50);
    harness.engine().ProcessSwitchState(kTroughSecond, 0);
    harness.engine().ProcessSwitchState(kTroughRest, 1);
    harness.AdvanceAndUpdate(500);

    CHECK(LastSwitchSent(harness, kOutholeToRom) != 1);
  }

  SUBCASE("both banks in one ball arm it, once") {
    RulesHarness harness(script);
    harness.LoadOrFail();
    harness.engine().SetAttractMode(false);

    harness.engine().ProcessSwitchState(kThreeBankSeries, 1);
    CHECK(harness.speech.empty());

    harness.engine().ProcessSwitchState(kFiveBankSeries, 1);
    REQUIRE(harness.speech.size() == 1);
    CHECK(harness.speech[0] == "Multiball ready");

    // Clearing a bank again is not a second multiball.
    harness.engine().ProcessSwitchState(kThreeBankSeries, 0);
    harness.engine().ProcessSwitchState(kThreeBankSeries, 1);
    CHECK(harness.speech.size() == 1);
  }

  SUBCASE("draining without starting it loses the chance until the next ball") {
    RulesHarness harness(script);
    harness.LoadOrFail();
    ClearBothBanks(harness);
    harness.engine().SetCurrentBall(2);

    // The eject hole now belongs to the ROM again.
    CHECK(harness.engine().ProcessCoilState(kCoilEjectHole, 1).forwardToBoard);
    CHECK_FALSE(SawCoilPulse(harness, kCoilBallRelease));
  }

  SUBCASE("the eject hole starts it: the ROM's coil is taken over") {
    RulesHarness harness(script);
    harness.LoadOrFail();
    ClearBothBanks(harness);

    // The ROM fires the eject hole the instant the ball lands in it.
    CHECK_FALSE(
        harness.engine().ProcessCoilState(kCoilEjectHole, 1).forwardToBoard);
    // ...and the ball's neighbour goes to the shooter lane instead.
    CHECK(SawCoilPulse(harness, kCoilBallRelease));
    CHECK(LastBallSearchHold(harness) == 1);

    bool spoken = false;
    for (const auto& text : harness.speech) {
      spoken = spoken || text == "Multiball";
    }
    CHECK(spoken);

    // The ball is still in the hole, so the ROM asks again. It stays ours.
    CHECK_FALSE(
        harness.engine().ProcessCoilState(kCoilEjectHole, 1).forwardToBoard);
  }

  SUBCASE("a flipper button is not a plunge; a playfield switch is") {
    RulesHarness harness(script);
    harness.LoadOrFail();
    harness.engine().SetButtonSwitches({kFlipperButton});
    ClearBothBanks(harness);
    harness.engine().ProcessCoilState(kCoilEjectHole, 1);

    harness.engine().ProcessSwitchState(kFlipperButton, 1);
    CHECK_FALSE(SawCoilPulse(harness, kCoilEjectHole));
    CHECK(LastBallSearchHold(harness) == 1);

    harness.engine().ProcessSwitchState(kJetBumper, 1);
    CHECK(SawCoilPulse(harness, kCoilEjectHole));
    CHECK(LastBallSearchHold(harness) == 0);
  }

  SUBCASE("plunging straight into the trough counts as plunging") {
    RulesHarness harness(script);
    harness.LoadOrFail();
    ClearBothBanks(harness);
    harness.engine().ProcessCoilState(kCoilEjectHole, 1);

    harness.engine().ProcessSwitchState(kTroughRest, 1);
    CHECK(SawCoilPulse(harness, kCoilEjectHole));
    CHECK(LastBallSearchHold(harness) == 0);
  }

  SUBCASE("multiball running leaves the eject hole to the ROM") {
    RulesHarness harness(script);
    harness.LoadOrFail();
    ClearBothBanks(harness);
    harness.engine().ProcessCoilState(kCoilEjectHole, 1);
    harness.engine().ProcessSwitchState(kJetBumper, 1);

    // Two balls in play and no interception left: the ROM runs the machine.
    CHECK(harness.engine().ProcessCoilState(kCoilEjectHole, 1).forwardToBoard);
  }

  SUBCASE("a ball change releases the ball search hold") {
    RulesHarness harness(script);
    harness.LoadOrFail();
    ClearBothBanks(harness);
    harness.engine().ProcessCoilState(kCoilEjectHole, 1);
    REQUIRE(LastBallSearchHold(harness) == 1);

    // However the ball ended, the machine must not be left with its search off.
    harness.engine().SetCurrentBall(3);
    CHECK(LastBallSearchHold(harness) == 0);
  }
}
