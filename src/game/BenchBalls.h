#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "GameConfig.h"

// The balls of a machine that is not there.
//
// On a desk, with no boards, nothing moves when a coil fires: the trough never
// releases, a hole never kicks out, and a game sits waiting for a ball that will
// never reach the shooter lane. `--bench` puts a small ball model in the boards'
// place. It watches the coils PPUC drives and answers with the switches a real
// machine would close and open: a trough kick takes a ball out of the trough and
// rolls it into the shooter lane, a launch coil sends it onto the playfield, a
// hole's coil kicks it out again.
//
// What a ball does on the playfield is the player's business, and the player
// is a script (`--bench-play`): taps, a ball into a hole, a drain. So a whole
// game runs in the real binary, rules, GameCore and displays included, with
// nothing attached.
//
// No SDL and no threads in here: the switch thread owns it and gives it the
// time.

namespace bench
{

// A place a ball stops and waits: a popper, a scoop, a saucer. Its switch stays
// closed while a ball is in it, until `coil` fires. Then the ball goes to the
// playfield, or into the hole `to` (a popper that feeds a gun chamber).
struct Hole
{
  int sw = 0;
  int coil = 0;
  int to = 0;
};

struct Config
{
  bool present = false;  // a `bench:` block, or enough from `emGame:`

  int balls = 1;
  std::vector<int> troughSwitches;
  int troughKickCoil = 0;
  int shooterSwitch = 0;  // 0: a served ball goes straight onto the playfield
  int startSwitch = 0;    // for `start` in a play script

  std::vector<int> launchCoils;  // shooter lane -> playfield
  int plungerSwitch = 0;         // what the player pulls once a ball waits
  uint32_t plungerAfterMs = 2500;

  uint32_t serveMs = 600;          // trough -> shooter lane
  uint32_t tapMs = 80;             // how long a tapped switch stays closed
  std::vector<int> drainSwitches;  // rolled over on the way to the trough

  std::vector<Hole> holes;
};

// One line of a play script.
struct Step
{
  enum class Kind
  {
    Start,    // tap the start switch
    Tap,      // close the switches for tapMs
    Press,    // close and keep closed (a flipper button held)
    Release,  // open again
    Hole,     // a ball from the playfield drops into a hole
    Launch,   // a manual plunger: shooter lane -> playfield
    Drain,    // a ball from the playfield goes down the drain
    Say,      // print a line, to find one's place in a log
  };
  uint64_t atMs = 0;
  Kind kind = Kind::Tap;
  std::vector<int> switches;
  std::string text;
  int line = 0;
};

// A play script: one step per line, `<ms> <what> [switches]`, the time either
// from the start of the play or `+ms` after the line before. # comments.
//
//   1000  start
//   +4500 tap 61
//   +4000 hole 47
//   +4000 tap 41 43
//   +3000 drain
bool ParsePlay(const std::string& text, std::vector<Step>* pSteps, std::string* pError);

class Balls
{
 public:
  using Emit = std::function<void(int sw, uint8_t state)>;

  explicit Balls(Config config);

  // Every ball in the trough, switches closed: the machine as it is at power on.
  void Begin(uint64_t nowMs);

  // A coil PPUC drives. Only an off-to-on edge moves a ball.
  void OnCoil(int coil, bool on, uint64_t nowMs);

  // What the player does.
  void Do(const Step& step, uint64_t nowMs);

  // The switch changes that are due, oldest first.
  void Service(uint64_t nowMs, const Emit& emit);

  int InTrough() const { return m_trough; }
  int OnPlayfield() const { return m_playfield; }
  bool InShooterLane() const { return m_inShooter; }
  bool InHole(int sw) const;

  // What happened, a line each, for the log.
  std::vector<std::string> TakeLog();

 private:
  struct Pending
  {
    uint64_t atMs;
    uint64_t seq;
    std::function<void(const Emit&)> run;
  };

  void At(uint64_t atMs, std::function<void(const Emit&)> run);
  void Set(uint64_t atMs, int sw, uint8_t state);
  void Tap(uint64_t atMs, int sw);
  void Note(std::string line);

  Config m_config;
  int m_trough = 0;
  int m_playfield = 0;
  bool m_inShooter = false;
  bool m_serving = false;
  std::map<int, bool> m_holes;  // switch -> a ball is in it
  std::map<int, bool> m_coils;  // last level seen
  std::vector<Pending> m_pending;
  uint64_t m_seq = 0;
  std::vector<std::string> m_log;
};

// Plays a script on the balls, from the moment Begin() is called.
class Player
{
 public:
  explicit Player(std::vector<Step> steps) : m_steps(std::move(steps)) {}

  void Begin(uint64_t nowMs)
  {
    m_startMs = nowMs;
    m_started = true;
  }
  bool Started() const { return m_started; }
  // Runs the steps that are due. True once the last one has run.
  bool Service(uint64_t nowMs, Balls& balls);
  bool Finished() const { return m_next >= m_steps.size(); }

 private:
  std::vector<Step> m_steps;
  size_t m_next = 0;
  uint64_t m_startMs = 0;
  bool m_started = false;
};

}  // namespace bench
