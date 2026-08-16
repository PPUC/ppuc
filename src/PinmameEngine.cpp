#include "PinmameEngine.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>
#include <type_traits>
#include <unordered_set>

#include "PinmameNvramMapLoader.h"
#include "io-boards/PPUCPlatforms.h"
#include "libpinmame.h"

namespace
{

// libpinmame's callback typedefs take `void* const p_userData`, while the
// implementations here (inherited from the original ppuc.cpp code) take
// `const void*`. The two are not interconvertible as function pointer types, so
// the config assembly reinterpret_casts each one. Keeping the odd parameter type
// is deliberate: changing it is a separate cleanup with its own risk, and the
// cast is exactly what the code did before this seam existed.
#define PINMAME_CALLBACK_CAST(type, fn) reinterpret_cast<type>(fn)

// libpinmame hands the log argument through as a va_list on most builds and as a
// char* on others. Recover whichever it is from the callback typedef so the
// formatting below compiles either way.
template <typename T>
struct LogCallbackTraits;

template <typename R, typename A1, typename A2, typename A3, typename A4>
struct LogCallbackTraits<R (*)(A1, A2, A3, A4)>
{
  using Arg3 = A3;
};

#if defined(_WIN32) && !defined(_WIN64)
template <typename R, typename A1, typename A2, typename A3, typename A4>
struct LogCallbackTraits<R(__stdcall*)(A1, A2, A3, A4)>
{
  using Arg3 = A3;
};
#endif

using PinmameLogMessageArg = LogCallbackTraits<PinmameOnLogMessageCallback>::Arg3;

// libpinmame stores the user-data pointer in a process-global, so there can only
// ever be one live engine. Kept as a fallback in case a callback arrives with a
// null p_userData; a null dereference on the libpinmame thread is a bad way to
// discover that.
PinmameEngine* g_pInstance = nullptr;

PinmameEngine* EngineFrom(const void* p_userData)
{
  if (p_userData != nullptr)
  {
    return static_cast<PinmameEngine*>(const_cast<void*>(p_userData));
  }
  return g_pInstance;
}

void PINMAMECALLBACK TrampolineOnStateUpdated(int state, const void* p_userData)
{
  if (PinmameEngine* pEngine = EngineFrom(p_userData)) pEngine->HandleStateUpdated(state);
}

void PINMAMECALLBACK TrampolineOnDisplayAvailable(int index, int displayCount, PinmameDisplayLayout* p_displayLayout,
                                                  const void* p_userData)
{
  if (PinmameEngine* pEngine = EngineFrom(p_userData))
    pEngine->HandleDisplayAvailable(index, displayCount, p_displayLayout);
}

void PINMAMECALLBACK TrampolineOnDisplayUpdated(int index, void* p_displayData, PinmameDisplayLayout* p_displayLayout,
                                                const void* p_userData)
{
  if (PinmameEngine* pEngine = EngineFrom(p_userData))
    pEngine->HandleDisplayUpdated(index, p_displayData, p_displayLayout);
}

int PINMAMECALLBACK TrampolineOnAudioAvailable(PinmameAudioInfo* p_audioInfo, const void* p_userData)
{
  if (PinmameEngine* pEngine = EngineFrom(p_userData)) return pEngine->HandleAudioAvailable(p_audioInfo);
  return p_audioInfo != nullptr ? p_audioInfo->samplesPerFrame : 0;
}

int PINMAMECALLBACK TrampolineOnAudioUpdated(void* p_buffer, int samples, const void* p_userData)
{
  if (PinmameEngine* pEngine = EngineFrom(p_userData)) return pEngine->HandleAudioUpdated(p_buffer, samples);
  return samples;
}

void PINMAMECALLBACK TrampolineOnMechAvailable(int mechNo, PinmameMechInfo* p_mechInfo, const void* p_userData)
{
  if (PinmameEngine* pEngine = EngineFrom(p_userData)) pEngine->HandleMechAvailable(mechNo, p_mechInfo);
}

void PINMAMECALLBACK TrampolineOnMechUpdated(int mechNo, PinmameMechInfo* p_mechInfo, const void* p_userData)
{
  if (PinmameEngine* pEngine = EngineFrom(p_userData)) pEngine->HandleMechUpdated(mechNo, p_mechInfo);
}

void PINMAMECALLBACK TrampolineOnSolenoidUpdated(PinmameSolenoidState* p_solenoidState, const void* p_userData)
{
  if (p_solenoidState == nullptr) return;
  if (PinmameEngine* pEngine = EngineFrom(p_userData))
    pEngine->HandleSolenoidUpdated(p_solenoidState->solNo, p_solenoidState->state);
}

void PINMAMECALLBACK TrampolineOnConsoleDataUpdated(void* p_data, int size, const void* p_userData)
{
  if (PinmameEngine* pEngine = EngineFrom(p_userData)) pEngine->HandleConsoleDataUpdated(size);
}

int PINMAMECALLBACK TrampolineIsKeyPressed(PINMAME_KEYCODE keycode, const void* p_userData) { return 0; }

template <typename T>
void FormatPinmameLogMessage(PinmameEngine* pEngine, PINMAME_LOG_LEVEL logLevel, const char* format, T arg)
{
  const char* logMessage = format;
  char buffer[1024];

  if constexpr (std::is_same_v<T, char*> || std::is_same_v<T, const char*>)
  {
    if (arg)
    {
      logMessage = arg;
    }
  }
  else
  {
    vsnprintf(buffer, sizeof(buffer), format, arg);
    logMessage = buffer;
  }

  if (logLevel == PINMAME_LOG_LEVEL_INFO)
  {
    pEngine->EmitLog(false, logMessage);
  }
  else if (logLevel == PINMAME_LOG_LEVEL_ERROR)
  {
    pEngine->EmitLog(true, logMessage);
  }
}

void PINMAMECALLBACK TrampolineOnLogMessage(PINMAME_LOG_LEVEL logLevel, const char* format, PinmameLogMessageArg arg,
                                            const void* p_userData)
{
  if (PinmameEngine* pEngine = EngineFrom(p_userData)) FormatPinmameLogMessage(pEngine, logLevel, format, arg);
}

void PINMAMECALLBACK TrampolineOnSoundCommand(int boardNo, int cmd, const void* p_userData)
{
  if (PinmameEngine* pEngine = EngineFrom(p_userData)) pEngine->HandleSoundCommand(boardNo, cmd);
}

bool IsSegmentDisplayType(int displayType)
{
  switch (displayType & PINMAME_DISPLAY_TYPE_SEGMASK)
  {
    case PINMAME_DISPLAY_TYPE_SEG16:
    case PINMAME_DISPLAY_TYPE_SEG16R:
    case PINMAME_DISPLAY_TYPE_SEG10:
    case PINMAME_DISPLAY_TYPE_SEG9:
    case PINMAME_DISPLAY_TYPE_SEG8:
    case PINMAME_DISPLAY_TYPE_SEG8D:
    case PINMAME_DISPLAY_TYPE_SEG7:
    case PINMAME_DISPLAY_TYPE_SEG87:
    case PINMAME_DISPLAY_TYPE_SEG87F:
    case PINMAME_DISPLAY_TYPE_SEG98:
    case PINMAME_DISPLAY_TYPE_SEG98F:
    case PINMAME_DISPLAY_TYPE_SEG7S:
    case PINMAME_DISPLAY_TYPE_SEG7SC:
    case PINMAME_DISPLAY_TYPE_SEG16S:
    case PINMAME_DISPLAY_TYPE_SEG16N:
    case PINMAME_DISPLAY_TYPE_SEG16D:
      return true;
    default:
      return false;
  }
}

int DecodeB2SSegmentDigit(uint16_t bitState)
{
  switch (bitState & ~0x0080u)
  {
    case 0x003Fu: return 0;
    case 0x0006u:
    case 0x0300u: return 1;
    case 0x005Bu: return 2;
    case 0x004Fu: return 3;
    case 0x0066u: return 4;
    case 0x006Du: return 5;
    case 0x007Du:
    case 0x007Cu: return 6;
    case 0x0007u: return 7;
    case 0x007Fu: return 8;
    case 0x006Fu:
    case 0x0067u: return 9;
    default: return -1;
  }
}

std::mutex g_soundCommandDebugMutex;
std::unordered_set<uint64_t> g_soundCommandDebugSeen;

uint64_t SteadyNowMs()
{
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

constexpr uint64_t kTrackedStatePollIntervalMs = 500;

}  // namespace

std::string ResolveVpmPath(const std::string& pinmamePath)
{
  char buffer[PINMAME_MAX_PATH];
  const char* path = pinmamePath.empty() ? nullptr : pinmamePath.c_str();

#if defined(_WIN32) || defined(_WIN64)
  if (path != nullptr)
  {
    snprintf(buffer, sizeof(buffer), "%s%s", path,
             (path[0] != '\0' && path[strlen(path) - 1] != '\\' && path[strlen(path) - 1] != '/') ? "\\" : "");
  }
  else
  {
    snprintf(buffer, sizeof(buffer), "%s%s\\pinmame\\", getenv("HOMEDRIVE"), getenv("HOMEPATH"));
  }
#else
  if (path != nullptr)
  {
    snprintf(buffer, sizeof(buffer), "%s%s", path, (path[0] != '\0' && path[strlen(path) - 1] != '/') ? "/" : "");
  }
  else
  {
    snprintf(buffer, sizeof(buffer), "%s/.pinmame/", getenv("HOME"));
  }
#endif

  return std::string(buffer);
}

PinmameEngine::PinmameEngine(PinmameEngineOptions options) : m_options(std::move(options)) {}

PinmameEngine::~PinmameEngine() { Stop(); }

void PinmameEngine::SetHost(GameEngineHost* pHost) { m_pHost = pHost; }

bool PinmameEngine::Start(std::string& error)
{
  if (m_pHost == nullptr)
  {
    error = "PinmameEngine::Start called before SetHost";
    return false;
  }

  if (g_pInstance != nullptr)
  {
    error = "only one PinmameEngine can exist per process";
    return false;
  }
  g_pInstance = this;

  PinmameConfig config = {
      PINMAME_AUDIO_FORMAT_INT16,
      44100,
      "",
      PINMAME_CALLBACK_CAST(PinmameOnStateUpdatedCallback, &TrampolineOnStateUpdated),
      PINMAME_CALLBACK_CAST(PinmameOnDisplayAvailableCallback, &TrampolineOnDisplayAvailable),
      PINMAME_CALLBACK_CAST(PinmameOnDisplayUpdatedCallback, &TrampolineOnDisplayUpdated),
      PINMAME_CALLBACK_CAST(PinmameOnAudioAvailableCallback, &TrampolineOnAudioAvailable),
      PINMAME_CALLBACK_CAST(PinmameOnAudioUpdatedCallback, &TrampolineOnAudioUpdated),
      PINMAME_CALLBACK_CAST(PinmameOnMechAvailableCallback, &TrampolineOnMechAvailable),
      PINMAME_CALLBACK_CAST(PinmameOnMechUpdatedCallback, &TrampolineOnMechUpdated),
      PINMAME_CALLBACK_CAST(PinmameOnSolenoidUpdatedCallback, &TrampolineOnSolenoidUpdated),
      PINMAME_CALLBACK_CAST(PinmameOnConsoleDataUpdatedCallback, &TrampolineOnConsoleDataUpdated),
      PINMAME_CALLBACK_CAST(PinmameIsKeyPressedFunction, &TrampolineIsKeyPressed),
      PINMAME_CALLBACK_CAST(PinmameOnLogMessageCallback, &TrampolineOnLogMessage),
      PINMAME_CALLBACK_CAST(PinmameOnSoundCommandCallback, &TrampolineOnSoundCommand),
  };

  const std::string vpmPath = ResolveVpmPath(m_options.pinmamePath);
  snprintf((char*)config.vpmPath, PINMAME_MAX_PATH, "%s", vpmPath.c_str());

  // Must precede PinmameRun: the user-data pointer is what every trampoline uses
  // to find this instance.
  PinmameSetUserData(this);
  PinmameSetConfig(&config);

  // TODO: Add support for PINMAME_DMD_MODE_BRIGHTNESS in the libdmdutil
  // pipeline. For now, keep using RAW so monochrome DMD ROMs render correctly on
  // ZeDMD and SDLDMD.
  PinmameSetDmdMode(PINMAME_DMD_MODE_RAW);

  if (m_options.altsound)
  {
    PinmameSetSoundMode(PINMAME_SOUND_MODE_ALTSOUND);
  }

  PinmameSetHandleKeyboard(0);
  PinmameSetHandleMechanics(0);

  if (PinmameRun(m_options.rom.c_str()) != PINMAME_STATUS_OK)
  {
    error = "PinmameRun failed for ROM " + m_options.rom;
    g_pInstance = nullptr;
    return false;
  }

  m_started = true;
  return true;
}

void PinmameEngine::Stop()
{
  if (m_started)
  {
    // Joins the libpinmame game thread, which is what guarantees no trampoline
    // can fire after this returns. The host relies on that to tear down libppuc
    // and the DMD safely.
    PinmameStop();
    m_started = false;
  }
  if (g_pInstance == this)
  {
    g_pInstance = nullptr;
  }
}

bool PinmameEngine::IsReady() const { return m_runState.load(std::memory_order_acquire) != 0; }

bool PinmameEngine::TryGetIdentity(Identity* pIdentity) const
{
  if (pIdentity == nullptr)
  {
    return false;
  }
  if (m_identityKnown)
  {
    *pIdentity = m_identity;
    return true;
  }

  const PINMAME_HARDWARE_GEN hardwareGen = PinmameGetHardwareGen();
  if (hardwareGen == 0)
  {
    return false;
  }

  m_identity.name = m_options.rom;
  m_identity.hardwareGen = static_cast<uint64_t>(hardwareGen);
  m_identity.description = DescribeHardwareGen(static_cast<uint64_t>(hardwareGen));
  m_identityKnown = true;
  *pIdentity = m_identity;
  return true;
}

void PinmameEngine::Update()
{
  PollSoundCommands();
  PollTrackedState();
}

void PinmameEngine::PollTrackedState()
{
  if (!IsReady())
  {
    return;
  }

  if (!m_triedLoadingTracking)
  {
    Identity identity;
    if (!TryGetIdentity(&identity))
    {
      return;  // hardware generation not known yet; try again next tick
    }

    m_triedLoadingTracking = true;
    m_tracking.attemptedLoad = true;

    const char* pinmamePath = m_options.pinmamePath.empty() ? nullptr : m_options.pinmamePath.c_str();
    std::string trackingError;
    if (!TryLoadPinmameTrackingConfig(m_options.rom.c_str(), identity.hardwareGen, pinmamePath, &m_tracking,
                                      &trackingError))
    {
      if (m_options.debug || m_options.debugErrors)
      {
        printf("Ball/player tracking disabled: %s\n", trackingError.c_str());
      }
      return;
    }

    if (m_options.debug)
    {
      printf("Ball/player tracking using nvram map %s\n", m_tracking.mapPath.c_str());
    }
  }

  if (!m_tracking.loaded)
  {
    return;
  }

  const uint64_t nowMs = SteadyNowMs();
  if (nowMs < m_nextTrackedPollMs)
  {
    return;
  }
  m_nextTrackedPollMs = nowMs + kTrackedStatePollIntervalMs;

  const PinmameByteReader readMainCpuByte = [](uint32_t address, uint8_t* pValue)
  { return PinmameReadMainCPUByte(address, pValue) != 0; };

  if (m_tracking.currentBall.available)
  {
    uint8_t currentBall = 0;
    if (TryDecodeTrackedPinmameValue(m_tracking.currentBall, readMainCpuByte, &currentBall) &&
        (!m_hasLastBall || currentBall != m_lastBall))
    {
      m_lastBall = currentBall;
      m_hasLastBall = true;
      m_pHost->OnCurrentBallChanged(currentBall);
    }
  }

  if (m_tracking.currentPlayer.available)
  {
    uint8_t currentPlayer = 0;
    if (TryDecodeTrackedPinmameValue(m_tracking.currentPlayer, readMainCpuByte, &currentPlayer) &&
        (!m_hasLastPlayer || currentPlayer != m_lastPlayer))
    {
      m_lastPlayer = currentPlayer;
      m_hasLastPlayer = true;
      m_pHost->OnCurrentPlayerChanged(currentPlayer);
    }
  }
}

void PinmameEngine::PollSoundCommands()
{
  if (!m_options.debugSoundCommands)
  {
    return;
  }

  const int maxSoundCommands = PinmameGetMaxSoundCommands();
  if (maxSoundCommands <= 0)
  {
    return;
  }

  const size_t bytes = static_cast<size_t>(maxSoundCommands) * sizeof(PinmameSoundCommand);
  if (m_soundCommandScratch.size() < bytes)
  {
    m_soundCommandScratch.resize(bytes);
  }

  auto* commands = reinterpret_cast<PinmameSoundCommand*>(m_soundCommandScratch.data());
  const int count = PinmameGetNewSoundCommands(commands);
  for (int i = 0; i < count && i < maxSoundCommands; ++i)
  {
    HandleSoundCommand(-1, commands[i].sndNo);
  }
}

void PinmameEngine::PollChangedLamps(std::vector<GameEngineOutputChange>& changes)
{
  changes.clear();

  const int maxLamps = PinmameGetMaxLamps();
  if (maxLamps <= 0)
  {
    return;
  }

  const size_t bytes = static_cast<size_t>(maxLamps) * sizeof(PinmameLampState);
  if (m_lampScratch.size() < bytes)
  {
    m_lampScratch.resize(bytes);
  }

  auto* states = reinterpret_cast<PinmameLampState*>(m_lampScratch.data());
  const int count = PinmameGetChangedLamps(states);
  changes.reserve(static_cast<size_t>(std::max(0, count)));
  for (int i = 0; i < count; ++i)
  {
    changes.push_back({static_cast<uint16_t>(states[i].lampNo), static_cast<uint8_t>(states[i].state)});
  }
}

void PinmameEngine::PollChangedGis(std::vector<GameEngineOutputChange>& changes)
{
  changes.clear();

  // Only WPC reports usable GI through PinMAME. On every other platform libppuc
  // forces GI on at StartUpdates instead, and reading PinMAME's GI would fight
  // that.
  if (m_options.platform != PLATFORM_WPC)
  {
    return;
  }

  const int maxGis = PinmameGetMaxGIs();
  if (maxGis <= 0)
  {
    return;
  }

  const size_t bytes = static_cast<size_t>(maxGis) * sizeof(PinmameGIState);
  if (m_giScratch.size() < bytes)
  {
    m_giScratch.resize(bytes);
  }

  auto* states = reinterpret_cast<PinmameGIState*>(m_giScratch.data());
  const int count = PinmameGetChangedGIs(states);
  changes.reserve(static_cast<size_t>(std::max(0, count)));
  for (int i = 0; i < count; ++i)
  {
    changes.push_back({static_cast<uint16_t>(states[i].giNo), static_cast<uint8_t>(states[i].state)});
  }
}

void PinmameEngine::SendSwitch(int number, uint8_t state)
{
  // Switch numbers above 240 map into PinMAME's negative special-switch space:
  // 243 becomes -3.
  const int switchNumber = (number < 241) ? number : 240 - number;
  PinmameSetSwitch(switchNumber, state == 0 ? 0 : 1);
}

bool PinmameEngine::HasCapability(Capability capability) const
{
  switch (capability)
  {
    case Capability::ChangedGis:
      return m_options.platform == PLATFORM_WPC;
    case Capability::TracksBallAndPlayer:
      return m_tracking.loaded;
    case Capability::AudioStream:
      return !m_options.noSound;
    case Capability::SegmentDisplays:
      return true;
  }
  return false;
}

void PinmameEngine::HandleStateUpdated(int state)
{
  if (m_options.debug)
  {
    printf("OnStateUpdated(): state=%d\n", state);
  }

  if (state != 0)
  {
    m_runState.store(state, std::memory_order_release);
  }

  m_pHost->OnRunStateChanged(state);
}

void PinmameEngine::HandleSolenoidUpdated(int solNo, int state)
{
  const uint8_t coilState = state == 0 ? 0 : 1;

  if (m_options.debug || m_options.debugCoils)
  {
    printf("OnSolenoidUpdated: solenoid=%d, state=%d\n", solNo, coilState);
  }

  // PinMAME conflates "high power is live" with "a game is in progress"; both
  // ride the same coil. Report the second meaning separately so the host does
  // not have to know which solenoid number carries it.
  //
  // ORDER MATTERS: this is emitted *before* OnCoilChanged, because OnCoilChanged
  // is what notifies the rules engine, and a Lua onCoilChanged handler for the
  // game-on coil must observe the new attract mode rather than the old one. The
  // cost is that the game-on coil's hardware write happens a few microseconds
  // later than the other side effects; that coil is a relay, so it does not care.
  if (m_options.gameOnSolenoid != 0 && solNo == m_options.gameOnSolenoid)
  {
    m_pHost->OnGameRunningChanged(coilState != 0);
  }

  m_pHost->OnCoilChanged(static_cast<uint16_t>(solNo), coilState);
}

void PinmameEngine::HandleDisplayAvailable(int index, int displayCount, const void* pLayout)
{
  const auto* p_displayLayout = static_cast<const PinmameDisplayLayout*>(pLayout);

  if (m_options.debug)
  {
    printf(
        "OnDisplayAvailable(): index=%d, displayCount=%d, type=%d, top=%d, "
        "left=%d, width=%d, height=%d, "
        "depth=%d, length=%d\n",
        index, displayCount, p_displayLayout->type, p_displayLayout->top, p_displayLayout->left, p_displayLayout->width,
        p_displayLayout->height, p_displayLayout->depth, p_displayLayout->length);
  }
  if (p_displayLayout != nullptr && IsSegmentDisplayType(p_displayLayout->type))
  {
    GetSegmentDisplayDigitBase(index, p_displayLayout->length);
  }
}

void PinmameEngine::HandleDisplayUpdated(int index, const void* pData, const void* pLayout)
{
  const auto* p_displayLayout = static_cast<const PinmameDisplayLayout*>(pLayout);
  if (pData == nullptr || p_displayLayout == nullptr)
  {
    return;
  }

  if (m_options.debug)
  {
    printf(
        "OnDisplayUpdated(): index=%d, type=%d, top=%d, left=%d, width=%d, "
        "height=%d, depth=%d, length=%d\n",
        index, p_displayLayout->type, p_displayLayout->top, p_displayLayout->left, p_displayLayout->width,
        p_displayLayout->height, p_displayLayout->depth, p_displayLayout->length);
  }

  if (IsSegmentDisplayType(p_displayLayout->type) && p_displayLayout->length > 0)
  {
    const int base = GetSegmentDisplayDigitBase(index, p_displayLayout->length);
    const auto* segments = static_cast<const uint16_t*>(pData);
    DebugSegmentDisplayUpdate(index, p_displayLayout->type, base, segments, p_displayLayout->length);
    int score = 0;
    bool hasScoreDigit = false;
    for (int i = 0; i < p_displayLayout->length; ++i)
    {
      const int digit = DecodeB2SSegmentDigit(segments[i]);
      m_pHost->OnSegmentDigit(base + i, digit);
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
      m_pHost->OnPlayerScore(index + 1, score);
    }
  }

  // For DMD games the type is PINMAME_DISPLAY_TYPE_DMD.
  // For alphanumeric games that should be shown on a DMD, the type is
  // PINMAME_DISPLAY_TYPE_DMD | PINMAME_DISPLAY_TYPE_DMDSEG.
  // Some games like WPT have a second display on the playfield of type
  // PINMAME_DISPLAY_TYPE_DMD | PINMAME_DISPLAY_TYPE_DMDNOAA |
  // PINMAME_DISPLAY_TYPE_NODISP, which must not be rendered.
  if ((p_displayLayout->type & PINMAME_DISPLAY_TYPE_DMD) == PINMAME_DISPLAY_TYPE_DMD &&
      (p_displayLayout->type & PINMAME_DISPLAY_TYPE_NODISP) == 0)
  {
    m_pHost->OnDmdFrame(static_cast<const uint8_t*>(pData), p_displayLayout->depth, p_displayLayout->width,
                        p_displayLayout->height);
  }
}

int PinmameEngine::HandleAudioAvailable(const void* pAudioInfo)
{
  const auto* p_audioInfo = static_cast<const PinmameAudioInfo*>(pAudioInfo);
  if (p_audioInfo == nullptr)
  {
    return 0;
  }

  if (m_options.debug)
  {
    printf(
        "OnAudioAvailable(): format=%d, channels=%d, sampleRate=%.2f, "
        "framesPerSecond=%.2f, samplesPerFrame=%d, "
        "bufferSize=%d\n",
        p_audioInfo->format, p_audioInfo->channels, p_audioInfo->sampleRate, p_audioInfo->framesPerSecond,
        p_audioInfo->samplesPerFrame, p_audioInfo->bufferSize);
  }

  if (!m_options.noSound)
  {
    m_pHost->OnAudioFormat(static_cast<int>(p_audioInfo->sampleRate), p_audioInfo->channels,
                           p_audioInfo->samplesPerFrame);
  }
  return p_audioInfo->samplesPerFrame;
}

int PinmameEngine::HandleAudioUpdated(const void* pBuffer, int samples)
{
  m_pHost->OnAudioFrames(static_cast<const int16_t*>(pBuffer), samples);
  return samples;
}

void PinmameEngine::HandleSoundCommand(int boardNo, int cmd)
{
  if (m_options.debugSoundCommands)
  {
    const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(boardNo)) << 32) | static_cast<uint32_t>(cmd);
    bool firstSeen = false;
    {
      std::lock_guard<std::mutex> lock(g_soundCommandDebugMutex);
      firstSeen = g_soundCommandDebugSeen.insert(key).second;
    }
    printf("PinMAME sound command: board=%d id=%d hex=0x%X new=%d\n", boardNo, cmd, static_cast<unsigned int>(cmd),
           firstSeen ? 1 : 0);
  }

  m_pHost->OnSoundCommand(boardNo, cmd);
}

void PinmameEngine::EmitLog(bool error, const char* message) { m_pHost->OnLogMessage(error, message); }

void PinmameEngine::HandleMechAvailable(int mechNo, const void* pMechInfo)
{
  if (!m_options.debug)
  {
    return;
  }
  const auto* p_mechInfo = static_cast<const PinmameMechInfo*>(pMechInfo);
  printf(
      "OnMechAvailable: mechNo=%d, type=%d, length=%d, steps=%d, pos=%d, "
      "speed=%d\n",
      mechNo, p_mechInfo->type, p_mechInfo->length, p_mechInfo->steps, p_mechInfo->pos, p_mechInfo->speed);
}

void PinmameEngine::HandleMechUpdated(int mechNo, const void* pMechInfo)
{
  if (!m_options.debug)
  {
    return;
  }
  const auto* p_mechInfo = static_cast<const PinmameMechInfo*>(pMechInfo);
  printf(
      "OnMechUpdated: mechNo=%d, type=%d, length=%d, steps=%d, pos=%d, "
      "speed=%d\n",
      mechNo, p_mechInfo->type, p_mechInfo->length, p_mechInfo->steps, p_mechInfo->pos, p_mechInfo->speed);
}

void PinmameEngine::HandleConsoleDataUpdated(int size)
{
  if (m_options.debug)
  {
    printf("OnConsoleDataUpdated: size=%d\n", size);
  }
}

int PinmameEngine::GetSegmentDisplayDigitBase(int index, int length)
{
  auto it = m_segmentDisplayDigitBases.find(index);
  if (it != m_segmentDisplayDigitBases.end())
  {
    return it->second;
  }
  const int base = m_nextSegmentDisplayDigitBase;
  m_segmentDisplayDigitBases[index] = base;
  m_nextSegmentDisplayDigitBase += std::max(1, length);
  return base;
}

void PinmameEngine::DebugSegmentDisplayUpdate(int index, int type, int base, const uint16_t* segments, int length)
{
  if (!m_options.debug || segments == nullptr || length <= 0)
  {
    return;
  }

  std::ostringstream line;
  line << "B2S segment display update: index=" << index << " type=" << type << " base=" << base
       << " length=" << length << " raw=";
  for (int i = 0; i < length; ++i)
  {
    if (i > 0)
    {
      line << ',';
    }
    line << "0x" << std::hex << std::uppercase << segments[i] << std::dec;
  }
  line << " digits=";
  for (int i = 0; i < length; ++i)
  {
    if (i > 0)
    {
      line << ',';
    }
    line << DecodeB2SSegmentDigit(segments[i]);
  }

  std::string text = line.str();
  if (m_lastSegmentDisplayDebugLine[index] == text)
  {
    return;
  }
  m_lastSegmentDisplayDebugLine[index] = text;
  printf("%s\n", text.c_str());
}
