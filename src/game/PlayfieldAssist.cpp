#include "PlayfieldAssist.h"

#include <algorithm>
#include <chrono>

namespace
{
uint64_t SteadyNowMs()
{
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}
}  // namespace

const char* BallSaveStartName(BallSaveStart start)
{
  switch (start)
  {
    case BallSaveStart::TroughExit: return "troughExit";
    case BallSaveStart::ShooterLane: return "shooterLane";
    case BallSaveStart::FirstPlayfieldSwitch: return "firstPlayfieldSwitch";
  }
  return "unknown";
}

bool ParseBallSaveStart(const std::string& text, BallSaveStart* pStart)
{
  if (pStart == nullptr) return false;
  if (text == "troughExit") { *pStart = BallSaveStart::TroughExit; return true; }
  if (text == "shooterLane") { *pStart = BallSaveStart::ShooterLane; return true; }
  if (text == "firstPlayfieldSwitch") { *pStart = BallSaveStart::FirstPlayfieldSwitch; return true; }
  return false;
}

PlayfieldAssist::PlayfieldAssist() : m_clock(SteadyNowMs) {}

void PlayfieldAssist::SetTiltConfig(const TiltAssistConfig& config) { m_tilt = config; }

void PlayfieldAssist::SetBallSaveConfig(const BallSaveConfig& config)
{
  m_ballSave = config;

  // A machine with no lane switch cannot use the lane trigger. Fall back rather
  // than silently never arming the save, which would look exactly like the
  // feature being broken.
  if (m_ballSave.startOn == BallSaveStart::ShooterLane && m_ballSave.shooterLaneSwitch == 0)
  {
    m_ballSave.startOn = BallSaveStart::TroughExit;
  }
  if (m_ballSave.startOn == BallSaveStart::FirstPlayfieldSwitch && m_ballSave.playfieldSwitches.empty())
  {
    m_ballSave.startOn = BallSaveStart::TroughExit;
  }
}

void PlayfieldAssist::SetClock(ClockFn clock) { m_clock = clock ? clock : ClockFn(SteadyNowMs); }

uint64_t PlayfieldAssist::Now() const { return m_clock(); }

void PlayfieldAssist::Emit(Event event) { m_events.push_back(event); }

void PlayfieldAssist::Act(Action action) { m_actions.push_back(action); }

std::vector<PlayfieldAssist::Event> PlayfieldAssist::TakeEvents() { return std::move(m_events); }

std::vector<PlayfieldAssist::Action> PlayfieldAssist::TakeActions() { return std::move(m_actions); }

PlayfieldAssist::PlayerWarnings& PlayfieldAssist::Warnings(uint8_t player) { return m_warnings[player]; }

void PlayfieldAssist::SetCurrentPlayer(uint8_t player) { m_currentPlayer = player; }

void PlayfieldAssist::SetPlayActive(bool active)
{
  if (!active)
  {
    CancelBallSave();
    m_ballSavePending = false;
  }
  m_playActive = active;
  if (active)
  {
    m_tilted = false;
  }
}

void PlayfieldAssist::OnBallStart(uint8_t ball)
{
  m_currentBall = ball;
  m_playActive = true;
  m_tilted = false;
  m_savesThisBall = 0;
  m_lastTiltHitMs = 0;
  m_tiltBlankedUntilMs = 0;

  // The base allowance resets each ball; awarded warnings do not. That is what
  // makes an award worth chasing rather than a one-ball curiosity.
  Warnings(m_currentPlayer).used = 0;

  CancelBallSave();
  m_ballSavePending = m_ballSave.enabled && BallSaveAllowedThisBall();

  if (m_ballSavePending && m_ballSave.startOn == BallSaveStart::TroughExit)
  {
    // Nothing to wait for beyond the serve itself; the trough-exit edge arms it.
  }
}

void PlayfieldAssist::OnBallEnd()
{
  m_playActive = false;
  m_ballSavePending = false;
  CancelBallSave();
}

bool PlayfieldAssist::BallSaveAllowedThisBall() const
{
  if (m_ballSave.onlyOnBalls.empty())
  {
    return true;
  }
  return std::find(m_ballSave.onlyOnBalls.begin(), m_ballSave.onlyOnBalls.end(),
                   static_cast<int>(m_currentBall)) != m_ballSave.onlyOnBalls.end();
}

void PlayfieldAssist::AwardTiltWarnings(int count)
{
  if (count <= 0 || m_currentPlayer == 0)
  {
    return;
  }
  PlayerWarnings& warnings = Warnings(m_currentPlayer);
  const int updated = static_cast<int>(warnings.awarded) + count;
  warnings.awarded = static_cast<uint8_t>(std::clamp(updated, 0, 255));
  Emit({EventType::TiltWarningsAwarded, m_currentPlayer, count, GetWarningsRemaining(m_currentPlayer)});
}

uint8_t PlayfieldAssist::GetWarningsUsed(uint8_t player) const
{
  const auto it = m_warnings.find(player == 0 ? m_currentPlayer : player);
  return it == m_warnings.end() ? 0 : it->second.used;
}

uint8_t PlayfieldAssist::GetWarningsAllowed(uint8_t player) const
{
  const auto it = m_warnings.find(player == 0 ? m_currentPlayer : player);
  const uint8_t awarded = it == m_warnings.end() ? 0 : it->second.awarded;
  const int total = static_cast<int>(m_tilt.warnings) + awarded;
  return static_cast<uint8_t>(std::clamp(total, 0, 255));
}

uint8_t PlayfieldAssist::GetWarningsRemaining(uint8_t player) const
{
  const int remaining = static_cast<int>(GetWarningsAllowed(player)) - GetWarningsUsed(player);
  return static_cast<uint8_t>(std::max(0, remaining));
}

void PlayfieldAssist::GrantBallSave(uint32_t durationMs)
{
  if (durationMs == 0)
  {
    return;
  }
  ArmBallSave(durationMs);
}

void PlayfieldAssist::ArmBallSave(uint32_t durationMs)
{
  const uint64_t until = Now() + durationMs;
  // Extends rather than truncates: a rule granting three seconds during an
  // eight-second save should never shorten it.
  if (until <= m_ballSaveUntilMs)
  {
    return;
  }

  const bool wasActive = m_ballSaveUntilMs != 0;
  m_ballSaveUntilMs = until;
  m_ballSavePending = false;

  if (!wasActive)
  {
    if (m_ballSave.lamp != 0)
    {
      Act({ActionType::SetLamp, m_ballSave.lamp, 1, 0});
    }
    Emit({EventType::BallSaveArmed, m_currentPlayer, static_cast<int64_t>(durationMs), 0});
  }
}

void PlayfieldAssist::CancelBallSave()
{
  if (m_ballSaveUntilMs == 0)
  {
    return;
  }
  m_ballSaveUntilMs = 0;
  if (m_ballSave.lamp != 0)
  {
    Act({ActionType::SetLamp, m_ballSave.lamp, 0, 0});
  }
}

uint32_t PlayfieldAssist::GetBallSaveRemainingMs() const
{
  if (m_ballSaveUntilMs == 0)
  {
    return 0;
  }
  const uint64_t now = Now();
  return now >= m_ballSaveUntilMs ? 0 : static_cast<uint32_t>(m_ballSaveUntilMs - now);
}

bool PlayfieldAssist::IsTiltSwitch(int number) const
{
  return std::find(m_tilt.switches.begin(), m_tilt.switches.end(), number) != m_tilt.switches.end();
}

bool PlayfieldAssist::IsSlamSwitch(int number) const
{
  return std::find(m_tilt.slamSwitches.begin(), m_tilt.slamSwitches.end(), number) != m_tilt.slamSwitches.end();
}

bool PlayfieldAssist::IsDrainSwitch(int number) const
{
  return std::find(m_ballSave.drainSwitches.begin(), m_ballSave.drainSwitches.end(), number) !=
         m_ballSave.drainSwitches.end();
}

bool PlayfieldAssist::IsPlayfieldSwitch(int number) const
{
  return std::find(m_ballSave.playfieldSwitches.begin(), m_ballSave.playfieldSwitches.end(), number) !=
         m_ballSave.playfieldSwitches.end();
}

PlayfieldAssist::SwitchDecision PlayfieldAssist::ProcessSwitch(int number, uint8_t state)
{
  SwitchDecision decision;
  const uint64_t now = Now();

  // ---- Slam tilt: never warned, never blanked, always forwarded ----
  if (IsSlamSwitch(number) && state != 0)
  {
    m_tilted = true;
    CancelBallSave();
    Emit({EventType::SlamTilt, m_currentPlayer, 0, 0});
    return decision;
  }

  // ---- Tilt bob ----
  if (IsTiltSwitch(number) && state != 0)
  {
    if (!m_playActive || m_tilted)
    {
      // Outside play, or already tilted: the engine has no business seeing it.
      decision.forwardToEngine = false;
      return decision;
    }

    // Blanked after a warning so a still-swinging plumb cannot burn the rest.
    if (now < m_tiltBlankedUntilMs)
    {
      decision.forwardToEngine = false;
      return decision;
    }

    // One shove swings the bob through the ring repeatedly; that is one hit.
    if (m_lastTiltHitMs != 0 && now - m_lastTiltHitMs < m_tilt.debounceMs)
    {
      decision.forwardToEngine = false;
      return decision;
    }
    m_lastTiltHitMs = now;

    PlayerWarnings& warnings = Warnings(m_currentPlayer);
    if (warnings.used < GetWarningsAllowed(m_currentPlayer))
    {
      ++warnings.used;
      m_tiltBlankedUntilMs = now + m_tilt.warningBlankingMs;

      if (m_tilt.warningLamp != 0)
      {
        Act({ActionType::SetLamp, m_tilt.warningLamp, 1, 0});
        m_warningLampOn = true;
        m_warningLampUntilMs = now + m_tilt.warningLampMs;
      }

      Emit({EventType::TiltWarning, m_currentPlayer, warnings.used, GetWarningsRemaining(m_currentPlayer)});
      // The whole point: a warning is not a tilt, so the engine never sees it.
      decision.forwardToEngine = false;
      return decision;
    }

    // The allowance is spent. This hit is the tilt, and it IS forwarded, so a
    // ROM with its own tilt handling does the right thing without knowing the
    // host counted warnings for it.
    m_tilted = true;
    CancelBallSave();
    Emit({EventType::Tilt, m_currentPlayer, warnings.used, 0});
    return decision;
  }

  if (!m_playActive)
  {
    return decision;
  }

  // ---- Ball save arming ----
  if (m_ballSavePending && state != 0)
  {
    if (m_ballSave.startOn == BallSaveStart::ShooterLane && number == m_ballSave.shooterLaneSwitch)
    {
      // Closing the lane switch is the ball arriving; the save starts when it
      // leaves, which is handled on the opening edge below.
    }
    else if (m_ballSave.startOn == BallSaveStart::FirstPlayfieldSwitch && IsPlayfieldSwitch(number))
    {
      ArmBallSave(m_ballSave.durationMs);
    }
  }
  if (m_ballSavePending && state == 0 && m_ballSave.startOn == BallSaveStart::ShooterLane &&
      number == m_ballSave.shooterLaneSwitch)
  {
    ArmBallSave(m_ballSave.durationMs);
  }
  if (m_ballSavePending && state == 0 && m_ballSave.startOn == BallSaveStart::TroughExit && IsDrainSwitch(number))
  {
    ArmBallSave(m_ballSave.durationMs);
  }

  // ---- Ball save firing ----
  if (state != 0 && IsDrainSwitch(number) && IsBallSaveActive() && !m_tilted)
  {
    if (m_ballSave.maxSavesPerBall != 0 && m_savesThisBall >= m_ballSave.maxSavesPerBall)
    {
      return decision;
    }
    ++m_savesThisBall;

    if (m_ballSave.kickCoil != 0)
    {
      Act({ActionType::PulseCoil, m_ballSave.kickCoil, 1, m_ballSave.kickPulseMs});
    }
    Emit({EventType::BallSaved, m_currentPlayer, m_savesThisBall, GetBallSaveRemainingMs()});

    if (m_ballSave.maxSavesPerBall != 0 && m_savesThisBall >= m_ballSave.maxSavesPerBall)
    {
      CancelBallSave();
    }

    // The engine must never learn the ball came home, in either mode: GameCore
    // would end the ball, and a ROM would move to the next one.
    decision.forwardToEngine = false;
    return decision;
  }

  return decision;
}

void PlayfieldAssist::Update()
{
  const uint64_t now = Now();

  if (m_ballSaveUntilMs != 0 && now >= m_ballSaveUntilMs)
  {
    m_ballSaveUntilMs = 0;
    if (m_ballSave.lamp != 0)
    {
      Act({ActionType::SetLamp, m_ballSave.lamp, 0, 0});
    }
    Emit({EventType::BallSaveExpired, m_currentPlayer, 0, 0});
  }

  if (m_warningLampOn && now >= m_warningLampUntilMs)
  {
    m_warningLampOn = false;
    Act({ActionType::SetLamp, m_tilt.warningLamp, 0, 0});
  }
}
