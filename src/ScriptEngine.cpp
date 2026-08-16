#include "ScriptEngine.h"

#include <chrono>
#include <cstdio>
#include <random>

ScriptEngine::ScriptEngine(Options options, GameConfig gameConfig, MachineIo io)
    : m_options(std::move(options)),
      m_io(std::move(io)),
      m_canvas(m_options.dmdWidth, m_options.dmdHeight)
{
  m_game.SetConfig(gameConfig);
  m_canvas.SetColor(m_options.dmdColorR, m_options.dmdColorG, m_options.dmdColorB);
  m_defaultScreen.SetTitle(m_options.gameId.empty() ? "PPUC" : m_options.gameId);
  m_defaultScreen.SetAttractPageMs(gameConfig.attractPageMs);

  // GameCore ships a deterministic fallback so tests never flake; a real machine
  // wants match digits that actually vary.
  m_game.SetRandom(
      [](uint32_t bound) -> uint32_t
      {
        if (bound == 0) return 0;
        static std::mt19937 engine{std::random_device{}()};
        return std::uniform_int_distribution<uint32_t>(0, bound - 1)(engine);
      });

  m_renderer.SetDrawCallback(
      [this]()
      {
        const uint64_t nowMs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count());
        if (m_builtinScreen)
        {
          m_defaultScreen.Draw(m_canvas, m_game, nowMs);
        }
        else
        {
          m_canvas.Clear(0);
        }
        // Rules draw on top of the built-in screen, so a machine with no DMD
        // scripting still shows a score and one with scripting can add to it.
        if (m_luaDraw)
        {
          m_luaDraw();
        }
      });
}

ScriptEngine::~ScriptEngine() { Stop(); }

void ScriptEngine::SetHost(GameEngineHost* pHost) { m_pHost = pHost; }

void ScriptEngine::SetDmdDrawCallback(std::function<void()> draw) { m_luaDraw = std::move(draw); }

namespace
{
// Bridges the renderer to the host's DMD sink, which is the same
// GameEngineHost::OnDmdFrame that PinMAME frames travel through.
class HostDmdSink final : public DmdSink
{
 public:
  explicit HostDmdSink(GameEngineHost* pHost) : m_pHost(pHost) {}

  void Push(const uint8_t* data, uint16_t width, uint16_t height, uint8_t, uint8_t, uint8_t) override
  {
    if (m_pHost != nullptr)
    {
      // Depth 4: one byte per pixel, values 0..15. libdmdutil builds the palette
      // ramp from the tint and hands indexed data to every sink.
      m_pHost->OnDmdFrame(data, 4, width, height);
    }
  }

 private:
  GameEngineHost* m_pHost;
};
}  // namespace

bool ScriptEngine::Start(std::string& error)
{
  if (m_pHost == nullptr)
  {
    error = "ScriptEngine::Start called before SetHost";
    return false;
  }

  const GameConfig& config = m_game.GetConfig();
  if (!config.enabled)
  {
    error = "no emGame configuration found; a ROM-less game needs an emGame block in its io-boards.yaml";
    return false;
  }

  m_sink = std::make_unique<HostDmdSink>(m_pHost);
  m_renderer.SetSink(m_sink.get());

  // Deliberately does not emit outputs: the host has not necessarily called
  // PPUC::StartUpdates() yet. GameCore::Start() queues its initial lamp and GI
  // state, and the first Update() delivers it.
  m_game.Start();
  m_ready = true;

  // The single most likely first-run failure of this feature is a dead
  // playfield because high power was never enabled, so say which case we are in
  // rather than leaving the operator to guess.
  if (config.gameOnCoil != 0)
  {
    printf("GameCore: high power gated by coil %d, asserted for the duration of each game\n", config.gameOnCoil);
  }
  else
  {
    printf("GameCore: gameOnCoil not configured; the boards hold high power on for the whole session\n");
  }
  if (config.tilt.inhibitSwitch == 0)
  {
    printf(
        "GameCore: tilt has no inhibitSwitch configured. Scoring will be suppressed on tilt, but the flippers "
        "cannot be dropped. See docs/EM_GAMES.md.\n");
  }

  return true;
}

void ScriptEngine::Stop()
{
  m_renderer.SetSink(nullptr);
  m_sink.reset();
  m_ready = false;
}

bool ScriptEngine::IsReady() const { return m_ready; }

bool ScriptEngine::TryGetIdentity(Identity* pIdentity) const
{
  if (pIdentity == nullptr || !m_ready)
  {
    return false;
  }
  pIdentity->name = m_options.gameId;
  pIdentity->description = "GameCore (ROM-less)";
  pIdentity->hardwareGen = 0;
  return true;
}

void ScriptEngine::Update()
{
  if (!m_ready)
  {
    return;
  }

  m_game.Update();
  ApplyActions();
  DispatchEvents();

  // Attract vs play, reported separately from any coil so the host can run
  // music, translite and ball search without knowing how this machine is wired.
  const bool running = m_game.IsInGame();
  if (running != m_gameRunning)
  {
    m_gameRunning = running;
    m_pHost->OnGameRunningChanged(running);
  }

  const uint8_t ball = m_game.GetCurrentBall();
  if (ball != m_lastBall)
  {
    m_lastBall = ball;
    m_pHost->OnCurrentBallChanged(ball);
  }

  const uint8_t player = m_game.GetCurrentPlayer();
  if (player != m_lastPlayer)
  {
    m_lastPlayer = player;
    m_pHost->OnCurrentPlayerChanged(player);
  }

  m_renderer.Service(m_canvas);
}

void ScriptEngine::ApplyActions()
{
  for (const GameAction& action : m_game.TakeActions())
  {
    switch (action.type)
    {
      case GameActionType::PulseCoil:
        if (m_io.pulseCoil) m_io.pulseCoil(action.number, action.durationMs);
        break;
      case GameActionType::SetCoil:
        if (m_io.setCoil) m_io.setCoil(action.number, action.value);
        break;
      case GameActionType::SetLamp:
        if (m_io.setLamp) m_io.setLamp(action.number, action.value);
        break;
      case GameActionType::SetGi:
        if (m_io.setGi) m_io.setGi(action.number, action.value);
        break;
      case GameActionType::SetTiltInhibit:
        if (m_io.setTiltInhibit) m_io.setTiltInhibit(action.number, action.value);
        break;
      case GameActionType::BallSearch:
        if (m_io.requestBallSearch) m_io.requestBallSearch();
        break;
    }
  }
}

void ScriptEngine::DispatchEvents()
{
  for (const GameEvent& event : m_game.TakeEvents())
  {
    if (m_options.debug)
    {
      printf("GameCore event: type=%d player=%u ball=%u value=%lld\n", static_cast<int>(event.type), event.player,
             event.ball, static_cast<long long>(event.value));
    }
    if (m_onEvent)
    {
      m_onEvent(event);
    }
  }
}

void ScriptEngine::PollChangedLamps(std::vector<GameEngineOutputChange>& changes)
{
  // Lamps are driven directly by GameCore actions rather than polled: there is
  // no emulated lamp matrix to diff against.
  changes.clear();
}

void ScriptEngine::PollChangedGis(std::vector<GameEngineOutputChange>& changes) { changes.clear(); }

void ScriptEngine::SendSwitch(int number, uint8_t state) { m_game.OnSwitch(number, state); }

bool ScriptEngine::HasCapability(Capability capability) const
{
  switch (capability)
  {
    case Capability::ChangedGis:
      return false;  // GI is set directly, not polled
    case Capability::TracksBallAndPlayer:
      return true;  // GameCore owns them, so they are always known
    case Capability::AudioStream:
      return false;  // no emulated sound hardware
    case Capability::SegmentDisplays:
      return false;
  }
  return false;
}
