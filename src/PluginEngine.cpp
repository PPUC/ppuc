#include "PluginEngine.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <unordered_set>

#include "DmdSourceSelect.h"
#include "PinmameNvramMapLoader.h"
#include "PluginBus.h"
#include "SegmentDigitDecode.h"
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
  std::unique_ptr<CtrlItemConsumer<SegSrcId>> segSources;
  std::unique_ptr<CtrlItemConsumer<DisplaySrcId>> displaySources;

  // The one display PPUC renders, resolved to the provider's accessor. Null
  // when the machine has no DMD, which is the normal case for the alphanumeric
  // games PPUC mostly runs.
  struct DmdSource
  {
    void* context = nullptr;
    DisplayFrame(MSGPIAPI* GetIdentifyFrame)(void*) = nullptr;
    DisplayFrame(MSGPIAPI* GetRenderFrame)(void*) = nullptr;
    DmdSourceSelect::FrameSource source = DmdSourceSelect::FrameSource::None;
    unsigned int width = 0;
    unsigned int height = 0;
    int depth = 0;
    unsigned int lastFrameId = 0;
    bool hasFrame = false;
    unsigned int framesSinceReport = 0;
    unsigned int pollsSinceReport = 0;
    uint64_t nextReportMs = 0;
  };
  DmdSource dmd;
  bool hasDmd = false;

  // One published segment display, resolved to the provider's accessor.
  struct SegmentDisplay
  {
    uint32_t resId = 0;
    unsigned int elements = 0;
    // Where this display's first element lands in the flat digit numbering the
    // B2S host expects. Assigned by walking displays in resId order with an
    // nElements stride: libpinmame numbers them in sorted layout order (top,
    // then left), so this is stable across runs and matches reading order.
    int digitBase = 0;
    void* context = nullptr;
    // Per element: how many of its sixteen floats the provider actually writes.
    std::vector<int> segmentCounts;
    SegDisplayFrame(MSGPIAPI* Get)(void*) = nullptr;
    unsigned int lastFrameId = 0;
    bool hasFrame = false;
    // Per element, carried across polls: the mask feeds the hysteresis and the
    // digit suppresses repeat dispatches.
    std::vector<uint16_t> lastMask;
    std::vector<int> lastDigit;
  };
  std::vector<SegmentDisplay> segments;

  // Sound commands already seen this run, so the debug print can flag first
  // sightings. Lives here rather than on the engine because the bus trampoline
  // below is all that touches it.
  std::unordered_set<uint64_t> seenSoundCommands;

  static void MSGPIAPI OnAudioCmd(unsigned int, void* userData, void* msgData)
  {
    auto* self = static_cast<Impl*>(userData);
    const auto* msg = static_cast<const PinMAMEChildBoardEventMsg*>(msgData);
    if (self == nullptr || msg == nullptr)
    {
      return;
    }
    // "new" marks the first sighting this run, which is the point when
    // transcribing a pack: a ROM repeats the same handful of commands
    // constantly and only the first of each is worth writing down.
    const uint64_t key = (static_cast<uint64_t>(msg->boardNo) << 32) | msg->cmd;
    const bool firstSeen = self->seenSoundCommands.insert(key).second;
    std::printf("PinMAME sound command: board=%u id=%u hex=0x%X new=%d\n", msg->boardNo, msg->cmd, msg->cmd,
                firstSeen ? 1 : 0);
  }
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
                  switches[number] = {def.callContext, def.SetState, def.GetState};
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
  // The accessors were all just replaced. Whatever the engine believed about
  // these switches came through the old ones, so re-apply the ledger before
  // anything reads it: a source change in the middle of a game must not leave
  // the ROM with a switch it can no longer be told about.
  m_nextSwitchAuditMs = 0;
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
  const bool present =
      m_impl->controllers->With([](const std::vector<ControllerDef>& items) { return !items.empty(); });
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
          // A game starting invalidates what we last told the host about the
          // ball and the player, whatever the ROM's memory still reads.
          //
          // The host forces both to 0 when a game ends -- ball 0 means "no game"
          // to the rules -- and it does that directly, so this cache never hears
          // about it. A one-player game then starts again with the ROM reading
          // player 1, which is what the cache still holds, so nothing is
          // reported and the rules keep the 0 they were given at game over.
          // Every rule keyed on currentPlayer() was therefore dead from the
          // second game of a session onwards, and only a restart brought it
          // back. Re-announcing at the start of each game costs one poll.
          if (value != 0)
          {
            m_forgetTracked.store(true, std::memory_order_release);
          }
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

void PluginEngine::OnSegSrcChanged()
{
  std::vector<Impl::SegmentDisplay> next;

  m_impl->segSources->With(
      [&](const std::vector<SegSrcId>& sources)
      {
        std::vector<const SegSrcId*> mine;
        for (const SegSrcId& src : sources)
        {
          if (src.id.endpointId == m_pinmameEndpoint && src.GetState != nullptr && src.nElements != 0)
          {
            mine.push_back(&src);
          }
        }
        // resId order, not publication order: the digit numbering the backglass
        // sees must not depend on how the list happened to be assembled.
        std::sort(mine.begin(), mine.end(),
                  [](const SegSrcId* a, const SegSrcId* b) { return a->id.resId < b->id.resId; });

        int digitBase = 0;
        for (const SegSrcId* src : mine)
        {
          Impl::SegmentDisplay display;
          display.resId = src->id.resId;
          display.elements = src->nElements;
          display.digitBase = digitBase;
          display.context = src->callContext;
          display.Get = src->GetState;
          display.lastMask.assign(src->nElements, 0);
          display.lastDigit.assign(src->nElements, -2);
          display.segmentCounts.reserve(src->nElements);
          for (unsigned int i = 0; i < src->nElements; ++i)
          {
            display.segmentCounts.push_back(
                SegmentDigitDecode::SegmentCountForLayout(static_cast<int>(src->elementType[i])));
          }
          digitBase += static_cast<int>(src->nElements);
          next.push_back(std::move(display));
        }
      });

  // Carry the per-element history across a republish. Losing it would restart
  // the hysteresis from "all off" and blink the whole display.
  for (Impl::SegmentDisplay& display : next)
  {
    for (const Impl::SegmentDisplay& previous : m_impl->segments)
    {
      if (previous.resId == display.resId && previous.elements == display.elements)
      {
        display.lastMask = previous.lastMask;
        display.lastDigit = previous.lastDigit;
        break;
      }
    }
  }

  m_impl->segments = std::move(next);

  if (m_options.debug)
  {
    std::printf("PluginEngine: %zu segment displays\n", m_impl->segments.size());
    for (const Impl::SegmentDisplay& display : m_impl->segments)
    {
      std::printf("  display %u: %u elements, digits %d..%d\n", display.resId, display.elements, display.digitBase,
                  display.digitBase + static_cast<int>(display.elements) - 1);
    }
  }
}

void PluginEngine::OnDisplaySrcChanged()
{
  static_assert(CTLPI_DISPLAY_ID_FORMAT_BITPLANE2 == 1u && CTLPI_DISPLAY_ID_FORMAT_BITPLANE4 == 2u,
                "DmdSourceSelect mirrors these identify formats to stay free of the plugin SDK");
  static_assert(CTLPI_DISPLAY_FORMAT_SRGB888 == 2u && CTLPI_DISPLAY_FORMAT_SRGB565 == 3u,
                "DmdSourceSelect mirrors these frame formats to stay free of the plugin SDK");

  m_impl->hasDmd = false;

  m_impl->displaySources->With(
      [&](const std::vector<DisplaySrcId>& sources)
      {
        // Every published display is a candidate, whichever endpoint it came
        // from. PPUC runs exactly one controller, so there is nothing to confuse
        // its displays with, and the alternatives all get something wrong: the
        // controller's own displays are absent on an alphanumeric machine, where
        // alphadmd supplies them, and they are the *wrong* pick when a colorizer
        // is running, because the colorized output lives on the colorizer's
        // endpoint. Selection sorts it out from the override chain.
        std::vector<const DisplaySrcId*> published;
        std::vector<DmdSourceSelect::Candidate> candidates;
        for (const DisplaySrcId& src : sources)
        {
          DmdSourceSelect::Candidate candidate;
          candidate.id = src.id.id;
          candidate.overrideId = src.overrideId.id;
          candidate.resId = src.id.resId;
          candidate.width = src.width;
          candidate.height = src.height;
          candidate.identifyFormat = src.identifyFormat;
          candidate.hasIdentifyFrame = src.GetIdentifyFrame != nullptr;
          candidate.frameFormat = src.frameFormat;
          candidate.hasRenderFrame = src.GetRenderFrame != nullptr;
          published.push_back(&src);
          candidates.push_back(candidate);
        }

        const int chosen = DmdSourceSelect::SelectMainDisplay(candidates);
        if (chosen < 0)
        {
          return;
        }

        const DisplaySrcId& src = *published[static_cast<size_t>(chosen)];
        m_impl->dmd = {};
        m_impl->dmd.context = src.callContext;
        m_impl->dmd.GetIdentifyFrame = src.GetIdentifyFrame;
        m_impl->dmd.GetRenderFrame = src.GetRenderFrame;
        m_impl->dmd.source = DmdSourceSelect::SourceFor(candidates[static_cast<size_t>(chosen)]);
        m_impl->dmd.width = src.width;
        m_impl->dmd.height = src.height;
        m_impl->dmd.depth = DmdSourceSelect::DepthForIdentifyFormat(src.identifyFormat);
        m_impl->hasDmd = true;

        if (m_options.debug)
        {
          const char* how = m_impl->dmd.source == DmdSourceSelect::FrameSource::Identify      ? "indexed"
                            : m_impl->dmd.source == DmdSourceSelect::FrameSource::RenderRgb16 ? "colorized RGB16"
                                                                                              : "colorized RGB24";
          std::printf("PluginEngine: DMD display %u.%u, %ux%u, %s (of %zu published)\n", src.id.endpointId,
                      src.id.resId, src.width, src.height, how, candidates.size());
        }
      });
}

void PluginEngine::SampleDmd()
{
  if (!m_impl->hasDmd || m_pHost == nullptr)
  {
    return;
  }

  Impl::DmdSource& dmd = m_impl->dmd;
  ++dmd.pollsSinceReport;

  const bool colorized = dmd.source != DmdSourceSelect::FrameSource::Identify;
  const DisplayFrame frame = colorized ? dmd.GetRenderFrame(dmd.context) : dmd.GetIdentifyFrame(dmd.context);
  if (frame.frame == nullptr)
  {
    return;
  }
  if (dmd.hasFrame && frame.frameId == dmd.lastFrameId)
  {
    return;
  }
  dmd.lastFrameId = frame.frameId;
  dmd.hasFrame = true;

  switch (dmd.source)
  {
    case DmdSourceSelect::FrameSource::Identify:
      m_pHost->OnDmdFrame(static_cast<const uint8_t*>(frame.frame), dmd.depth, static_cast<int>(dmd.width),
                          static_cast<int>(dmd.height));
      break;
    case DmdSourceSelect::FrameSource::RenderRgb16:
      m_pHost->OnDmdRgb16Frame(static_cast<const uint16_t*>(frame.frame), static_cast<int>(dmd.width),
                               static_cast<int>(dmd.height));
      break;
    case DmdSourceSelect::FrameSource::RenderRgb24:
      m_pHost->OnDmdRgb24Frame(static_cast<const uint8_t*>(frame.frame), static_cast<int>(dmd.width),
                               static_cast<int>(dmd.height));
      break;
    case DmdSourceSelect::FrameSource::None:
      break;
  }
  ++dmd.framesSinceReport;
}

void PluginEngine::ReportDmdRate()
{
  // Reported on a wall clock rather than when a frame arrives, so a display
  // producing nothing prints "0 frames/s" instead of printing nothing at all.
  // Silence is the case worth seeing: a wrongly selected display, a stalled
  // emulation and a machine sitting on a static screen all look identical, and
  // only the number tells them apart from a report that never ran.
  if (!m_options.debug || !m_impl->hasDmd)
  {
    return;
  }

  Impl::DmdSource& dmd = m_impl->dmd;
  const uint64_t now = NowMs();
  if (dmd.nextReportMs == 0)
  {
    dmd.nextReportMs = now + 1000;
    return;
  }
  if (now < dmd.nextReportMs)
  {
    return;
  }
  // Polls as well as frames. The sample runs from the main loop, so a low poll
  // rate means the loop itself is stalling and the frame count below it is an
  // undercount rather than a quiet ROM -- a distinction the callback-driven
  // engine never had to make.
  // Depth only means anything for an indexed frame; a colorized one carries its
  // own colour, and "depth 0" printed beside it just looks like a fault.
  if (dmd.source == DmdSourceSelect::FrameSource::Identify)
  {
    std::printf("DMD: %u frames/s (%u polls/s), %ux%u depth %d\n", dmd.framesSinceReport, dmd.pollsSinceReport,
                dmd.width, dmd.height, dmd.depth);
  }
  else
  {
    std::printf("DMD: %u frames/s (%u polls/s), %ux%u colorized\n", dmd.framesSinceReport, dmd.pollsSinceReport,
                dmd.width, dmd.height);
  }
  dmd.framesSinceReport = 0;
  dmd.pollsSinceReport = 0;
  dmd.nextReportMs = now + 1000;
}

void PluginEngine::SampleSegments()
{
  if (m_pHost == nullptr)
  {
    return;
  }

  for (Impl::SegmentDisplay& display : m_impl->segments)
  {
    const SegDisplayFrame frame = display.Get(display.context);
    if (frame.frame == nullptr)
    {
      continue;
    }
    // frameId only bumps when the luminances actually changed, so an idle
    // display costs one indirect call and a compare. It is not enough on its
    // own, though: the first frame after a source change legitimately carries
    // the id it already had.
    if (display.hasFrame && frame.frameId == display.lastFrameId)
    {
      continue;
    }
    display.lastFrameId = frame.frameId;
    display.hasFrame = true;

    int score = 0;
    bool hasScoreDigit = false;
    for (unsigned int i = 0; i < display.elements; ++i)
    {
      const float* luminances = frame.frame + i * SegmentDigitDecode::kSegmentsPerElement;
      const uint16_t mask =
          SegmentDigitDecode::MaskFromLuminance(luminances, display.segmentCounts[i], display.lastMask[i]);
      display.lastMask[i] = mask;

      const int digit = SegmentDigitDecode::DecodeDigit(mask);
      if (digit != display.lastDigit[i])
      {
        display.lastDigit[i] = digit;
        m_pHost->OnSegmentDigit(display.digitBase + static_cast<int>(i), digit);
      }

      // Preserved from PinmameEngine: leading blanks are skipped, but a blank
      // after a digit is a trailing zero the display simply is not lighting.
      if (digit >= 0)
      {
        hasScoreDigit = true;
        score = score * 10 + digit;
      }
      else if (hasScoreDigit)
      {
        score *= 10;
      }
    }

    if (hasScoreDigit)
    {
      m_pHost->OnPlayerScore(static_cast<int>(display.resId) + 1, score);
    }

    if (m_options.debugSegments)
    {
      std::printf("Segment display %u base=%d:", display.resId, display.digitBase);
      for (unsigned int i = 0; i < display.elements; ++i)
      {
        // The mask as well as the digit: a display that renders nothing is
        // almost always segments that never crossed the threshold, and only the
        // mask distinguishes that from a mask the table has no digit for.
        std::printf(" %04X", display.lastMask[i]);
        if (display.lastDigit[i] >= 0)
        {
          std::printf("(%d)", display.lastDigit[i]);
        }
      }
      if (hasScoreDigit)
      {
        std::printf(" score=%d", score);
      }
      std::printf("\n");
    }
  }
}

// Hands over what SampleOutputs collected, and leaves nothing behind.
//
// The caller's vector is emptied first on purpose. A bare swap put the
// caller's previous list back into the engine, and the main loop polls far
// more often than SampleOutputs runs - every 20us against outputPollHz - so
// the two vectors traded the same changes back and forth and every poll in
// between delivered them again. A lamp that came on once was reported as
// coming on over and over, without ever going off: rules that act when a lamp
// lights fired repeatedly, and the same false changes reached the media host
// and the boards.
bool PluginEngine::SetPaused(bool paused)
{
  if (!m_started || !m_controller.IsValid())
  {
    return false;
  }
  // VPinMAME.Controller exposes Pause as a read/write property, which reaches
  // PinmamePause underneath: the emulation stops where it is rather than being
  // starved of input, so the ball in play is still in play afterwards.
  ScriptVariant arg{};
  arg.vBool = paused ? 1 : 0;
  if (!m_controller.Call("Pause", {"bool"}, &arg))
  {
    return false;
  }

  // Read it back. A property that took the call but did not take the value
  // would leave the tests firing coils into a running game, which is the one
  // outcome worth being sure about.
  ScriptVariant state{};
  if (m_controller.Call("Pause", {}, nullptr, &state))
  {
    return (state.vBool != 0) == paused;
  }
  return true;
}

void PluginEngine::PollChangedLamps(std::vector<GameEngineOutputChange>& changes)
{
  changes.clear();
  changes.swap(m_lampChanges);
}

void PluginEngine::PollChangedGis(std::vector<GameEngineOutputChange>& changes)
{
  if (!HasCapability(Capability::ChangedGis))
  {
    changes.clear();
    return;
  }
  changes.clear();
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
  const uint8_t value = state == 0 ? 0 : 1;
  // The ledger records what the engine was told, not what the machine is
  // doing. A switch the rules deliberately keep from the ROM stays kept from
  // it: the audit repeats this, and never contradicts a decision made above.
  //
  // Written before the accessor is looked up, and deliberately so. The boards
  // report their switches as soon as the bus is up, which is before the plugin
  // has announced its mappings; a state dropped here would never be reported
  // again, because libppuc only reports changes. Remembering it means the audit
  // applies it the moment the mappings arrive -- and the audit runs at once
  // after a mappings change, so nothing waits a second for it.
  m_sentSwitchValues[sw] = value;
  auto it = m_switchesByNumber.find(sw);
  if (it == m_switchesByNumber.end())
  {
    m_unsentSwitches.insert(sw);
    return;
  }
  m_unsentSwitches.erase(sw);
  it->second.Set(it->second.context, &value);
}

void PluginEngine::AuditSwitches()
{

  for (const auto& [number, value] : m_sentSwitchValues)
  {
    const auto it = m_switchesByNumber.find(number);
    if (it == m_switchesByNumber.end())
    {
      continue;
    }

    const bool neverSent = m_unsentSwitches.erase(number) != 0;
    if (!neverSent && it->second.Get != nullptr)
    {
      // Compared before writing, so the ordinary case costs a read and
      // nothing else. Nothing is re-sent that the engine already agrees
      // with, which is what keeps this from being able to score a point it
      // should not: a switch is a level, and the level is not touched unless
      // it is wrong.
      uint8_t current = 0;
      it->second.Get(it->second.context, &current);
      const uint8_t normalized = current == 0 ? 0 : 1;
      if (normalized == value)
      {
        continue;
      }
      ++m_switchCorrections;
      std::printf("PluginEngine: switch %d reads %u in the engine but was set to %u; correcting (%u so far)\n",
                  number, static_cast<unsigned>(normalized), static_cast<unsigned>(value), m_switchCorrections);
    }
    it->second.Set(it->second.context, &value);
  }
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
  // Observed, never forwarded. libpinmame broadcasts this itself, so AltSound
  // and anything else on the bus already have it; relaying it through
  // GameEngineHost::OnSoundCommand would deliver every command twice. PPUC
  // subscribes only to print them, which is how an AltSound pack gets built.
  if (m_options.debugSoundCommands)
  {
    api.SubscribeMsg(m_bus.HostEndpointId(), m_onAudioCmdId, &Impl::OnAudioCmd, m_impl.get());
    m_subscribedAudioCmd = true;
  }

  m_impl->controllers = std::make_unique<CtrlItemConsumer<ControllerDef>>(
      &api, m_bus.HostEndpointId(), CTLPI_CONTROLLERS_GET_MSG, CTLPI_CONTROLLERS_ON_CHG_MSG,
      [this](std::vector<ControllerDef>& items)
      { std::erase_if(items, [this](const ControllerDef& c) { return c.endpointId != m_pinmameEndpoint; }); }, []() {},
      [this]() { OnControllersChanged(); });
  m_impl->controllers->Subscribe();

  m_impl->states = std::make_unique<CtrlItemConsumer<StateSrcId>>(
      &api, m_bus.HostEndpointId(), CTLPI_STATE_GET_SRC_MSG, CTLPI_STATE_ON_SRC_CHG_MSG,
      [](std::vector<StateSrcId>&) {}, [this]() { OnStateSrcAboutToChange(); }, [this]() { OnStateSrcChanged(); });
  m_impl->states->Subscribe();

  // No about-to-change hook, unlike the solenoids: segments are only ever read
  // from Update() on the main thread, which is the same thread this callback
  // arrives on, so the accessors cannot go stale mid-poll.
  m_impl->segSources = std::make_unique<CtrlItemConsumer<SegSrcId>>(
      &api, m_bus.HostEndpointId(), CTLPI_SEG_GET_SRC_MSG, CTLPI_SEG_ON_SRC_CHG_MSG, [](std::vector<SegSrcId>&) {},
      []() {}, [this]() { OnSegSrcChanged(); });
  m_impl->segSources->Subscribe();

  // Same main-thread argument as the segments: polled only from Update(), and
  // this callback arrives on the same thread, so no gate is needed.
  m_impl->displaySources = std::make_unique<CtrlItemConsumer<DisplaySrcId>>(
      &api, m_bus.HostEndpointId(), CTLPI_DISPLAY_GET_SRC_MSG, CTLPI_DISPLAY_ON_SRC_CHG_MSG,
      [](std::vector<DisplaySrcId>&) {}, []() {}, [this]() { OnDisplaySrcChanged(); });
  m_impl->displaySources->Subscribe();

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
  if (m_impl->displaySources)
  {
    m_impl->displaySources->Unsubscribe();
    m_impl->displaySources.reset();
  }
  m_impl->hasDmd = false;
  if (m_impl->segSources)
  {
    m_impl->segSources->Unsubscribe();
    m_impl->segSources.reset();
  }
  m_impl->segments.clear();
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
  if (m_subscribedAudioCmd)
  {
    api.UnsubscribeMsg(m_onAudioCmdId, &Impl::OnAudioCmd, m_impl.get());
    m_subscribedAudioCmd = false;
  }
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
    // A ROM nobody has mapped is a normal outcome, not a failure -- but it is
    // reported either way, and not only under --debug.
    //
    // Ball and player numbers are what onBallChanged, onPlayerChanged, the
    // per-ball reset of tilt warnings and ball-save arming are all built on. A
    // ROM with no map has none of them, and used to say so only to somebody who
    // had already guessed to turn debugging on: from the outside a rule that
    // never fires looks like a broken rule rather than a game the map does not
    // cover. One line at startup is the difference between a mystery and a fact.
    const bool tracking = TryLoadPinmameTrackingConfig(
        m_identity.name.c_str(), m_identity.hardwareGen,
        m_options.pinmamePath.empty() ? nullptr : m_options.pinmamePath.c_str(), &m_tracking, &trackingError);
    if (tracking)
    {
      std::printf("Ball and player tracking: %s\n", m_tracking.mapPath.c_str());
    }
    else
    {
      std::printf(
          "Ball and player tracking unavailable for %s: %s\n"
          "  onBallChanged and onPlayerChanged will not fire, and per-ball tilt warnings and ball save are off.\n",
          m_identity.name.c_str(), trackingError.c_str());
    }
  }
  if (!m_tracking.loaded)
  {
    return;
  }

  // Set from the poll thread when a game starts; taken here, on the thread that
  // owns the cache, so the two never touch the same fields.
  if (m_forgetTracked.exchange(false, std::memory_order_acq_rel))
  {
    m_hasLastBall = false;
    m_hasLastPlayer = false;
    m_nextTrackedPollMs = 0;
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
  if (TryDecodeTrackedPinmameValue(m_tracking.currentBall, read, &ball))
  {
    m_undecodableBall = false;
    if (!m_hasLastBall || ball != m_lastBall)
    {
      m_hasLastBall = true;
      m_lastBall = ball;
      m_pHost->OnCurrentBallChanged(ball);
    }
  }
  else
  {
    ReportUndecodableTrackedField("ball", m_tracking.currentBall, read, m_undecodableBall);
  }

  uint8_t player = 0;
  if (TryDecodeTrackedPinmameValue(m_tracking.currentPlayer, read, &player))
  {
    m_undecodablePlayer = false;
    if (!m_hasLastPlayer || player != m_lastPlayer)
    {
      m_hasLastPlayer = true;
      m_lastPlayer = player;
      m_pHost->OnCurrentPlayerChanged(player);
    }
  }
  else
  {
    ReportUndecodableTrackedField("player", m_tracking.currentPlayer, read, m_undecodablePlayer);
  }
}

// A mapped field the ROM's memory does not answer for.
//
// The decoder refuses a byte whose nibbles are not valid BCD, which is the right
// answer -- 0xFF is not ball 255 -- but refusing silently means currentBall()
// simply stays 0, and a rule keyed on it never fires with nothing anywhere
// saying why. Uninitialised RAM reads 0xFF, so this is what a ROM that did not
// start cleanly looks like from the outside.
//
// Reported once per spell, not once per poll: the poll runs several times a
// second and a stuck field would otherwise fill the log.
void PluginEngine::ReportUndecodableTrackedField(const char* what, const PinmameTrackedField& field,
                                                 const PinmameByteReader& read, bool& alreadyReported)
{
  if (alreadyReported || !field.available)
  {
    return;
  }
  alreadyReported = true;

  uint8_t raw = 0;
  if (!read(field.address, &raw))
  {
    std::printf("Tracking: cannot read the %s at 0x%02X; PinMAME did not answer the memory read.\n", what,
                static_cast<unsigned>(field.address));
    return;
  }
  std::printf(
      "Tracking: the %s at 0x%02X reads 0x%02X, which does not decode; currentBall()/currentPlayer() stay 0 and rules\n"
      "  keyed on them cannot fire. Uninitialised or disturbed ROM memory reads like this.\n",
      what, static_cast<unsigned>(field.address), static_cast<unsigned>(raw));
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

  // Once a second. Rare enough to cost nothing, often enough that a divergence
  // is gone before a player notices the ball has not come back.
  const uint64_t nowMs = NowMs();
  if (nowMs >= m_nextSwitchAuditMs)
  {
    m_nextSwitchAuditMs = nowMs + 1000;
    AuditSwitches();
  }

  // Throttled deliberately: the main loop ticks every 20 us, and each sample
  // now costs a PWM integration inside libpinmame rather than a cached diff.
  const uint64_t now = NowMs();
  if (now >= m_nextOutputSampleMs && m_gateOpen.load(std::memory_order_acquire))
  {
    m_nextOutputSampleMs = now + static_cast<uint64_t>(1000 / std::max(1, m_options.outputPollHz));
    SampleOutputs();
  }

  // 60 Hz: a segment display is a 60 Hz device and sampling faster only costs
  // PWM integrations inside libpinmame.
  if (now >= m_nextSegmentSampleMs)
  {
    m_nextSegmentSampleMs = now + 16;
    SampleSegments();
  }

  // Faster than the segments, and not for smoothness: the identify frame is
  // what a Serum colorization keys on, and a ROM can put out frames faster than
  // 60 Hz. Sampling at the display's own rate would alias them, and a missed
  // key frame is a missed scene, not a dropped one.
  if (now >= m_nextDmdSampleMs)
  {
    m_nextDmdSampleMs = now + 8;
    SampleDmd();
  }
  ReportDmdRate();

  PollTrackedState();
}
