// Tests for the bench ball model (--bench).
//
// The model stands in for a machine, so what matters is that it answers each
// coil the way the machine would and keeps count: a ball that the model loses
// or invents makes a game on the desk behave in ways the real one never does,
// and that costs more time than having no bench at all.

#include <map>
#include <vector>

#include "doctest.h"
#include "game/BenchBalls.h"

namespace
{

// Dirty Harry's devices, as its bench: block names them.
bench::Config DirtyHarry()
{
  bench::Config c;
  c.present = true;
  c.balls = 4;
  c.troughSwitches = {32, 33, 34, 35};
  c.troughKickCoil = 1;
  c.shooterSwitch = 15;
  c.startSwitch = 13;
  c.launchCoils = {2, 3};
  c.plungerSwitch = 11;
  c.plungerAfterMs = 2000;
  c.serveMs = 600;
  c.drainSwitches = {26};
  c.holes = {{47, 14, 0}, {45, 5, 44}, {44, 3, 0}};
  return c;
}

// Records what the machine would see: the switch states and every change.
struct Machine
{
  std::map<int, uint8_t> switches;
  std::vector<std::pair<int, uint8_t>> changes;

  bench::Balls::Emit Emit()
  {
    return [this](int sw, uint8_t state)
    {
      switches[sw] = state;
      changes.push_back({sw, state});
    };
  }
  bool Closed(int sw) const
  {
    const auto it = switches.find(sw);
    return it != switches.end() && it->second != 0;
  }
  int TroughClosed() const
  {
    int n = 0;
    for (int sw : {32, 33, 34, 35}) n += Closed(sw) ? 1 : 0;
    return n;
  }
};

void Pulse(bench::Balls& balls, int coil, uint64_t at)
{
  balls.OnCoil(coil, true, at);
  balls.OnCoil(coil, false, at + 30);
}

bench::Step StepOf(bench::Step::Kind kind, std::vector<int> switches = {})
{
  bench::Step step;
  step.kind = kind;
  step.switches = std::move(switches);
  return step;
}

}  // namespace

TEST_CASE("bench: every ball starts in the trough")
{
  bench::Balls balls(DirtyHarry());
  Machine m;
  balls.Begin(0);
  balls.Service(0, m.Emit());
  CHECK(m.TroughClosed() == 4);
  CHECK(balls.InTrough() == 4);
  CHECK(balls.OnPlayfield() == 0);
}

TEST_CASE("bench: a trough kick serves a ball to the shooter lane and the plunger is pulled")
{
  bench::Balls balls(DirtyHarry());
  Machine m;
  balls.Begin(0);
  balls.Service(0, m.Emit());

  Pulse(balls, 1, 1000);
  balls.Service(1100, m.Emit());
  CHECK(m.TroughClosed() == 3);
  CHECK_FALSE(m.Closed(15));  // still rolling

  balls.Service(1600, m.Emit());
  CHECK(m.Closed(15));
  CHECK(balls.InShooterLane());

  // The gun handle trigger, 2 s later, tapped.
  balls.Service(3600, m.Emit());
  CHECK(m.Closed(11));
  balls.Service(3700, m.Emit());
  CHECK_FALSE(m.Closed(11));
}

TEST_CASE("bench: a launch coil only moves a ball that is in the shooter lane")
{
  bench::Balls balls(DirtyHarry());
  Machine m;
  balls.Begin(0);
  Pulse(balls, 2, 100);  // nothing there
  CHECK(balls.OnPlayfield() == 0);

  Pulse(balls, 1, 1000);
  balls.Service(1600, m.Emit());
  Pulse(balls, 2, 1700);
  balls.Service(1800, m.Emit());
  CHECK_FALSE(m.Closed(15));
  CHECK(balls.OnPlayfield() == 1);
  CHECK_FALSE(balls.InShooterLane());
}

TEST_CASE("bench: only the rising edge of a coil counts")
{
  bench::Balls balls(DirtyHarry());
  Machine m;
  balls.Begin(0);
  balls.OnCoil(1, true, 100);
  balls.OnCoil(1, true, 120);  // still on: the same kick
  balls.Service(1000, m.Emit());
  CHECK(balls.InTrough() == 3);
}

TEST_CASE("bench: a second kick while a ball waits in the lane is not a second ball")
{
  bench::Balls balls(DirtyHarry());
  Machine m;
  balls.Begin(0);
  Pulse(balls, 1, 100);
  Pulse(balls, 1, 300);  // still rolling
  balls.Service(1000, m.Emit());
  Pulse(balls, 1, 1100);  // waiting in the lane
  balls.Service(2000, m.Emit());
  CHECK(balls.InTrough() == 3);
  bool warned = false;
  for (const auto& line : balls.TakeLog()) warned = warned || line.find("ignored") != std::string::npos;
  CHECK(warned);
}

TEST_CASE("bench: a hole keeps its ball until its coil fires, and may feed another")
{
  bench::Balls balls(DirtyHarry());
  Machine m;
  balls.Begin(0);
  Pulse(balls, 1, 0);
  balls.Service(600, m.Emit());
  Pulse(balls, 2, 700);
  REQUIRE(balls.OnPlayfield() == 1);

  balls.Do(StepOf(bench::Step::Kind::Hole, {45}), 1000);
  balls.Service(1000, m.Emit());
  CHECK(m.Closed(45));
  CHECK(balls.OnPlayfield() == 0);

  Pulse(balls, 14, 1500);  // another hole's coil
  balls.Service(1600, m.Emit());
  CHECK(m.Closed(45));

  Pulse(balls, 5, 2000);  // the gun popper: into the chamber
  balls.Service(2400, m.Emit());
  CHECK_FALSE(m.Closed(45));
  CHECK(m.Closed(44));
  CHECK(balls.OnPlayfield() == 0);

  Pulse(balls, 3, 3000);  // the gun fires it
  balls.Service(3100, m.Emit());
  CHECK_FALSE(m.Closed(44));
  CHECK(balls.OnPlayfield() == 1);
}

TEST_CASE("bench: a drain rolls over the outlane and closes the next trough switch")
{
  bench::Balls balls(DirtyHarry());
  Machine m;
  balls.Begin(0);
  Pulse(balls, 1, 0);
  balls.Service(600, m.Emit());
  Pulse(balls, 2, 700);
  balls.Service(800, m.Emit());
  REQUIRE(m.TroughClosed() == 3);

  m.changes.clear();
  balls.Do(StepOf(bench::Step::Kind::Drain), 1000);
  balls.Service(2000, m.Emit());
  CHECK(m.TroughClosed() == 4);
  CHECK(balls.InTrough() == 4);
  REQUIRE(m.changes.size() >= 3);
  CHECK(m.changes[0] == std::make_pair(26, uint8_t{1}));  // the outlane first
  CHECK(m.changes.back() == std::make_pair(35, uint8_t{1}));

  // Nothing left to drain: said, not counted.
  balls.Do(StepOf(bench::Step::Kind::Drain), 3000);
  balls.Service(4000, m.Emit());
  CHECK(balls.InTrough() == 4);
}

TEST_CASE("bench: a machine with no shooter lane switch serves onto the playfield")
{
  bench::Config c;
  c.present = true;
  c.balls = 1;
  c.troughSwitches = {11};
  c.troughKickCoil = 1;
  bench::Balls balls(c);
  Machine m;
  balls.Begin(0);
  Pulse(balls, 1, 0);
  balls.Service(1000, m.Emit());
  CHECK(balls.OnPlayfield() == 1);
  CHECK_FALSE(m.Closed(11));
}

TEST_CASE("bench: a manual plunger is a launch step")
{
  bench::Config c = DirtyHarry();
  c.launchCoils.clear();
  c.plungerSwitch = 0;
  bench::Balls balls(c);
  Machine m;
  balls.Begin(0);
  Pulse(balls, 1, 0);
  balls.Service(1000, m.Emit());
  balls.Do(StepOf(bench::Step::Kind::Launch), 1000);
  balls.Service(1000, m.Emit());
  CHECK(balls.OnPlayfield() == 1);
  CHECK_FALSE(m.Closed(15));
}

TEST_CASE("bench: play scripts")
{
  std::vector<bench::Step> steps;
  std::string error;
  REQUIRE(
      bench::ParsePlay("# Car Chase\n"
                       "1000 start\n"
                       "+4500 tap 61   # a sling\n"
                       "+4000 hole 47\n"
                       "\n"
                       "+400 tap 41 43\n"
                       "20000 press 112\n"
                       "+200 release 112\n"
                       "+10 say end of the mode\n"
                       "+3000 drain\n",
                       &steps, &error));
  REQUIRE(steps.size() == 8);
  CHECK(steps[0].kind == bench::Step::Kind::Start);
  CHECK(steps[0].atMs == 1000);
  CHECK(steps[1].atMs == 5500);
  CHECK(steps[1].switches == std::vector<int>{61});
  CHECK(steps[2].kind == bench::Step::Kind::Hole);
  CHECK(steps[3].switches == std::vector<int>{41, 43});
  CHECK(steps[4].atMs == 20000);
  CHECK(steps[6].kind == bench::Step::Kind::Say);
  CHECK(steps[6].text == "end of the mode");
  CHECK(steps[7].atMs == 23210);
  CHECK(steps[7].line == 10);

  CHECK_FALSE(bench::ParsePlay("1000 kick 47\n", &steps, &error));
  CHECK(error.find("line 1") != std::string::npos);
  CHECK_FALSE(bench::ParsePlay("1000 tap\n", &steps, &error));
  CHECK_FALSE(bench::ParsePlay("1000 drain 26\n", &steps, &error));
  CHECK_FALSE(bench::ParsePlay("1000 hole 46 47\n", &steps, &error));
  CHECK_FALSE(bench::ParsePlay("2000 start\n1000 drain\n", &steps, &error));
  CHECK(error.find("back in time") != std::string::npos);
  CHECK_FALSE(bench::ParsePlay("soon start\n", &steps, &error));
}

TEST_CASE("bench: the player runs steps from when it begins")
{
  std::vector<bench::Step> steps;
  std::string error;
  REQUIRE(bench::ParsePlay("1000 start\n+500 tap 61\n", &steps, &error));
  bench::Player player(steps);
  bench::Balls balls(DirtyHarry());
  Machine m;
  balls.Begin(0);

  CHECK_FALSE(player.Service(50000, balls));  // not begun: the engine is not up
  player.Begin(10000);
  CHECK_FALSE(player.Service(10999, balls));
  balls.Service(10999, m.Emit());
  CHECK_FALSE(m.Closed(13));
  CHECK_FALSE(player.Service(11000, balls));
  balls.Service(11000, m.Emit());
  CHECK(m.Closed(13));
  CHECK(player.Service(11500, balls));
  balls.Service(11500, m.Emit());
  CHECK(m.Closed(61));
}
