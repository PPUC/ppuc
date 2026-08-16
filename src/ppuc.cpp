#if defined(_WIN32) || defined(_WIN64)
#define SIGHUP 1
#define SIGKILL 9
#define SIGQUIT 3
#define SIGINT 2
#endif

#include "PPUC.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdlib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>

#include "Uf2Image.h"
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "DMDUtil/Config.h"
#include "DMDUtil/ConsoleDMD.h"
#include "DMDUtil/DMDUtil.h"
#include "ppuc_version.h"  // <--- HINZUGEFÜGT
#ifdef PPUC_USE_KMSDMD
#include "KMSDMD/KMSDMD.h"
#else
#include "SDLDMD/SDLDMD.h"
#endif
#include "AudioOutput.h"
#include "LuaRulesEngine.h"
#include "MediaPluginHost.h"
#include "GameEngine.h"
#include "PinmameEngine.h"
#include "ScriptEngine.h"
#include "game/GameConfigYaml.h"
#include "LuaGameApi.h"
#include "game/GameCore.h"
#include "game/PlayfieldAssist.h"
#include "SDL3/SDL.h"
#include "SDL3/SDL_filesystem.h"
#include "SDL3_image/SDL_image.h"
#include "SpeechCliSupport.h"
#include "SpeechService.h"
#include "cargs.h"
#include "io-boards/Event.h"
#include "io-boards/PPUCProtocolV2.h"
#include "io-boards/PPUCPlatforms.h"
#include "libpinmame.h"

#if !defined(_WIN32) && !defined(_WIN64)
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#else
#include <windows.h>
#endif

#define MAIN_LOOP_SLEEP_US 20  // Main loop sleep time in microseconds
constexpr uint32_t kDefaultSwitchRefreshIdleMs = 15000;
constexpr uint32_t kDefaultOutputFrameIntervalMs = 4;
constexpr uint32_t kDefaultBallSearchDelayMs = 15000;
constexpr uint32_t kDefaultBallSearchRoundDelayMs = 5000;
constexpr uint32_t kBallSearchCoilPulseMs = 200;

DMDUtil::DMD* pDmd;
PPUC* pPpuc;
std::unique_ptr<LuaRulesEngine> pLuaRulesEngine;
std::unique_ptr<AudioOutput> pAudioOutput;
std::unique_ptr<MediaPluginHost> pMediaPluginHost;
std::unique_ptr<SpeechService> pSpeechService;

constexpr char kBoardEffectTriggerSource = 'F';

void ConfigureSDLVideoDriverForHeadlessLinux()
{
#if defined(__linux__) && !defined(__ANDROID__)
  if (std::getenv("SDL_VIDEODRIVER") == nullptr && std::getenv("DISPLAY") == nullptr &&
      std::getenv("WAYLAND_DISPLAY") == nullptr)
  {
    setenv("SDL_VIDEODRIVER", "kmsdrm", 0);
  }
#endif
}

bool ResolveWindowPositionForScreen(int screenIndex, int offsetX, int offsetY, int* pResolvedX, int* pResolvedY)
{
  if (pResolvedX == nullptr || pResolvedY == nullptr)
  {
    return false;
  }

  if (screenIndex < 0)
  {
    *pResolvedX = offsetX;
    *pResolvedY = offsetY;
    return true;
  }

  int numDisplays = 0;
  SDL_DisplayID* pDisplays = SDL_GetDisplays(&numDisplays);
  if (pDisplays == nullptr)
  {
    return false;
  }

  const bool validDisplay = screenIndex < numDisplays;
  SDL_DisplayID displayId = 0;
  if (validDisplay)
  {
    displayId = pDisplays[screenIndex];
  }
  SDL_free(pDisplays);

  if (!validDisplay)
  {
    SDL_SetError("Invalid screen index %d", screenIndex);
    return false;
  }

  SDL_Rect displayBounds;
  if (!SDL_GetDisplayBounds(displayId, &displayBounds))
  {
    return false;
  }

  *pResolvedX = displayBounds.x + offsetX;
  *pResolvedY = displayBounds.y + offsetY;
  return true;
}

bool PositionWindowOnScreen(SDL_Window* pWindow, int screenIndex, int offsetX = 0, int offsetY = 0)
{
  if (pWindow == nullptr || screenIndex < 0)
  {
    return true;
  }

  int resolvedX = 0;
  int resolvedY = 0;
  if (!ResolveWindowPositionForScreen(screenIndex, offsetX, offsetY, &resolvedX, &resolvedY))
  {
    return false;
  }

  SDL_SetWindowPosition(pWindow, resolvedX, resolvedY);
  while (!SDL_SyncWindow(pWindow));
  return true;
}

#ifdef PPUC_USE_KMSDMD
struct LoadedRGB24Image
{
  uint16_t width = 0;
  uint16_t height = 0;
  std::vector<uint8_t> pixels;
};

bool LoadRGB24Image(const char* path, LoadedRGB24Image* pImage, std::string* pError)
{
  if (path == nullptr || pImage == nullptr)
  {
    if (pError) *pError = "invalid translite image parameters";
    return false;
  }

  SDL_Surface* pSurface = IMG_Load(path);
  if (pSurface == nullptr)
  {
    if (pError) *pError = std::string("IMG_Load Error: ") + SDL_GetError();
    return false;
  }

  SDL_Surface* pConverted = SDL_ConvertSurface(pSurface, SDL_PIXELFORMAT_RGB24);
  SDL_DestroySurface(pSurface);
  if (pConverted == nullptr)
  {
    if (pError) *pError = std::string("SDL_ConvertSurface Error: ") + SDL_GetError();
    return false;
  }

  pImage->width = static_cast<uint16_t>(pConverted->w);
  pImage->height = static_cast<uint16_t>(pConverted->h);
  pImage->pixels.resize(static_cast<size_t>(pImage->width) * pImage->height * 3u);

  for (uint16_t y = 0; y < pImage->height; ++y)
  {
    const uint8_t* pSourceRow =
        static_cast<const uint8_t*>(pConverted->pixels) + static_cast<size_t>(y) * pConverted->pitch;
    uint8_t* pDestinationRow = pImage->pixels.data() + static_cast<size_t>(y) * pImage->width * 3u;
    std::memcpy(pDestinationRow, pSourceRow, static_cast<size_t>(pImage->width) * 3u);
  }

  SDL_DestroySurface(pConverted);
  return true;
}

bool RenderTransliteImage(const LoadedRGB24Image& image, DMDUtil::KMSDMD* pDisplay)
{
  if (pDisplay == nullptr || image.pixels.empty()) return false;
  pDisplay->Update(const_cast<uint8_t*>(image.pixels.data()), image.width, image.height);
  return true;
}
#endif

SDL_Window* pTransliteWindow;
SDL_Renderer* pTransliteRenderer;
SDL_Texture* pTransliteTexture;
SDL_Texture* pTransliteAttractTexture;
#ifdef PPUC_USE_KMSDMD
std::unique_ptr<DMDUtil::KMSDMD> pTransliteDisplay;
LoadedRGB24Image transliteImage;
LoadedRGB24Image transliteAttractImage;
#endif
enum class RenderCommand
{
  RENDER_GAME,
  RENDER_ATTRACT
};

struct RenderRequest
{
  RenderCommand command;
};

std::queue<RenderRequest> renderQueue;
std::mutex renderMutex;
RenderCommand currentTransliteCommand = RenderCommand::RENDER_ATTRACT;

static bool HasTransliteAttractImage()
{
#ifdef PPUC_USE_KMSDMD
  return !transliteAttractImage.pixels.empty();
#else
  return pTransliteAttractTexture != nullptr;
#endif
}

static void QueueTransliteRender(RenderCommand command)
{
  std::lock_guard<std::mutex> lock(renderMutex);
  if (currentTransliteCommand == command)
  {
    return;
  }
  currentTransliteCommand = command;
  renderQueue.push(RenderRequest{command});
}

bool opt_debug = false;
bool opt_debug_errors = false;
bool opt_debug_switches = false;
bool opt_debug_coils = false;
bool opt_debug_lamps = false;
bool opt_debug_effects = false;
bool opt_debug_sound_commands = false;
bool opt_no_serial = false;
bool opt_no_sound = false;
// "pinmame" runs an original ROM under libpinmame; "script" runs the ROM-less
// game core. Deliberately explicit rather than inferred from whether a ROM zip
// happens to exist: a machine with no console that silently picked the wrong
// engine is a miserable thing to debug.
const char* opt_engine = "pinmame";
bool opt_no_display = false;
uint32_t opt_exit_after_ms = 0;
bool opt_speech = false;
bool opt_greeting = false;
const char* opt_music_files = NULL;
uint32_t opt_music_gap_ms = 2000;
const char* opt_speech_backend = "auto";
const char* opt_speech_voice = NULL;
const char* opt_speech_rate_arg = NULL;
const char* opt_speech_pitch_arg = NULL;
bool opt_interactive = false;
bool opt_serum = false;
bool opt_pup = false;
bool opt_altsound = false;
bool opt_b2s = false;
float opt_b2s_segment_angle_degrees = 9.0f;
float opt_b2s_segment_glow = 1.4f;
bool opt_b2s_segment_smoothing = true;
const char* opt_game_folder = NULL;
const char* opt_plugin_dir = NULL;
const char* opt_pup_folder = NULL;
const char* opt_altsound_folder = NULL;
bool opt_console_display = false;
bool opt_hard_reset = false;
const char* opt_virtual_dmd_renderer = "dots";
const char* opt_pinmame_path = NULL;
const char* opt_rom = NULL;
std::atomic<bool> ball_search_game_running{false};
std::atomic<bool> running{true};
volatile std::sig_atomic_t shutdown_requested = 0;
static uint64_t CurrentUnixMs();

static void PrintFlushedLogLine(const char* prefix, const char* message)
{
  const char* safeMessage = message ? message : "";
  const size_t len = strlen(safeMessage);
  const bool hasTrailingNewline = len > 0 && safeMessage[len - 1] == '\n';

  fputs(prefix, stdout);
  fputs(safeMessage, stdout);
  if (!hasTrailingNewline)
  {
    fputc('\n', stdout);
  }
  fflush(stdout);
}

// Tilt warnings and ball save, in the switch path ahead of whichever engine is
// running. Engine-neutral on purpose: this is what gives an early-electronic ROM
// features it was never written to have.
PlayfieldAssist g_playfieldAssist;
bool g_playfieldAssistEnabled = false;
// Non-null only under engine: script. The ROM path needs no back-reference: the
// tilting bob hit is forwarded to PinMAME like any other switch.
GameCore* g_pGameCore = nullptr;
// Collected during option handling, loaded once the engine exists.
std::vector<std::string> g_ruleScripts;

// Arbitrates between what the game engine wants an output to do and what a Lua
// rule has temporarily overridden it to do. Engine-neutral: "engine" here is
// PinMAME or the ROM-less game core, and the arbitration is the same either way.
//
// THREADING. ApplyEngineCoil runs on the engine's thread (libpinmame has its
// own), while Service() and the rules actions run on the main loop, so every
// method that touches the maps takes m_mutex. The lock is never held across a
// call into LuaRulesEngine: the path OnCoilChanged -> OnCoilState -> a Lua
// onCoilChanged handler -> ppuc.pulseCoil -> HandleAction comes straight back in
// here, and a non-recursive mutex held across that would deadlock.
struct InterceptorOutputOverrides
{
  struct CoilPulse
  {
    std::chrono::steady_clock::time_point until{};
  };

  struct LampBlink
  {
    uint32_t onMs = 250;
    uint32_t offMs = 250;
    bool outputOn = false;
    std::chrono::steady_clock::time_point nextToggle{};
  };

  std::unordered_map<int, uint8_t> engineCoils;
  std::unordered_map<int, uint8_t> engineLamps;
  std::unordered_map<int, CoilPulse> coilPulses;
  std::unordered_map<int, LampBlink> lampBlinks;
  std::mutex mutex;

  // Set by main() to route a rules-injected switch into whichever engine is
  // running. Keeps this struct free of any engine header.
  std::function<void(int, uint8_t)> sendSwitch;

  void ApplyEngineCoil(PPUC* controller, int number, uint8_t state)
  {
    std::lock_guard<std::mutex> lock(mutex);
    engineCoils[number] = state == 0 ? 0 : 1;
    const auto now = std::chrono::steady_clock::now();
    const auto pulseIt = coilPulses.find(number);
    if (pulseIt != coilPulses.end() && now < pulseIt->second.until)
    {
      controller->SetSolenoidState(number, 1);
      return;
    }
    controller->SetSolenoidState(number, state == 0 ? 0 : 1);
  }

  void ApplyEngineLamp(PPUC* controller, int number, uint8_t state)
  {
    std::lock_guard<std::mutex> lock(mutex);
    engineLamps[number] = state == 0 ? 0 : 1;
    if (lampBlinks.find(number) != lampBlinks.end())
    {
      return;
    }
    controller->SetLampState(number, state == 0 ? 0 : 1);
  }

  void PulseCoil(PPUC* controller, int number, uint32_t durationMs)
  {
    std::lock_guard<std::mutex> lock(mutex);
    const auto now = std::chrono::steady_clock::now();
    auto& pulse = coilPulses[number];
    const auto until = now + std::chrono::milliseconds(durationMs == 0 ? 1 : durationMs);
    if (until > pulse.until)
    {
      pulse.until = until;
    }
    controller->SetSolenoidState(number, 1);
  }

  void StartBlinkLamp(PPUC* controller, int number, uint32_t onMs, uint32_t offMs)
  {
    std::lock_guard<std::mutex> lock(mutex);
    const uint32_t normalizedOnMs = onMs == 0 ? 250 : onMs;
    const uint32_t normalizedOffMs = offMs == 0 ? 250 : offMs;
    const auto existing = lampBlinks.find(number);
    if (existing != lampBlinks.end())
    {
      existing->second.onMs = normalizedOnMs;
      existing->second.offMs = normalizedOffMs;
      return;
    }

    auto& blink = lampBlinks[number];
    blink.onMs = normalizedOnMs;
    blink.offMs = normalizedOffMs;
    blink.outputOn = true;
    blink.nextToggle = std::chrono::steady_clock::now() + std::chrono::milliseconds(blink.onMs);
    controller->SetLampState(number, 1);
  }

  void StopBlinkLamp(PPUC* controller, int number)
  {
    std::lock_guard<std::mutex> lock(mutex);
    lampBlinks.erase(number);
    const uint8_t restore = engineLamps.count(number) == 0 ? 0 : engineLamps[number];
    controller->SetLampState(number, restore);
  }

  void Service(PPUC* controller)
  {
    std::lock_guard<std::mutex> lock(mutex);
    const auto now = std::chrono::steady_clock::now();

    auto coilIt = coilPulses.begin();
    while (coilIt != coilPulses.end())
    {
      if (now >= coilIt->second.until)
      {
        const int number = coilIt->first;
        coilIt = coilPulses.erase(coilIt);
        const uint8_t restore = engineCoils.count(number) == 0 ? 0 : engineCoils[number];
        controller->SetSolenoidState(number, restore);
      }
      else
      {
        ++coilIt;
      }
    }

    for (auto& entry : lampBlinks)
    {
      LampBlink& blink = entry.second;
      if (now < blink.nextToggle)
      {
        continue;
      }

      blink.outputOn = !blink.outputOn;
      blink.nextToggle = now + std::chrono::milliseconds(blink.outputOn ? blink.onMs : blink.offMs);
      controller->SetLampState(entry.first, blink.outputOn ? 1 : 0);
    }
  }

  void HandleAction(PPUC* controller, const RulesAction& action)
  {
    switch (action.type)
    {
      case RulesActionType::SendSwitchToCpu:
        if (sendSwitch)
        {
          sendSwitch(action.number, action.state);
        }
        break;
      case RulesActionType::PulseCoil:
        PulseCoil(controller, action.number, action.durationMs);
        break;
      case RulesActionType::StartBlinkLamp:
        StartBlinkLamp(controller, action.number, action.onMs, action.offMs);
        break;
      case RulesActionType::StopBlinkLamp:
        StopBlinkLamp(controller, action.number);
        break;
      case RulesActionType::GrantBallSave:
        g_playfieldAssist.GrantBallSave(action.durationMs);
        break;
    }
  }
};

InterceptorOutputOverrides g_interceptorOutputs;

static uint64_t CurrentUnixMs()
{
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count());
}


static bool ParseUint32Strict(const char* text, uint32_t* outValue)
{
  if (!text || !outValue || text[0] == '\0')
  {
    return false;
  }

  char* end = nullptr;
  const unsigned long value = strtoul(text, &end, 10);
  if (end == text || *end != '\0')
  {
    return false;
  }

  if (value > UINT32_MAX)
  {
    return false;
  }

  *outValue = static_cast<uint32_t>(value);
  return true;
}

static bool ValidateSkippedBoardsCsv(const char* csv)
{
  if (!csv || csv[0] == '\0')
  {
    return true;
  }

  std::string token;
  for (const char* p = csv;; ++p)
  {
    const char c = *p;
    if (c == ',' || c == '\0')
    {
      if (token.empty())
      {
        return false;
      }

      uint32_t board = 0;
      if (!ParseUint32Strict(token.c_str(), &board) || board > 255)
      {
        return false;
      }

      token.clear();
      if (c == '\0')
      {
        return true;
      }
      continue;
    }

    if (!isdigit(static_cast<unsigned char>(c)))
    {
      return false;
    }
    token.push_back(c);
  }
}

static std::string TrimIniValue(const std::string& value)
{
  size_t start = 0;
  while (start < value.size() && isspace(static_cast<unsigned char>(value[start])))
  {
    ++start;
  }

  size_t end = value.size();
  while (end > start && isspace(static_cast<unsigned char>(value[end - 1])))
  {
    --end;
  }

  return value.substr(start, end - start);
}

static bool ParseIniBool(const std::string& value, bool defaultValue = false)
{
  std::string normalized;
  normalized.reserve(value.size());
  for (const unsigned char ch : value)
  {
    normalized.push_back(static_cast<char>(tolower(ch)));
  }

  if (normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on")
  {
    return true;
  }
  if (normalized == "0" || normalized == "false" || normalized == "no" || normalized == "off")
  {
    return false;
  }
  return defaultValue;
}

static float ParseIniFloat(const std::string& value, float defaultValue)
{
  char* end = nullptr;
  const float parsed = strtof(value.c_str(), &end);
  if (end == value.c_str())
  {
    return defaultValue;
  }
  while (end != nullptr && *end != '\0')
  {
    if (!isspace(static_cast<unsigned char>(*end)))
    {
      return defaultValue;
    }
    ++end;
  }
  return parsed;
}

static bool HasOptionValue(const char* value) { return value != nullptr && value[0] != '\0'; }

static std::string ResolvePupVideosPathForDmd(const char* pupFolder,
                                              const char* romName)
{
  const char* fallbackHome = getenv("HOME");
  const std::filesystem::path base =
      HasOptionValue(pupFolder)
          ? std::filesystem::path(pupFolder)
          : std::filesystem::path(fallbackHome ? fallbackHome : "");
  if (base.empty())
  {
    return {};
  }

  std::error_code ec;
  const std::filesystem::path pupvideos = base / "pupvideos";
  if (std::filesystem::is_directory(pupvideos, ec))
  {
    if (!HasOptionValue(romName))
    {
      return pupvideos.string();
    }

    const std::filesystem::path romPath = pupvideos / romName;
    if (std::filesystem::is_directory(romPath, ec))
    {
      return pupvideos.string();
    }
  }

  return base.string();
}

static const char* DuplicateIniString(const std::string& value)
{
  char* copy = static_cast<char*>(malloc(value.size() + 1));
  if (copy == nullptr)
  {
    return nullptr;
  }
  memcpy(copy, value.c_str(), value.size() + 1);
  return copy;
}

static const char* DuplicateOptionalIniString(const std::string& value)
{
  if (value.empty())
  {
    return nullptr;
  }
  return DuplicateIniString(value);
}

static const char* DuplicatePathString(const std::filesystem::path& path)
{
  return DuplicateIniString(path.string());
}

static bool PathExtensionEquals(const std::filesystem::path& path, std::initializer_list<const char*> extensions)
{
  std::string extension = path.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  for (const char* candidate : extensions)
  {
    if (extension == candidate)
    {
      return true;
    }
  }
  return false;
}

static std::optional<std::filesystem::path> FindFirstExistingPath(
    const std::filesystem::path& directory, std::initializer_list<const char*> baseNames,
    std::initializer_list<const char*> extensions)
{
  std::error_code ec;
  for (const char* baseName : baseNames)
  {
    for (const char* extension : extensions)
    {
      std::filesystem::path candidate = directory / (std::string(baseName) + extension);
      if (std::filesystem::is_regular_file(candidate, ec))
      {
        return candidate;
      }
      ec.clear();
    }
  }
  return std::nullopt;
}

static std::string CollectMusicFilesCsv(const std::filesystem::path& musicDirectory)
{
  std::error_code ec;
  if (!std::filesystem::is_directory(musicDirectory, ec))
  {
    return {};
  }

  std::vector<std::filesystem::path> files;
  for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(musicDirectory, ec))
  {
    if (ec)
    {
      return {};
    }
    if (!entry.is_regular_file(ec))
    {
      ec.clear();
      continue;
    }
    const std::filesystem::path path = entry.path();
    if (PathExtensionEquals(path, {".mp3", ".ogg", ".wav", ".flac", ".opus", ".m4a"}))
    {
      files.push_back(path);
    }
  }
  std::sort(files.begin(), files.end());

  std::string csv;
  for (const std::filesystem::path& file : files)
  {
    if (!csv.empty())
    {
      csv += ",";
    }
    csv += file.string();
  }
  return csv;
}

static bool CollectRulesScripts(const char* pathArg, std::vector<std::string>& scripts, std::string& error)
{
  scripts.clear();
  error.clear();

  if (!HasOptionValue(pathArg))
  {
    error = "Rules path is empty";
    return false;
  }

  std::error_code ec;
  const std::filesystem::path rulesPath(pathArg);
  if (std::filesystem::is_regular_file(rulesPath, ec))
  {
    scripts.push_back(rulesPath.string());
    return true;
  }

  if (std::filesystem::is_directory(rulesPath, ec))
  {
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(rulesPath, ec))
    {
      if (ec)
      {
        error = "Unable to read Lua rules directory";
        return false;
      }
      if (!entry.is_regular_file(ec))
      {
        ec.clear();
        continue;
      }
      const std::filesystem::path filePath = entry.path();
      if (filePath.extension() == ".lua")
      {
        scripts.push_back(filePath.string());
      }
    }

    std::sort(scripts.begin(), scripts.end());
    if (scripts.empty())
    {
      error = "Lua rules directory contains no .lua files";
      return false;
    }
    return true;
  }

  error = "Rules path is not a file or directory";
  return false;
}

static const char* kAnsiStrikeOn = "\033[9m";
static const char* kAnsiStrikeOff = "\033[0m";

static void PrintMaybeStruckLine(bool struck, const char* format, ...)
{
  va_list args;
  va_start(args, format);
  if (struck)
  {
    fputs(kAnsiStrikeOn, stdout);
  }
  vprintf(format, args);
  if (struck)
  {
    fputs(kAnsiStrikeOff, stdout);
    fputs("  [skipped]", stdout);
  }
  fputc('\n', stdout);
  va_end(args);
}

static bool IsVirtualizedBenchSwitch(PPUC* pPpuc, const PPUCSwitch& vswitch)
{
  return pPpuc->IsBoardVirtualized(vswitch.board) || pPpuc->IsSwitchVirtualized(vswitch.number);
}

static bool IsVirtualizedBenchCoil(PPUC* pPpuc, const PPUCCoil& coil) { return pPpuc->IsBoardVirtualized(coil.board); }

static bool IsVirtualizedBenchLamp(PPUC* pPpuc, const PPUCLamp& lamp) { return pPpuc->IsBoardVirtualized(lamp.board); }

enum class BenchTestMode
{
  SWITCHES,
  COILS,
  LAMPS,
  GI,
  FLASHERS
};

enum class BenchOutputKind
{
  SOLENOID,
  LAMP,
  GI
};

struct BenchOutputStep
{
  BenchOutputKind kind;
  uint8_t board;
  uint8_t port;
  uint8_t number;
  uint8_t value;
  uint8_t type;
  std::string description;
  uint32_t color;
  int onDurationMs;
  int offDurationMs;
};

struct BenchTestRunner
{
  BenchTestMode mode;
  std::vector<BenchOutputStep> steps;
  std::unordered_map<int, std::vector<BenchOutputStep>> stepsByNumber;
  std::vector<std::string> interactiveMenuLines;
  std::deque<BenchOutputStep> pendingInteractiveSteps;
  bool interactive = false;
  size_t index = 0;
  bool outputActive = false;
  bool waitingBetweenSteps = false;
  std::chrono::steady_clock::time_point phaseDeadline;
  std::chrono::steady_clock::time_point nextStepReadyAt;
  bool restoreGiOnExit = false;
  bool printedSwitchHeader = false;
  bool switchFeedbackGiState = false;
  bool initialSwitchStatesPrimed = false;
  std::unordered_map<int, uint8_t> initialSwitchStates;
  std::unordered_map<int, uint8_t> currentSwitchStates;
  std::chrono::steady_clock::time_point switchFeedbackOffUntil{};
  std::string interactiveInput;
};

static void ApplyBenchOutput(PPUC* pPpuc, const BenchOutputStep& step, bool on);

struct BallSearchRunner
{
  std::vector<BenchOutputStep> steps;
  std::unordered_set<int> buttonSwitchNumbers;
  std::unordered_map<int, uint8_t> currentSwitchStates;
  std::chrono::steady_clock::time_point nextSearchAt{};
  std::chrono::steady_clock::time_point phaseDeadline{};
  size_t index = 0;
  bool outputActive = false;
  bool runningRound = false;
};

static std::vector<BenchOutputStep> BuildBallSearchSteps(PPUC* pPpuc)
{
  std::vector<BenchOutputStep> steps;
  for (const auto& coil : pPpuc->GetCoils())
  {
    if (!coil.ballSearch)
    {
      continue;
    }
    if (coil.type != PWM_TYPE_SOLENOID && coil.type != PWM_TYPE_FLASHER &&
        coil.type != PWM_TYPE_MOTOR && coil.type != PWM_TYPE_SHAKER)
    {
      continue;
    }
    if (IsVirtualizedBenchCoil(pPpuc, coil))
    {
      continue;
    }
    steps.push_back({BenchOutputKind::SOLENOID, coil.board, coil.port, coil.number, 1, coil.type, coil.description, 0,
                     static_cast<int>(kBallSearchCoilPulseMs), 0});
  }
  return steps;
}

static BallSearchRunner CreateBallSearchRunner(PPUC* pPpuc, uint32_t delayMs)
{
  BallSearchRunner runner;
  runner.steps = BuildBallSearchSteps(pPpuc);
  for (const auto& ppucSwitch : pPpuc->GetSwitches())
  {
    if (ppucSwitch.button)
    {
      runner.buttonSwitchNumbers.insert(ppucSwitch.number);
    }
  }
  runner.nextSearchAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(delayMs);
  return runner;
}

static void ResetBallSearchIdle(BallSearchRunner& runner, uint32_t delayMs)
{
  runner.nextSearchAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(delayMs);
}

static void CancelActiveBallSearch(PPUC* pPpuc, BallSearchRunner& runner)
{
  if (runner.outputActive && runner.index < runner.steps.size())
  {
    ApplyBenchOutput(pPpuc, runner.steps[runner.index], false);
  }
  runner.outputActive = false;
  runner.runningRound = false;
  runner.index = 0;
}

static bool IsAnyBallSearchButtonPressed(const BallSearchRunner& runner)
{
  for (const int number : runner.buttonSwitchNumbers)
  {
    const auto it = runner.currentSwitchStates.find(number);
    if (it != runner.currentSwitchStates.end() && it->second != 0)
    {
      return true;
    }
  }
  return false;
}

static void NoteBallSearchSwitchUpdate(PPUC* pPpuc, BallSearchRunner& runner, int switchNumber, uint8_t state,
                                       uint32_t delayMs)
{
  runner.currentSwitchStates[switchNumber] = state;
  if (runner.buttonSwitchNumbers.find(switchNumber) != runner.buttonSwitchNumbers.end())
  {
    return;
  }
  CancelActiveBallSearch(pPpuc, runner);
  ResetBallSearchIdle(runner, delayMs);
}

static void ServiceBallSearchRunner(PPUC* pPpuc, BallSearchRunner& runner, bool gameRunning, uint32_t delayMs,
                                    uint32_t roundDelayMs)
{
  if (runner.steps.empty())
  {
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  if (!gameRunning)
  {
    CancelActiveBallSearch(pPpuc, runner);
    runner.nextSearchAt = now + std::chrono::milliseconds(delayMs);
    return;
  }

  if (IsAnyBallSearchButtonPressed(runner))
  {
    CancelActiveBallSearch(pPpuc, runner);
    runner.nextSearchAt = now + std::chrono::milliseconds(delayMs);
    return;
  }

  if (!runner.runningRound && now < runner.nextSearchAt)
  {
    return;
  }

  if (!runner.runningRound)
  {
    runner.runningRound = true;
    runner.index = 0;
    runner.outputActive = false;
  }

  if (runner.outputActive)
  {
    if (now < runner.phaseDeadline)
    {
      return;
    }
    ApplyBenchOutput(pPpuc, runner.steps[runner.index], false);
    runner.outputActive = false;
    ++runner.index;
  }

  if (runner.index >= runner.steps.size())
  {
    runner.runningRound = false;
    runner.index = 0;
    runner.nextSearchAt = now + std::chrono::milliseconds(roundDelayMs);
    return;
  }

  ApplyBenchOutput(pPpuc, runner.steps[runner.index], true);
  runner.outputActive = true;
  runner.phaseDeadline = now + std::chrono::milliseconds(kBallSearchCoilPulseMs);
}

#if !defined(_WIN32) && !defined(_WIN64)
class ScopedRawTerminalMode
{
 public:
  explicit ScopedRawTerminalMode(bool enable)
  {
    if (!enable || !isatty(STDIN_FILENO))
    {
      return;
    }

    if (tcgetattr(STDIN_FILENO, &original_) != 0)
    {
      return;
    }

    termios raw = original_;
    raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0)
    {
      active_ = true;
    }
  }

  ~ScopedRawTerminalMode()
  {
    if (active_)
    {
      tcsetattr(STDIN_FILENO, TCSANOW, &original_);
    }
  }

 private:
  bool active_ = false;
  termios original_{};
};
#else
class ScopedRawTerminalMode
{
 public:
  explicit ScopedRawTerminalMode(bool /*enable*/) {}
};
#endif

static bool ComputeSwitchFeedbackGiOn(const BenchTestRunner& runner, std::chrono::steady_clock::time_point now)
{
  return now >= runner.switchFeedbackOffUntil;
}

static void UpdateSwitchFeedbackGi(PPUC* pPpuc, BenchTestRunner& runner)
{
  if (runner.mode != BenchTestMode::SWITCHES || pPpuc->GetPlatform() == PLATFORM_WPC)
  {
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  const bool giOn = ComputeSwitchFeedbackGiOn(runner, now);
  if (giOn == runner.switchFeedbackGiState)
  {
    return;
  }

  pPpuc->SetGIState(/* string */ 1, giOn ? 8 : 0);
  runner.switchFeedbackGiState = giOn;
}

static void PrintBenchStepDetails(const BenchOutputStep& step)
{
  printf("\nBoard: %d\nPort: %d\nNumber: %d\nDescription: %s\n", step.board, step.port, step.number,
         step.description.c_str());
  if (step.kind == BenchOutputKind::LAMP)
  {
    printf("Color: %08X\n", step.color);
  }
  if (step.kind == BenchOutputKind::GI)
  {
    printf("Brightness: %d\n", step.value);
  }
}

static void PrintInteractiveBenchPrompt(const BenchTestRunner& runner)
{
  const char* label = "item";
  switch (runner.mode)
  {
    case BenchTestMode::COILS:
      label = "coil/flasher";
      break;
    case BenchTestMode::LAMPS:
      label = "lamp";
      break;
    case BenchTestMode::FLASHERS:
      label = "flasher";
      break;
    default:
      break;
  }

  printf("Enter %s number, or q/ESC to quit: %s", label, runner.interactiveInput.c_str());
  fflush(stdout);
}

static void PrintInteractiveBenchMenu(const BenchTestRunner& runner)
{
  if (!runner.interactiveMenuLines.empty())
  {
    for (const auto& line : runner.interactiveMenuLines)
    {
      printf("%s\n", line.c_str());
    }
    printf("\n");
  }
  PrintInteractiveBenchPrompt(runner);
}

static void ApplyBenchOutput(PPUC* pPpuc, const BenchOutputStep& step, bool on)
{
  switch (step.kind)
  {
    case BenchOutputKind::SOLENOID:
      pPpuc->SetSolenoidState(step.number, on ? 1 : 0);
      break;
    case BenchOutputKind::LAMP:
      if (step.type == LED_TYPE_LAMP)
      {
        pPpuc->SetLampState(step.number, on ? 1 : 0);
      }
      else
      {
        pPpuc->SetSolenoidState(step.number, on ? 1 : 0);
      }
      break;
    case BenchOutputKind::GI:
      pPpuc->SetGIState(step.number, on ? step.value : 0);
      break;
  }
}

static std::vector<BenchOutputStep> BuildCoilTestSteps(PPUC* pPpuc, uint8_t number)
{
  std::vector<BenchOutputStep> steps;
  for (const auto& coil : pPpuc->GetCoils())
  {
    if (coil.type != PWM_TYPE_SOLENOID && coil.type != PWM_TYPE_FLASHER)
    {
      continue;
    }
    if (number != 0 && coil.number != number)
    {
      continue;
    }
    if (IsVirtualizedBenchCoil(pPpuc, coil))
    {
      continue;
    }
    steps.push_back(
        {BenchOutputKind::SOLENOID, coil.board, coil.port, coil.number, 1, coil.type, coil.description, 0, 200, 1000});
  }
  return steps;
}

static std::vector<BenchOutputStep> BuildLampTestSteps(PPUC* pPpuc, uint8_t number)
{
  std::vector<BenchOutputStep> steps;
  for (const auto& lamp : pPpuc->GetLamps())
  {
    if (lamp.type != LED_TYPE_LAMP)
    {
      continue;
    }
    if (number != 0 && lamp.number != number)
    {
      continue;
    }
    if (IsVirtualizedBenchLamp(pPpuc, lamp))
    {
      continue;
    }
    steps.push_back({BenchOutputKind::LAMP, lamp.board, lamp.port, lamp.number, 1, lamp.type, lamp.description,
                     lamp.color, number != 0 ? 10000 : 2000, number != 0 ? 0 : 1000});
  }

  for (const auto& coil : pPpuc->GetCoils())
  {
    if (coil.type != PWM_TYPE_LAMP)
    {
      continue;
    }
    if (number != 0 && coil.number != number)
    {
      continue;
    }
    if (IsVirtualizedBenchCoil(pPpuc, coil))
    {
      continue;
    }
    steps.push_back({BenchOutputKind::LAMP, coil.board, coil.port, coil.number, 1, coil.type, coil.description, 0,
                     number != 0 ? 10000 : 2000, number != 0 ? 0 : 1000});
  }

  return steps;
}

static std::vector<BenchOutputStep> BuildFlasherTestSteps(PPUC* pPpuc, uint8_t number)
{
  std::vector<BenchOutputStep> steps;
  for (const auto& lamp : pPpuc->GetLamps())
  {
    if (lamp.type != LED_TYPE_FLASHER)
    {
      continue;
    }
    if (number != 0 && lamp.number != number)
    {
      continue;
    }
    if (IsVirtualizedBenchLamp(pPpuc, lamp))
    {
      continue;
    }
    for (uint8_t i = 0; i < 3; ++i)
    {
      steps.push_back({BenchOutputKind::SOLENOID, lamp.board, lamp.port, lamp.number, 1, lamp.type, lamp.description,
                       lamp.color, 200, 1000});
    }
  }

  for (const auto& coil : pPpuc->GetCoils())
  {
    if (coil.type != PWM_TYPE_FLASHER)
    {
      continue;
    }
    if (number != 0 && coil.number != number)
    {
      continue;
    }
    if (IsVirtualizedBenchCoil(pPpuc, coil))
    {
      continue;
    }
    for (uint8_t i = 0; i < 3; ++i)
    {
      steps.push_back({BenchOutputKind::SOLENOID, coil.board, coil.port, coil.number, 1, coil.type, coil.description, 0,
                       200, 1000});
    }
  }

  return steps;
}

static std::vector<BenchOutputStep> BuildGiTestSteps(PPUC* pPpuc, uint8_t number)
{
  std::vector<BenchOutputStep> steps;
  const uint8_t maxStrings = pPpuc->GetPlatform() == PLATFORM_WPC ? 8 : 1;
  for (uint8_t i = 1; i <= maxStrings; ++i)
  {
    if (number != 0 && number != i)
    {
      continue;
    }
    steps.push_back({BenchOutputKind::GI, 0, 0, i, 8, 0, "GI String", 0, 5000, 1000});
  }
  return steps;
}

static void PrintSwitchTestHeader(PPUC* pPpuc)
{
  printf("Switch Test\n");
  printf("=========\n");

  const auto switches = pPpuc->GetSwitches();
  if (!switches.empty())
  {
    printf("Configured switches:\n");
    for (const auto& vswitch : switches)
    {
      PrintMaybeStruckLine(IsVirtualizedBenchSwitch(pPpuc, vswitch), "  #%d  board=%d port=%d  %s", vswitch.number,
                           vswitch.board, vswitch.port, vswitch.description.c_str());
    }
    printf("\nWaiting for switch activity...\n");
    fflush(stdout);
  }
}

static void PrintCoilTestHeader(PPUC* pPpuc, uint8_t number)
{
  printf("Coil Test\n");
  printf("=========\n");
  printf("Configured coils/flashers:\n");
  for (const auto& coil : pPpuc->GetCoils())
  {
    if (coil.type != PWM_TYPE_SOLENOID && coil.type != PWM_TYPE_FLASHER)
    {
      continue;
    }
    if (number != 0 && coil.number != number)
    {
      continue;
    }
    PrintMaybeStruckLine(IsVirtualizedBenchCoil(pPpuc, coil), "  #%d  board=%d port=%d  %s", coil.number, coil.board,
                         coil.port, coil.description.c_str());
  }
  printf("\n");
}

static void PrintLampTestHeader(PPUC* pPpuc, uint8_t number)
{
  printf("Lamp Test\n");
  printf("=========\n");
  printf("Configured lamps:\n");
  for (const auto& lamp : pPpuc->GetLamps())
  {
    if (lamp.type != LED_TYPE_LAMP)
    {
      continue;
    }
    if (number != 0 && lamp.number != number)
    {
      continue;
    }
    PrintMaybeStruckLine(IsVirtualizedBenchLamp(pPpuc, lamp), "  #%d  board=%d port=%d  %s", lamp.number, lamp.board,
                         lamp.port, lamp.description.c_str());
  }
  for (const auto& coil : pPpuc->GetCoils())
  {
    if (coil.type != PWM_TYPE_LAMP)
    {
      continue;
    }
    if (number != 0 && coil.number != number)
    {
      continue;
    }
    PrintMaybeStruckLine(IsVirtualizedBenchCoil(pPpuc, coil), "  #%d  board=%d port=%d  %s", coil.number, coil.board,
                         coil.port, coil.description.c_str());
  }
  printf("\n");
}

static void PrintFlasherTestHeader(PPUC* pPpuc, uint8_t number)
{
  printf("\nFlasher Test\n");
  printf("=========\n");
  printf("Configured flashers:\n");
  for (const auto& lamp : pPpuc->GetLamps())
  {
    if (lamp.type != LED_TYPE_FLASHER)
    {
      continue;
    }
    if (number != 0 && lamp.number != number)
    {
      continue;
    }
    PrintMaybeStruckLine(IsVirtualizedBenchLamp(pPpuc, lamp), "  #%d  board=%d port=%d  %s", lamp.number, lamp.board,
                         lamp.port, lamp.description.c_str());
  }
  for (const auto& coil : pPpuc->GetCoils())
  {
    if (coil.type != PWM_TYPE_FLASHER)
    {
      continue;
    }
    if (number != 0 && coil.number != number)
    {
      continue;
    }
    PrintMaybeStruckLine(IsVirtualizedBenchCoil(pPpuc, coil), "  #%d  board=%d port=%d  %s", coil.number, coil.board,
                         coil.port, coil.description.c_str());
  }
  printf("\n");
}

static BenchTestRunner CreateBenchTestRunner(PPUC* pPpuc, BenchTestMode mode, uint8_t number)
{
  BenchTestRunner runner;
  runner.mode = mode;
  runner.interactive = opt_interactive && (mode == BenchTestMode::COILS || mode == BenchTestMode::LAMPS ||
                                           mode == BenchTestMode::FLASHERS);

  switch (mode)
  {
    case BenchTestMode::SWITCHES:
      PrintSwitchTestHeader(pPpuc);
      runner.printedSwitchHeader = true;
      break;
    case BenchTestMode::COILS:
      PrintCoilTestHeader(pPpuc, number);
      runner.steps = BuildCoilTestSteps(pPpuc, number);
      break;
    case BenchTestMode::LAMPS:
      PrintLampTestHeader(pPpuc, number);
      pPpuc->SetGIState(/* string */ 1, 0);
      runner.restoreGiOnExit = pPpuc->GetPlatform() != PLATFORM_WPC;
      runner.steps = BuildLampTestSteps(pPpuc, number);
      break;
    case BenchTestMode::GI:
      printf("\nGI Test\n");
      printf("=========\n");
      runner.steps = BuildGiTestSteps(pPpuc, number);
      break;
    case BenchTestMode::FLASHERS:
      PrintFlasherTestHeader(pPpuc, number);
      runner.steps = BuildFlasherTestSteps(pPpuc, number);
      break;
  }

  if (runner.interactive)
  {
    for (const auto& step : runner.steps)
    {
      runner.stepsByNumber[step.number].push_back(step);
    }
    std::vector<int> orderedNumbers;
    orderedNumbers.reserve(runner.stepsByNumber.size());
    for (const auto& entry : runner.stepsByNumber)
    {
      orderedNumbers.push_back(entry.first);
    }
    std::sort(orderedNumbers.begin(), orderedNumbers.end());
    orderedNumbers.erase(std::unique(orderedNumbers.begin(), orderedNumbers.end()), orderedNumbers.end());

    for (const int itemNumber : orderedNumbers)
    {
      const auto it = runner.stepsByNumber.find(itemNumber);
      if (it == runner.stepsByNumber.end() || it->second.empty())
      {
        continue;
      }

      const BenchOutputStep& step = it->second.front();
      char buffer[512];
      snprintf(buffer, sizeof(buffer), "  #%d  board=%d port=%d  %s", step.number, step.board, step.port,
               step.description.c_str());
      runner.interactiveMenuLines.emplace_back(buffer);
    }

    PrintInteractiveBenchPrompt(runner);
  }

  return runner;
}

static void PrimeBenchSwitchStates(PPUC* pPpuc, BenchTestRunner& runner)
{
  if (runner.mode != BenchTestMode::SWITCHES)
  {
    return;
  }

  const auto switches = pPpuc->GetSwitches();
  PPUCSwitchState* switchState = nullptr;
  while ((switchState = pPpuc->GetNextSwitchState()) != nullptr)
  {
    auto it = std::find_if(switches.begin(), switches.end(),
                           [switchState](const PPUCSwitch& vswitch) { return vswitch.number == switchState->number; });
    if (it != switches.end() && IsVirtualizedBenchSwitch(pPpuc, *it))
    {
      continue;
    }

    const uint8_t normalizedState = switchState->state == 0 ? 0 : 1;
    runner.initialSwitchStates[switchState->number] = normalizedState;
    runner.currentSwitchStates[switchState->number] = normalizedState;
  }

  runner.initialSwitchStatesPrimed = true;
}


// Firmware image found in --firmware-path, named <controller>-<major>.<minor>.<patch>.uf2,
// which is what the io-boards CI produces.
struct FirmwareImage
{
    std::string path;
    uint32_t ordinal = 0;
    std::string version;
    uint8_t boardType = 0;
    bool hasBuildId = false;
    uint32_t buildId = 0;
    bool found = false;
};

// Whether this image should replace what the board is running.
//
// A higher version always wins - that is the rule for releases and it does not
// change. The rest exists because the firmware version is maintained by hand
// and does not move between snapshots: two materially different images
// routinely call themselves the same version, so version alone cannot answer
// the question for anything built between releases.
//
// Same version with a different build id counts as out of date, but only when
// explicitly allowed. "Different" is not "newer", and without the opt-in a
// snapshot pointed at a machine running a release would quietly replace it.
//
// A build id of zero means "not recorded" on either side and is never treated
// as a difference, or a locally built board would look perpetually out of
// date and be reflashed on every start.
static bool FirmwareIsOutOfDate(const PPUCBoardVersion& board, const FirmwareImage& image, bool allowDev)
{
    if (board.FirmwareOrdinal() < image.ordinal)
    {
        return true;
    }
    if (!allowDev || board.FirmwareOrdinal() != image.ordinal)
    {
        return false;
    }
    if (!image.hasBuildId || image.buildId == 0 || board.buildId == 0)
    {
        return false;
    }
    return board.buildId != image.buildId;
}

// Newest image for one board type.
//
// Matching on type is not tidiness: firmware is board-specific, and a
// directory holding images for several boards would otherwise hand every board
// whichever file happened to carry the highest version. The name before the
// version must be a board type the protocol knows; anything else is ignored
// rather than guessed at.
static FirmwareImage FindNewestFirmwareImage(const char* directory, uint8_t boardType)
{
    FirmwareImage best;
    if (!directory)
    {
        return best;
    }

    DIR* dir = opendir(directory);
    if (!dir)
    {
        printf("PPUC: firmware path '%s' cannot be opened; no update will be attempted\n", directory);
        return best;
    }

    while (struct dirent* entry = readdir(dir))
    {
        const uf2::FirmwareFileName parsed = uf2::ParseFirmwareFileName(entry->d_name);
        if (!parsed.valid)
        {
            continue;
        }
        if (ppuc::v2::BoardTypeFromName(parsed.boardTypeName.c_str()) != boardType)
        {
            continue;
        }

        // Ordered by version, then by whether a build id is present: given a
        // release and a snapshot of the same version, the snapshot is the more
        // specific answer and the one a dev run means to use.
        const bool better = !best.found || parsed.versionOrdinal > best.ordinal ||
                            (parsed.versionOrdinal == best.ordinal && parsed.hasBuildId && !best.hasBuildId);
        if (better)
        {
            best.found = true;
            best.ordinal = parsed.versionOrdinal;
            best.boardType = boardType;
            best.version = parsed.version;
            best.hasBuildId = parsed.hasBuildId;
            best.buildId = parsed.buildId;
            best.path = std::string(directory) + "/" + entry->d_name;
        }
    }
    closedir(dir);
    return best;
}

// Asks every board what it is running and says what would change.
//
// Flashing is not implemented yet, so this reports intent rather than acting
// on it. Reporting first is deliberate: the decision of which boards are out
// of date is the part worth getting visibly right before anything writes to
// one.

static void FirmwareProgress(uint8_t board, size_t sent, size_t total, void*)
{
    // One line per 10%, so a two-minute transfer says something without
    // scrolling a log nobody can read.
    static size_t lastDecile = 0;
    const size_t decile = total ? (sent * 10 / total) : 10;
    if (decile != lastDecile || sent == total)
    {
        printf("PPUC: board %u firmware %zu%% (%zu/%zu bytes)\n", board, decile * 10, sent, total);
        fflush(stdout);
        lastDecile = decile;
    }
}

// Flashes every board older than the supplied image.
//
// Boards that did not report a version are skipped deliberately: they are
// either absent or running firmware from before the admin envelope existed,
// and in both cases we would be sending an image to something that cannot
// have agreed to receive it. Those are flashed over USB instead.
// Why this board must not be flashed unattended, or nullptr when it may be.
//
// Firmware for a board type nobody has run drives pins whose direction has
// only ever been read off a schematic. On this hardware that is not a
// misbehaving output, it is an output pushed into an input - the same hazard
// that put board type on the wire to begin with. Someone flashing over USB has
// chosen that; an update that happens by itself while the machine boots has
// not.
static const char* FirmwareUpdateBlockedReason(const PPUCBoardVersion& board, bool allowUnvalidated)
{
    if (allowUnvalidated || ppuc::v2::BoardTypeValidatedOnHardware(board.boardType))
    {
        return nullptr;
    }
    return "firmware for this board type has never been run on real hardware";
}

static void PerformFirmwareUpdates(PPUC* pPpuc, const std::vector<PPUCBoardVersion>& versions,
                                   const char* firmwarePath, bool allowDev, bool allowUnvalidated)
{
    // The runtime loop must not transmit into the middle of a transfer.
    pPpuc->StopUpdates();

    size_t updated = 0, failed = 0;
    for (const PPUCBoardVersion& v : versions)
    {
        if (!v.responded)
        {
            continue;
        }

        const FirmwareImage meta = FindNewestFirmwareImage(firmwarePath, v.boardType);
        if (!meta.found || !FirmwareIsOutOfDate(v, meta, allowDev))
        {
            continue;
        }
        if (FirmwareUpdateBlockedReason(v, allowUnvalidated) != nullptr)
        {
            continue;
        }

        // Parsed per board, since each type has its own image. An unusable
        // file stops that board rather than the whole run.
        const uf2::Uf2Image parsed = uf2::LoadUf2File(meta.path);
        if (!parsed.valid)
        {
            ++failed;
            printf("PPUC: board %u: firmware image is unusable: %s\n", v.board, parsed.error.c_str());
            continue;
        }
        if (parsed.familyId != 0 && parsed.familyId != uf2::kUf2FamilyRp2040)
        {
            ++failed;
            printf("PPUC: board %u: image is not for an RP2040 (family 0x%08x)\n", v.board, parsed.familyId);
            continue;
        }

        const char* tn = ppuc::v2::BoardTypeName(v.boardType);
        printf("PPUC: updating board %u (%s) from %s to %s\n", v.board, tn ? tn : "?",
               v.FirmwareVersion().c_str(), meta.version.c_str());

        const PPUCFirmwareUpdateResult result = pPpuc->UpdateBoardFirmware(
            v.board, meta.boardType, parsed.data.data(), parsed.data.size(), FirmwareProgress, nullptr);

        if (result.ok)
        {
            ++updated;
            printf("PPUC: board %u updated; it is rebooting into the new firmware\n", v.board);
        }
        else
        {
            ++failed;
            printf("PPUC: board %u update failed after %zu bytes: %s (status %u)\n", v.board, result.bytesSent,
                   result.error.c_str(), result.status);
        }
    }

    if (updated > 0)
    {
        // Boards reboot and re-enumerate; configuration happens from scratch
        // afterwards, so the game must not start against half-configured
        // hardware.
        printf("PPUC: %zu board(s) updated, %zu failed. Restart ppuc-pinmame to run the game.\n", updated, failed);
    }
    else if (failed > 0)
    {
        printf("PPUC: no boards were updated; %zu attempt(s) failed\n", failed);
    }
}

static void ReportBoardFirmware(PPUC* pPpuc, const char* firmwarePath, bool allowUpdate, bool allowDev,
                                bool allowUnvalidated)
{
    const std::vector<PPUCBoardVersion> versions = pPpuc->QueryBoardVersions();
    if (versions.empty())
    {
        return;
    }

    for (const PPUCBoardVersion& v : versions)
    {
        if (v.responded)
        {
            const char* name = ppuc::v2::BoardTypeName(v.boardType);
            char build[24] = "";
            if (v.buildId != 0)
            {
                snprintf(build, sizeof(build), "+%08x", v.buildId);
            }
            printf("PPUC: board %u is %s, firmware %s%s (admin protocol %u.%u)\n", v.board,
                   name ? name : "an unknown board type", v.FirmwareVersion().c_str(), build,
                   v.adminProtocolMajor, v.adminProtocolMinor);
        }
        else
        {
            // Either absent, or running firmware from before the admin
            // envelope existed. Both are worth saying out loud.
            printf("PPUC: board %u did not report a firmware version\n", v.board);
        }
    }

    if (!firmwarePath)
    {
        return;
    }

    // One image per board type, selected per board: a directory holding
    // firmware for several boards has no single "newest image", and treating
    // it as if it did would hand every board whichever file carried the
    // highest version.
    size_t outdated = 0, blocked = 0;
    for (const PPUCBoardVersion& v : versions)
    {
        if (!v.responded)
        {
            continue;
        }

        const char* typeName = ppuc::v2::BoardTypeName(v.boardType);
        const FirmwareImage image = FindNewestFirmwareImage(firmwarePath, v.boardType);
        if (!image.found)
        {
            printf("PPUC: no %s image in '%s' for board %u\n", typeName ? typeName : "(unknown type)",
                   firmwarePath, v.board);
            continue;
        }

        if (!FirmwareIsOutOfDate(v, image, allowDev))
        {
            continue;
        }

        const char* blockedReason = FirmwareUpdateBlockedReason(v, allowUnvalidated);
        if (blockedReason != nullptr)
        {
            ++blocked;
            printf("PPUC: board %u (%s) has a newer image (%s -> %s) but will not be updated: %s\n", v.board,
                   typeName ? typeName : "?", v.FirmwareVersion().c_str(), image.version.c_str(), blockedReason);
            continue;
        }

        ++outdated;
        printf("PPUC: board %u (%s) is out of date: %s -> %s\n", v.board, typeName ? typeName : "?",
               v.FirmwareVersion().c_str(), image.version.c_str());
    }

    if (blocked > 0)
    {
        printf("PPUC: %zu board(s) skipped; flash them over USB, or pass "
               "--allow-unvalidated-firmware-update to accept the risk\n",
               blocked);
    }

    if (outdated == 0)
    {
        printf("PPUC: all boards are up to date\n");
    }
    else if (!allowUpdate)
    {
        printf("PPUC: %zu board(s) would be updated; pass --allow-firmware-update to permit it\n", outdated);
    }
    else
    {
        PerformFirmwareUpdates(pPpuc, versions, firmwarePath, allowDev, allowUnvalidated);
    }
}

static void WaitForCleanSwitchReplyCycle(PPUC* pPpuc, uint32_t baselineCount)
{
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline)
  {
    while (pPpuc->GetNextSwitchState() != nullptr)
    {
    }

    if (pPpuc->GetCleanSwitchReplyChainCount() > baselineCount)
    {
      return;
    }

    std::this_thread::sleep_for(std::chrono::microseconds(MAIN_LOOP_SLEEP_US));
  }
}

static void DrainSwitchUpdatesForTest(PPUC* pPpuc, BenchTestRunner& runner)
{
  if (runner.mode != BenchTestMode::SWITCHES)
  {
    while (pPpuc->GetNextSwitchState() != nullptr)
    {
    }
    return;
  }

  const auto switches = pPpuc->GetSwitches();
  PPUCSwitchState* switchState = nullptr;
  while ((switchState = pPpuc->GetNextSwitchState()) != nullptr)
  {
    auto it = std::find_if(switches.begin(), switches.end(),
                           [switchState](const PPUCSwitch& vswitch) { return vswitch.number == switchState->number; });
    if (it != switches.end() && IsVirtualizedBenchSwitch(pPpuc, *it))
    {
      continue;
    }

    const uint8_t normalizedState = switchState->state == 0 ? 0 : 1;
    if (!runner.initialSwitchStatesPrimed)
    {
      continue;
    }

    const auto currentIt = runner.currentSwitchStates.find(switchState->number);
    const uint8_t previousState = currentIt == runner.currentSwitchStates.end()
                                      ? runner.initialSwitchStates[switchState->number]
                                      : currentIt->second;
    const auto initialIt = runner.initialSwitchStates.find(switchState->number);
    if (initialIt == runner.initialSwitchStates.end())
    {
      continue;
    }
    runner.currentSwitchStates[switchState->number] = normalizedState;
    const uint8_t initialState = initialIt->second;
    const bool wasActive = previousState != initialState;
    const bool isActive = normalizedState != initialState;
    if (!wasActive && isActive)
    {
      const auto offUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
      if (offUntil > runner.switchFeedbackOffUntil)
      {
        runner.switchFeedbackOffUntil = offUntil;
      }
      if (pSpeechService != nullptr && it != switches.end())
      {
        pSpeechService->SpeakSwitchActivated(*it);
      }
    }
    const char* stateName = switchState->state ? "closed" : "open";

    if (it != switches.end())
    {
      printf("Switch updated: #%d, %d (%s)\nBoard: %d\nPort: %d\nDescription: %s\n\n", switchState->number,
             switchState->state, stateName, it->board, it->port, it->description.c_str());
    }
    else
    {
      printf("Switch updated: #%d, %d (%s)\n\n", switchState->number, switchState->state, stateName);
    }
    fflush(stdout);
  }

  UpdateSwitchFeedbackGi(pPpuc, runner);
}

static bool PollInteractiveBenchInput(BenchTestRunner& runner)
{
  if (!runner.interactive)
  {
    return true;
  }

#if defined(_WIN32) || defined(_WIN64)
  return true;
#else
  fd_set readSet;
  FD_ZERO(&readSet);
  FD_SET(STDIN_FILENO, &readSet);
  timeval timeout{0, 0};

  if (select(STDIN_FILENO + 1, &readSet, nullptr, nullptr, &timeout) <= 0 || !FD_ISSET(STDIN_FILENO, &readSet))
  {
    return true;
  }

  char ch = '\0';
  while (read(STDIN_FILENO, &ch, 1) == 1)
  {
    if (ch == 27 || ch == 'q' || ch == 'Q')
    {
      printf("\n");
      fflush(stdout);
      running = false;
      return false;
    }

    if (ch == '\r' || ch == '\n')
    {
      printf("\n");
      if (!runner.interactiveInput.empty())
      {
        const int selectedNumber = atoi(runner.interactiveInput.c_str());
        auto it = runner.stepsByNumber.find(selectedNumber);
        if (it != runner.stepsByNumber.end())
        {
          for (const auto& step : it->second)
          {
            runner.pendingInteractiveSteps.push_back(step);
          }
        }
        else
        {
          printf("Unknown test item number: %d\n", selectedNumber);
        }
        runner.interactiveInput.clear();
      }
      PrintInteractiveBenchMenu(runner);
      continue;
    }

    if (ch == 127 || ch == '\b')
    {
      if (!runner.interactiveInput.empty())
      {
        runner.interactiveInput.pop_back();
        printf("\b \b");
        fflush(stdout);
      }
      continue;
    }

    if (ch >= '0' && ch <= '9')
    {
      runner.interactiveInput.push_back(ch);
      putchar(ch);
      fflush(stdout);
    }
  }

  return true;
#endif
}

static bool ServiceBenchTestRunner(PPUC* pPpuc, BenchTestRunner& runner)
{
  DrainSwitchUpdatesForTest(pPpuc, runner);

  if (runner.mode == BenchTestMode::SWITCHES)
  {
    UpdateSwitchFeedbackGi(pPpuc, runner);
    return true;
  }

  if (!PollInteractiveBenchInput(runner))
  {
    return false;
  }

  if (runner.interactive)
  {
    const auto now = std::chrono::steady_clock::now();
    if (runner.waitingBetweenSteps)
    {
      if (now < runner.nextStepReadyAt)
      {
        return true;
      }
      runner.waitingBetweenSteps = false;
    }

    if (!runner.outputActive)
    {
      if (runner.pendingInteractiveSteps.empty())
      {
        return true;
      }

      const BenchOutputStep& step = runner.pendingInteractiveSteps.front();
      PrintBenchStepDetails(step);
      ApplyBenchOutput(pPpuc, step, true);
      runner.outputActive = true;
      runner.phaseDeadline = now + std::chrono::milliseconds(step.onDurationMs);
      return true;
    }

    if (now < runner.phaseDeadline)
    {
      return true;
    }

    const BenchOutputStep step = runner.pendingInteractiveSteps.front();
    ApplyBenchOutput(pPpuc, step, false);
    runner.outputActive = false;
    runner.pendingInteractiveSteps.pop_front();
    if (!runner.pendingInteractiveSteps.empty() && step.offDurationMs > 0)
    {
      runner.waitingBetweenSteps = true;
      runner.nextStepReadyAt = now + std::chrono::milliseconds(step.offDurationMs);
    }
    return true;
  }

  if (runner.index >= runner.steps.size())
  {
    return false;
  }

  const auto now = std::chrono::steady_clock::now();
  if (runner.waitingBetweenSteps)
  {
    if (now < runner.nextStepReadyAt)
    {
      return true;
    }
    runner.waitingBetweenSteps = false;
  }

  BenchOutputStep& step = runner.steps[runner.index];

  if (!runner.outputActive)
  {
    PrintBenchStepDetails(step);
    ApplyBenchOutput(pPpuc, step, true);
    runner.outputActive = true;
    runner.phaseDeadline = now + std::chrono::milliseconds(step.onDurationMs);
    return true;
  }

  if (now < runner.phaseDeadline)
  {
    return true;
  }

  ApplyBenchOutput(pPpuc, step, false);
  runner.outputActive = false;
  ++runner.index;
  if (runner.index < runner.steps.size() && step.offDurationMs > 0)
  {
    runner.waitingBetweenSteps = true;
    runner.nextStepReadyAt = now + std::chrono::milliseconds(step.offDurationMs);
  }
  return runner.index < runner.steps.size();
}

static void CleanupBenchTestRunner(PPUC* pPpuc, const BenchTestRunner& runner)
{
  for (const auto& step : runner.steps)
  {
    ApplyBenchOutput(pPpuc, step, false);
  }

  if (runner.restoreGiOnExit)
  {
    printf("\nRestoring GI String 1 to brightness %d\n", 8);
    pPpuc->SetGIState(/* string */ 1, /* full brightness */ 8);
  }
  else if (runner.mode == BenchTestMode::SWITCHES && pPpuc->GetPlatform() != PLATFORM_WPC)
  {
    pPpuc->SetGIState(/* string */ 1, /* full brightness */ 8);
  }
}

static struct cag_option options[] = {
    {.identifier = '$',
     .access_name = "game",
     .value_name = "VALUE",
     .description = "Game folder containing io-boards.yaml, ppuc.ini, rules, pup, and pinmame assets"},
    {.identifier = 'c',
     .access_letters = "c",
     .access_name = "config",
     .value_name = "VALUE",
     .description = "Path to config file (optional)"},
    {.identifier = 'z',
     .access_name = "ini-file",
     .value_name = "VALUE",
     .description = "Path to ppuc runtime INI file (optional)"},
    {.identifier = 'r',
     .access_letters = "r",
     .access_name = "rom",
     .value_name = "VALUE",
     .description = "Path to ROM file (optional, overwrites setting in config file)"},
    {.identifier = 'b',
     .access_letters = "b",
     .access_name = "backbox-address",
     .value_name = "VALUE",
     .description = "Connect backbox via network address (optional)"},
    {.identifier = 'a',
     .access_name = "backbox-port",
     .value_name = "VALUE",
     .description = "Backbox port (optional, default is 6789)"},
    {.identifier = 's',
     .access_letters = "s",
     .access_name = "serial",
     .value_name = "VALUE",
     .description = "Serial device (optional, overwrites setting in config file)"},
    {.identifier = 'A',
     .access_name = "pinmame-path",
     .value_name = "VALUE",
     .description = "PinMAME base folder (optional, default is the per-user pinmame folder)"},
    {.identifier = 'n',
     .access_letters = "n",
     .access_name = "no-serial",
     .value_name = NULL,
     .description = "No serial communication to controllers (optional)"},
    {.identifier = 'M', .access_name = "no-sound", .value_name = NULL, .description = "Turn off sound (optional)"},
    {.identifier = 't',
     .access_name = "engine",
     .value_name = "VALUE",
     .description = "Game engine: pinmame (default) or script for ROM-less games"},
    {.identifier = '[',
     .access_name = "no-display",
     .value_name = NULL,
     .description = "Do not open or search for any DMD (optional, for headless runs)"},
    {.identifier = ']',
     .access_name = "exit-after-ms",
     .value_name = "VALUE",
     .description = "Exit after the given number of milliseconds (optional, for tests and burn-in)"},
    {.identifier = 'W',
     .access_name = "speech",
     .value_name = NULL,
     .description = "Enable speech callouts (optional)"},
    {.identifier = 'Y',
     .access_name = "greeting",
     .value_name = NULL,
     .description = "Speak a startup greeting for speech debugging (optional)"},
    {.identifier = 'o',
     .access_name = "music-files",
     .value_name = "VALUE",
     .description = "Comma-separated MP3 playlist for in-game background music (optional)"},
    {.identifier = 'q',
     .access_name = "music-gap-ms",
     .value_name = "VALUE",
     .description = "Gap between background music tracks in milliseconds (optional, default 2000)"},
    {.identifier = 'U',
     .access_name = "speech-backend",
     .value_name = "VALUE",
     .description = "Speech backend: auto, flite, espeak-ng (optional)"},
    {.identifier = 'v',
     .access_name = "speech-voice",
     .value_name = "VALUE",
     .description = "Speech voice name, mainly for espeak-ng (optional)"},
    {.identifier = 'w',
     .access_name = "speech-rate",
     .value_name = "VALUE",
     .description = "Speech rate in words per minute for espeak-ng (optional)"},
    {.identifier = 'x',
     .access_name = "speech-pitch",
     .value_name = "VALUE",
     .description = "Speech pitch 0-100 for espeak-ng (optional)"},
    {.identifier = 'u',
     .access_letters = "u",
     .access_name = "serum",
     .value_name = NULL,
     .description = "Enable legacy Serum/AltColor colorization (optional)"},
    {.identifier = '~',
     .access_name = "altcolor",
     .value_name = NULL,
     .description = "Enable AltColor DMD colorization (optional)"},
    {.identifier = 'T',
     .access_name = "serum-timeout",
     .value_name = "VALUE",
     .description = "Serum timeout in milliseconds to ignore unknown frames (optional)"},
    {.identifier = 'P',
     .access_name = "serum-skip-frames",
     .value_name = "VALUE",
     .description = "Serum ignore number of unknown frames (optional)"},
    {.identifier = 'p',
     .access_letters = "p",
     .access_name = "pup",
     .value_name = NULL,
     .description = "Enable PUP backglass videos through the plugin host (optional)"},
    {.identifier = '9',
     .access_name = "altsound",
     .value_name = NULL,
     .description = "Enable AltSound through the plugin host (optional)"},
    {.identifier = '?',
     .access_name = "b2s",
     .value_name = NULL,
     .description = "Enable B2S backglass rendering through the plugin host (optional)"},
    {.identifier = '^',
     .access_name = "plugin-dir",
     .value_name = "VALUE",
     .description = "VPX plugin directory (optional, default ../vpinball/plugins)"},
    {.identifier = '&',
     .access_name = "pup-folder",
     .value_name = "VALUE",
     .description = "PinUp Player base folder containing pupvideos (optional)"},
    {.identifier = '*',
     .access_name = "altsound-folder",
     .value_name = "VALUE",
     .description = "AltSound folder or base folder (optional)"},
    {.identifier = 'y',
     .access_name = "rules",
     .value_name = "VALUE",
     .description = "Path to Lua rules file (optional)"},
    {.identifier = 'i',
     .access_letters = "i",
     .access_name = "console-display",
     .value_name = NULL,
     .description = "Enable console display (optional)"},
    {.identifier = 'm',
     .access_letters = "m",
     .access_name = "dump-display",
     .value_name = NULL,
     .description = "Write DMD txt dump files (optional)"},
    {.identifier = 'X',
     .access_name = "dump-dmd-txt",
     .value_name = NULL,
     .description = "Write DMD txt dump files (optional)"},
    {.identifier = 'd',
     .access_letters = "d",
     .access_name = "debug",
     .value_name = NULL,
     .description = "Enable all debug output (optional)"},
    {.identifier = 'e',
     .access_letters = "e",
     .access_name = "debug-errors",
     .value_name = NULL,
     .description = "Enable communication/protocol error details without full debug output (optional)"},
    {.identifier = '6',
     .access_name = "skip-boards",
     .value_name = "VALUE",
     .description = "Skip configured boards by CSV list without editing the game YAML"},
    {.identifier = '(',
     .access_name = "firmware-path",
     .value_name = "PATH",
     .description = "Directory holding board firmware images (<board type>-<version>[+<build id>].uf2)"},
    {.identifier = '|',
     .access_name = "allow-unvalidated-firmware-update",
     .description = "Update boards whose type has never been validated on hardware (USB flashing is safer)"},
    {.identifier = ')',
     .access_name = "allow-dev-firmware-update",
     .description = "Also update a board whose firmware matches by version but was built from a different commit"},
    {.identifier = '#',
     .access_name = "allow-firmware-update",
     .description = "Permit flashing boards whose firmware is older than the image in --firmware-path"},
    {.identifier = '7',
     .access_name = "switch-reply-delay-us",
     .value_name = "VALUE",
     .description = "Per-board switch reply delay in microseconds"},
    {.identifier = 'g',
     .access_name = "switch-refresh-idle-ms",
     .value_name = "VALUE",
     .description = "Force a full switch refresh after this many ms without non-button switch updates"},
    {.identifier = '%',
     .access_name = "output-frame-interval-ms",
     .value_name = "VALUE",
     .description = "Runtime output frame interval in milliseconds; lower values increase switch poll cadence"},
    {.identifier = 'B',
     .access_name = "ball-search",
     .value_name = NULL,
     .description = "Enable host-side ball search for coils marked ballSearch in the game YAML"},
    {.identifier = '!',
     .access_name = "ball-search-delay-ms",
     .value_name = "VALUE",
     .description = "Start ball search after this many ms without non-button switch updates during play"},
    {.identifier = '@',
     .access_name = "ball-search-round-delay-ms",
     .value_name = "VALUE",
     .description = "Delay between complete ball-search coil rounds in milliseconds"},
    {.identifier = '8',
     .access_name = "close-coin-door",
     .value_name = NULL,
     .description = "Force the configured coin-door-closed switch closed when it is virtualized"},
    {.identifier = 'V',
     .access_name = "hard-reset",
     .value_name = NULL,
     .description = "Use hard reset instead of soft restart for board startup"},
    {.identifier = 'S',
     .access_name = "debug-switches",
     .value_name = NULL,
     .description = "Enable switches debug output (optional)"},
    {.identifier = 'C',
     .access_name = "debug-coils",
     .value_name = NULL,
     .description = "Enable coils debug output (optional)"},
    {.identifier = 'L',
     .access_name = "debug-lamps",
     .value_name = NULL,
     .description = "Enable lamps debug output (optional)"},
    {.identifier = 'f',
     .access_name = "debug-effects",
     .value_name = NULL,
     .description = "Enable effect trigger debug output (optional)"},
    {.identifier = '{',
     .access_name = "debug-sound-commands",
     .value_name = NULL,
     .description = "Print PinMAME sound command IDs for building altsound packs (optional)"},
    {.identifier = '0', .access_name = "switch-test", .value_name = NULL, .description = "Run switch test"},
    {.identifier = '1', .access_name = "coil-test", .value_name = NULL, .description = "Run coil test"},
    {.identifier = '2', .access_name = "lamp-test", .value_name = NULL, .description = "Run lamp test"},
    {.identifier = '3', .access_name = "gi-test", .value_name = NULL, .description = "Run lamp test"},
    {.identifier = '4', .access_name = "flasher-test", .value_name = NULL, .description = "Run flasher test"},
    {.identifier = 'Z',
     .access_name = "interactive",
     .value_name = NULL,
     .description = "Interactive coil/lamp/flasher test selection"},
    {.identifier = '5',
     .access_name = "number",
     .value_name = "VALUE",
     .description = "Specifiy a specific number for coil/lamp/GI/flasher test"},
    {.identifier = 'D',
     .access_name = "translite",
     .value_name = "VALUE",
     .description = "Translite file, only used for game in play if a attract mode translite is set as well"},
    {.identifier = 'E',
     .access_name = "translite-attract",
     .value_name = "VALUE",
     .description = "Translite file for attract mode"},
    {.identifier = 'F',
     .access_name = "translite-window",
     .value_name = NULL,
     .description = "Show translite in window instead of fullscreen"},
    {.identifier = 'G',
     .access_name = "translite-width",
     .value_name = "VALUE",
     .description = "Translite width, default 1920"},
    {.identifier = 'H',
     .access_name = "translite-height",
     .value_name = "VALUE",
     .description = "Translite height, default 1080"},
    {.identifier = 'I',
     .access_name = "translite-screen",
     .value_name = "VALUE",
     .description = "Show translite on a specific screen"},
    {.identifier = 'J', .access_name = "virtual-dmd", .value_name = NULL, .description = "Show virtual DMD"},
    {.identifier = 'K',
     .access_name = "virtual-dmd-hd",
     .value_name = NULL,
     .description = "Show virtual DMD in HD mode"},
    {.identifier = 'N',
     .access_name = "virtual-dmd-window",
     .value_name = NULL,
     .description = "Show virtual DMD in window instead of fullscreen"},
    {.identifier = 'O',
     .access_name = "virtual-dmd-width",
     .value_name = "VALUE",
     .description = "Virtual DMD width, default 1920"},
    {.identifier = 'Q',
     .access_name = "virtual-dmd-height",
     .value_name = "VALUE",
     .description = "Virtual DMD height, default 1080"},
    {.identifier = 'R',
     .access_name = "virtual-dmd-screen",
     .value_name = "VALUE",
     .description = "Show virtual DMD on a specific screen"},
    {.identifier = 'l',
     .access_name = "virtual-dmd-renderer",
     .value_name = "VALUE",
     .description = "Virtual DMD renderer: dots, squares, scale2x, scale4x, scale2x-dots, scale4x-dots, "
                    "scale2x-squares, scale4x-squares, smooth, xbrz"},
    {.identifier = 'j',
     .access_name = "virtual-dmd-x",
     .value_name = "VALUE",
     .description = "Virtual DMD x position relative to the selected screen"},
    {.identifier = 'k',
     .access_name = "virtual-dmd-y",
     .value_name = "VALUE",
     .description = "Virtual DMD y position relative to the selected screen"},
    {.identifier = 'h', .access_letters = "h", .access_name = "help", .description = "Show help"}};

void DMDUTILCALLBACK DMDUtilLogCallback(DMDUtil_LogLevel logLevel, const char* format, va_list args)
{
  char buffer[1024];
  vsnprintf(buffer, sizeof(buffer), format, args);
  uint64_t now = CurrentUnixMs();

  if (logLevel == DMDUtil_LogLevel_INFO)
  {
    char logLine[1152];
    snprintf(logLine, sizeof(logLine), "%" PRIu64 " INFO: %s", now, buffer);
    PrintFlushedLogLine("", logLine);
  }
  else if (logLevel == DMDUtil_LogLevel_DEBUG)
  {
    char logLine[1152];
    snprintf(logLine, sizeof(logLine), "%" PRIu64 " DEBUG: %s", now, buffer);
    PrintFlushedLogLine("", logLine);
  }
  else if (logLevel == DMDUtil_LogLevel_ERROR)
  {
    char logLine[1152];
    snprintf(logLine, sizeof(logLine), "%" PRIu64 " ERROR: %s", now, buffer);
    PrintFlushedLogLine("", logLine);
  }
}

void DMDUTILCALLBACK OnDmdPupTrigger(uint16_t id, void* userData)
{
  auto* mediaHost = static_cast<MediaPluginHost*>(userData);
  if (mediaHost != nullptr)
  {
    mediaHost->QueueDmdTrigger(id);
  }
}

void signal_handler_graceful(int sig)
{
  running = false;
  shutdown_requested = 1;
}

// The host side of the engine seam. Every method here is a body that used to
// live in a PinMAME callback, with the PinMAME types stripped out; the fan-out
// to libppuc, the interceptor, LuaRulesEngine, the media host and the DMD is
// unchanged.
//
// Methods may be called from the engine's own thread. That is the contract
// GameEngineHost documents, and it is why the interceptor takes a lock.
struct PpucEngineHost final : GameEngineHost
{
  void OnCoilChanged(uint16_t number, uint8_t state) override
  {
    if (pMediaPluginHost != nullptr)
    {
      pMediaPluginHost->QueueEvent('S', number, state);
    }

    g_interceptorOutputs.ApplyEngineCoil(pPpuc, number, state);

    for (const PPUCCoilGiMapping& mapping : pPpuc->GetCoilGiMappings())
    {
      if (mapping.coil != number)
      {
        continue;
      }
      const uint8_t brightness = state != 0 ? mapping.onBrightness : mapping.offBrightness;
      if (opt_debug || opt_debug_coils)
      {
        printf("Coil GI mapping: solenoid=%d, state=%d, gi=%u, brightness=%u\n", number, state, mapping.gi, brightness);
      }
      pPpuc->SetGIState(mapping.gi, brightness);
    }

    // Deliberately outside any interceptor lock: a Lua onCoilChanged handler may
    // call ppuc.pulseCoil, which comes straight back into the interceptor.
    if (pLuaRulesEngine)
    {
      pLuaRulesEngine->OnCoilState(number, state);
    }
  }

  void OnGameRunningChanged(bool gameRunning) override
  {
    ball_search_game_running.store(gameRunning, std::memory_order_release);
    if (pAudioOutput != nullptr)
    {
      pAudioOutput->SetMusicEnabled(gameRunning);
    }

    if (gameRunning)
    {
      if (opt_debug || opt_debug_coils)
      {
        printf("Game started\n");
      }
      QueueTransliteRender(RenderCommand::RENDER_GAME);
    }
    else if (HasTransliteAttractImage())
    {
      if (opt_debug || opt_debug_coils)
      {
        printf("Game stopped\n");
      }
      QueueTransliteRender(RenderCommand::RENDER_ATTRACT);
    }

    if (pLuaRulesEngine)
    {
      pLuaRulesEngine->SetAttractMode(!gameRunning);
      if (!gameRunning)
      {
        pLuaRulesEngine->SetCurrentBall(0);
        pLuaRulesEngine->SetCurrentPlayer(0);
      }
    }

    if (g_playfieldAssistEnabled)
    {
      // Neither feature should act in attract.
      g_playfieldAssist.SetPlayActive(gameRunning);
    }
  }

  void OnRunStateChanged(int state) override
  {
    if (state == 0)
    {
      running = false;
      shutdown_requested = 1;
    }
  }

  void OnDmdFrame(const uint8_t* pData, int depth, int width, int height) override
  {
    pDmd->UpdateData(const_cast<uint8_t*>(pData), depth, width, height, 255, 255, 255);
  }

  void OnSegmentDigit(int digit, int value) override
  {
    if (pMediaPluginHost != nullptr)
    {
      pMediaPluginHost->QueueSegmentDisplay(digit, value);
    }
  }

  void OnPlayerScore(int player, int score) override
  {
    if (pMediaPluginHost != nullptr)
    {
      pMediaPluginHost->QueuePlayerScore(player, score);
    }
  }

  int OnAudioFormat(int sampleRate, int channels, int samplesPerFrame) override
  {
    if (pAudioOutput != nullptr)
    {
      pAudioOutput->ConfigureGameFormat(sampleRate, channels);
    }
    else
    {
      printf("Audio output not initialized.\n");
    }
    return samplesPerFrame;
  }

  void OnAudioFrames(const int16_t* pSamples, int samples) override
  {
    if (pAudioOutput != nullptr)
    {
      pAudioOutput->QueueGameFrames(pSamples, static_cast<size_t>(samples));
    }
  }

  void OnSoundCommand(int boardNo, int cmd) override
  {
    if (pMediaPluginHost != nullptr)
    {
      pMediaPluginHost->OnSoundCommand(boardNo, cmd);
    }
  }

  void OnCurrentBallChanged(uint8_t ball) override
  {
    if (pLuaRulesEngine)
    {
      pLuaRulesEngine->SetCurrentBall(ball);
    }
    // Drives the per-ball reset of warnings and the ball-save arming, in both
    // engines: under PinMAME this comes from the NVRAM tracking poll, under
    // GameCore from the ball number it owns. Ball 0 means no game in progress.
    if (g_playfieldAssistEnabled)
    {
      if (ball == 0)
      {
        g_playfieldAssist.OnBallEnd();
      }
      else
      {
        g_playfieldAssist.OnBallStart(ball);
      }
    }
  }

  void OnCurrentPlayerChanged(uint8_t player) override
  {
    if (pLuaRulesEngine)
    {
      pLuaRulesEngine->SetCurrentPlayer(player);
    }
    if (g_playfieldAssistEnabled)
    {
      g_playfieldAssist.SetCurrentPlayer(player);
    }
  }

  void OnLogMessage(bool error, const char* message) override
  {
    PrintFlushedLogLine(error ? "ERROR: " : "INFO: ", message);
  }
};

// Translates a GameCore event into the matching `ppuc.on*` handler.
//
// Kept as an explicit table rather than a generated name, because the argument
// list is part of the API and a rules author reads it here.
static void DispatchGameEventToRules(const GameEvent& event)
{
  if (pLuaRulesEngine == nullptr)
  {
    return;
  }

  switch (event.type)
  {
    case GameEventType::AttractStart: pLuaRulesEngine->CallGameHandler("onAttractStart"); break;
    case GameEventType::AttractEnd: pLuaRulesEngine->CallGameHandler("onAttractEnd"); break;
    case GameEventType::GameStart: pLuaRulesEngine->CallGameHandler("onGameStart", {event.value}); break;
    case GameEventType::PlayerAdded: pLuaRulesEngine->CallGameHandler("onPlayerAdded", {event.player}); break;
    case GameEventType::StartRejected:
      pLuaRulesEngine->CallGameHandler("onStartRejected", {StartRejectReasonName(event.reason)});
      break;
    case GameEventType::BallStart:
      pLuaRulesEngine->CallGameHandler("onBallStart", {event.player, event.ball});
      break;
    case GameEventType::BallServed:
      pLuaRulesEngine->CallGameHandler("onBallServed", {event.player, event.ball});
      break;
    case GameEventType::BallServeFailed:
      pLuaRulesEngine->CallGameHandler("onBallServeFailed", {event.value});
      break;
    case GameEventType::BallStuck:
      pLuaRulesEngine->CallGameHandler("onBallStuck", {event.player, event.ball});
      break;
    case GameEventType::BallEnd: pLuaRulesEngine->CallGameHandler("onBallEnd", {event.player, event.ball}); break;
    case GameEventType::BonusCount:
      // Must reach ppuc.game.bonusDone(), or the bonus times out and the game
      // continues anyway. A rules bug does not brick the machine.
      pLuaRulesEngine->CallGameHandler("onBonusCount", {event.player, event.ball});
      break;
    case GameEventType::ExtraBall: pLuaRulesEngine->CallGameHandler("onExtraBall", {event.player}); break;
    case GameEventType::Score:
      pLuaRulesEngine->CallGameHandler("onScore", {event.player, event.value, event.total});
      break;
    case GameEventType::TiltWarning:
      pLuaRulesEngine->CallGameHandler("onTiltWarning", {event.player, event.value, event.total});
      break;
    case GameEventType::Tilt: pLuaRulesEngine->CallGameHandler("onTilt", {event.player, event.ball}); break;
    case GameEventType::SlamTilt: pLuaRulesEngine->CallGameHandler("onSlamTilt"); break;
    case GameEventType::Replay: pLuaRulesEngine->CallGameHandler("onReplay", {event.player, event.value}); break;
    case GameEventType::Match: pLuaRulesEngine->CallGameHandler("onMatch", {event.value, event.total}); break;
    case GameEventType::GameEnd:
      pLuaRulesEngine->CallGameHandler("onGameEnd", {event.player, event.value, event.total});
      break;
    case GameEventType::CreditsChanged:
      pLuaRulesEngine->CallGameHandler("onCreditsChanged", {event.value});
      break;
  }
}

// Applies whatever tilt warnings and ball save asked for this tick.
//
// The tilt decision reaches the engines differently, and deliberately so:
// PlayfieldAssist already forwarded the bob switch for the hit that tilts, so a
// ROM tilts itself; GameCore does not watch the bob at all and is told directly.
static void ServicePlayfieldAssist(GameEngine* pEngine)
{
  if (!g_playfieldAssistEnabled)
  {
    return;
  }

  g_playfieldAssist.Update();

  for (const PlayfieldAssist::Action& action : g_playfieldAssist.TakeActions())
  {
    switch (action.type)
    {
      case PlayfieldAssist::ActionType::PulseCoil:
        g_interceptorOutputs.PulseCoil(pPpuc, action.number, action.durationMs);
        break;
      case PlayfieldAssist::ActionType::SetLamp:
        g_interceptorOutputs.ApplyEngineLamp(pPpuc, action.number, action.value);
        break;
    }
  }

  for (const PlayfieldAssist::Event& event : g_playfieldAssist.TakeEvents())
  {
    switch (event.type)
    {
      case PlayfieldAssist::EventType::TiltWarning:
        printf("Tilt warning %lld of %lld for player %u\n", static_cast<long long>(event.value),
               static_cast<long long>(event.value + event.total), event.player);
        if (pLuaRulesEngine != nullptr)
        {
          pLuaRulesEngine->CallGameHandler("onTiltWarning", {event.player, event.value, event.total});
        }
        break;
      case PlayfieldAssist::EventType::Tilt:
        if (g_pGameCore != nullptr)
        {
          g_pGameCore->Tilt();
        }
        break;
      case PlayfieldAssist::EventType::SlamTilt:
        if (g_pGameCore != nullptr)
        {
          g_pGameCore->SlamTilt();
        }
        break;
      case PlayfieldAssist::EventType::BallSaved:
        printf("Ball saved (%lld this ball)\n", static_cast<long long>(event.value));
        if (pLuaRulesEngine != nullptr)
        {
          pLuaRulesEngine->CallGameHandler("onBallSaved", {event.player, event.value});
        }
        break;
      case PlayfieldAssist::EventType::BallSaveArmed:
      case PlayfieldAssist::EventType::BallSaveExpired:
      case PlayfieldAssist::EventType::TiltWarningsAwarded:
        break;
    }
  }
}

int main(int argc, char** argv)
{
  // version info
  printf("PPUC Version: %s\n", PPUC_EXECUTABLE_VERSION);
  printf("Commit SHA: %s\n", PPUC_EXECUTABLE_SHA);

  // Setup signal handlers to allow graceful termination
  signal(SIGINT, signal_handler_graceful);
  signal(SIGHUP, signal_handler_graceful);
  signal(SIGTERM, signal_handler_graceful);
  signal(SIGQUIT, signal_handler_graceful);
  signal(SIGABRT, signal_handler_graceful);

  char identifier;
  cag_option_context cag_context;
  const char* config_file = NULL;
  const char* opt_ini_file = NULL;
  const char* opt_rules = NULL;
  bool opt_rules_enabled = false;
  const char* opt_backbox_address = NULL;
  uint16_t opt_backbox_port = 6789;
  const char* opt_serial = NULL;
  const char* opt_skip_boards = NULL;
  const char* opt_firmware_path = NULL;
  bool opt_allow_firmware_update = false;
  bool opt_allow_dev_firmware_update = false;
  bool opt_allow_unvalidated_firmware_update = false;
  const char* opt_switch_reply_delay_us_arg = NULL;
  uint32_t opt_switch_reply_delay_us = 0;
  const char* opt_switch_refresh_idle_ms_arg = NULL;
  uint32_t opt_switch_refresh_idle_ms = kDefaultSwitchRefreshIdleMs;
  const char* opt_output_frame_interval_ms_arg = NULL;
  uint32_t opt_output_frame_interval_ms = kDefaultOutputFrameIntervalMs;
  bool opt_ball_search = false;
  const char* opt_ball_search_delay_ms_arg = NULL;
  uint32_t opt_ball_search_delay_ms = kDefaultBallSearchDelayMs;
  const char* opt_ball_search_round_delay_ms_arg = NULL;
  uint32_t opt_ball_search_round_delay_ms = kDefaultBallSearchRoundDelayMs;
  uint8_t opt_coil_hold_frames = 3;
  bool opt_close_coin_door = false;
  uint8_t opt_serum_timeout = 0;
  uint8_t opt_serum_skip_frames = 0;
  bool opt_dump = false;
  bool opt_switch_test = false;
  bool opt_coil_test = false;
  bool opt_lamp_test = false;
  bool opt_gi_test = false;
  bool opt_flasher_test = false;
  uint8_t opt_number = 0;
  const char* opt_translite = NULL;
  const char* opt_translite_attract = NULL;
  bool opt_translite_window = false;
  uint16_t opt_translite_width = 1920;
  uint16_t opt_translite_height = 1080;
  int8_t opt_translite_screen = -1;
  bool opt_virtual_dmd = false;
  bool opt_virtual_dmd_hd = false;
  bool opt_virtual_dmd_window = false;
  uint16_t opt_virtual_dmd_width = 1280;
  uint16_t opt_virtual_dmd_height = 320;
  int8_t opt_virtual_dmd_screen = -1;
  int opt_virtual_dmd_x = SDL_WINDOWPOS_UNDEFINED;
  int opt_virtual_dmd_y = SDL_WINDOWPOS_UNDEFINED;
  int opt_virtual_dmd_rotation = 0;
  int opt_rounded_corners = 0;
#ifdef PPUC_USE_KMSDMD
  DMDUtil::KMSDMD* pVirtualDMD = nullptr;
#else
  DMDUtil::SDLDMD* pVirtualDMD = nullptr;
#endif

  for (int i = 1; i < argc; ++i)
  {
    const char* arg = argv[i];
    if (strcmp(arg, "-z") == 0 || strcmp(arg, "--ini-file") == 0)
    {
      if (i + 1 < argc)
      {
        opt_ini_file = argv[++i];
      }
    }
    else if (strcmp(arg, "--game") == 0)
    {
      if (i + 1 < argc)
      {
        opt_game_folder = argv[++i];
      }
    }
    else if (strcmp(arg, "--b2s") == 0)
    {
      opt_b2s = true;
    }
    else if (strncmp(arg, "--ini-file=", 11) == 0)
    {
      opt_ini_file = arg + 11;
    }
    else if (strncmp(arg, "--game=", 7) == 0)
    {
      opt_game_folder = arg + 7;
    }
  }

  if (HasOptionValue(opt_game_folder))
  {
    const std::filesystem::path gameFolder(opt_game_folder);
    if (opt_ini_file == nullptr)
    {
      const std::filesystem::path iniPath = gameFolder / "ppuc.ini";
      std::error_code ec;
      if (std::filesystem::is_regular_file(iniPath, ec))
      {
        opt_ini_file = DuplicatePathString(iniPath);
      }
    }
    if (config_file == nullptr)
    {
      config_file = DuplicatePathString(gameFolder / "io-boards.yaml");
    }
    if (opt_pinmame_path == nullptr)
    {
      opt_pinmame_path = DuplicatePathString(gameFolder / "pinmame");
    }
    if (opt_pup_folder == nullptr)
    {
      opt_pup_folder = DuplicatePathString(gameFolder / "pup");
    }
  }

  const auto loadIniFile = [&](const char* iniFile) -> bool
  {
    if (iniFile == nullptr || iniFile[0] == '\0')
    {
      return true;
    }

    std::ifstream file(iniFile);
    if (!file.is_open())
    {
      fprintf(stderr, "Unable to open ini file: %s\n", iniFile);
      return false;
    }

    std::string section;
    std::string line;
    while (std::getline(file, line))
    {
      std::string trimmed = TrimIniValue(line);
      if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';')
      {
        continue;
      }

      if (trimmed.front() == '[' && trimmed.back() == ']')
      {
        section = TrimIniValue(trimmed.substr(1, trimmed.size() - 2));
        continue;
      }

      const size_t equals = trimmed.find('=');
      if (equals == std::string::npos)
      {
        continue;
      }

      const std::string key = TrimIniValue(trimmed.substr(0, equals));
      const std::string value = TrimIniValue(trimmed.substr(equals + 1));
      if (value.empty())
      {
        continue;
      }

      if (section == "Game")
      {
        if (key == "Rom")
          opt_rom = DuplicateOptionalIniString(value);
        else if (key == "Engine")
          opt_engine = DuplicateIniString(value);
      }
      else if (section == "Paths")
      {
        if (key == "ConfigFile")
          config_file = DuplicateIniString(value);
        else if (key == "Rom")
          opt_rom = DuplicateOptionalIniString(value);
        else if (key == "Serial")
          opt_serial = DuplicateOptionalIniString(value);
        else if (key == "PinmamePath")
          opt_pinmame_path = DuplicateOptionalIniString(value);
        else if (key == "Rules")
        {
          opt_rules = DuplicateOptionalIniString(value);
          opt_rules_enabled = HasOptionValue(opt_rules);
        }
        else if (key == "MusicFiles")
          opt_music_files = DuplicateOptionalIniString(value);
        else if (key == "MusicGapMs")
          opt_music_gap_ms = static_cast<uint32_t>(atoi(value.c_str()));
        else if (key == "Translite")
          opt_translite = DuplicateOptionalIniString(value);
        else if (key == "TransliteAttract")
          opt_translite_attract = DuplicateOptionalIniString(value);
      }
      else if (section == "Backbox")
      {
        if (key == "Address")
          opt_backbox_address = DuplicateOptionalIniString(value);
        else if (key == "Port")
          opt_backbox_port = static_cast<uint16_t>(atoi(value.c_str()));
      }
      else if (section == "Runtime")
      {
        if (key == "NoSerial")
          opt_no_serial = ParseIniBool(value);
        else if (key == "NoDisplay")
          opt_no_display = ParseIniBool(value);
        else if (key == "NoSound")
          opt_no_sound = ParseIniBool(value);
        else if (key == "Debug")
          opt_debug = ParseIniBool(value);
        else if (key == "DebugErrors")
          opt_debug_errors = ParseIniBool(value);
        else if (key == "DebugSwitches")
          opt_debug_switches = ParseIniBool(value);
        else if (key == "DebugCoils")
          opt_debug_coils = ParseIniBool(value);
        else if (key == "DebugLamps")
          opt_debug_lamps = ParseIniBool(value);
        else if (key == "DebugEffects")
          opt_debug_effects = ParseIniBool(value);
        else if (key == "DebugSoundCommands")
          opt_debug_sound_commands = ParseIniBool(value);
        else if (key == "Serum" || key == "AltColor")
          opt_serum = ParseIniBool(value);
        else if (key == "Rules")
          opt_rules_enabled = ParseIniBool(value);
        else if (key == "SerumTimeout")
          opt_serum_timeout = static_cast<uint8_t>(atoi(value.c_str()));
        else if (key == "SerumSkipFrames")
          opt_serum_skip_frames = static_cast<uint8_t>(atoi(value.c_str()));
        else if (key == "PUP")
          opt_pup = ParseIniBool(value);
        else if (key == "AltSound")
          opt_altsound = ParseIniBool(value);
        else if (key == "B2S")
          opt_b2s = ParseIniBool(value);
        else if (key == "B2SSegmentAngleDegrees")
          opt_b2s_segment_angle_degrees = static_cast<float>(atof(value.c_str()));
        else if (key == "B2SSegmentGlow")
          opt_b2s_segment_glow = static_cast<float>(atof(value.c_str()));
        else if (key == "B2SSegmentSmoothing")
          opt_b2s_segment_smoothing = ParseIniBool(value);
        else if (key == "PluginDir")
          opt_plugin_dir = DuplicateOptionalIniString(value);
        else if (key == "PUPFolder")
          opt_pup_folder = DuplicateOptionalIniString(value);
        else if (key == "AltSoundFolder")
          opt_altsound_folder = DuplicateOptionalIniString(value);
        else if (key == "ConsoleDisplay")
          opt_console_display = ParseIniBool(value);
        else if (key == "DumpDisplay" || key == "DumpDmdTxt")
          opt_dump = ParseIniBool(value);
        else if (key == "SkipBoards")
          opt_skip_boards = DuplicateOptionalIniString(value);
        else if (key == "FirmwarePath")
          opt_firmware_path = DuplicateOptionalIniString(value);
        else if (key == "AllowFirmwareUpdate")
          opt_allow_firmware_update = ParseIniBool(value);
        else if (key == "AllowDevFirmwareUpdate")
          opt_allow_dev_firmware_update = ParseIniBool(value);
        else if (key == "AllowUnvalidatedFirmwareUpdate")
          opt_allow_unvalidated_firmware_update = ParseIniBool(value);
        else if (key == "SwitchReplyDelayUs")
          opt_switch_reply_delay_us_arg = DuplicateOptionalIniString(value);
        else if (key == "SwitchRefreshIdleMs")
          opt_switch_refresh_idle_ms_arg = DuplicateOptionalIniString(value);
        else if (key == "OutputFrameIntervalMs")
          opt_output_frame_interval_ms_arg = DuplicateOptionalIniString(value);
        else if (key == "BallSearch")
          opt_ball_search = ParseIniBool(value);
        else if (key == "BallSearchDelayMs")
          opt_ball_search_delay_ms_arg = DuplicateOptionalIniString(value);
        else if (key == "BallSearchRoundDelayMs")
          opt_ball_search_round_delay_ms_arg = DuplicateOptionalIniString(value);
        else if (key == "CoilHoldFrames")
          opt_coil_hold_frames = static_cast<uint8_t>(atoi(value.c_str()));
        else if (key == "CloseCoinDoor")
          opt_close_coin_door = ParseIniBool(value);
        else if (key == "HardReset")
          opt_hard_reset = ParseIniBool(value);
      }
      else if (section == "Speech")
      {
        if (key == "Enabled")
          opt_speech = ParseIniBool(value);
        else if (key == "Greeting")
          opt_greeting = ParseIniBool(value);
        else if (key == "Backend")
          opt_speech_backend = DuplicateOptionalIniString(value);
        else if (key == "Voice")
          opt_speech_voice = DuplicateOptionalIniString(value);
        else if (key == "Rate")
          opt_speech_rate_arg = DuplicateOptionalIniString(value);
        else if (key == "Pitch")
          opt_speech_pitch_arg = DuplicateOptionalIniString(value);
      }
      else if (section == "OutputFilters")
      {
        if (key == "RoundedCorners") opt_rounded_corners = atoi(value.c_str());
      }
      else if (section == "BenchTest")
      {
        if (key == "SwitchTest")
          opt_switch_test = ParseIniBool(value);
        else if (key == "CoilTest")
          opt_coil_test = ParseIniBool(value);
        else if (key == "LampTest")
          opt_lamp_test = ParseIniBool(value);
        else if (key == "GITest")
          opt_gi_test = ParseIniBool(value);
        else if (key == "FlasherTest")
          opt_flasher_test = ParseIniBool(value);
        else if (key == "Interactive")
          opt_interactive = ParseIniBool(value);
        else if (key == "Number")
          opt_number = static_cast<uint8_t>(atoi(value.c_str()));
      }
      else if (section == "Translite")
      {
        if (key == "Window")
          opt_translite_window = ParseIniBool(value);
        else if (key == "Width")
          opt_translite_width = static_cast<uint16_t>(atoi(value.c_str()));
        else if (key == "Height")
          opt_translite_height = static_cast<uint16_t>(atoi(value.c_str()));
        else if (key == "Screen")
          opt_translite_screen = static_cast<int8_t>(atoi(value.c_str()));
      }
      else if (section == "VirtualDMD")
      {
        if (key == "Enabled")
          opt_virtual_dmd = ParseIniBool(value);
        else if (key == "HD")
          opt_virtual_dmd_hd = ParseIniBool(value);
        else if (key == "Window")
          opt_virtual_dmd_window = ParseIniBool(value);
        else if (key == "Width")
          opt_virtual_dmd_width = static_cast<uint16_t>(atoi(value.c_str()));
        else if (key == "Height")
          opt_virtual_dmd_height = static_cast<uint16_t>(atoi(value.c_str()));
        else if (key == "Screen")
          opt_virtual_dmd_screen = static_cast<int8_t>(atoi(value.c_str()));
        else if (key == "X")
          opt_virtual_dmd_x = atoi(value.c_str());
        else if (key == "Y")
          opt_virtual_dmd_y = atoi(value.c_str());
        else if (key == "Rotation")
          opt_virtual_dmd_rotation = atoi(value.c_str());
        else if (key == "Renderer" && !value.empty())
          opt_virtual_dmd_renderer = DuplicateOptionalIniString(value);
      }
    }

    return true;
  };

  if (!loadIniFile(opt_ini_file))
  {
    return 1;
  }

  if (HasOptionValue(opt_game_folder))
  {
    const std::filesystem::path gameFolder(opt_game_folder);
    if (!HasOptionValue(opt_music_files))
    {
      const std::string musicFiles = CollectMusicFilesCsv(gameFolder / "music");
      if (!musicFiles.empty())
      {
        opt_music_files = DuplicateIniString(musicFiles);
      }
    }
    if (!opt_b2s && !HasOptionValue(opt_translite))
    {
      const auto transliteOn = FindFirstExistingPath(
          gameFolder, {"translite-on", "translite"}, {".png", ".jpg", ".jpeg", ".bmp", ".webp"});
      if (transliteOn)
      {
        opt_translite = DuplicatePathString(*transliteOn);
      }
    }
    if (!opt_b2s && !HasOptionValue(opt_translite_attract))
    {
      const auto transliteOff = FindFirstExistingPath(
          gameFolder, {"translite-off", "translite-attract"}, {".png", ".jpg", ".jpeg", ".bmp", ".webp"});
      if (transliteOff)
      {
        opt_translite_attract = DuplicatePathString(*transliteOff);
      }
    }
  }

  cag_option_init(&cag_context, options, CAG_ARRAY_SIZE(options), argc, argv);
  while (cag_option_fetch(&cag_context))
  {
    identifier = cag_option_get_identifier(&cag_context);
    switch (identifier)
    {
      case 'c':
        config_file = cag_option_get_value(&cag_context);
        break;
      case 'z':
        opt_ini_file = cag_option_get_value(&cag_context);
        break;
      case '$':
        opt_game_folder = cag_option_get_value(&cag_context);
        break;
      case 'r':
        opt_rom = cag_option_get_value(&cag_context);
        break;
      case 'b':
        opt_backbox_address = cag_option_get_value(&cag_context);
        break;
      case 'a':
        opt_backbox_port = atoi(cag_option_get_value(&cag_context));
        break;
      case 's':
        opt_serial = cag_option_get_value(&cag_context);
        break;
      case 'n':
        opt_no_serial = true;
        break;
      case 't':
        opt_engine = cag_option_get_value(&cag_context);
        break;
      case '[':
        opt_no_display = true;
        break;
      case ']':
      {
        uint32_t parsed = 0;
        if (!ParseUint32Strict(cag_option_get_value(&cag_context), &parsed))
        {
          printf("Invalid --exit-after-ms value.\n");
          return 1;
        }
        opt_exit_after_ms = parsed;
        break;
      }
      case 'M':
        opt_no_sound = true;
        break;
      case 'W':
        opt_speech = true;
        break;
      case 'Y':
        opt_greeting = true;
        break;
      case 'o':
        opt_music_files = cag_option_get_value(&cag_context);
        break;
      case 'q':
        opt_music_gap_ms = static_cast<uint32_t>(atoi(cag_option_get_value(&cag_context)));
        break;
      case 'U':
        opt_speech_backend = cag_option_get_value(&cag_context);
        break;
      case 'v':
        opt_speech_voice = cag_option_get_value(&cag_context);
        break;
      case 'w':
        opt_speech_rate_arg = cag_option_get_value(&cag_context);
        break;
      case 'x':
        opt_speech_pitch_arg = cag_option_get_value(&cag_context);
        break;
      case 'u':
        opt_serum = true;
        break;
      case '~':
        opt_serum = true;
        break;
      case 'A':
        opt_pinmame_path = cag_option_get_value(&cag_context);
        break;
      case 'T':
        opt_serum_timeout = atoi(cag_option_get_value(&cag_context));
        break;
      case 'P':
        opt_serum_skip_frames = atoi(cag_option_get_value(&cag_context));
        break;
      case 'p':
        opt_pup = true;
        break;
      case '9':
        opt_altsound = true;
        break;
      case '?':
        opt_b2s = true;
        break;
      case '^':
        opt_plugin_dir = cag_option_get_value(&cag_context);
        break;
      case '&':
        opt_pup_folder = cag_option_get_value(&cag_context);
        break;
      case '*':
        opt_altsound_folder = cag_option_get_value(&cag_context);
        break;
      case 'y':
        opt_rules = cag_option_get_value(&cag_context);
        opt_rules_enabled = HasOptionValue(opt_rules);
        break;
      case 'i':
        opt_console_display = true;
        break;
      case 'm':
        opt_dump = true;
        break;
      case 'X':
        opt_dump = true;
        break;
      case 'd':
        opt_debug = true;
        opt_debug_errors = true;
        break;
      case 'e':
        opt_debug_errors = true;
        break;
      case '6':
        opt_skip_boards = cag_option_get_value(&cag_context);
        break;
      case '(':
        opt_firmware_path = cag_option_get_value(&cag_context);
        break;
      case '#':
        opt_allow_firmware_update = true;
        break;
      case ')':
        opt_allow_dev_firmware_update = true;
        break;
      case '|':
        opt_allow_unvalidated_firmware_update = true;
        break;
      case '7':
        opt_switch_reply_delay_us_arg = cag_option_get_value(&cag_context);
        break;
      case 'g':
        opt_switch_refresh_idle_ms_arg = cag_option_get_value(&cag_context);
        break;
      case '%':
        opt_output_frame_interval_ms_arg = cag_option_get_value(&cag_context);
        break;
      case 'B':
        opt_ball_search = true;
        break;
      case '!':
        opt_ball_search_delay_ms_arg = cag_option_get_value(&cag_context);
        break;
      case '@':
        opt_ball_search_round_delay_ms_arg = cag_option_get_value(&cag_context);
        break;
      case '8':
        opt_close_coin_door = true;
        break;
      case 'V':
        opt_hard_reset = true;
        break;
      case 'S':
        opt_debug_switches = true;
        break;
      case 'C':
        opt_debug_coils = true;
        break;
      case 'L':
        opt_debug_lamps = true;
        break;
      case 'f':
        opt_debug_effects = true;
        break;
      case '{':
        opt_debug_sound_commands = true;
        break;
      case '0':
        opt_switch_test = true;
        break;
      case '1':
        opt_coil_test = true;
        break;
      case '2':
        opt_lamp_test = true;
        break;
      case '3':
        opt_gi_test = true;
        break;
      case '4':
        opt_flasher_test = true;
        break;
      case 'Z':
        opt_interactive = true;
        break;
      case '5':
        opt_number = atoi(cag_option_get_value(&cag_context));
        break;
      case 'D':
        opt_translite = cag_option_get_value(&cag_context);
        break;
      case 'E':
        opt_translite_attract = cag_option_get_value(&cag_context);
        break;
      case 'F':
        opt_translite_window = true;
        break;
      case 'G':
        opt_translite_width = atoi(cag_option_get_value(&cag_context));
        break;
      case 'H':
        opt_translite_height = atoi(cag_option_get_value(&cag_context));
        break;
      case 'I':
        opt_translite_screen = atoi(cag_option_get_value(&cag_context));
        break;
      case 'J':
        opt_virtual_dmd = true;
        break;
      case 'K':
        opt_virtual_dmd = true;
        opt_virtual_dmd_hd = true;
        break;
      case 'N':
        opt_virtual_dmd_window = true;
        break;
      case 'O':
        opt_virtual_dmd_width = atoi(cag_option_get_value(&cag_context));
        break;
      case 'Q':
        opt_virtual_dmd_height = atoi(cag_option_get_value(&cag_context));
        break;
      case 'R':
        opt_virtual_dmd_screen = atoi(cag_option_get_value(&cag_context));
        break;
      case 'l':
        opt_virtual_dmd_renderer = cag_option_get_value(&cag_context);
        break;
      case 'j':
        opt_virtual_dmd_x = atoi(cag_option_get_value(&cag_context));
        break;
      case 'k':
        opt_virtual_dmd_y = atoi(cag_option_get_value(&cag_context));
        break;
      case 'h':
      printf("Use option --game to specify the game folder.\nAny other option is optional to override the settings from within the game folder.\n");
        printf("Usage: ppuc --game <game_folder> [OPTION]...\n");
        cag_option_print(options, CAG_ARRAY_SIZE(options), stdout);
        return 0;
      default:
        if (cag_option_get_error_index(&cag_context) >= 0)
        {
          fprintf(stderr, "Unknown command line option: ");
          cag_option_print_error(&cag_context, stderr);
          fprintf(stderr, "Usage: ppuc --game <game_folder> [OPTION]...\n");
          cag_option_print(options, CAG_ARRAY_SIZE(options), stderr);
          return 1;
        }
        break;
    }
  }

  for (int i = cag_option_get_index(&cag_context); i < argc; ++i)
  {
    const char* arg = argv[i];
    if (arg && arg[0] == '-' && arg[1] != '\0')
    {
      fprintf(stderr, "Unknown command line option: %s\n", arg);
      fprintf(stderr, "Usage: ppuc [OPTION]...\n");
      cag_option_print(options, CAG_ARRAY_SIZE(options), stderr);
      return 1;
    }
  }

  if (!config_file)
  {
    printf("No config file provided. Use option -c /path/to/config/file.\n");
    return -1;
  }

  if (!ValidateSkippedBoardsCsv(opt_skip_boards))
  {
    fprintf(stderr, "Invalid value for --skip-boards: %s\n", opt_skip_boards ? opt_skip_boards : "(null)");
    return 1;
  }

  if (opt_switch_reply_delay_us_arg && !ParseUint32Strict(opt_switch_reply_delay_us_arg, &opt_switch_reply_delay_us))
  {
    fprintf(stderr, "Invalid value for --switch-reply-delay-us: %s\n", opt_switch_reply_delay_us_arg);
    return 1;
  }
  if (opt_switch_refresh_idle_ms_arg &&
      !ParseUint32Strict(opt_switch_refresh_idle_ms_arg, &opt_switch_refresh_idle_ms))
  {
    fprintf(stderr, "Invalid value for --switch-refresh-idle-ms: %s\n", opt_switch_refresh_idle_ms_arg);
    return 1;
  }
  if (opt_switch_refresh_idle_ms == 0)
  {
    fprintf(stderr, "--switch-refresh-idle-ms must be greater than 0 because switch re-reading is always active\n");
    return 1;
  }
  if (opt_output_frame_interval_ms_arg &&
      !ParseUint32Strict(opt_output_frame_interval_ms_arg, &opt_output_frame_interval_ms))
  {
    fprintf(stderr, "Invalid value for --output-frame-interval-ms: %s\n", opt_output_frame_interval_ms_arg);
    return 1;
  }
  if (opt_output_frame_interval_ms == 0)
  {
    fprintf(stderr, "--output-frame-interval-ms must be greater than 0\n");
    return 1;
  }
  if (opt_ball_search_delay_ms_arg && !ParseUint32Strict(opt_ball_search_delay_ms_arg, &opt_ball_search_delay_ms))
  {
    fprintf(stderr, "Invalid value for --ball-search-delay-ms: %s\n", opt_ball_search_delay_ms_arg);
    return 1;
  }
  if (opt_ball_search_round_delay_ms_arg &&
      !ParseUint32Strict(opt_ball_search_round_delay_ms_arg, &opt_ball_search_round_delay_ms))
  {
    fprintf(stderr, "Invalid value for --ball-search-round-delay-ms: %s\n", opt_ball_search_round_delay_ms_arg);
    return 1;
  }

  SpeechBackend speechBackend = SpeechBackend::Auto;
  SpeechOptions speechOptions;
  std::string speechValidationError;
  if (!ParseSpeechCliOptions(opt_speech_backend, opt_speech_voice, opt_speech_rate_arg, opt_speech_pitch_arg,
                             &speechBackend, &speechOptions, &speechValidationError))
  {
    fprintf(stderr, "%s\n", speechValidationError.c_str());
    return 1;
  }

  if (opt_close_coin_door && opt_no_serial)
  {
    fprintf(stderr, "--close-coin-door requires serial communication and cannot be used with --no-serial\n");
    return 1;
  }

  if (opt_interactive && !(opt_coil_test || opt_lamp_test || opt_flasher_test))
  {
    fprintf(stderr, "--interactive is only supported with --coil-test, --lamp-test, or --flasher-test\n");
    return 1;
  }

  const bool useScriptEngine = opt_engine != nullptr && strcmp(opt_engine, "script") == 0;
  if (!useScriptEngine && (opt_engine == nullptr || strcmp(opt_engine, "pinmame") != 0))
  {
    printf("Unknown engine '%s'. Valid values are 'pinmame' and 'script'.\n", opt_engine ? opt_engine : "(null)");
    return 1;
  }

  pPpuc = new PPUC();

  // Load config file. But options set via command line are preferred.
  try
  {
    pPpuc->LoadConfiguration(config_file);
  }
  catch (const std::exception& e)
  {
    fprintf(stderr, "%s\n", e.what());
    return 1;
  }

  if (!opt_debug)
  {
    opt_debug = pPpuc->GetDebug();
  }

  // The emGame block lives in the same io-boards.yaml libppuc just parsed.
  // libppuc ignores root keys it does not name, so this needs no libppuc schema
  // change -- but every device number is cross-checked against the machine
  // libppuc actually parsed, which turns "I press start and nothing happens"
  // into a startup error that names the switch.
  //
  // Parsed here rather than at engine construction because the tilt inhibit
  // switch has to reach libppuc before Connect(): it is sent to the boards as a
  // config topic during session setup.
  GameConfig g_gameConfig;
  if (useScriptEngine)
  {
    std::string gameConfigError;
    if (!LoadGameConfigFromYaml(config_file, &g_gameConfig, &gameConfigError))
    {
      printf("%s\n", gameConfigError.c_str());
      return 1;
    }

    if (g_gameConfig.tilt.inhibitSwitch != 0)
    {
      pPpuc->SetTiltSwitch(static_cast<uint8_t>(g_gameConfig.tilt.inhibitSwitch));
    }
  }

  // Tilt warnings and ball save are loaded for BOTH engines. Giving an
  // early-electronic ROM features it never had is much of the point.
  {
    TiltAssistConfig tiltAssist;
    BallSaveConfig ballSave;
    std::string assistError;
    if (!LoadPlayfieldAssistFromYaml(config_file, &tiltAssist, &ballSave, &assistError))
    {
      printf("%s\n", assistError.c_str());
      return 1;
    }

    g_playfieldAssistEnabled =
          !tiltAssist.switches.empty() || !tiltAssist.slamSwitches.empty() || ballSave.enabled;
    if (g_playfieldAssistEnabled)
    {
      g_playfieldAssist.SetTiltConfig(tiltAssist);
      g_playfieldAssist.SetBallSaveConfig(ballSave);

      if (!tiltAssist.switches.empty())
      {
        printf("Tilt: %zu switch(es), %u warning(s), %u ms swing filter, %u ms blanking after a warning\n",
               tiltAssist.switches.size(), tiltAssist.warnings, tiltAssist.debounceMs,
               tiltAssist.warningBlankingMs);
      }
      if (ballSave.enabled)
      {
        printf("Ball save: %u ms, starting on %s\n", ballSave.durationMs,
               BallSaveStartName(g_playfieldAssist.GetBallSaveConfig().startOn));
      }
    }
  }

  if (opt_rom)
  {
    pPpuc->SetRom(opt_rom);
  }
  else
  {
    opt_rom = pPpuc->GetRom();
  }

  if (HasOptionValue(opt_game_folder))
  {
    const std::filesystem::path gameFolder(opt_game_folder);
    if (opt_rules_enabled && !HasOptionValue(opt_rules))
    {
      // Only when the folder is actually there. A ROM-less machine driven
      // entirely by its emGame block is a supported configuration, and a game
      // folder with no rules/ directory must not be a startup error. An
      // explicitly given --rules that does not exist still is one.
      std::error_code ec;
      const std::filesystem::path rulesFolder = gameFolder / "rules";
      if (std::filesystem::is_directory(rulesFolder, ec))
      {
        opt_rules = DuplicatePathString(rulesFolder);
      }
    }
    if (!HasOptionValue(opt_altsound_folder) && HasOptionValue(opt_rom))
    {
      opt_altsound_folder = DuplicatePathString(gameFolder / "pinmame" / "altsound" / opt_rom);
    }
  }

  if (!ValidateSpeechAudioUsage(opt_no_sound,
                                (opt_speech || HasOptionValue(opt_speech_voice) ||
                                 HasOptionValue(opt_speech_rate_arg) || HasOptionValue(opt_speech_pitch_arg)),
                                opt_greeting, &speechValidationError))
  {
    fprintf(stderr, "%s\n", speechValidationError.c_str());
    return 1;
  }

  if (opt_no_sound && HasOptionValue(opt_music_files))
  {
    fprintf(stderr, "--music-files requires audio output and cannot be used with --no-sound\n");
    return 1;
  }

  if (!opt_no_sound)
  {
    // Initialize the sound device
    if (!SDL_Init(SDL_INIT_AUDIO))
    {
      printf("SDL_Init failed: %s\n", SDL_GetError());
      return 1;
    }

    pAudioOutput = std::make_unique<AudioOutput>();
    if (!pAudioOutput->Initialize())
    {
      printf("Audio output init failed: %s\n", SDL_GetError());
      return 1;
    }
    pAudioOutput->SetMusicTrackGapMs(opt_music_gap_ms);

    if (HasOptionValue(opt_music_files))
    {
      std::string musicError;
      if (!pAudioOutput->LoadMusicFilesCsv(opt_music_files, &musicError))
      {
        fprintf(stderr, "Music init failed: %s\n", musicError.c_str());
        return 1;
      }
      pAudioOutput->SetMusicEnabled(false);
    }
  }

  if (opt_pup || opt_altsound || opt_b2s)
  {
    pMediaPluginHost = std::make_unique<MediaPluginHost>(pAudioOutput.get());
    MediaPluginHost::Options mediaOptions;
    mediaOptions.enablePup = opt_pup;
    mediaOptions.enableAltSound = opt_altsound;
    mediaOptions.enableB2S = opt_b2s;
    mediaOptions.debug = opt_debug;
    mediaOptions.pluginDir = opt_plugin_dir;
    mediaOptions.pupFolder = opt_pup_folder;
    mediaOptions.altSoundFolder = opt_altsound_folder;
    std::string mediaTablePath;
    if (HasOptionValue(opt_game_folder) && HasOptionValue(opt_rom))
    {
      mediaTablePath = (std::filesystem::path(opt_game_folder) / (std::string(opt_rom) + ".vpx")).string();
      mediaOptions.tablePath = mediaTablePath.c_str();
    }
    else
    {
      mediaOptions.tablePath = config_file;
    }
    mediaOptions.prefPath = getenv("HOME");
    mediaOptions.gameId = opt_rom;
    mediaOptions.backglassWidth = opt_translite_width > 0 ? opt_translite_width : 1920;
    mediaOptions.backglassHeight = opt_translite_height > 0 ? opt_translite_height : 1080;
    mediaOptions.backglassScreen = opt_translite_screen;
    mediaOptions.b2sSegmentAngleDegrees = opt_b2s_segment_angle_degrees;
    mediaOptions.b2sSegmentGlow = opt_b2s_segment_glow;
    mediaOptions.b2sSegmentSmoothing = opt_b2s_segment_smoothing;
    std::string mediaError;
    if (!pMediaPluginHost->Initialize(mediaOptions, &mediaError))
    {
      fprintf(stderr, "Media plugin init failed: %s\n", mediaError.c_str());
      pMediaPluginHost.reset();
    }
  }

  const auto initializeSpeechIfNeeded = [&]() -> bool
  {
    if (pSpeechService != nullptr || !(opt_speech || opt_greeting))
    {
      return true;
    }

    std::string speechBackendError;
    if (!CreateConfiguredSpeechService(*pAudioOutput, speechBackend, speechOptions, &pSpeechService,
                                       &speechBackendError))
    {
      fprintf(stderr, "Speech init failed: %s\n",
              speechBackendError.empty() ? "unknown backend error" : speechBackendError.c_str());
      return false;
    }
    PrintSpeechConfiguration(opt_speech_backend, speechOptions, opt_speech_rate_arg, opt_speech_pitch_arg);

    return true;
  };

  if (opt_greeting)
  {
    std::string speechBackendError;
    if (!CreateConfiguredSpeechService(*pAudioOutput, speechBackend, speechOptions, &pSpeechService,
                                       &speechBackendError))
    {
      fprintf(stderr, "Speech init failed: %s\n",
              speechBackendError.empty() ? "unknown backend error" : speechBackendError.c_str());
      return 1;
    }
    PrintSpeechConfiguration(opt_speech_backend, speechOptions, opt_speech_rate_arg, opt_speech_pitch_arg);
    pSpeechService->SpeakText("P P U C, the pinball power-up controller.");
  }

  const bool bench_test_mode = opt_switch_test || opt_coil_test || opt_lamp_test || opt_gi_test || opt_flasher_test;
  const bool diagnostic_mode = bench_test_mode || opt_debug || opt_debug_errors || opt_debug_switches ||
                               opt_debug_coils || opt_debug_lamps || opt_debug_effects;
  if (diagnostic_mode)
  {
    opt_translite = NULL;
    opt_translite_attract = NULL;
  }

#ifndef PPUC_USE_KMSDMD
  if (opt_translite || opt_virtual_dmd)
  {
    ConfigureSDLVideoDriverForHeadlessLinux();
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
      printf("SDL_Init Error: %s\n", SDL_GetError());
      return 1;
    }
  }
#endif

  if (opt_translite)
  {
#ifdef PPUC_USE_KMSDMD
    std::string transliteLoadError;
    if (!LoadRGB24Image(opt_translite, &transliteImage, &transliteLoadError))
    {
      printf("%s\n", transliteLoadError.c_str());
      return 1;
    }

    if (opt_translite_attract && !LoadRGB24Image(opt_translite_attract, &transliteAttractImage, &transliteLoadError))
    {
      printf("%s\n", transliteLoadError.c_str());
      return 1;
    }

    const LoadedRGB24Image& initialImage =
        !transliteAttractImage.pixels.empty() ? transliteAttractImage : transliteImage;
    pTransliteDisplay = std::make_unique<DMDUtil::KMSDMD>(
        "PPUC Translite", opt_translite_width, opt_translite_height, initialImage.width, initialImage.height,
        opt_translite_screen, 0, 0, DMDUtil::KMSDMD::RenderingMode::SmoothScaling, DMDUtil::KMSDMD::Rotation::Rotate0);

    if (!pTransliteDisplay || !pTransliteDisplay->IsReady())
    {
      printf("KMS translite setup failed: %s\n", pTransliteDisplay ? pTransliteDisplay->GetError() : "unknown error");
      pTransliteDisplay.reset();
      return 1;
    }

    if (!RenderTransliteImage(initialImage, pTransliteDisplay.get()))
    {
      printf("Failed to render initial translite image\n");
      return 1;
    }
#else
    if (!SDL_CreateWindowAndRenderer("PPUC Translite", opt_translite_width, opt_translite_height,
                                     opt_translite_window ? SDL_WINDOW_BORDERLESS : SDL_WINDOW_FULLSCREEN,
                                     &pTransliteWindow, &pTransliteRenderer))
    {
      printf("SDL couldn't create translite window/renderer: %s\n", SDL_GetError());
      return SDL_APP_FAILURE;
    }

    if (!PositionWindowOnScreen(pTransliteWindow, opt_translite_screen))
    {
      printf("Failed to position translite window: %s\n", SDL_GetError());
      SDL_DestroyRenderer(pTransliteRenderer);
      SDL_DestroyWindow(pTransliteWindow);
      SDL_Quit();
      return 1;
    }

    pTransliteTexture = IMG_LoadTexture(pTransliteRenderer, opt_translite);
    if (!pTransliteTexture)
    {
      printf("IMG_LoadTexture Error: %s\n", SDL_GetError());
      SDL_DestroyRenderer(pTransliteRenderer);
      SDL_DestroyWindow(pTransliteWindow);
      SDL_Quit();
      return 1;
    }

    if (opt_translite_attract)
    {
      pTransliteAttractTexture = IMG_LoadTexture(pTransliteRenderer, opt_translite_attract);
      if (!pTransliteAttractTexture)
      {
        printf("IMG_LoadTexture Error: %s\n", SDL_GetError());
        SDL_DestroyTexture(pTransliteTexture);
        SDL_DestroyRenderer(pTransliteRenderer);
        SDL_DestroyWindow(pTransliteWindow);
        SDL_Quit();
        return 1;
      }
      SDL_RenderTexture(pTransliteRenderer, pTransliteAttractTexture, nullptr, nullptr);
    }
    else
    {
      SDL_RenderTexture(pTransliteRenderer, pTransliteTexture, nullptr, nullptr);
    }

    SDL_RenderPresent(pTransliteRenderer);
    SDL_FlushRenderer(pTransliteRenderer);
#endif
  }

  if (opt_virtual_dmd)
  {
    // SDLDMD now owns the SDL window/renderer lifecycle.
  }

  pPpuc->SetDebug(opt_debug);
  pPpuc->SetDebugErrors(opt_debug_errors);
  pPpuc->SetForceHardReset(opt_hard_reset);
  pPpuc->SetSkippedBoardsCsv(opt_skip_boards);
  pPpuc->SetCoilHoldFrames(opt_coil_hold_frames);
  pPpuc->SetSwitchReplyDelayUs(opt_switch_reply_delay_us);
  pPpuc->SetSwitchRefreshIdleMs(opt_switch_refresh_idle_ms);
  pPpuc->SetOutputFrameIntervalMs(opt_output_frame_interval_ms);
  pPpuc->SetDisableFastFlipForTests(opt_switch_test || opt_coil_test || opt_lamp_test || opt_gi_test ||
                                   opt_flasher_test);

  if (opt_debug || opt_debug_errors)
  {
    char logLine[256];
    snprintf(logLine, sizeof(logLine), "%" PRIu64 " INFO: Logging started", CurrentUnixMs());
    PrintFlushedLogLine("", logLine);
  }

  if (opt_serial)
  {
    pPpuc->SetSerial(opt_serial);
  }
  else
  {
    // opt_serial will be ignored by ZeDMD later.
    opt_serial = pPpuc->GetSerial();
  }

  if (opt_switch_test || opt_coil_test || opt_lamp_test || opt_gi_test || opt_flasher_test)
  {
    if (!pPpuc->Connect())
    {
      printf("Unable to open serial communication to PPUC boards on %s.\n", opt_serial ? opt_serial : "(null)");
      return 1;
    }

    if (!initializeSpeechIfNeeded())
    {
      return 1;
    }

    pPpuc->StartUpdates();
    const bool testRequiresHighPower = opt_coil_test || opt_flasher_test;
    const bool closeVirtualCoinDoorForTest =
        testRequiresHighPower && opt_close_coin_door && pPpuc->IsSwitchVirtualized(pPpuc->GetCoinDoorClosedSwitch());

    if (closeVirtualCoinDoorForTest)
    {
      pPpuc->SetSwitchState(pPpuc->GetCoinDoorClosedSwitch(), 1);
    }
    pPpuc->SetSolenoidState(pPpuc->GetGameOnSolenoid(), testRequiresHighPower ? 1 : 0);

    BenchTestMode testMode = BenchTestMode::SWITCHES;
    if (opt_lamp_test)
    {
      testMode = BenchTestMode::LAMPS;
    }
    else if (opt_gi_test)
    {
      testMode = BenchTestMode::GI;
    }
    else if (opt_flasher_test)
    {
      testMode = BenchTestMode::FLASHERS;
    }
    else if (opt_coil_test)
    {
      testMode = BenchTestMode::COILS;
    }

    BenchTestRunner testRunner = CreateBenchTestRunner(pPpuc, testMode, opt_number);
    if (testRunner.mode != BenchTestMode::SWITCHES && testRunner.steps.empty())
    {
      printf("No matching test items configured.\n");
    }
    else
    {
      running = true;
      ScopedRawTerminalMode rawTerminal(testRunner.interactive);
      if (testRunner.mode == BenchTestMode::SWITCHES)
      {
        const uint32_t cleanChainBaseline = pPpuc->GetCleanSwitchReplyChainCount();
        WaitForCleanSwitchReplyCycle(pPpuc, cleanChainBaseline);
      }
      PrimeBenchSwitchStates(pPpuc, testRunner);
      while (running && ServiceBenchTestRunner(pPpuc, testRunner))
      {
        std::this_thread::sleep_for(std::chrono::microseconds(MAIN_LOOP_SLEEP_US));
      }
      CleanupBenchTestRunner(pPpuc, testRunner);
    }

    pPpuc->SetSolenoidState(pPpuc->GetGameOnSolenoid(), 0);
    if (closeVirtualCoinDoorForTest)
    {
      pPpuc->SetSwitchState(pPpuc->GetCoinDoorClosedSwitch(), 0);
    }
    pPpuc->StopUpdates();
    pPpuc->Disconnect();

    return 0;
  }

  // Initialize displays.
  // ZeDMD messes with USB ports. when searching for the DMD.
  // So it is important to start that search before the RS485 BUS gets
  // initialized.
  DMDUtil::Config* dmdConfig = DMDUtil::Config::GetInstance();
  dmdConfig->SetLogCallback(DMDUtilLogCallback);
  if (HasOptionValue(opt_ini_file))
  {
    dmdConfig->parseConfigFile(opt_ini_file);
  }
  dmdConfig->SetRoundedCorners(opt_rounded_corners);

  if (opt_rules_enabled && HasOptionValue(opt_rules))
  {
    pLuaRulesEngine = std::make_unique<LuaRulesEngine>();
    pLuaRulesEngine->SetDebug(opt_debug || opt_debug_effects);
    pLuaRulesEngine->SetSwitchGroups(pPpuc->GetSwitchGroups());
    pLuaRulesEngine->SetActionCallback(
        [](const RulesAction& action)
        {
          if (pPpuc != nullptr)
          {
            g_interceptorOutputs.HandleAction(pPpuc, action);
          }
        });
    pLuaRulesEngine->SetSpeechCallback(
        [](const std::string& text)
        {
          if (pSpeechService != nullptr)
          {
            pSpeechService->SpeakText(text);
          }
        });
    pLuaRulesEngine->SetTriggerCallback(
        [](const char source, const uint16_t id, const uint8_t value)
        {
          if (source == kBoardEffectTriggerSource)
          {
            if (opt_debug || opt_debug_effects)
            {
              printf("Effect trigger emitted: id=%u value=%u\n", id, value);
            }
            if (pPpuc != nullptr)
            {
              pPpuc->TriggerEvent(EVENT_SOURCE_EFFECT, id, value);
            }
            return;
          }

          if (pMediaPluginHost)
          {
            pMediaPluginHost->QueueEvent(source, id, value);
          }
          else if (pDmd)
          {
            pDmd->SetPUPTrigger(source, id, value);
          }
        });

    std::vector<std::string> ruleScripts;
    std::string error;
    if (!CollectRulesScripts(opt_rules, ruleScripts, error))
    {
      printf("%s: %s\n", error.c_str(), opt_rules);
      return 1;
    }

    // Deliberately not loaded yet. `ppuc.game` and `ppuc.dmd` are registered
    // when the Lua state is built, so the scripts cannot run until the engine
    // exists -- a rules file that reads ppuc.game.switch("start") at load time
    // would otherwise see nil.
    g_ruleScripts = ruleScripts;
  }

  // The engine owns the PinMAME configuration now; the host only needs the
  // resolved PinMAME directory to locate the Serum altcolor folder.
  const std::string vpmPath = ResolveVpmPath(opt_pinmame_path ? opt_pinmame_path : "");

  if (opt_backbox_address)
  {
    dmdConfig->SetDMDServerAddr(opt_backbox_address);
    dmdConfig->SetDMDServerPort(opt_backbox_port);
    dmdConfig->SetDMDServer(true);
  }

  if (opt_serum)
  {
    char altcolorPath[PINMAME_MAX_PATH + 10];
#if defined(_WIN32) || defined(_WIN64)
    snprintf(altcolorPath, PINMAME_MAX_PATH + 8, "%saltcolor", vpmPath.c_str());
#else
    snprintf(altcolorPath, PINMAME_MAX_PATH + 8, "%saltcolor", vpmPath.c_str());
#endif

    dmdConfig->SetLogCallback(DMDUtilLogCallback);
    dmdConfig->SetLogLevel(DMDUtil_LogLevel_INFO);
    dmdConfig->SetAltColorPath(altcolorPath);
    dmdConfig->SetAltColor(true);

    if (opt_serum_timeout)
    {
      dmdConfig->SetIgnoreUnknownFramesTimeout(opt_serum_timeout);
    }
    if (opt_serum_skip_frames)
    {
      dmdConfig->SetMaximumUnknownFramesToSkip(opt_serum_skip_frames);
    }
  }
  else
  {
    dmdConfig->SetAltColor(false);
  }

  if (opt_pup)
  {
    const std::string dmdPupVideosPath =
        ResolvePupVideosPathForDmd(opt_pup_folder, opt_rom);
    dmdConfig->SetPUPVideosPath(dmdPupVideosPath.c_str());
    dmdConfig->SetPUPCapture(true);
    dmdConfig->SetPUPTriggerCallback(OnDmdPupTrigger, pMediaPluginHost.get());
  }

  if (opt_debug)
  {
    printf("Finding displays...\n");
    dmdConfig->SetZeDMDDebug(opt_debug);
    dmdConfig->SetLogLevel(DMDUtil_LogLevel_DEBUG);
  }

  pDmd = new DMDUtil::DMD();
  pDmd->SetRomName(opt_rom);
  if (!opt_no_display)
  {
    // Probes USB for ZeDMD and friends, and IsFinding() below blocks until it
    // finishes. Skipping it is what makes a headless run possible.
    pDmd->FindDisplays();
  }

  if (opt_console_display)
  {
    pDmd->CreateConsoleDMD(!opt_debug);
  }

  if (opt_dump)
  {
    pDmd->DumpDMDTxt();
  }

  if (opt_virtual_dmd)
  {
#ifdef PPUC_USE_KMSDMD
    DMDUtil::KMSDMD::RenderingMode virtualDmdRenderingMode = DMDUtil::KMSDMD::RenderingMode::Dots;
    if (!DMDUtil::ParseKMSDMDRenderingMode(opt_virtual_dmd_renderer, &virtualDmdRenderingMode))
    {
      printf(
          "Unsupported virtual DMD renderer '%s'. Use one of: dots, squares, scale2x, scale4x, "
          "scale2x-dots, scale4x-dots, scale2x-squares, scale4x-squares, smooth, xbrz\n",
          opt_virtual_dmd_renderer);
      return 1;
    }

    DMDUtil::KMSDMD::Rotation virtualDmdRotation = DMDUtil::KMSDMD::Rotation::Rotate0;
    if (!DMDUtil::ParseKMSDMDRotation(std::to_string(opt_virtual_dmd_rotation).c_str(), &virtualDmdRotation))
    {
      printf("Unsupported virtual DMD rotation '%d'. Use one of: 0, 90, 180, 270\n", opt_virtual_dmd_rotation);
      return 1;
    }

    const int kmsVirtualDmdX = opt_virtual_dmd_x == SDL_WINDOWPOS_UNDEFINED ? 0 : opt_virtual_dmd_x;
    const int kmsVirtualDmdY = opt_virtual_dmd_y == SDL_WINDOWPOS_UNDEFINED ? 0 : opt_virtual_dmd_y;
    pVirtualDMD =
        DMDUtil::CreateKMSDMD(*pDmd, "PPUC DMD", opt_virtual_dmd_width, opt_virtual_dmd_height,
                              opt_virtual_dmd_hd ? 256 : 128, opt_virtual_dmd_hd ? 64 : 32, opt_virtual_dmd_screen,
                              kmsVirtualDmdX, kmsVirtualDmdY, virtualDmdRenderingMode, virtualDmdRotation);

    if (!pVirtualDMD)
    {
      printf("KMS couldn't create virtual DMD output.\n");
      return 1;
    }
#else
    DMDUtil::SDLDMD::RenderingMode virtualDmdRenderingMode = DMDUtil::SDLDMD::RenderingMode::Dots;
    if (!DMDUtil::ParseSDLDMDRenderingMode(opt_virtual_dmd_renderer, &virtualDmdRenderingMode))
    {
      printf(
          "Unsupported virtual DMD renderer '%s'. Use one of: dots, squares, scale2x, scale4x, "
          "scale2x-dots, scale4x-dots, scale2x-squares, scale4x-squares, smooth, xbrz\n",
          opt_virtual_dmd_renderer);
      return SDL_APP_FAILURE;
    }

    DMDUtil::SDLDMD::Rotation virtualDmdRotation = DMDUtil::SDLDMD::Rotation::Rotate0;
    if (!DMDUtil::ParseSDLDMDRotation(std::to_string(opt_virtual_dmd_rotation).c_str(), &virtualDmdRotation))
    {
      printf("Unsupported virtual DMD rotation '%d'. Use one of: 0, 90, 180, 270\n", opt_virtual_dmd_rotation);
      return SDL_APP_FAILURE;
    }

    pVirtualDMD =
        DMDUtil::CreateSDLDMD(*pDmd, "PPUC DMD", opt_virtual_dmd_width, opt_virtual_dmd_height,
                              opt_virtual_dmd_window ? SDL_WINDOW_BORDERLESS : SDL_WINDOW_FULLSCREEN,
                              opt_virtual_dmd_hd ? 256 : 128, opt_virtual_dmd_hd ? 64 : 32, opt_virtual_dmd_screen,
                              opt_virtual_dmd_x, opt_virtual_dmd_y, virtualDmdRenderingMode, virtualDmdRotation);

    if (!pVirtualDMD)
    {
      printf("SDL couldn't create virtual DMD window/renderer: %s\n", SDL_GetError());
      return SDL_APP_FAILURE;
    }
#endif
  }

  while (!opt_no_display && pDmd->IsFinding()) std::this_thread::sleep_for(std::chrono::milliseconds(100));

  if (!opt_no_serial && !pPpuc->Connect())
  {
    printf("Unable to open serial communication to PPUC boards on %s.\n", opt_serial ? opt_serial : "(null)");
    return 1;
  }
  if (!opt_no_serial)
  {
    ReportBoardFirmware(pPpuc, opt_firmware_path, opt_allow_firmware_update, opt_allow_dev_firmware_update,
                        opt_allow_unvalidated_firmware_update);
  }

  BallSearchRunner ballSearchRunner =
      opt_no_serial || !opt_ball_search ? BallSearchRunner{} : CreateBallSearchRunner(pPpuc, opt_ball_search_delay_ms);
  if ((opt_debug || opt_debug_coils) && !opt_no_serial && !ballSearchRunner.steps.empty())
  {
    printf("Ball search configured: coils=%zu delayMs=%u roundDelayMs=%u\n", ballSearchRunner.steps.size(),
           opt_ball_search_delay_ms, opt_ball_search_round_delay_ms);
  }

  if (!initializeSpeechIfNeeded())
  {
    return 1;
  }

  PpucEngineHost engineHost;
  std::unique_ptr<GameEngine> pEngine;

  if (useScriptEngine)
  {
    ScriptEngine::Options scriptOptions;
    scriptOptions.gameId = opt_rom ? opt_rom : "";
    scriptOptions.dmdWidth = opt_virtual_dmd_hd ? 256 : 128;
    scriptOptions.dmdHeight = opt_virtual_dmd_hd ? 64 : 32;
    scriptOptions.debug = opt_debug;

    ScriptEngine::MachineIo machineIo;
    // Routed through the interceptor rather than straight to libppuc, so a Lua
    // rule can override a GameCore-driven output the same way it can override a
    // PinMAME-driven one.
    machineIo.pulseCoil = [](int number, uint32_t durationMs)
    { g_interceptorOutputs.PulseCoil(pPpuc, number, durationMs); };
    machineIo.setCoil = [](int number, uint8_t state)
    { g_interceptorOutputs.ApplyEngineCoil(pPpuc, number, state); };
    machineIo.setLamp = [](int number, uint8_t state)
    { g_interceptorOutputs.ApplyEngineLamp(pPpuc, number, state); };
    machineIo.setGi = [](int giString, uint8_t level) { pPpuc->SetGIState(giString, level); };
    machineIo.setTiltInhibit = [](int number, uint8_t state)
    {
      // Only succeeds when the switch belongs to a board the host owns. On a
      // machine without one, tilt still suppresses scoring; it just cannot drop
      // a flipper the player is holding.
      pPpuc->SetSwitchState(number, state);
    };

    auto scriptEngine = std::make_unique<ScriptEngine>(std::move(scriptOptions), g_gameConfig, std::move(machineIo));
    // Lets the tilt decision reach GameCore. GameCore does not watch the bob
    // itself; PlayfieldAssist counted the warnings and decided.
    g_pGameCore = &scriptEngine->Game();

    // Must precede LoadScripts: this is what makes ppuc.game and ppuc.dmd exist.
    if (pLuaRulesEngine)
    {
      pLuaRulesEngine->SetGameCore(&scriptEngine->Game());
      pLuaRulesEngine->SetDmdCanvas(&scriptEngine->Canvas());
    }

    // The game core asks the script for permission on a start or an add-player.
    // An absent handler allows: credit policy is a rules decision, but having no
    // rules must never make the machine unplayable.
    scriptEngine->Game().SetAllowCallback(
        [](const char* what, int arg) -> bool
        {
          if (pLuaRulesEngine == nullptr)
          {
            return true;
          }
          return pLuaRulesEngine->CallQueryHandler(what, {arg}, true);
        });

    scriptEngine->SetEventCallback([](const GameEvent& event) { DispatchGameEventToRules(event); });

    // Rules draw on top of the built-in screen, on ticks that will actually
    // flush, so ppuc.onDmdFrame runs at most ~30 times a second no matter what
    // the author writes.
    scriptEngine->SetDmdDrawCallback(
        []()
        {
          if (pLuaRulesEngine != nullptr)
          {
            pLuaRulesEngine->CallGameHandler("onDmdFrame");
          }
        });

    pEngine = std::move(scriptEngine);
  }
  else
  {
    PinmameEngineOptions engineOptions;
    engineOptions.rom = opt_rom ? opt_rom : "";
    engineOptions.pinmamePath = opt_pinmame_path ? opt_pinmame_path : "";
    engineOptions.platform = pPpuc->GetPlatform();
    engineOptions.gameOnSolenoid = pPpuc->GetGameOnSolenoid();
    engineOptions.altsound = opt_altsound;
    engineOptions.noSound = opt_no_sound;
    engineOptions.debug = opt_debug;
    engineOptions.debugCoils = opt_debug_coils;
    engineOptions.debugSoundCommands = opt_debug_sound_commands;
    engineOptions.debugErrors = opt_debug_errors;
    pEngine = std::make_unique<PinmameEngine>(std::move(engineOptions));
  }

  pEngine->SetHost(&engineHost);

  if (pLuaRulesEngine && !g_ruleScripts.empty())
  {
    std::string rulesError;
    if (!pLuaRulesEngine->LoadScripts(g_ruleScripts, rulesError))
    {
      printf("%s: %s\n", rulesError.c_str(), opt_rules ? opt_rules : "(rules)");
      return 1;
    }
    for (const std::string& ruleScript : g_ruleScripts)
    {
      printf("Loaded Lua rules from %s\n", ruleScript.c_str());
    }
  }

  // Rules-injected switches go to whichever engine is running.
  g_interceptorOutputs.sendSwitch = [&pEngine](int number, uint8_t state) { pEngine->SendSwitch(number, state); };

  std::string engineError;
  if (pEngine->Start(engineError))
  {
    // Setup signal handlers to allow graceful termination
    signal(SIGINT, signal_handler_graceful);
    signal(SIGHUP, signal_handler_graceful);
    signal(SIGTERM, signal_handler_graceful);
    signal(SIGQUIT, signal_handler_graceful);
    signal(SIGABRT, signal_handler_graceful);

    std::vector<GameEngineOutputChange> lampChanges;
    std::vector<GameEngineOutputChange> giChanges;
    GameEngine::Identity identity;
    bool loggedIdentity = false;
    const bool pollGis = pEngine->HasCapability(GameEngine::Capability::ChangedGis);

    ball_search_game_running.store(false, std::memory_order_release);
    pPpuc->StartUpdates();
    if (opt_close_coin_door)
    {
      pPpuc->SetSwitchState(pPpuc->GetCoinDoorClosedSwitch(), 1);
    }

    const auto loopStartedAt = std::chrono::steady_clock::now();

    while (running)
    {
      std::this_thread::sleep_for(std::chrono::microseconds(MAIN_LOOP_SLEEP_US));

      if (opt_exit_after_ms != 0 &&
          std::chrono::steady_clock::now() - loopStartedAt >= std::chrono::milliseconds(opt_exit_after_ms))
      {
        printf("Exiting after %u ms as requested.\n", opt_exit_after_ms);
        running = false;
        break;
      }

      if (!loggedIdentity && pEngine->TryGetIdentity(&identity))
      {
        printf("Game engine started: game=%s hardware=%s\n", identity.name.c_str(), identity.description.c_str());
        if (pMediaPluginHost != nullptr)
        {
          pMediaPluginHost->SetGameInfo(identity.name.c_str(), identity.hardwareGen);
          pMediaPluginHost->OnGameStart();
        }
        loggedIdentity = true;
      }

      // Runs in both loop phases, before the readiness gate: the engine has its
      // own startup work (identity, tracking-map load) that must proceed while
      // the host is still waiting for it to come up.
      pEngine->Update();

      if (!pEngine->IsReady())
      {
        if (pLuaRulesEngine)
        {
          pLuaRulesEngine->Update();
          if (pLuaRulesEngine->HasFatalError())
          {
            printf("Lua rules error: %s\n", pLuaRulesEngine->GetFatalError().c_str());
            running = false;
          }
        }
        g_interceptorOutputs.Service(pPpuc);
        if (pMediaPluginHost != nullptr)
        {
          pMediaPluginHost->Process();
        }
        continue;
      }

      PPUCSwitchState* switchState;
      while ((switchState = pPpuc->GetNextSwitchState()) != nullptr)
      {
        const uint8_t newSwitchState = switchState->state == 0 ? 0 : 1;
        NoteBallSearchSwitchUpdate(pPpuc, ballSearchRunner, switchState->number, newSwitchState,
                                   opt_ball_search_delay_ms);

        // Tilt warnings and ball save decide first: a warning hit and a saved
        // drain must never reach the engine at all, under either engine.
        PlayfieldAssist::SwitchDecision assistDecision;
        if (g_playfieldAssistEnabled)
        {
          assistDecision = g_playfieldAssist.ProcessSwitch(switchState->number, newSwitchState);
        }

        LuaRulesEngine::SwitchProcessResult switchProcess;
        if (pLuaRulesEngine)
        {
          switchProcess = pLuaRulesEngine->ProcessSwitchState(switchState->number, newSwitchState);
          if (pLuaRulesEngine->HasFatalError())
          {
            printf("Lua rules error: %s\n", pLuaRulesEngine->GetFatalError().c_str());
            running = false;
          }
        }

        // Switches between 200 and 240 are custom switches within the io-boards which should not be sent to
        // pinmame. Switches above 240 will become negative values, for example 243 => -3.
        if (assistDecision.forwardToEngine && switchProcess.forwardToCpu &&
            (switchState->number < 200 || switchState->number > 241))
        {
          pEngine->SendSwitch(switchState->number, newSwitchState);
        }

        if (opt_debug || opt_debug_switches)
        {
          printf("Switch updated: #%d, %d\n", switchState->number, newSwitchState);
        }

        if (pMediaPluginHost != nullptr)
        {
          pMediaPluginHost->QueueEvent('W', switchState->number, newSwitchState);
        }

        delete switchState;
      }

      ServiceBallSearchRunner(pPpuc, ballSearchRunner,
                              ball_search_game_running.load(std::memory_order_acquire),
                              opt_ball_search_delay_ms, opt_ball_search_round_delay_ms);

      pEngine->PollChangedLamps(lampChanges);
      for (const GameEngineOutputChange& change : lampChanges)
      {
        const uint16_t lampNo = change.number;
        const uint8_t lampState = change.value == 0 ? 0 : 1;

        if (opt_debug || opt_debug_lamps)
        {
          printf("Lamp updated: #%d, %d\n", lampNo, lampState);
        }

        if (pMediaPluginHost != nullptr)
        {
          pMediaPluginHost->QueueEvent('L', lampNo, lampState);
        }

        g_interceptorOutputs.ApplyEngineLamp(pPpuc, static_cast<int>(lampNo), lampState);

        if (pLuaRulesEngine)
        {
          pLuaRulesEngine->OnLampState(static_cast<int>(lampNo), lampState);
          if (pLuaRulesEngine->HasFatalError())
          {
            printf("Lua rules error: %s\n", pLuaRulesEngine->GetFatalError().c_str());
            running = false;
          }
        }
      }

      if (pollGis)
      {
        pEngine->PollChangedGis(giChanges);
        for (const GameEngineOutputChange& change : giChanges)
        {
          const uint8_t giNo = static_cast<uint8_t>(change.number);
          const uint8_t giState = static_cast<uint8_t>(change.value);

          if (opt_debug || opt_debug_lamps)
          {
            printf("GI updated: #%d, %d\n", giNo, giState);
          }

          if (pMediaPluginHost != nullptr)
          {
            pMediaPluginHost->QueueEvent('G', giNo, giState);
          }

          pPpuc->SetGIState(giNo, giState);
        }
      }

      if (pLuaRulesEngine)
      {
        pLuaRulesEngine->Update();
        if (pLuaRulesEngine->HasFatalError())
        {
          printf("Lua rules error: %s\n", pLuaRulesEngine->GetFatalError().c_str());
          running = false;
        }
      }
      ServicePlayfieldAssist(pEngine.get());
      g_interceptorOutputs.Service(pPpuc);

      if (pMediaPluginHost != nullptr)
      {
        pMediaPluginHost->Process();
      }

      {  // Needs to be a separate scope for the lock_guard
        // Process any pending render requests
        std::lock_guard<std::mutex> lock(renderMutex);
        while (!renderQueue.empty())
        {
          const RenderRequest& request = renderQueue.front();

          switch (request.command)
          {
            case RenderCommand::RENDER_GAME:
              printf("Rendering translite\n");
#ifdef PPUC_USE_KMSDMD
              if (!RenderTransliteImage(transliteImage, pTransliteDisplay.get()))
              {
                printf("Failed to render translite\n");
              }
#else
              if (!SDL_SetRenderDrawColor(pTransliteRenderer, 0, 0, 0, 255) || !SDL_RenderClear(pTransliteRenderer) ||
                  !SDL_RenderTexture(pTransliteRenderer, pTransliteTexture, nullptr, nullptr) ||
                  !SDL_RenderPresent(pTransliteRenderer) || !SDL_FlushRenderer(pTransliteRenderer))
              {
                printf("Failed to render translite: %s\n", SDL_GetError());
              }
#endif
              break;

            case RenderCommand::RENDER_ATTRACT:
              printf("Rendering attract translite\n");
#ifdef PPUC_USE_KMSDMD
              if (!RenderTransliteImage(transliteAttractImage.pixels.empty() ? transliteImage : transliteAttractImage,
                                        pTransliteDisplay.get()))
              {
                printf("Failed to render attract translite\n");
              }
#else
              if (!SDL_SetRenderDrawColor(pTransliteRenderer, 0, 0, 0, 255) || !SDL_RenderClear(pTransliteRenderer) ||
                  !SDL_RenderTexture(pTransliteRenderer, pTransliteAttractTexture, nullptr, nullptr) ||
                  !SDL_RenderPresent(pTransliteRenderer) || !SDL_FlushRenderer(pTransliteRenderer))
              {
                printf("Failed to render attract translite: %s\n", SDL_GetError());
              }
#endif
              break;
          }
          renderQueue.pop();
        }
      }

#ifndef PPUC_USE_KMSDMD
      SDL_Event event;
      if (SDL_PollEvent(&event))
      {
        switch (event.type)
        {
          case SDL_EVENT_QUIT:
            running = false;
            break;
          case SDL_EVENT_KEY_DOWN:
            if (opt_debug) printf("Key pressed: %d\n", event.key.key);
            switch (event.key.key)
            {
              case SDLK_ESCAPE:
                running = false;
                break;
              case 53:  // 5
                // Coin Right on Williams Flash
                pEngine->SendSwitch(4, 1);
                break;
              case 13:  // Enter
                // Game Start on Williams Flash
                pEngine->SendSwitch(3, 1);
                break;
            }
            break;
        }
      }
#endif
    }

    if (shutdown_requested && opt_serum)
    {
      // Emergency shutdown path:
      // Serum/libdmdutil teardown can race across worker threads when interrupted by signal.
      // Exit the process before Pinmame/DMD teardown runs to avoid use-after-free in external code.
      CancelActiveBallSearch(pPpuc, ballSearchRunner);
      pPpuc->StopUpdates();
      if (!opt_no_serial)
      {
        pPpuc->Disconnect();
      }
      fflush(stdout);
      _Exit(0);
    }

    // Stop the engine first: Stop() joins the engine's thread, which is what
    // guarantees no output sink fires into a stopped updater or a disconnected
    // bus during teardown.
    pEngine->Stop();
    CancelActiveBallSearch(pPpuc, ballSearchRunner);
    pPpuc->StopUpdates();
  }
  else
  {
    printf("%s\n", engineError.c_str());
  }

  if (!opt_no_serial)
  {
    // Report how often the bus needed its recovery mechanisms before tearing
    // it down. Several host-side timeouts and retries were tuned by trial
    // against a bus that misbehaved for reasons since addressed, and there was
    // no way to tell which of them still earn their keep. A session that ends
    // with zeros here is evidence one can be tightened or removed; anything
    // non-zero says it is still load-bearing, and where to look next.
    const PPUCBusHealth health = pPpuc->GetBusHealth();
    printf("PPUC: bus health: %u switch reply chains, %u clean, %u missed, "
           "%u session resync(s), %u config ack retries, %u config ack timeouts, "
           "%u serial write failures, %u CRC errors\n",
           health.switchReplyChains, health.switchReplyChainsClean,
           health.switchReplyMisses, health.sessionResyncs,
           health.configAckRetries, health.configAckTimeouts,
           health.serialWriteFailures, health.frameCrcErrors);

    // Recorded in RAM rather than logged: the target has a read-only root and
    // no console. If this run was started by hand over ssh, this is where the
    // detail of anything that went wrong comes out.
    const std::vector<std::string> anomalies = pPpuc->GetRecentAnomalies();
    if (!anomalies.empty())
    {
      printf("PPUC: last %zu unexpected condition(s):\n", anomalies.size());
      for (const std::string& line : anomalies)
      {
        printf("PPUC:   %s\n", line.c_str());
      }
    }

    // Close the serial device
    pPpuc->Disconnect();
  }

  bool quitSDL = false;

  if (pMediaPluginHost)
  {
    pMediaPluginHost->Shutdown();
    pMediaPluginHost.reset();
    quitSDL = true;
  }

  if (pAudioOutput)
  {
    pSpeechService.reset();
    pAudioOutput->Shutdown();
    pAudioOutput.reset();
    quitSDL = true;
  }

  if (pTransliteTexture)
  {
    SDL_DestroyTexture(pTransliteTexture);
    if (pTransliteAttractTexture)
    {
      SDL_DestroyTexture(pTransliteAttractTexture);
    }
    SDL_DestroyRenderer(pTransliteRenderer);
    SDL_DestroyWindow(pTransliteWindow);
    quitSDL = true;
  }

#ifdef PPUC_USE_KMSDMD
  if (pTransliteDisplay)
  {
    pTransliteDisplay.reset();
  }
#endif

  if (pVirtualDMD)
  {
#ifdef PPUC_USE_KMSDMD
    if (pDmd) DMDUtil::DestroyKMSDMD(*pDmd, pVirtualDMD);
#else
    if (pDmd) DMDUtil::DestroySDLDMD(*pDmd, pVirtualDMD);
#endif
    pVirtualDMD = nullptr;
#ifndef PPUC_USE_KMSDMD
    quitSDL = true;
#endif
  }

  if (pDmd)
  {
    // Serum teardown can race in worker threads on signal-driven shutdown.
    // Avoid deleting the DMD instance in that path and let process exit reclaim memory.
    if (!(shutdown_requested && opt_serum))
    {
      delete pDmd;
      pDmd = nullptr;
    }
    else
    {
      printf("Skipping DMD teardown after signal while Serum is active.\n");
    }
  }

  if (quitSDL)
  {
    SDL_QuitSubSystem(SDL_INIT_AUDIO | SDL_INIT_VIDEO);
  }

  return 0;
}
