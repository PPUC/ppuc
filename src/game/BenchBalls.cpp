#include "BenchBalls.h"

#include <algorithm>
#include <cstdlib>
#include <sstream>

namespace bench
{

bool ParsePlay(const std::string& text, std::vector<Step>* pSteps, std::string* pError)
{
  std::vector<Step> steps;
  std::istringstream in(text);
  std::string line;
  int lineNo = 0;
  uint64_t last = 0;
  auto fail = [&](const std::string& message)
  {
    if (pError) *pError = "line " + std::to_string(lineNo) + ": " + message;
    return false;
  };
  while (std::getline(in, line))
  {
    ++lineNo;
    const size_t hash = line.find('#');
    if (hash != std::string::npos) line.erase(hash);
    std::istringstream words(line);
    std::string when;
    if (!(words >> when)) continue;

    Step step;
    step.line = lineNo;
    const bool relative = when[0] == '+';
    const std::string digits = relative ? when.substr(1) : when;
    char* end = nullptr;
    const unsigned long long ms = std::strtoull(digits.c_str(), &end, 10);
    if (digits.empty() || end == nullptr || *end != '\0') return fail("'" + when + "' is not a time in ms");
    step.atMs = relative ? last + ms : ms;
    if (step.atMs < last) return fail("goes back in time");
    last = step.atMs;

    std::string what;
    if (!(words >> what)) return fail("a time and nothing to do");
    if (what == "say")
    {
      step.kind = Step::Kind::Say;
      std::getline(words, step.text);
      step.text.erase(0, step.text.find_first_not_of(' '));
      steps.push_back(step);
      continue;
    }

    std::string number;
    while (words >> number)
    {
      const long sw = std::strtol(number.c_str(), &end, 10);
      if (end == nullptr || *end != '\0' || sw <= 0) return fail("'" + number + "' is not a switch number");
      step.switches.push_back(static_cast<int>(sw));
    }

    bool takesSwitches = true;
    if (what == "start")
      step.kind = Step::Kind::Start, takesSwitches = false;
    else if (what == "tap")
      step.kind = Step::Kind::Tap;
    else if (what == "press")
      step.kind = Step::Kind::Press;
    else if (what == "release")
      step.kind = Step::Kind::Release;
    else if (what == "hole")
      step.kind = Step::Kind::Hole;
    else if (what == "launch")
      step.kind = Step::Kind::Launch, takesSwitches = false;
    else if (what == "drain")
      step.kind = Step::Kind::Drain, takesSwitches = false;
    else
      return fail("'" + what + "' - use start, tap, press, release, hole, launch, drain or say");

    if (takesSwitches && step.switches.empty()) return fail(what + " needs a switch number");
    if (!takesSwitches && !step.switches.empty()) return fail(what + " takes no switch number");
    if (step.kind == Step::Kind::Hole && step.switches.size() != 1) return fail("hole takes one switch");
    steps.push_back(step);
  }
  *pSteps = std::move(steps);
  return true;
}

Balls::Balls(Config config) : m_config(std::move(config))
{
  for (const Hole& hole : m_config.holes) m_holes[hole.sw] = false;
}

bool Balls::InHole(int sw) const
{
  const auto it = m_holes.find(sw);
  return it != m_holes.end() && it->second;
}

std::vector<std::string> Balls::TakeLog()
{
  std::vector<std::string> out;
  out.swap(m_log);
  return out;
}

void Balls::Note(std::string line) { m_log.push_back(std::move(line)); }

void Balls::At(uint64_t atMs, std::function<void(const Emit&)> run)
{
  m_pending.push_back({atMs, m_seq++, std::move(run)});
}

void Balls::Set(uint64_t atMs, int sw, uint8_t state)
{
  if (sw == 0) return;
  At(atMs, [sw, state](const Emit& emit) { emit(sw, state); });
}

void Balls::Tap(uint64_t atMs, int sw)
{
  Set(atMs, sw, 1);
  Set(atMs + m_config.tapMs, sw, 0);
}

// Trough switches fill from the first: with n balls home, the first n are
// closed. What a trough counts is how many, which is all GameCore reads.
static int TroughSwitchAt(const Config& config, int index)
{
  if (index < 0 || index >= static_cast<int>(config.troughSwitches.size())) return 0;
  return config.troughSwitches[static_cast<size_t>(index)];
}

void Balls::Begin(uint64_t nowMs)
{
  m_trough = m_config.balls;
  m_playfield = 0;
  m_inShooter = false;
  m_serving = false;
  for (int i = 0; i < m_trough; ++i) Set(nowMs, TroughSwitchAt(m_config, i), 1);
  Note("bench: " + std::to_string(m_trough) + " ball(s) in the trough");
}

void Balls::OnCoil(int coil, bool on, uint64_t nowMs)
{
  bool& level = m_coils[coil];
  const bool rising = on && !level;
  level = on;
  if (!rising) return;

  if (coil == m_config.troughKickCoil)
  {
    if (m_trough == 0)
    {
      Note("bench: trough kick with no ball in the trough");
    }
    else if (m_serving || m_inShooter)
    {
      // A real trough would stack the second ball behind the first. No game
      // does this on purpose, so it is reported rather than modelled.
      Note("bench: trough kick while a ball is still in the shooter lane; ignored");
    }
    else
    {
      --m_trough;
      Set(nowMs + 30, TroughSwitchAt(m_config, m_trough), 0);
      m_serving = true;
      const uint64_t arrives = nowMs + m_config.serveMs;
      At(arrives,
         [this, arrives](const Emit& emit)
         {
           m_serving = false;
           if (m_config.shooterSwitch == 0)
           {
             ++m_playfield;
             Note("bench: ball served onto the playfield");
             return;
           }
           m_inShooter = true;
           emit(m_config.shooterSwitch, 1);
           Note("bench: ball in the shooter lane");
           if (m_config.plungerSwitch != 0)
           {
             Tap(arrives + m_config.plungerAfterMs, m_config.plungerSwitch);
           }
         });
    }
  }

  if (m_inShooter &&
      std::find(m_config.launchCoils.begin(), m_config.launchCoils.end(), coil) != m_config.launchCoils.end())
  {
    m_inShooter = false;
    ++m_playfield;
    Set(nowMs + 30, m_config.shooterSwitch, 0);
    Note("bench: ball launched");
  }

  for (const Hole& hole : m_config.holes)
  {
    if (hole.coil != coil || !m_holes[hole.sw]) continue;
    m_holes[hole.sw] = false;
    Set(nowMs + 30, hole.sw, 0);
    if (hole.to != 0)
    {
      m_holes[hole.to] = true;
      Set(nowMs + 300, hole.to, 1);
      Note("bench: ball kicked from " + std::to_string(hole.sw) + " into " + std::to_string(hole.to));
    }
    else
    {
      ++m_playfield;
      Note("bench: ball kicked out of " + std::to_string(hole.sw));
    }
  }
}

void Balls::Do(const Step& step, uint64_t nowMs)
{
  switch (step.kind)
  {
    case Step::Kind::Say:
      Note("bench: " + step.text);
      break;
    case Step::Kind::Start:
      if (m_config.startSwitch == 0)
      {
        Note("bench: line " + std::to_string(step.line) + ": no start switch configured");
        break;
      }
      Tap(nowMs, m_config.startSwitch);
      break;
    case Step::Kind::Tap:
      for (int sw : step.switches) Tap(nowMs, sw);
      break;
    case Step::Kind::Press:
      for (int sw : step.switches) Set(nowMs, sw, 1);
      break;
    case Step::Kind::Release:
      for (int sw : step.switches) Set(nowMs, sw, 0);
      break;
    case Step::Kind::Hole:
    {
      const int sw = step.switches.front();
      if (m_holes.find(sw) == m_holes.end())
      {
        // Not a hole the model knows: it would never be kicked out, so the
        // switch is only tapped. Said, because it is almost certainly a typo.
        Note("bench: line " + std::to_string(step.line) + ": " + std::to_string(sw) +
             " is not a hole in bench.holes; tapped instead");
        Tap(nowMs, sw);
        break;
      }
      if (m_playfield == 0)
      {
        Note("bench: line " + std::to_string(step.line) + ": no ball on the playfield to go into " +
             std::to_string(sw));
        break;
      }
      if (m_holes[sw])
      {
        Note("bench: line " + std::to_string(step.line) + ": " + std::to_string(sw) + " already holds a ball");
        break;
      }
      --m_playfield;
      m_holes[sw] = true;
      Set(nowMs, sw, 1);
      break;
    }
    case Step::Kind::Launch:
      if (!m_inShooter)
      {
        Note("bench: line " + std::to_string(step.line) + ": no ball in the shooter lane to launch");
        break;
      }
      m_inShooter = false;
      ++m_playfield;
      Set(nowMs, m_config.shooterSwitch, 0);
      Note("bench: ball launched");
      break;
    case Step::Kind::Drain:
    {
      if (m_playfield == 0)
      {
        Note("bench: line " + std::to_string(step.line) + ": no ball on the playfield to drain");
        break;
      }
      --m_playfield;
      uint64_t at = nowMs;
      for (int sw : m_config.drainSwitches)
      {
        Tap(at, sw);
        at += 150;
      }
      At(at + 300,
         [this](const Emit& emit)
         {
           const int sw = TroughSwitchAt(m_config, m_trough);
           ++m_trough;
           if (sw != 0) emit(sw, 1);
           Note("bench: ball drained, " + std::to_string(m_trough) + " in the trough");
         });
      break;
    }
  }
}

void Balls::Service(uint64_t nowMs, const Emit& emit)
{
  // Due ones run in order; one may schedule more, which wait for the next pass
  // unless they are due already.
  for (;;)
  {
    auto due = m_pending.end();
    for (auto it = m_pending.begin(); it != m_pending.end(); ++it)
    {
      if (it->atMs > nowMs) continue;
      if (due == m_pending.end() || it->atMs < due->atMs || (it->atMs == due->atMs && it->seq < due->seq)) due = it;
    }
    if (due == m_pending.end()) return;
    auto run = std::move(due->run);
    m_pending.erase(due);
    run(emit);
  }
}

bool Player::Service(uint64_t nowMs, Balls& balls)
{
  if (!m_started) return false;
  while (m_next < m_steps.size() && m_startMs + m_steps[m_next].atMs <= nowMs)
  {
    balls.Do(m_steps[m_next], m_startMs + m_steps[m_next].atMs);
    ++m_next;
  }
  return Finished();
}

}  // namespace bench
