#include "GameCore.h"

#include <algorithm>
#include <chrono>

namespace
{
// A transition may cause another transition in the same tick (a drain settles,
// the ball ends, the bonus is skipped, the next ball starts). The budget stops
// a rules bug that ping-pongs states from hanging the main loop; it degrades to
// "one more tick" rather than freezing the machine.
constexpr int kMaxTransitionsPerUpdate = 16;
constexpr int kMaxEdgesPerUpdate = 64;

uint64_t SteadyNowMs()
{
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

uint64_t ScoreCeiling(uint8_t digits)
{
  uint64_t ceiling = 1;
  for (uint8_t i = 0; i < digits; ++i)
  {
    ceiling *= 10;
  }
  return ceiling;
}
}  // namespace

const char* StartRejectReasonName(StartRejectReason reason)
{
  switch (reason)
  {
    case StartRejectReason::NoCredits: return "noCredits";
    case StartRejectReason::NotEnoughBalls: return "notEnoughBalls";
    case StartRejectReason::TooLate: return "tooLate";
    case StartRejectReason::TooManyPlayers: return "tooManyPlayers";
    case StartRejectReason::Tilted: return "tilted";
    case StartRejectReason::ScriptVetoed: return "scriptVetoed";
    case StartRejectReason::AlreadyStarting: return "alreadyStarting";
  }
  return "unknown";
}

const char* GameStateName(GameState state)
{
  switch (state)
  {
    case GameState::Attract: return "attract";
    case GameState::GameStarting: return "gameStarting";
    case GameState::BallStarting: return "ballStarting";
    case GameState::BallInLane: return "ballInLane";
    case GameState::BallInPlay: return "inPlay";
    case GameState::BallDraining: return "ballDraining";
    case GameState::BallEnding: return "ballEnding";
    case GameState::BonusCount: return "bonus";
    case GameState::GameEnding: return "gameEnding";
    case GameState::GameOver: return "gameOver";
  }
  return "unknown";
}

GameCore::GameCore() : m_clock(SteadyNowMs)
{
  m_random = [](uint32_t bound) -> uint32_t
  {
    if (bound == 0) return 0;
    // Deterministic by default. Match digits are cosmetic, and a machine that
    // shows the same digits every game is far less confusing than one whose
    // tests are flaky. ScriptEngine installs a real source at startup.
    static uint32_t seed = 0x2545F491u;
    seed = seed * 1664525u + 1013904223u;
    return (seed >> 16) % bound;
  };
}

void GameCore::SetConfig(const GameConfig& config) { m_config = config; }

void GameCore::SetClock(ClockFn clock) { m_clock = clock ? clock : ClockFn(SteadyNowMs); }

void GameCore::SetRandom(RandomFn random)
{
  if (random) m_random = random;
}

void GameCore::SetAllowCallback(AllowFn allow) { m_allow = std::move(allow); }

uint64_t GameCore::Now() const { return m_clock(); }

void GameCore::Emit(GameEvent event) { m_events.push_back(event); }

void GameCore::Act(GameAction action) { m_actions.push_back(action); }

void GameCore::PulseCoil(int number, uint32_t durationMs)
{
  if (number <= 0) return;
  Act({GameActionType::PulseCoil, number, 1, durationMs});
}

void GameCore::SetLamp(int number, uint8_t state)
{
  if (number <= 0) return;
  Act({GameActionType::SetLamp, number, state, 0});
}

void GameCore::SetGi(uint8_t level)
{
  for (int string : m_config.giStrings)
  {
    Act({GameActionType::SetGi, string, level, 0});
  }
}

bool GameCore::Allow(const char* what, int arg) { return m_allow ? m_allow(what, arg) : true; }

std::vector<GameEvent> GameCore::TakeEvents() { return std::move(m_events); }

std::vector<GameAction> GameCore::TakeActions() { return std::move(m_actions); }

GameCore::PlayerRecord& GameCore::CurrentPlayer()
{
  static PlayerRecord dummy;
  if (m_currentPlayer == 0 || m_currentPlayer > m_players.size())
  {
    dummy = PlayerRecord{};
    return dummy;
  }
  return m_players[m_currentPlayer - 1];
}

const GameCore::PlayerRecord* GameCore::GetPlayer(uint8_t player) const
{
  const uint8_t index = player == 0 ? m_currentPlayer : player;
  if (index == 0 || index > m_players.size()) return nullptr;
  return &m_players[index - 1];
}

bool GameCore::IsInGame() const
{
  return m_state != GameState::Attract && m_state != GameState::GameOver;
}

uint8_t GameCore::GetCurrentBall() const
{
  const PlayerRecord* player = GetPlayer(0);
  return player ? player->ball : 0;
}

uint64_t GameCore::GetScore(uint8_t player) const
{
  const PlayerRecord* record = GetPlayer(player);
  return record ? record->score : 0;
}

uint8_t GameCore::GetHighPlayer() const
{
  uint8_t best = 0;
  uint64_t bestScore = 0;
  for (uint8_t i = 0; i < m_playerCount; ++i)
  {
    if (best == 0 || m_players[i].score > bestScore)
    {
      best = static_cast<uint8_t>(i + 1);
      bestScore = m_players[i].score;
    }
  }
  return best;
}

uint64_t GameCore::GetHighScore() const { return GetScore(GetHighPlayer()); }

bool GameCore::IsTilted(uint8_t player) const
{
  const PlayerRecord* record = GetPlayer(player);
  return record && record->tilted;
}

uint8_t GameCore::GetExtraBalls(uint8_t player) const
{
  const PlayerRecord* record = GetPlayer(player);
  return record ? record->extraBalls : 0;
}

int GameCore::ResolveCoil(const std::string& name) const
{
  const auto it = m_config.names.coils.find(name);
  return it == m_config.names.coils.end() ? 0 : it->second;
}

int GameCore::ResolveSwitch(const std::string& name) const
{
  const auto it = m_config.names.switches.find(name);
  return it == m_config.names.switches.end() ? 0 : it->second;
}

int GameCore::ResolveLamp(const std::string& name) const
{
  const auto it = m_config.names.lamps.find(name);
  return it == m_config.names.lamps.end() ? 0 : it->second;
}

uint8_t GameCore::GetSwitchState(int number) const
{
  const auto it = m_switchStates.find(number);
  return it == m_switchStates.end() ? 0 : it->second;
}

void GameCore::OnSwitch(int number, uint8_t state)
{
  const uint8_t normalized = state == 0 ? 0 : 1;
  m_switchStates[number] = normalized;
  m_edges.push_back({number, normalized});
}

void GameCore::Start()
{
  m_stateSinceMs = Now();
  m_troughStableSinceMs = m_stateSinceMs;
  UpdateTroughCount();
  m_believedTrough = m_stableTrough;
  EnterAttract();
}

void GameCore::Update()
{
  UpdateTroughCount();
  ProcessSwitchEdges();

  for (int i = 0; i < kMaxTransitionsPerUpdate; ++i)
  {
    const GameState before = m_state;
    RunStateMachine();
    if (m_state == before)
    {
      break;
    }
  }
}

void GameCore::UpdateTroughCount()
{
  uint8_t closed = 0;
  for (int number : m_config.trough.switches)
  {
    if (GetSwitchState(number)) ++closed;
  }

  const uint64_t now = Now();
  if (closed != m_stableTrough)
  {
    m_stableTrough = closed;
    m_troughStableSinceMs = now;
  }
  else if (now - m_troughStableSinceMs >= m_config.trough.settleMs)
  {
    m_believedTrough = m_stableTrough;
  }
}

void GameCore::ProcessSwitchEdges()
{
  m_startHandledThisTick = false;

  int processed = 0;
  while (!m_edges.empty() && processed < kMaxEdgesPerUpdate)
  {
    const SwitchEdge edge = m_edges.front();
    m_edges.pop_front();
    ++processed;

    if (edge.state == 0)
    {
      continue;  // every rule below is on the closing edge
    }

    if (m_config.startSwitch != 0 && edge.number == m_config.startSwitch)
    {
      HandleStartPressed();
      continue;
    }

    if (m_config.serviceCreditSwitch != 0 && edge.number == m_config.serviceCreditSwitch)
    {
      AddCredits(1);
      continue;
    }

    if (std::find(m_config.coinSwitches.begin(), m_config.coinSwitches.end(), edge.number) !=
        m_config.coinSwitches.end())
    {
      AddCredits(m_config.creditsPerCoin);
      continue;
    }

  }
}

void GameCore::HandleStartPressed()
{
  if (m_startHandledThisTick)
  {
    Emit({GameEventType::StartRejected, 0, 0, 0, 0, StartRejectReason::AlreadyStarting});
    return;
  }
  m_startHandledThisTick = true;

  if (m_state == GameState::Attract || m_state == GameState::GameOver)
  {
    StartGame();
    return;
  }

  AddPlayer();
}

bool GameCore::StartGame()
{
  if (IsInGame())
  {
    return AddPlayer();
  }

  if (!m_config.freePlay && m_credits == 0)
  {
    Emit({GameEventType::StartRejected, 0, 0, 0, 0, StartRejectReason::NoCredits});
    return false;
  }

  if (m_believedTrough < 1)
  {
    Emit({GameEventType::StartRejected, 0, 0, 0, 0, StartRejectReason::NotEnoughBalls});
    Act({GameActionType::BallSearch, 0, 1, 0});
    return false;
  }

  if (!Allow("canStartGame", 0))
  {
    Emit({GameEventType::StartRejected, 0, 0, 0, 0, StartRejectReason::ScriptVetoed});
    return false;
  }

  if (!m_config.freePlay)
  {
    AddCredits(-1);
  }

  BeginGame();
  return true;
}

bool GameCore::AddPlayer()
{
  if (!IsInGame())
  {
    return StartGame();
  }

  // The test is on the CURRENT player's ball, not on a global ball counter: a
  // player added during ball one gets their full complement of balls, which is
  // the historically correct behaviour.
  const PlayerRecord* current = GetPlayer(0);
  if (current != nullptr && current->ball > m_config.addPlayerThroughBall)
  {
    Emit({GameEventType::StartRejected, 0, 0, 0, 0, StartRejectReason::TooLate});
    return false;
  }

  if (m_playerCount >= m_config.maxPlayers)
  {
    Emit({GameEventType::StartRejected, 0, 0, 0, 0, StartRejectReason::TooManyPlayers});
    return false;
  }

  for (uint8_t i = 0; i < m_playerCount; ++i)
  {
    if (m_players[i].tilted)
    {
      Emit({GameEventType::StartRejected, 0, 0, 0, 0, StartRejectReason::Tilted});
      return false;
    }
  }

  if (!m_config.freePlay && m_credits == 0)
  {
    Emit({GameEventType::StartRejected, 0, 0, 0, 0, StartRejectReason::NoCredits});
    return false;
  }

  if (!Allow("canAddPlayer", m_playerCount + 1))
  {
    Emit({GameEventType::StartRejected, 0, 0, 0, 0, StartRejectReason::ScriptVetoed});
    return false;
  }

  if (!m_config.freePlay)
  {
    AddCredits(-1);
  }

  ++m_playerCount;
  if (m_players.size() < m_playerCount)
  {
    m_players.resize(m_playerCount);
  }
  m_players[m_playerCount - 1] = PlayerRecord{};

  Emit({GameEventType::PlayerAdded, m_playerCount, 0, 0, 0, StartRejectReason::NoCredits});
  RefreshBackboxLamps();
  return true;
}

void GameCore::BeginGame()
{
  m_players.assign(m_config.maxPlayers, PlayerRecord{});
  m_playerCount = 1;
  m_currentPlayer = 1;
  m_matchDigits = -1;
  m_ballsInPlay = 0;
  m_ballVars.clear();

  Emit({GameEventType::AttractEnd, 0, 0, 0, 0, StartRejectReason::NoCredits});

  if (m_config.gameOnCoil != 0)
  {
    Act({GameActionType::SetCoil, m_config.gameOnCoil, 1, 0});
  }
  SetGi(m_config.giOnLevel);
  SetLamp(m_config.gameOverLamp, 0);
  SetLamp(m_config.tiltLamp, 0);
  SetLamp(m_config.ballInPlayLamp, 1);

  Emit({GameEventType::GameStart, 1, 0, 1, 0, StartRejectReason::NoCredits});
  EnterState(GameState::GameStarting);
}

void GameCore::EnterAttract()
{
  m_playerCount = 0;
  m_currentPlayer = 0;
  m_ballsInPlay = 0;
  m_matchDigits = -1;
  m_ballVars.clear();

  if (m_config.gameOnCoil != 0)
  {
    Act({GameActionType::SetCoil, m_config.gameOnCoil, 0, 0});
  }
  ClearTilt();
  SetGi(m_config.giOnLevel);
  SetLamp(m_config.gameOverLamp, 1);
  SetLamp(m_config.ballInPlayLamp, 0);
  SetLamp(m_config.shootAgainLamp, 0);
  RefreshBackboxLamps();

  m_attractPageSinceMs = Now();
  Emit({GameEventType::AttractStart, 0, 0, 0, 0, StartRejectReason::NoCredits});
  EnterState(GameState::Attract);
}

void GameCore::EnterState(GameState state)
{
  m_state = state;
  m_stateSinceMs = Now();
  m_deadlineMs = 0;
}

void GameCore::BeginBall()
{
  PlayerRecord& player = CurrentPlayer();

  // An extra ball replays the same ball number and keeps per-ball variables:
  // "shoot again" is a continuation, not a new ball.
  if (!player.tilted || !player.tiltedThroughGame)
  {
    player.tilted = false;
  }

  m_ballVars.clear();
  m_kickAttempts = 0;
  m_bonusDone = false;

  ClearTilt();
  SetGi(m_config.giOnLevel);
  SetLamp(m_config.tiltLamp, 0);
  SetLamp(m_config.ballInPlayLamp, 1);
  RefreshBackboxLamps();

  Emit({GameEventType::BallStart, m_currentPlayer, player.ball, 0, 0, StartRejectReason::NoCredits});
  EnterState(GameState::BallStarting);
  ServeKick();
}

void GameCore::ServeKick()
{
  ++m_kickAttempts;
  PulseCoil(m_config.trough.kickCoil, m_config.trough.kickPulseMs);
  m_deadlineMs = Now() + m_config.trough.kickRetryMs;
}

void GameCore::FinishBall()
{
  m_ballsInPlay = 0;
  PlayerRecord& player = CurrentPlayer();
  Emit({GameEventType::BallEnd, m_currentPlayer, player.ball, 0, 0, StartRejectReason::NoCredits});
  EnterState(GameState::BallEnding);
}

void GameCore::HandleSlamTilt()
{
  if (!IsInGame())
  {
    return;
  }
  ApplyTilt(true);
}

void GameCore::ApplyTilt(bool slam)
{
  PlayerRecord& player = CurrentPlayer();
  if (player.tilted && !slam)
  {
    return;  // already tilted this ball
  }
  player.tilted = true;
  player.tiltedThroughGame = !m_config.tilt.endsBallOnly || slam;

  // Flippers: assert the host-owned inhibit switch. The boards see it on the
  // bus and stop honouring fast-flip inputs locally, which is the only way to
  // drop a flipper the player is holding.
  if (m_config.tilt.inhibitSwitch != 0 && !m_tiltInhibitAsserted)
  {
    Act({GameActionType::SetTiltInhibit, m_config.tilt.inhibitSwitch, 1, 0});
    m_tiltInhibitAsserted = true;
  }

  // GI off. Every other coil stays live: the outhole kicker and the trough
  // eject are exactly what is needed to get the balls back.
  if (m_config.tilt.giOff)
  {
    SetGi(0);
  }
  SetLamp(m_config.tiltLamp, 1);

  if (slam)
  {
    for (uint8_t i = 0; i < m_playerCount; ++i)
    {
      m_players[i].finished = true;
      m_players[i].tilted = true;
    }
    if (m_config.gameOnCoil != 0)
    {
      Act({GameActionType::SetCoil, m_config.gameOnCoil, 0, 0});
    }
    Emit({GameEventType::SlamTilt, m_currentPlayer, player.ball, 0, 0, StartRejectReason::NoCredits});
    EnterState(GameState::GameEnding);
    return;
  }

  Emit({GameEventType::Tilt, m_currentPlayer, player.ball, 0, 0, StartRejectReason::NoCredits});
  m_deadlineMs = Now() + m_config.tilt.recoverTimeoutMs;
}

void GameCore::ClearTilt()
{
  if (m_tiltInhibitAsserted && m_config.tilt.inhibitSwitch != 0)
  {
    Act({GameActionType::SetTiltInhibit, m_config.tilt.inhibitSwitch, 0, 0});
  }
  m_tiltInhibitAsserted = false;
}

bool GameCore::ScoringAllowed() const
{
  // BallEnding and BonusCount are included deliberately: the bonus is collected
  // after the ball has left play, and a rule counting it down through
  // ppuc.game.addScore has to be able to score. Without these two the entire
  // bonus mechanism is inert.
  if (m_state != GameState::BallInPlay && m_state != GameState::BallInLane &&
      m_state != GameState::BallEnding && m_state != GameState::BonusCount)
  {
    return false;
  }
  const PlayerRecord* player = GetPlayer(0);
  return player != nullptr && !player->tilted;
}

void GameCore::AddScore(int64_t points, uint8_t player)
{
  // Discarded here rather than in every rules file: a tilted machine that still
  // scores is the single most common homebrew rules bug.
  if (points == 0 || !ScoringAllowed())
  {
    return;
  }

  const uint8_t index = player == 0 ? m_currentPlayer : player;
  if (index == 0 || index > m_playerCount)
  {
    return;
  }

  PlayerRecord& record = m_players[index - 1];
  const uint64_t ceiling = ScoreCeiling(m_config.scoreDigits);
  int64_t total = static_cast<int64_t>(record.score) + points;
  if (total < 0) total = 0;
  record.score = static_cast<uint64_t>(total) % ceiling;

  Emit({GameEventType::Score, index, record.ball, points, static_cast<int64_t>(record.score),
        StartRejectReason::NoCredits});
  CheckReplay(index);
}

void GameCore::SetScore(uint64_t points, uint8_t player)
{
  const uint8_t index = player == 0 ? m_currentPlayer : player;
  if (index == 0 || index > m_playerCount) return;
  m_players[index - 1].score = points % ScoreCeiling(m_config.scoreDigits);
}

void GameCore::AwardExtraBall(uint8_t player)
{
  if (!ScoringAllowed()) return;
  const uint8_t index = player == 0 ? m_currentPlayer : player;
  if (index == 0 || index > m_playerCount) return;

  ++m_players[index - 1].extraBalls;
  SetLamp(m_config.shootAgainLamp, 1);
}

void GameCore::AddCredits(int delta)
{
  const int updated = static_cast<int>(m_credits) + delta;
  const uint16_t clamped =
      static_cast<uint16_t>(std::clamp(updated, 0, static_cast<int>(m_config.maxCredits)));
  if (clamped == m_credits) return;
  m_credits = clamped;
  Emit({GameEventType::CreditsChanged, 0, 0, m_credits, 0, StartRejectReason::NoCredits});
}

void GameCore::Knock() { PulseCoil(m_config.knockerCoil, m_config.knockerPulseMs); }

void GameCore::CheckReplay(uint8_t player)
{
  if (player == 0 || player > m_playerCount) return;
  PlayerRecord& record = m_players[player - 1];

  // Thresholds are awarded in order and once each per game, so a player who
  // crosses two at once collects both, and a score that keeps climbing does not
  // re-award the same threshold.
  while (record.replaysAwarded < m_config.replay.thresholds.size() &&
         record.score >= m_config.replay.thresholds[record.replaysAwarded])
  {
    const uint64_t threshold = m_config.replay.thresholds[record.replaysAwarded];
    ++record.replaysAwarded;

    if (m_config.replay.awardCredit) AddCredits(1);
    if (m_config.replay.awardExtraBall) ++record.extraBalls;
    Knock();
    Emit({GameEventType::Replay, player, record.ball, static_cast<int64_t>(threshold), 0,
          StartRejectReason::NoCredits});
  }
}

void GameCore::DrawMatch()
{
  if (!m_config.match.enabled) return;

  m_matchDigits = static_cast<int>(m_random(10)) * 10;
  int matchedMask = 0;
  for (uint8_t i = 0; i < m_playerCount; ++i)
  {
    if (static_cast<int>(m_players[i].score % 100) == m_matchDigits)
    {
      matchedMask |= (1 << i);
      if (m_config.match.awardCredit) AddCredits(1);
    }
  }

  if (matchedMask != 0)
  {
    Knock();
    SetLamp(m_config.matchLamp, 1);
  }
  Emit({GameEventType::Match, 0, 0, m_matchDigits, matchedMask, StartRejectReason::NoCredits});
}

void GameCore::RefreshBackboxLamps()
{
  for (size_t i = 0; i < m_config.playerUpLamps.size(); ++i)
  {
    const bool on = m_currentPlayer != 0 && (i + 1) == m_currentPlayer;
    SetLamp(m_config.playerUpLamps[i], on ? 1 : 0);
  }
}

void GameCore::AdvancePlayer()
{
  for (uint8_t step = 0; step < m_playerCount; ++step)
  {
    const uint8_t next = static_cast<uint8_t>((m_currentPlayer % m_playerCount) + 1);
    m_currentPlayer = next;
    if (!m_players[next - 1].finished)
    {
      // A player added mid-game joins with ball 0, so their first turn takes
      // them to ball 1 and they get a full complement.
      ++m_players[next - 1].ball;
      RefreshBackboxLamps();
      BeginBall();
      return;
    }
  }

  EnterState(GameState::GameEnding);
}

void GameCore::Tilt()
{
  if (m_state == GameState::BallInPlay || m_state == GameState::BallInLane)
  {
    ApplyTilt(false);
  }
}

void GameCore::SlamTilt() { HandleSlamTilt(); }

void GameCore::EndBall()
{
  if (m_state == GameState::BallInPlay || m_state == GameState::BallInLane ||
      m_state == GameState::BallDraining)
  {
    FinishBall();
  }
}

void GameCore::EndGame()
{
  if (IsInGame())
  {
    for (uint8_t i = 0; i < m_playerCount; ++i)
    {
      m_players[i].finished = true;
    }
    EnterState(GameState::GameEnding);
  }
}

void GameCore::ServeBall()
{
  if (m_state == GameState::BallStarting)
  {
    ServeKick();
  }
}

void GameCore::BonusDone() { m_bonusDone = true; }

int64_t GameCore::GetVar(const std::string& name, uint8_t player) const
{
  const PlayerRecord* record = GetPlayer(player);
  if (record == nullptr) return 0;
  const auto it = record->vars.find(name);
  return it == record->vars.end() ? 0 : it->second;
}

void GameCore::SetVar(const std::string& name, int64_t value, uint8_t player)
{
  const uint8_t index = player == 0 ? m_currentPlayer : player;
  if (index == 0 || index > m_players.size()) return;
  m_players[index - 1].vars[name] = value;
}

int64_t GameCore::AddVar(const std::string& name, int64_t delta, uint8_t player)
{
  const uint8_t index = player == 0 ? m_currentPlayer : player;
  if (index == 0 || index > m_players.size()) return 0;
  auto& value = m_players[index - 1].vars[name];
  value += delta;
  return value;
}

int64_t GameCore::GetBallVar(const std::string& name) const
{
  const auto it = m_ballVars.find(name);
  return it == m_ballVars.end() ? 0 : it->second;
}

void GameCore::SetBallVar(const std::string& name, int64_t value) { m_ballVars[name] = value; }

int64_t GameCore::AddBallVar(const std::string& name, int64_t delta)
{
  auto& value = m_ballVars[name];
  value += delta;
  return value;
}

uint8_t GameCore::BallsOut() const
{
  // Derived, never accumulated. A counter maintained by serve/drain events
  // drifts the first time a ball is nudged out of the trough by hand; the
  // difference against a known physical ball count cannot.
  if (m_believedTrough >= m_config.ballCount) return 0;
  return static_cast<uint8_t>(m_config.ballCount - m_believedTrough);
}

void GameCore::RunStateMachine()
{
  const uint64_t now = Now();
  const bool deadlinePassed = m_deadlineMs != 0 && now >= m_deadlineMs;
  m_ballsInPlay = BallsOut();

  switch (m_state)
  {
    case GameState::Attract:
      break;

    case GameState::GameStarting:
      // Exists only so onGameStart runs before onBallStart without a same-tick
      // ordering hack.
      m_currentPlayer = 1;
      m_players[0].ball = 1;
      RefreshBackboxLamps();
      BeginBall();
      break;

    case GameState::BallStarting:
      if (m_ballsInPlay > 0)
      {
        Emit({GameEventType::BallServed, m_currentPlayer, GetCurrentBall(), 0, 0, StartRejectReason::NoCredits});
        if (m_config.shooterLane.laneSwitch != 0)
        {
          EnterState(GameState::BallInLane);
          m_deadlineMs = now + m_config.shooterLane.launchTimeoutMs;
        }
        else
        {
          EnterState(GameState::BallInPlay);
        }
        break;
      }

      if (deadlinePassed)
      {
        if (m_kickAttempts <= m_config.trough.kickRetries)
        {
          ServeKick();
        }
        else
        {
          Emit({GameEventType::BallServeFailed, m_currentPlayer, GetCurrentBall(), m_kickAttempts, 0,
                StartRejectReason::NoCredits});
          Act({GameActionType::BallSearch, 0, 1, 0});
          m_kickAttempts = 0;
          m_deadlineMs = now + m_config.trough.kickRetryMs;
        }
      }
      break;

    case GameState::BallInLane:
      if (m_ballsInPlay == 0)
      {
        // Drained straight out of the lane.
        EnterState(GameState::BallDraining);
        m_deadlineMs = now + m_config.trough.settleMs;
        break;
      }
      if (GetSwitchState(m_config.shooterLane.laneSwitch) == 0 &&
          now - m_stateSinceMs >= m_config.shooterLane.serveSettleMs)
      {
        EnterState(GameState::BallInPlay);
        break;
      }
      if (deadlinePassed)
      {
        Emit({GameEventType::BallStuck, m_currentPlayer, GetCurrentBall(), 0, 0, StartRejectReason::NoCredits});
        Act({GameActionType::BallSearch, 0, 1, 0});
        m_deadlineMs = now + m_config.shooterLane.launchTimeoutMs;
      }
      break;

    case GameState::BallInPlay:
    {
      if (m_ballsInPlay == 0)
      {
        EnterState(GameState::BallDraining);
        m_deadlineMs = now + m_config.trough.settleMs;
        break;
      }

      // A tilted ball is often trapped, and on many machines the outhole kicker
      // is dead while tilted. Rather than strand the machine, give up after the
      // timeout and let the ordinary serve path recover.
      const PlayerRecord* player = GetPlayer(0);
      if (player != nullptr && player->tilted && deadlinePassed)
      {
        ClearTilt();
        SetGi(m_config.giOnLevel);
        FinishBall();
      }
      break;
    }

    case GameState::BallDraining:
      if (m_ballsInPlay > 0)
      {
        // The count fell back: chatter, not a drain. This is the case that would
        // otherwise silently eat a ball.
        EnterState(GameState::BallInPlay);
        break;
      }
      if (deadlinePassed)
      {
        FinishBall();
      }
      break;

    case GameState::BallEnding:
    {
      // Bonus is collected at the end of EVERY ball, including the one that
      // awarded an extra ball. Only afterwards is the extra ball considered.
      const PlayerRecord* player = GetPlayer(0);
      if (player != nullptr && player->tilted && m_config.tilt.skipBonus)
      {
        m_bonusDone = true;
        EnterState(GameState::BonusCount);
        break;
      }

      m_bonusDone = false;
      Emit({GameEventType::BonusCount, m_currentPlayer, GetCurrentBall(), 0, 0, StartRejectReason::NoCredits});
      EnterState(GameState::BonusCount);
      m_deadlineMs = now + m_config.bonusTimeoutMs;
      break;
    }

    case GameState::BonusCount:
    {
      // A rules bug that never calls bonusDone() must not brick the machine, so
      // the timeout proceeds rather than erroring.
      if (!m_bonusDone && !deadlinePassed)
      {
        break;
      }

      PlayerRecord& player = CurrentPlayer();

      // Decided against the tilt state of the ball that just ended, and captured
      // BEFORE the tilt is cleared below. Tilting on the ball that awarded an
      // extra ball forfeits it; clearing first would silently hand it back.
      const bool tiltBlocksExtraBall = player.tilted && !m_config.tilt.extraBallSurvivesTilt;

      // Tilt clears once every ball is home -- not merely the one that was in
      // play. That is the right condition for a multiball machine and costs no
      // more than counting one.
      if (player.tilted && !player.tiltedThroughGame && m_ballsInPlay == 0)
      {
        player.tilted = false;
        ClearTilt();
        SetGi(m_config.giOnLevel);
        SetLamp(m_config.tiltLamp, 0);
      }

      if (player.extraBalls > 0 && !tiltBlocksExtraBall)
      {
        --player.extraBalls;
        if (player.extraBalls == 0)
        {
          SetLamp(m_config.shootAgainLamp, 0);
        }
        Emit({GameEventType::ExtraBall, m_currentPlayer, player.ball, 0, 0, StartRejectReason::NoCredits});

        // Same ball number, and per-ball variables survive: shoot again is a
        // continuation of this ball, not a new one.
        m_kickAttempts = 0;
        m_bonusDone = false;
        EnterState(GameState::BallStarting);
        ServeKick();
        break;
      }

      if (player.ball >= m_config.ballsPerGame)
      {
        player.finished = true;
      }

      AdvancePlayer();
      break;
    }

    case GameState::GameEnding:
    {
      if (m_config.gameOnCoil != 0)
      {
        Act({GameActionType::SetCoil, m_config.gameOnCoil, 0, 0});
      }
      ClearTilt();
      SetGi(m_config.giOnLevel);
      SetLamp(m_config.ballInPlayLamp, 0);
      SetLamp(m_config.gameOverLamp, 1);

      DrawMatch();

      Emit({GameEventType::GameEnd, m_playerCount, 0, GetHighPlayer(), static_cast<int64_t>(GetHighScore()),
            StartRejectReason::NoCredits});
      EnterState(GameState::GameOver);
      m_deadlineMs = now + m_config.gameOverHoldMs;
      break;
    }

    case GameState::GameOver:
      if (deadlinePassed)
      {
        EnterAttract();
      }
      break;
  }
}
