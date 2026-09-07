#include "PluginEngine.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "PluginBus.h"
#include "PinmameNvramMapLoader.h"
#include "pinmame/PinMAMEPlugin.h"
#include "plugins/ControllerPlugin.h"

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

using namespace PinballPlugin::Controller;

namespace
{

uint64_t NowMs()
{
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

constexpr int kPlatformWpc = 1;  // PLATFORM_WPC, mirrored to avoid a libppuc include
constexpr uint64_t kTrackedStatePollIntervalMs = 500;

}  // namespace

// The consumers live here rather than in the header so that PluginEngine.h does
// not drag the plugin SDK into everything that includes it.
struct PluginEngine::Impl
{
  std::unique_ptr<CtrlItemConsumer<ControllerDef>> controllers;
  std::unique_ptr<CtrlItemConsumer<StateSrcId>> states;
};

PluginEngine::PluginEngine(PluginBus& bus, Options options)
    : m_bus(bus), m_options(std::move(options)), m_impl(std::make_unique<Impl>())
{
}

PluginEngine::~PluginEngine() { Stop(); }

void PluginEngine::SetHost(GameEngineHost* pHost) { m_pHost = pHost; }

bool PluginEngine::IsReady() const { return m_runState.load(std::memory_order_acquire) != 0; }

bool PluginEngine::HasCapability(Capability capability) const
{
  switch (capability)
  {
    case Capability::ChangedGis:
      // PinMAME's GI is unusable outside WPC; libppuc forces GI on there.
      return m_options.platform == kPlatformWpc;
    case Capability::TracksBallAndPlayer:
      return m_tracking.loaded;
    case Capability::AudioStream:
      return !m_options.noSound;
    case Capability::SegmentDisplays:
      return true;
  }
  return false;
}

// ---- Source changes ------------------------------------------------------

void PluginEngine::OnStateSrcAboutToChange()
{
  // Close the gate and wait for the poll thread to leave its fetch phase. The
  // provider is about to free the StateDefs the plan points into, so this must
  // complete before it returns. The wait is bounded by one fetch (a few
  // microseconds), never by a host sink: dispatch happens outside the gate
  // precisely so OnCoilChanged, which takes the interceptor lock and writes the
  // serial bus, cannot stall the provider.
  m_gateOpen.store(false, std::memory_order_seq_cst);
  while (m_gateActive.load(std::memory_order_acquire) != 0)
  {
    std::this_thread::yield();
  }
  m_coilPlan.reset();
}

void PluginEngine::OnStateSrcChanged()
{
  static uint32_t s_generation = 0;

  auto plan = std::make_unique<CoilPlan>();
  plan->generation = ++s_generation;

  std::vector<OutputEntry> lamps;
  std::vector<OutputEntry> gis;
  std::unordered_map<int, SwitchEntry> switches;

  // With() is the only safe way to touch the provider's list, and it is taken
  // on the message thread only.
  m_impl->states->With(
      [&](const std::vector<StateSrcId>& sources)
      {
        for (const StateSrcId& src : sources)
        {
          if (src.id.endpointId != m_pinmameEndpoint || src.stateDefs == nullptr)
          {
            continue;
          }
          for (unsigned int i = 0; i < src.nStates; ++i)
          {
            const StateDef& def = src.stateDefs[i];
            switch (src.id.resId)
            {
              case PMPI_GROUP_VPM_SOLENOID:
                // The VPinMAME-compatible group, so coil semantics match what
                // OnSolenoidUpdated used to deliver.
                if (def.GetState != nullptr)
                {
                  plan->entries.push_back({static_cast<uint16_t>(def.mappingId), def.callContext, def.GetState});
                }
                break;
              case PMPI_GROUP_VPM_LAMP:
                if (def.GetState != nullptr)
                {
                  lamps.push_back({static_cast<uint16_t>(def.mappingId), def.callContext, def.GetState});
                }
                break;
              case PMPI_GROUP_VPM_GI:
                if (def.GetState != nullptr)
                {
                  gis.push_back({static_cast<uint16_t>(def.mappingId), def.callContext, def.GetState});
                }
                break;
              case PMPI_GROUP_SWITCH:
                // Switch numbers are signed -- cabinet switches are negative --
                // and arrive truncated into a uint32_t mappingId. The int16_t
                // round-trip is what recovers the sign; without it every
                // cabinet switch silently fails to map.
                if (def.SetState != nullptr)
                {
                  const int number = static_cast<int>(static_cast<int16_t>(def.mappingId));
                  switches[number] = {def.callContext, def.SetState};
                }
                break;
              default:
                break;
            }
          }
        }
      });

  m_lamps = std::move(lamps);
  m_gis = std::move(gis);
  m_switchesByNumber = std::move(switches);
  // Re-announce every output after a source change: a restarted game must not
  // inherit the previous run's edge state.
  m_lastLamp.assign(m_lamps.size(), 0);
  m_lastGi.assign(m_gis.size(), 0);

  m_coilPlan = std::move(plan);
  m_gateOpen.store(true, std::memory_order_seq_cst);

  if (m_options.debug)
  {
    std::printf("PluginEngine: %zu solenoids, %zu lamps, %zu GIs, %zu switches\n", m_coilPlan->entries.size(),
                m_lamps.size(), m_gis.size(), m_switchesByNumber.size());
  }
}

void PluginEngine::OnControllersChanged()
{
  const bool present = m_impl->controllers->With([](const std::vector<ControllerDef>& items) { return !items.empty(); });
  const int state = present ? 1 : 0;
  const int previous = m_runState.exchange(state, std::memory_order_release);
  if (previous != state && m_pHost != nullptr && !m_stopping)
  {
    m_pHost->OnRunStateChanged(state);
  }
}

// ---- The real-time coil path --------------------------------------------

void PluginEngine::PollThreadMain()
{
#if defined(__linux__)
  // Best effort: without it a 1 ms sleep on a loaded Pi can overshoot to 5-10
  // ms, and the tail is what matters here, not the mean.
  sched_param param{};
  param.sched_priority = 40;
  if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0 && m_options.debug)
  {
    std::printf("PluginEngine: SCHED_FIFO unavailable, coil polling runs at normal priority\n");
  }
#endif

  const auto period = std::chrono::microseconds(1000000 / std::max(1, m_options.coilPollHz));
  auto deadline = std::chrono::steady_clock::now();

  uint32_t localGeneration = 0;
  std::vector<uint16_t> numbers;
  std::vector<uint8_t> last;
  std::vector<uint8_t> raw;

  uint64_t ticks = 0;
  uint64_t fetches = 0;
  uint64_t worstLateUs = 0;
  uint64_t nextReportMs = NowMs() + 1000;

  while (!m_stopRequested.load(std::memory_order_acquire))
  {
    deadline += period;
    ++ticks;

    // --- fetch phase: inside the gate, touching provider memory ---
    m_gateActive.fetch_add(1, std::memory_order_seq_cst);
    if (m_gateOpen.load(std::memory_order_seq_cst))
    {
      const CoilPlan& plan = *m_coilPlan;
      if (plan.generation != localGeneration)
      {
        localGeneration = plan.generation;
        numbers.clear();
        for (const OutputEntry& e : plan.entries)
        {
          numbers.push_back(e.number);
        }
        last.assign(plan.entries.size(), 0);
        raw.assign(plan.entries.size(), 0);
      }
      for (size_t i = 0; i < plan.entries.size(); ++i)
      {
        plan.entries[i].Get(plan.entries[i].context, &raw[i]);
      }
      m_gateActive.fetch_sub(1, std::memory_order_release);
      ++fetches;

      // --- dispatch phase: outside the gate, PPUC-owned memory only ---
      for (size_t i = 0; i < numbers.size(); ++i)
      {
        const uint8_t value = raw[i] != 0 ? 1 : 0;
        if (value == last[i])
        {
          continue;
        }
        last[i] = value;
        if (m_pHost == nullptr)
        {
          continue;
        }
        // Order is load-bearing: a rules handler reacting to the coil must
        // already see the new attract state.
        if (m_options.gameOnSolenoid != 0 && numbers[i] == m_options.gameOnSolenoid)
        {
          m_pHost->OnGameRunningChanged(value != 0);
        }
        if (m_options.debugCoils)
        {
          std::printf("Coil updated: #%u, %u\n", numbers[i], value);
        }
        m_pHost->OnCoilChanged(numbers[i], value);
      }
    }
    else
    {
      m_gateActive.fetch_sub(1, std::memory_order_release);
      localGeneration = 0;
    }

    // The mean is uninteresting -- it is dominated by the source's own 60 Hz
    // quantization. The tail is what would hurt, so report the worst overshoot.
    const auto now = std::chrono::steady_clock::now();
    if (now > deadline)
    {
      const auto lateUs =
          static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now - deadline).count());
      worstLateUs = std::max(worstLateUs, lateUs);
    }
    if (m_options.debugCoils)
    {
      const uint64_t nowMs = NowMs();
      if (nowMs >= nextReportMs)
      {
        nextReportMs = nowMs + 1000;
        std::printf("Coil poll: %llu ticks/s, %llu fetches/s, worst overshoot %llu us\n",
                    static_cast<unsigned long long>(ticks), static_cast<unsigned long long>(fetches),
                    static_cast<unsigned long long>(worstLateUs));
        ticks = 0;
        fetches = 0;
        worstLateUs = 0;
      }
    }

    std::this_thread::sleep_until(deadline);
  }
}

// ---- Main-thread sampling ------------------------------------------------

void PluginEngine::SampleOutputs()
{
  m_lampChanges.clear();
  m_giChanges.clear();

  uint8_t value = 0;
  for (size_t i = 0; i < m_lamps.size(); ++i)
  {
    m_lamps[i].Get(m_lamps[i].context, &value);
    const uint8_t level = value != 0 ? 1 : 0;
    if (level != m_lastLamp[i])
    {
      m_lastLamp[i] = level;
      m_lampChanges.push_back({m_lamps[i].number, level});
    }
  }
  for (size_t i = 0; i < m_gis.size(); ++i)
  {
    m_gis[i].Get(m_gis[i].context, &value);
    if (value != m_lastGi[i])
    {
      m_lastGi[i] = value;
      m_giChanges.push_back({m_gis[i].number, value});
    }
  }
}

void PluginEngine::PollChangedLamps(std::vector<GameEngineOutputChange>& changes) { changes.swap(m_lampChanges); }

void PluginEngine::PollChangedGis(std::vector<GameEngineOutputChange>& changes)
{
  if (!HasCapability(Capability::ChangedGis))
  {
    changes.clear();
    return;
  }
  changes.swap(m_giChanges);
}

void PluginEngine::SendSwitch(int number, uint8_t state)
{
  // Preserved from PinmameEngine: 200..241 are board-local and never reach the
  // ROM, and >=241 maps into PinMAME's negative cabinet-switch space.
  if (number >= 200 && number <= 241)
  {
    return;
  }
  const int sw = (number < 241) ? number : 240 - number;
  auto it = m_switchesByNumber.find(sw);
  if (it == m_switchesByNumber.end())
  {
    return;
  }
  const uint8_t value = state == 0 ? 0 : 1;
  it->second.Set(it->second.context, &value);
}

// ---- Lifecycle -----------------------------------------------------------

bool PluginEngine::Start(std::string& error)
{
  if (m_started)
  {
    error = "PluginEngine is already started";
    return false;
  }
  if (m_options.rom.empty())
  {
    error = "no ROM configured";
    return false;
  }

  const MsgPluginAPI& api = m_bus.Api();
  m_pinmameEndpoint = api.GetPluginEndpoint(m_options.pluginId.c_str());
  if (m_pinmameEndpoint == 0)
  {
    error = "plugin '" + m_options.pluginId + "' is not loaded";
    return false;
  }

  m_getMachineStateId = api.GetMsgID(PMPI_NAMESPACE, PMPI_GET_MACHINE_STATE);
  m_readMemoryId = api.GetMsgID(PMPI_NAMESPACE, PMPI_READ_MEMORY);
  m_onAudioCmdId = api.GetMsgID(PMPI_NAMESPACE, PMPI_EVT_ON_AUDIO_CMD);

  m_impl->controllers = std::make_unique<CtrlItemConsumer<ControllerDef>>(
      &api, m_bus.HostEndpointId(), CTLPI_CONTROLLERS_GET_MSG, CTLPI_CONTROLLERS_ON_CHG_MSG,
      [this](std::vector<ControllerDef>& items)
      {
        std::erase_if(items, [this](const ControllerDef& c) { return c.endpointId != m_pinmameEndpoint; });
      },
      []() {}, [this]() { OnControllersChanged(); });
  m_impl->controllers->Subscribe();

  m_impl->states = std::make_unique<CtrlItemConsumer<StateSrcId>>(
      &api, m_bus.HostEndpointId(), CTLPI_STATE_GET_SRC_MSG, CTLPI_STATE_ON_SRC_CHG_MSG,
      [](std::vector<StateSrcId>&) {}, [this]() { OnStateSrcAboutToChange(); }, [this]() { OnStateSrcChanged(); });
  m_impl->states->Subscribe();

  // PinMAME is entered only through its COM override; there is no bus message
  // that starts a ROM, and PPUC has no table script to do it the way VPX does.
  const ScriptClassDef* classDef = m_bus.ComOverride("VPinMAME.Controller");
  if (classDef == nullptr || !m_controller.Create(classDef))
  {
    error = "VPinMAME.Controller is not available from the plugin script API";
    return false;
  }

  ScriptVariant arg{};
  const size_t len = m_options.rom.length() + 1;
  char* buffer = new char[len];
  std::memcpy(buffer, m_options.rom.c_str(), len);
  arg.vString = {[](ScriptString* s) { delete[] s->string; }, buffer};
  const bool nameSet = m_controller.Call("GameName", {"string"}, &arg);
  if (arg.vString.Release != nullptr)
  {
    arg.vString.Release(&arg.vString);
  }
  if (!nameSet)
  {
    error = "could not set GameName on VPinMAME.Controller";
    return false;
  }
  std::string scriptError;
  if (m_bus.TakeLastScriptError(&scriptError))
  {
    error = "unknown ROM '" + m_options.rom + "': " + scriptError;
    return false;
  }

  // Blocks until PinMAME is either running or has given up.
  if (!m_controller.Call("Run", {}))
  {
    error = "VPinMAME.Controller has no Run()";
    return false;
  }

  ScriptVariant running{};
  if (m_controller.Call("Running", {}, nullptr, &running) && running.vBool == 0)
  {
    error = "PinMAME failed to start ROM '" + m_options.rom + "'";
    return false;
  }

  m_started = true;
  m_stopRequested.store(false, std::memory_order_release);
  // Spawned with the gate shut: it idles until OnStateSrcChanged publishes a
  // plan, which happens once the game has actually come up.
  m_pollThread = std::thread(&PluginEngine::PollThreadMain, this);
  return true;
}

void PluginEngine::Stop()
{
  if (!m_started)
  {
    return;
  }
  m_stopping = true;

  // Join first. This is what replaces PinmameStop()'s join of the emulation
  // thread as the guarantee that no sink fires during teardown, and it is also
  // what makes unsubscribing safe: the quiesce handshake then finds the gate
  // already idle.
  m_stopRequested.store(true, std::memory_order_release);
  if (m_pollThread.joinable())
  {
    m_pollThread.join();
  }

  if (m_controller.IsValid())
  {
    m_controller.Call("Stop", {});
  }

  // Mandatory: CtrlItemConsumer's destructor asserts it is not still subscribed.
  if (m_impl->states)
  {
    m_impl->states->Unsubscribe();
    m_impl->states.reset();
  }
  if (m_impl->controllers)
  {
    m_impl->controllers->Unsubscribe();
    m_impl->controllers.reset();
  }

  const MsgPluginAPI& api = m_bus.Api();
  for (unsigned int* id : {&m_getMachineStateId, &m_readMemoryId, &m_onAudioCmdId})
  {
    if (*id != 0)
    {
      api.ReleaseMsgID(*id);
      *id = 0;
    }
  }

  m_controller.Release();
  m_coilPlan.reset();
  m_started = false;
}

bool PluginEngine::TryGetIdentity(Identity* pIdentity) const
{
  if (m_identityKnown)
  {
    if (pIdentity != nullptr)
    {
      *pIdentity = m_identity;
    }
    return true;
  }
  if (m_pinmameEndpoint == 0 || m_getMachineStateId == 0)
  {
    return false;
  }

  PinMAMEMachineStateMsg msg{};
  msg.version = 1;
  m_bus.Api().SendMsg(m_bus.HostEndpointId(), m_getMachineStateId, m_pinmameEndpoint, &msg);
  // The handler returns early when PinMAME is not running and never writes
  // anything, so a null game is the "not yet" signal.
  if (msg.game == nullptr)
  {
    return false;
  }

  // Copy immediately: these point at emulator globals.
  m_identity.name = msg.game;
  m_identity.hardwareGen = msg.hardwareGen;
  m_identity.description = DescribeHardwareGen(msg.hardwareGen);
  m_identityKnown = true;
  if (pIdentity != nullptr)
  {
    *pIdentity = m_identity;
  }
  return true;
}

void PluginEngine::PollTrackedState()
{
  if (!m_identityKnown || m_pHost == nullptr)
  {
    return;
  }
  if (!m_triedLoadingTracking)
  {
    m_triedLoadingTracking = true;
    std::string trackingError;
    // A ROM nobody has mapped is a normal outcome, not a failure.
    if (!TryLoadPinmameTrackingConfig(m_identity.name.c_str(), m_identity.hardwareGen,
                                      m_options.pinmamePath.empty() ? nullptr : m_options.pinmamePath.c_str(),
                                      &m_tracking, &trackingError)
        && m_options.debug)
    {
      std::printf("PluginEngine: no NVRAM map for %s: %s\n", m_identity.name.c_str(), trackingError.c_str());
    }
  }
  if (!m_tracking.loaded)
  {
    return;
  }

  const uint64_t now = NowMs();
  if (now < m_nextTrackedPollMs)
  {
    return;
  }
  m_nextTrackedPollMs = now + kTrackedStatePollIntervalMs;

  // PMPI_READ_MEMORY replaces PinmameReadMainCPUByte. The decoder already took
  // an injected reader, which is why it needs no change at all.
  const PinmameByteReader read = [this](uint32_t address, uint8_t* pValue)
  {
    PinMAMEReadMemoryMsg msg{};
    msg.version = 1;
    msg.address = address;
    msg.size = 1;
    msg.data = pValue;
    msg.read = 0;
    m_bus.Api().SendMsg(m_bus.HostEndpointId(), m_readMemoryId, m_pinmameEndpoint, &msg);
    return msg.read == 1;
  };

  uint8_t ball = 0;
  if (TryDecodeTrackedPinmameValue(m_tracking.currentBall, read, &ball) && (!m_hasLastBall || ball != m_lastBall))
  {
    m_hasLastBall = true;
    m_lastBall = ball;
    m_pHost->OnCurrentBallChanged(ball);
  }
  uint8_t player = 0;
  if (TryDecodeTrackedPinmameValue(m_tracking.currentPlayer, read, &player) && (!m_hasLastPlayer || player != m_lastPlayer))
  {
    m_hasLastPlayer = true;
    m_lastPlayer = player;
    m_pHost->OnCurrentPlayerChanged(player);
  }
}

void PluginEngine::Update()
{
  if (!m_started)
  {
    return;
  }
  if (!m_identityKnown)
  {
    TryGetIdentity(nullptr);
  }

  // Throttled deliberately: the main loop ticks every 20 us, and each sample
  // now costs a PWM integration inside libpinmame rather than a cached diff.
  const uint64_t now = NowMs();
  if (now >= m_nextOutputSampleMs && m_gateOpen.load(std::memory_order_acquire))
  {
    m_nextOutputSampleMs = now + static_cast<uint64_t>(1000 / std::max(1, m_options.outputPollHz));
    SampleOutputs();
  }

  PollTrackedState();
}
