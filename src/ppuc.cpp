#if defined(_WIN32) || defined(_WIN64)
#define SIGHUP 1
#define SIGKILL 9
#define SIGQUIT 3
#define SIGINT 2
#endif

#include "PPUC.h"
#include "ppuc_version.h" // <--- HINZUGEFÜGT

#include <ctype.h>
#include <climits>
#include <inttypes.h>
#include <stdlib.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <queue>
#include <fstream>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>

#include "DMDUtil/Config.h"
#include "DMDUtil/ConsoleDMD.h"
#include "DMDUtil/DMDUtil.h"
#ifdef PPUC_USE_KMSDMD
#include "KMSDMD/KMSDMD.h"
#else
#include "SDLDMD/SDLDMD.h"
#endif
#include "AudioOutput.h"
#include "PUPTriggerEngine.h"
#include "SDL3/SDL.h"
#include "SDL3_image/SDL_image.h"
#include "SpeechCliSupport.h"
#include "SpeechService.h"
#include "SpeechTriggerMap.h"
#include "cargs.h"
#include "io-boards/Event.h"
#include "io-boards/PPUCPlatforms.h"
#include "libpinmame.h"

#if !defined(_WIN32) && !defined(_WIN64)
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#endif

#define MAIN_LOOP_SLEEP_US 20  // Main loop sleep time in microseconds

DMDUtil::DMD* pDmd;
PPUC* ppuc;
std::unique_ptr<PUPTriggerEngine> pPUPTriggerEngine;
std::unique_ptr<SpeechTriggerMap> pSpeechTriggerMap;
std::unique_ptr<AudioOutput> pAudioOutput;
std::unique_ptr<SpeechService> pSpeechService;

constexpr char kSpeechTriggerSource = 'O';
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
    const uint8_t* pSourceRow = static_cast<const uint8_t*>(pConverted->pixels) + static_cast<size_t>(y) * pConverted->pitch;
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

bool opt_debug = false;
bool opt_debug_errors = false;
bool opt_debug_switches = false;
bool opt_debug_coils = false;
bool opt_debug_lamps = false;
bool opt_no_serial = false;
bool opt_no_sound = false;
bool opt_speech = false;
bool opt_greeting = false;
const char* opt_speech_file = NULL;
const char* opt_speech_backend = "auto";
const char* opt_speech_voice = NULL;
const char* opt_speech_rate_arg = NULL;
const char* opt_speech_pitch_arg = NULL;
bool opt_interactive = false;
bool opt_serum = false;
bool opt_pup = false;
bool opt_console_display = false;
bool opt_hard_reset = false;
const char* opt_virtual_dmd_renderer = "dots";
const char* opt_pinmame_path = NULL;
const char* opt_rom = NULL;
std::atomic<int> game_state{0};
bool running = true;
volatile std::sig_atomic_t shutdown_requested = 0;

template <typename T>
struct LogCallbackTraits;

template <typename R, typename A1, typename A2, typename A3, typename A4>
struct LogCallbackTraits<R (*)(A1, A2, A3, A4)>
{
  using Arg3 = A3;
};

#if defined(_WIN32) && !defined(_WIN64)
template <typename R, typename A1, typename A2, typename A3, typename A4>
struct LogCallbackTraits<R(__stdcall *)(A1, A2, A3, A4)>
{
  using Arg3 = A3;
};
#endif

using PinmameLogMessageArg = LogCallbackTraits<PinmameOnLogMessageCallback>::Arg3;

#define PINMAME_CALLBACK_CAST(type, fn) reinterpret_cast<type>(fn)

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

static bool HasOptionValue(const char* value)
{
  return value != nullptr && value[0] != '\0';
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

static bool IsVirtualizedBenchSwitch(PPUC* ppuc, const PPUCSwitch& vswitch)
{
  return ppuc->IsBoardVirtualized(vswitch.board) ||
         ppuc->IsSwitchVirtualized(vswitch.number);
}

static bool IsVirtualizedBenchCoil(PPUC* ppuc, const PPUCCoil& coil)
{
  return ppuc->IsBoardVirtualized(coil.board);
}

static bool IsVirtualizedBenchLamp(PPUC* ppuc, const PPUCLamp& lamp)
{
  return ppuc->IsBoardVirtualized(lamp.board);
}

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

static bool ComputeSwitchFeedbackGiOn(const BenchTestRunner& runner,
                                      std::chrono::steady_clock::time_point now)
{
  return now >= runner.switchFeedbackOffUntil;
}

static void UpdateSwitchFeedbackGi(PPUC* ppuc, BenchTestRunner& runner)
{
  if (runner.mode != BenchTestMode::SWITCHES || ppuc->GetPlatform() == PLATFORM_WPC)
  {
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  const bool giOn = ComputeSwitchFeedbackGiOn(runner, now);
  if (giOn == runner.switchFeedbackGiState)
  {
    return;
  }

  ppuc->SetGIState(/* string */ 1, giOn ? 8 : 0);
  runner.switchFeedbackGiState = giOn;
}

static void PrintBenchStepDetails(const BenchOutputStep& step)
{
  printf("\nBoard: %d\nPort: %d\nNumber: %d\nDescription: %s\n",
         step.board, step.port, step.number, step.description.c_str());
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

  printf("Enter %s number, or q/ESC to quit: %s", label,
         runner.interactiveInput.c_str());
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

static void ApplyBenchOutput(PPUC* ppuc, const BenchOutputStep& step, bool on)
{
  switch (step.kind)
  {
    case BenchOutputKind::SOLENOID:
      ppuc->SetSolenoidState(step.number, on ? 1 : 0);
      break;
    case BenchOutputKind::LAMP:
      if (step.type == LED_TYPE_LAMP)
      {
        ppuc->SetLampState(step.number, on ? 1 : 0);
      }
      else
      {
        ppuc->SetSolenoidState(step.number, on ? 1 : 0);
      }
      break;
    case BenchOutputKind::GI:
      ppuc->SetGIState(step.number, on ? step.value : 0);
      break;
  }
}

static std::vector<BenchOutputStep> BuildCoilTestSteps(PPUC* ppuc, uint8_t number)
{
  std::vector<BenchOutputStep> steps;
  for (const auto& coil : ppuc->GetCoils())
  {
    if (coil.type != PWM_TYPE_SOLENOID && coil.type != PWM_TYPE_FLASHER)
    {
      continue;
    }
    if (number != 0 && coil.number != number)
    {
      continue;
    }
    if (IsVirtualizedBenchCoil(ppuc, coil))
    {
      continue;
    }
    steps.push_back({BenchOutputKind::SOLENOID, coil.board, coil.port, coil.number, 1, coil.type,
                     coil.description, 0, 200, 1000});
  }
  return steps;
}

static std::vector<BenchOutputStep> BuildLampTestSteps(PPUC* ppuc, uint8_t number)
{
  std::vector<BenchOutputStep> steps;
  for (const auto& lamp : ppuc->GetLamps())
  {
    if (lamp.type != LED_TYPE_LAMP)
    {
      continue;
    }
    if (number != 0 && lamp.number != number)
    {
      continue;
    }
    if (IsVirtualizedBenchLamp(ppuc, lamp))
    {
      continue;
    }
    steps.push_back({BenchOutputKind::LAMP, lamp.board, lamp.port, lamp.number, 1, lamp.type,
                     lamp.description, lamp.color, number != 0 ? 10000 : 2000, number != 0 ? 0 : 1000});
  }

  for (const auto& coil : ppuc->GetCoils())
  {
    if (coil.type != PWM_TYPE_LAMP)
    {
      continue;
    }
    if (number != 0 && coil.number != number)
    {
      continue;
    }
    if (IsVirtualizedBenchCoil(ppuc, coil))
    {
      continue;
    }
    steps.push_back({BenchOutputKind::LAMP, coil.board, coil.port, coil.number, 1, coil.type,
                     coil.description, 0, number != 0 ? 10000 : 2000, number != 0 ? 0 : 1000});
  }

  return steps;
}

static std::vector<BenchOutputStep> BuildFlasherTestSteps(PPUC* ppuc, uint8_t number)
{
  std::vector<BenchOutputStep> steps;
  for (const auto& lamp : ppuc->GetLamps())
  {
    if (lamp.type != LED_TYPE_FLASHER)
    {
      continue;
    }
    if (number != 0 && lamp.number != number)
    {
      continue;
    }
    if (IsVirtualizedBenchLamp(ppuc, lamp))
    {
      continue;
    }
    for (uint8_t i = 0; i < 3; ++i)
    {
      steps.push_back({BenchOutputKind::SOLENOID, lamp.board, lamp.port, lamp.number, 1, lamp.type,
                       lamp.description, lamp.color, 200, 1000});
    }
  }

  for (const auto& coil : ppuc->GetCoils())
  {
    if (coil.type != PWM_TYPE_FLASHER)
    {
      continue;
    }
    if (number != 0 && coil.number != number)
    {
      continue;
    }
    if (IsVirtualizedBenchCoil(ppuc, coil))
    {
      continue;
    }
    for (uint8_t i = 0; i < 3; ++i)
    {
      steps.push_back({BenchOutputKind::SOLENOID, coil.board, coil.port, coil.number, 1, coil.type,
                       coil.description, 0, 200, 1000});
    }
  }

  return steps;
}

static std::vector<BenchOutputStep> BuildGiTestSteps(PPUC* ppuc, uint8_t number)
{
  std::vector<BenchOutputStep> steps;
  const uint8_t maxStrings = ppuc->GetPlatform() == PLATFORM_WPC ? 8 : 1;
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

static void PrintSwitchTestHeader(PPUC* ppuc)
{
  printf("Switch Test\n");
  printf("=========\n");

  const auto switches = ppuc->GetSwitches();
  if (!switches.empty())
  {
    printf("Configured switches:\n");
    for (const auto& vswitch : switches)
    {
      PrintMaybeStruckLine(IsVirtualizedBenchSwitch(ppuc, vswitch),
                           "  #%d  board=%d port=%d  %s", vswitch.number,
                           vswitch.board, vswitch.port,
                           vswitch.description.c_str());
    }
    printf("\nWaiting for switch activity...\n");
    fflush(stdout);
  }
}

static void PrintCoilTestHeader(PPUC* ppuc, uint8_t number)
{
  printf("Coil Test\n");
  printf("=========\n");
  printf("Configured coils/flashers:\n");
  for (const auto& coil : ppuc->GetCoils())
  {
    if (coil.type != PWM_TYPE_SOLENOID && coil.type != PWM_TYPE_FLASHER)
    {
      continue;
    }
    if (number != 0 && coil.number != number)
    {
      continue;
    }
    PrintMaybeStruckLine(IsVirtualizedBenchCoil(ppuc, coil),
                         "  #%d  board=%d port=%d  %s", coil.number,
                         coil.board, coil.port, coil.description.c_str());
  }
  printf("\n");
}

static void PrintLampTestHeader(PPUC* ppuc, uint8_t number)
{
  printf("Lamp Test\n");
  printf("=========\n");
  printf("Configured lamps:\n");
  for (const auto& lamp : ppuc->GetLamps())
  {
    if (lamp.type != LED_TYPE_LAMP)
    {
      continue;
    }
    if (number != 0 && lamp.number != number)
    {
      continue;
    }
    PrintMaybeStruckLine(IsVirtualizedBenchLamp(ppuc, lamp),
                         "  #%d  board=%d port=%d  %s", lamp.number,
                         lamp.board, lamp.port, lamp.description.c_str());
  }
  for (const auto& coil : ppuc->GetCoils())
  {
    if (coil.type != PWM_TYPE_LAMP)
    {
      continue;
    }
    if (number != 0 && coil.number != number)
    {
      continue;
    }
    PrintMaybeStruckLine(IsVirtualizedBenchCoil(ppuc, coil),
                         "  #%d  board=%d port=%d  %s", coil.number,
                         coil.board, coil.port, coil.description.c_str());
  }
  printf("\n");
}

static void PrintFlasherTestHeader(PPUC* ppuc, uint8_t number)
{
  printf("\nFlasher Test\n");
  printf("=========\n");
  printf("Configured flashers:\n");
  for (const auto& lamp : ppuc->GetLamps())
  {
    if (lamp.type != LED_TYPE_FLASHER)
    {
      continue;
    }
    if (number != 0 && lamp.number != number)
    {
      continue;
    }
    PrintMaybeStruckLine(IsVirtualizedBenchLamp(ppuc, lamp),
                         "  #%d  board=%d port=%d  %s", lamp.number,
                         lamp.board, lamp.port, lamp.description.c_str());
  }
  for (const auto& coil : ppuc->GetCoils())
  {
    if (coil.type != PWM_TYPE_FLASHER)
    {
      continue;
    }
    if (number != 0 && coil.number != number)
    {
      continue;
    }
    PrintMaybeStruckLine(IsVirtualizedBenchCoil(ppuc, coil),
                         "  #%d  board=%d port=%d  %s", coil.number,
                         coil.board, coil.port, coil.description.c_str());
  }
  printf("\n");
}

static BenchTestRunner CreateBenchTestRunner(PPUC* ppuc, BenchTestMode mode,
                                             uint8_t number)
{
  BenchTestRunner runner;
  runner.mode = mode;
  runner.interactive =
      opt_interactive &&
      (mode == BenchTestMode::COILS || mode == BenchTestMode::LAMPS ||
       mode == BenchTestMode::FLASHERS);

  switch (mode)
  {
    case BenchTestMode::SWITCHES:
      PrintSwitchTestHeader(ppuc);
      runner.printedSwitchHeader = true;
      break;
    case BenchTestMode::COILS:
      PrintCoilTestHeader(ppuc, number);
      runner.steps = BuildCoilTestSteps(ppuc, number);
      break;
    case BenchTestMode::LAMPS:
      PrintLampTestHeader(ppuc, number);
      ppuc->SetGIState(/* string */ 1, 0);
      runner.restoreGiOnExit = ppuc->GetPlatform() != PLATFORM_WPC;
      runner.steps = BuildLampTestSteps(ppuc, number);
      break;
    case BenchTestMode::GI:
      printf("\nGI Test\n");
      printf("=========\n");
      runner.steps = BuildGiTestSteps(ppuc, number);
      break;
    case BenchTestMode::FLASHERS:
      PrintFlasherTestHeader(ppuc, number);
      runner.steps = BuildFlasherTestSteps(ppuc, number);
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
    orderedNumbers.erase(std::unique(orderedNumbers.begin(), orderedNumbers.end()),
                         orderedNumbers.end());

    for (const int itemNumber : orderedNumbers)
    {
      const auto it = runner.stepsByNumber.find(itemNumber);
      if (it == runner.stepsByNumber.end() || it->second.empty())
      {
        continue;
      }

      const BenchOutputStep& step = it->second.front();
      char buffer[512];
      snprintf(buffer, sizeof(buffer), "  #%d  board=%d port=%d  %s",
               step.number, step.board, step.port, step.description.c_str());
      runner.interactiveMenuLines.emplace_back(buffer);
    }

    PrintInteractiveBenchPrompt(runner);
  }

  return runner;
}

static void PrimeBenchSwitchStates(PPUC* ppuc, BenchTestRunner& runner)
{
  if (runner.mode != BenchTestMode::SWITCHES)
  {
    return;
  }

  const auto switches = ppuc->GetSwitches();
  PPUCSwitchState* switchState = nullptr;
  while ((switchState = ppuc->GetNextSwitchState()) != nullptr)
  {
    auto it = std::find_if(switches.begin(), switches.end(),
                           [switchState](const PPUCSwitch& vswitch) {
                             return vswitch.number == switchState->number;
                           });
    if (it != switches.end() && IsVirtualizedBenchSwitch(ppuc, *it))
    {
      continue;
    }

    const uint8_t normalizedState = switchState->state == 0 ? 0 : 1;
    runner.initialSwitchStates[switchState->number] = normalizedState;
    runner.currentSwitchStates[switchState->number] = normalizedState;
  }

  runner.initialSwitchStatesPrimed = true;
}

static void WaitForCleanSwitchReplyCycle(PPUC* ppuc, uint32_t baselineCount)
{
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline)
  {
    while (ppuc->GetNextSwitchState() != nullptr)
    {
    }

    if (ppuc->GetCleanSwitchReplyChainCount() > baselineCount)
    {
      return;
    }

    std::this_thread::sleep_for(
        std::chrono::microseconds(MAIN_LOOP_SLEEP_US));
  }
}

static void DrainSwitchUpdatesForTest(PPUC* ppuc, BenchTestRunner& runner)
{
  if (runner.mode != BenchTestMode::SWITCHES)
  {
    while (ppuc->GetNextSwitchState() != nullptr)
    {
    }
    return;
  }

  const auto switches = ppuc->GetSwitches();
  PPUCSwitchState* switchState = nullptr;
  while ((switchState = ppuc->GetNextSwitchState()) != nullptr)
  {
    auto it = std::find_if(switches.begin(), switches.end(),
                           [switchState](const PPUCSwitch& vswitch) {
                             return vswitch.number == switchState->number;
                           });
    if (it != switches.end() && IsVirtualizedBenchSwitch(ppuc, *it))
    {
      continue;
    }

    const uint8_t normalizedState = switchState->state == 0 ? 0 : 1;
    if (!runner.initialSwitchStatesPrimed)
    {
      continue;
    }

    const auto currentIt = runner.currentSwitchStates.find(switchState->number);
    const uint8_t previousState =
        currentIt == runner.currentSwitchStates.end()
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
      const auto offUntil =
          std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
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
      printf("Switch updated: #%d, %d (%s)\nBoard: %d\nPort: %d\nDescription: %s\n\n",
             switchState->number, switchState->state, stateName, it->board,
             it->port, it->description.c_str());
    }
    else
    {
      printf("Switch updated: #%d, %d (%s)\n\n", switchState->number,
             switchState->state, stateName);
    }
    fflush(stdout);
  }

  UpdateSwitchFeedbackGi(ppuc, runner);
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

  if (select(STDIN_FILENO + 1, &readSet, nullptr, nullptr, &timeout) <= 0 ||
      !FD_ISSET(STDIN_FILENO, &readSet))
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

static bool ServiceBenchTestRunner(PPUC* ppuc, BenchTestRunner& runner)
{
  DrainSwitchUpdatesForTest(ppuc, runner);

  if (runner.mode == BenchTestMode::SWITCHES)
  {
    UpdateSwitchFeedbackGi(ppuc, runner);
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
      ApplyBenchOutput(ppuc, step, true);
      runner.outputActive = true;
      runner.phaseDeadline = now + std::chrono::milliseconds(step.onDurationMs);
      return true;
    }

    if (now < runner.phaseDeadline)
    {
      return true;
    }

    const BenchOutputStep step = runner.pendingInteractiveSteps.front();
    ApplyBenchOutput(ppuc, step, false);
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
    ApplyBenchOutput(ppuc, step, true);
    runner.outputActive = true;
    runner.phaseDeadline = now + std::chrono::milliseconds(step.onDurationMs);
    return true;
  }

  if (now < runner.phaseDeadline)
  {
    return true;
  }

  ApplyBenchOutput(ppuc, step, false);
  runner.outputActive = false;
  ++runner.index;
  if (runner.index < runner.steps.size() && step.offDurationMs > 0)
  {
    runner.waitingBetweenSteps = true;
    runner.nextStepReadyAt = now + std::chrono::milliseconds(step.offDurationMs);
  }
  return runner.index < runner.steps.size();
}

static void CleanupBenchTestRunner(PPUC* ppuc, const BenchTestRunner& runner)
{
  for (const auto& step : runner.steps)
  {
    ApplyBenchOutput(ppuc, step, false);
  }

  if (runner.restoreGiOnExit)
  {
    printf("\nRestoring GI String 1 to brightness %d\n", 8);
    ppuc->SetGIState(/* string */ 1, /* full brightness */ 8);
  }
  else if (runner.mode == BenchTestMode::SWITCHES &&
           ppuc->GetPlatform() != PLATFORM_WPC)
  {
    ppuc->SetGIState(/* string */ 1, /* full brightness */ 8);
  }
}

static struct cag_option options[] = {
    {.identifier = 'c',
     .access_letters = "c",
     .access_name = "config",
     .value_name = "VALUE",
     .description = "Path to config file (required)"},
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
    {.identifier = 'W', .access_name = "speech", .value_name = NULL, .description = "Enable speech callouts (optional)"},
    {.identifier = 'Y', .access_name = "greeting", .value_name = NULL, .description = "Speak a startup greeting for speech debugging (optional)"},
    {.identifier = '9',
     .access_name = "speech-file",
     .value_name = "VALUE",
     .description = "Path to speech trigger text file (optional)"},
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
     .description = "Enable Serum colorization (optional)"},
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
     .value_name = "VALUE",
     .description = "Enable PUP videos (optional)"},
    {.identifier = 'y',
     .access_name = "pup-triggers",
     .value_name = "VALUE",
     .description = "Path to PUP trigger rules file (optional)"},
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
    {.identifier = '7',
     .access_name = "switch-reply-delay-us",
     .value_name = "VALUE",
     .description = "Experimental per-board switch reply delay in microseconds"},
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
    {.identifier = '0', .access_name = "switch-test", .value_name = NULL, .description = "Run switch test"},
    {.identifier = '1', .access_name = "coil-test", .value_name = NULL, .description = "Run coil test"},
    {.identifier = '2', .access_name = "lamp-test", .value_name = NULL, .description = "Run lamp test"},
    {.identifier = '3', .access_name = "gi-test", .value_name = NULL, .description = "Run lamp test"},
    {.identifier = '4', .access_name = "flasher-test", .value_name = NULL, .description = "Run flasher test"},
    {.identifier = 'Z', .access_name = "interactive", .value_name = NULL, .description = "Interactive coil/lamp/flasher test selection"},
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
     .description = "Virtual DMD renderer: dots, squares, scale2x, scale4x, scale2x-dots, scale4x-dots, scale2x-squares, scale4x-squares, smooth, xbrz"},
    {.identifier = 'j',
     .access_name = "virtual-dmd-x",
     .value_name = "VALUE",
     .description = "Virtual DMD x position relative to the selected screen"},
    {.identifier = 'k',
     .access_name = "virtual-dmd-y",
     .value_name = "VALUE",
     .description = "Virtual DMD y position relative to the selected screen"},
    {.identifier = 'h', .access_letters = "h", .access_name = "help", .description = "Show help"}};

void PINMAMECALLBACK Game(PinmameGame* game)
{
  printf(
      "Game(): name=%s, description=%s, manufacturer=%s, year=%s, "
      "flags=%lu, found=%d\n",
      game->name, game->description, game->manufacturer, game->year, (unsigned long)game->flags, game->found);
}

void PINMAMECALLBACK OnStateUpdated(int state, const void* p_userData)
{
  if (opt_debug)
  {
    printf("OnStateUpdated(): state=%d\n", state);
  }

  if (!state)
  {
    running = false;
    shutdown_requested = 1;
    return;
  }
  else
  {
    /*
    PinmameMechConfig mechConfig;
    memset(&mechConfig, 0, sizeof(mechConfig));

    mechConfig.sol1 = 11;
    mechConfig.length = 240;
    mechConfig.steps = 240;
    mechConfig.type = PINMAME_MECH_FLAGS_NONLINEAR | PINMAME_MECH_FLAGS_REVERSE | PINMAME_MECH_FLAGS_ONESOL;
    mechConfig.sw[0].swNo = 32;
    mechConfig.sw[0].startPos = 0;
    mechConfig.sw[0].endPos = 5;

    PinmameSetMech(0, &mechConfig);
    */

    game_state.store(state, std::memory_order_release);
  }
}

template <typename T>
static void PrintPinmameLogMessage(PINMAME_LOG_LEVEL logLevel, const char* format, T arg)
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
    PrintFlushedLogLine("INFO: ", logMessage);
  }
  else if (logLevel == PINMAME_LOG_LEVEL_ERROR)
  {
    PrintFlushedLogLine("ERROR: ", logMessage);
  }
}

void PINMAMECALLBACK OnLogMessage(PINMAME_LOG_LEVEL logLevel, const char* format, PinmameLogMessageArg arg,
                                  const void* p_userData)
{
  PrintPinmameLogMessage(logLevel, format, arg);
}

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

void PINMAMECALLBACK OnDisplayAvailable(int index, int displayCount, PinmameDisplayLayout* p_displayLayout,
                                        const void* p_userData)
{
  if (opt_debug)
  {
    printf(
        "OnDisplayAvailable(): index=%d, displayCount=%d, type=%d, top=%d, "
        "left=%d, width=%d, height=%d, "
        "depth=%d, length=%d\n",
        index, displayCount, p_displayLayout->type, p_displayLayout->top, p_displayLayout->left, p_displayLayout->width,
        p_displayLayout->height, p_displayLayout->depth, p_displayLayout->length);
  }
}

void PINMAMECALLBACK OnDisplayUpdated(int index, void* p_displayData, PinmameDisplayLayout* p_displayLayout,
                                      const void* p_userData)
{
  if (p_displayData == nullptr)
  {
    return;
  }

  if (opt_debug)
  {
    printf(
        "OnDisplayUpdated(): index=%d, type=%d, top=%d, left=%d, width=%d, "
        "height=%d, depth=%d, length=%d\n",
        index, p_displayLayout->type, p_displayLayout->top, p_displayLayout->left, p_displayLayout->width,
        p_displayLayout->height, p_displayLayout->depth, p_displayLayout->length);
  }

  // For DMD games, the ype is PINMAME_DISPLAY_TYPE_DMD.
  // For alphanumeric games that should be shown on a DMD,
  // the type is PINMAME_DISPLAY_TYPE_DMD | PINMAME_DISPLAY_TYPE_DMDSEG.
  // For some games like WPT, there's a second display on the playfield of type
  // PINMAME_DISPLAY_TYPE_DMD | PINMAME_DISPLAY_TYPE_DMDNOAA | PINMAME_DISPLAY_TYPE_NODISP
  if ((p_displayLayout->type & PINMAME_DISPLAY_TYPE_DMD) == PINMAME_DISPLAY_TYPE_DMD &&
      (p_displayLayout->type & PINMAME_DISPLAY_TYPE_NODISP) == 0)
  {
    pDmd->UpdateData((uint8_t*)p_displayData, p_displayLayout->depth, p_displayLayout->width, p_displayLayout->height,
                     255, 255, 255);
  }
  else
  {
    switch (p_displayLayout->type)
    {
      case PINMAME_DISPLAY_TYPE_SEG16:    // 16 segments
      case PINMAME_DISPLAY_TYPE_SEG16R:   // 16 segments with comma and period
                                          // reversed
      case PINMAME_DISPLAY_TYPE_SEG10:    // 9 segments and comma
      case PINMAME_DISPLAY_TYPE_SEG9:     // 9 segments
      case PINMAME_DISPLAY_TYPE_SEG8:     // 7 segments and comma
      case PINMAME_DISPLAY_TYPE_SEG8D:    // 7 segments and period
      case PINMAME_DISPLAY_TYPE_SEG7:     // 7 segments
      case PINMAME_DISPLAY_TYPE_SEG87:    // 7 segments, comma every three
      case PINMAME_DISPLAY_TYPE_SEG87F:   // 7 segments, forced comma every three
      case PINMAME_DISPLAY_TYPE_SEG98:    // 9 segments, comma every three
      case PINMAME_DISPLAY_TYPE_SEG98F:   // 9 segments, forced comma every three
      case PINMAME_DISPLAY_TYPE_SEG7S:    // 7 segments, small
      case PINMAME_DISPLAY_TYPE_SEG7SC:   // 7 segments, small, with comma
      case PINMAME_DISPLAY_TYPE_SEG16S:   // 16 segments with split top and
                                          // bottom line
      case PINMAME_DISPLAY_TYPE_SEG16N:   // 16 segments without commas
      case PINMAME_DISPLAY_TYPE_SEG16D:   // 16 segments with periods only
      case PINMAME_DISPLAY_TYPE_SEGALL:   // maximum segment definition number
      case PINMAME_DISPLAY_TYPE_IMPORT:   // Link to another display layout
      case PINMAME_DISPLAY_TYPE_SEGMASK:  // Note that CORE_IMPORT must be part of the segmask as well!
      case PINMAME_DISPLAY_TYPE_SEG8H:
      case PINMAME_DISPLAY_TYPE_SEG7H:
      case PINMAME_DISPLAY_TYPE_SEG87H:
      case PINMAME_DISPLAY_TYPE_SEG87FH:
      case PINMAME_DISPLAY_TYPE_SEG7SH:
      case PINMAME_DISPLAY_TYPE_SEG7SCH:
#ifdef PINMAME_DISPLAY_TYPE_VIDEO_ROT90
      case PINMAME_DISPLAY_TYPE_VIDEO_ROT90:
#endif
        break;

      case PINMAME_DISPLAY_TYPE_VIDEO:  // VIDEO Display
        // @todo
        break;

      case PINMAME_DISPLAY_TYPE_DMD:     // DMD Display
        // handled above, just surpress a warning of missing cases here.
        break;

      case PINMAME_DISPLAY_TYPE_SEGHIBIT:
      case PINMAME_DISPLAY_TYPE_SEGREV:
      case PINMAME_DISPLAY_TYPE_DMDNOAA:
      case PINMAME_DISPLAY_TYPE_NODISP:
        break;

      default:
        break;
    }
  }
}

int PINMAMECALLBACK OnAudioAvailable(PinmameAudioInfo* p_audioInfo, const void* p_userData)
{
  if (opt_debug)
  {
    printf(
        "OnAudioAvailable(): format=%d, channels=%d, sampleRate=%.2f, "
        "framesPerSecond=%.2f, samplesPerFrame=%d, "
        "bufferSize=%d\n",
        p_audioInfo->format, p_audioInfo->channels, p_audioInfo->sampleRate, p_audioInfo->framesPerSecond,
        p_audioInfo->samplesPerFrame, p_audioInfo->bufferSize);
  }

  if (!opt_no_sound)
  {
    if (pAudioOutput != nullptr)
    {
      pAudioOutput->ConfigureGameFormat(static_cast<int>(p_audioInfo->sampleRate),
                                        p_audioInfo->channels);
    }
    else
    {
      printf("Audio output not initialized.\n");
    }
  }
  return p_audioInfo->samplesPerFrame;
}

int PINMAMECALLBACK OnAudioUpdated(void* p_buffer, int samples, const void* p_userData)
{
  if (pAudioOutput != nullptr)
  {
    pAudioOutput->QueueGameFrames(reinterpret_cast<const int16_t*>(p_buffer),
                                  static_cast<size_t>(samples));
  }
  return samples;
}

void PINMAMECALLBACK OnSolenoidUpdated(PinmameSolenoidState* p_solenoidState, const void* p_userData)
{
  const uint8_t coilState = p_solenoidState->state == 0 ? 0 : 1;
  const bool isGameOnCoil = p_solenoidState->solNo == ppuc->GetGameOnSolenoid();

  if (opt_debug || opt_debug_coils)
  {
    printf("OnSolenoidUpdated: solenoid=%d, state=%d\n", p_solenoidState->solNo, coilState);
  }

  if (pPUPTriggerEngine)
  {
    if (isGameOnCoil)
    {
      pPUPTriggerEngine->SetAttractMode(coilState == 0);
    }
    pPUPTriggerEngine->OnCoilState(p_solenoidState->solNo, coilState);
  }

  ppuc->SetSolenoidState(p_solenoidState->solNo, coilState);

  if (isGameOnCoil)
  {
    RenderRequest request;
    if (coilState)
    {
      if (opt_debug || opt_debug_coils)
      {
        printf("Game started: solenoid=%d, state=%d\n", p_solenoidState->solNo, coilState);
      }
      request.command = RenderCommand::RENDER_GAME;
    }
    else
    {
      if (opt_debug || opt_debug_coils)
      {
        printf("Game stopped: solenoid=%d, state=%d\n", p_solenoidState->solNo, coilState);
      }
      if (pTransliteAttractTexture)
      {
        request.command = RenderCommand::RENDER_ATTRACT;
      }
    }

    std::lock_guard<std::mutex> lock(renderMutex);
    renderQueue.push(request);
  }
}

void PINMAMECALLBACK OnMechAvailable(int mechNo, PinmameMechInfo* p_mechInfo, const void* p_userData)
{
  if (opt_debug)
  {
    printf(
        "OnMechAvailable: mechNo=%d, type=%d, length=%d, steps=%d, pos=%d, "
        "speed=%d\n",
        mechNo, p_mechInfo->type, p_mechInfo->length, p_mechInfo->steps, p_mechInfo->pos, p_mechInfo->speed);
  }
}

void PINMAMECALLBACK OnMechUpdated(int mechNo, PinmameMechInfo* p_mechInfo, const void* p_userData)
{
  if (opt_debug)
  {
    printf(
        "OnMechUpdated: mechNo=%d, type=%d, length=%d, steps=%d, pos=%d, "
        "speed=%d\n",
        mechNo, p_mechInfo->type, p_mechInfo->length, p_mechInfo->steps, p_mechInfo->pos, p_mechInfo->speed);
  }
}

void PINMAMECALLBACK OnConsoleDataUpdated(void* p_data, int size, const void* p_userData)
{
  if (opt_debug)
  {
    printf("OnConsoleDataUpdated: size=%d\n", size);
  }
}

int PINMAMECALLBACK IsKeyPressed(PINMAME_KEYCODE keycode, const void* p_userData) { return 0; }

void signal_handler_graceful(int sig)
{
  running = false;
  shutdown_requested = 1;
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
  const char* opt_pup_triggers = NULL;
  const char* opt_backbox_address = NULL;
  uint16_t opt_backbox_port = 6789;
  const char* opt_serial = NULL;
  const char* opt_skip_boards = NULL;
  const char* opt_switch_reply_delay_us_arg = NULL;
  uint32_t opt_switch_reply_delay_us = 0;
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
    else if (strncmp(arg, "--ini-file=", 11) == 0)
    {
      opt_ini_file = arg + 11;
    }
  }

  const auto loadIniFile = [&](const char* iniFile) -> bool {
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

      if (section == "Paths")
      {
        if (key == "ConfigFile")
          config_file = DuplicateIniString(value);
        else if (key == "Rom")
          opt_rom = DuplicateOptionalIniString(value);
        else if (key == "Serial")
          opt_serial = DuplicateOptionalIniString(value);
        else if (key == "PinmamePath")
          opt_pinmame_path = DuplicateOptionalIniString(value);
        else if (key == "PUPTriggers")
          opt_pup_triggers = DuplicateOptionalIniString(value);
        else if (key == "SpeechFile")
          opt_speech_file = DuplicateOptionalIniString(value);
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
        else if (key == "Serum")
          opt_serum = ParseIniBool(value);
        else if (key == "SerumTimeout")
          opt_serum_timeout = static_cast<uint8_t>(atoi(value.c_str()));
        else if (key == "SerumSkipFrames")
          opt_serum_skip_frames = static_cast<uint8_t>(atoi(value.c_str()));
        else if (key == "PUP")
          opt_pup = ParseIniBool(value);
        else if (key == "ConsoleDisplay")
          opt_console_display = ParseIniBool(value);
        else if (key == "DumpDisplay" || key == "DumpDmdTxt")
          opt_dump = ParseIniBool(value);
        else if (key == "SkipBoards")
          opt_skip_boards = DuplicateOptionalIniString(value);
        else if (key == "SwitchReplyDelayUs")
          opt_switch_reply_delay_us_arg = DuplicateOptionalIniString(value);
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
        if (key == "RoundedCorners")
          opt_rounded_corners = atoi(value.c_str());
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
      case 'M':
        opt_no_sound = true;
        break;
      case 'W':
        opt_speech = true;
        break;
      case 'Y':
        opt_greeting = true;
        break;
      case '9':
        opt_speech_file = cag_option_get_value(&cag_context);
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
      case 'A':
        opt_pinmame_path = cag_option_get_value(&cag_context);
        break;
      case 't':
        opt_serum_timeout = atoi(cag_option_get_value(&cag_context));
        break;
      case 'P':
        opt_serum_skip_frames = atoi(cag_option_get_value(&cag_context));
        break;
      case 'p':
        opt_pup = true;
        break;
      case 'y':
        opt_pup_triggers = cag_option_get_value(&cag_context);
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
      case '7':
        opt_switch_reply_delay_us_arg = cag_option_get_value(&cag_context);
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
        printf("Usage: ppuc [OPTION]...\n");
        cag_option_print(options, CAG_ARRAY_SIZE(options), stdout);
        return 0;
      default:
        if (cag_option_get_error_index(&cag_context) >= 0)
        {
          fprintf(stderr, "Unknown command line option: ");
          cag_option_print_error(&cag_context, stderr);
          fprintf(stderr, "Usage: ppuc [OPTION]...\n");
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
    fprintf(stderr, "Invalid value for --skip-boards: %s\n",
            opt_skip_boards ? opt_skip_boards : "(null)");
    return 1;
  }

  if (opt_switch_reply_delay_us_arg &&
      !ParseUint32Strict(opt_switch_reply_delay_us_arg,
                         &opt_switch_reply_delay_us))
  {
    fprintf(stderr, "Invalid value for --switch-reply-delay-us: %s\n",
            opt_switch_reply_delay_us_arg);
    return 1;
  }

  SpeechBackend speechBackend = SpeechBackend::Auto;
  SpeechOptions speechOptions;
  std::string speechValidationError;
  if (!ParseSpeechCliOptions(opt_speech_backend, opt_speech_voice,
                             opt_speech_rate_arg, opt_speech_pitch_arg,
                             &speechBackend, &speechOptions,
                             &speechValidationError))
  {
    fprintf(stderr, "%s\n", speechValidationError.c_str());
    return 1;
  }

  if (opt_close_coin_door && opt_no_serial)
  {
    fprintf(stderr,
            "--close-coin-door requires serial communication and cannot be used with --no-serial\n");
    return 1;
  }

  if (opt_interactive &&
      !(opt_coil_test || opt_lamp_test || opt_flasher_test))
  {
    fprintf(stderr,
            "--interactive is only supported with --coil-test, --lamp-test, or --flasher-test\n");
    return 1;
  }

  if (!ValidateSpeechAudioUsage(
          opt_no_sound,
          (opt_speech || HasOptionValue(opt_speech_voice) ||
           HasOptionValue(opt_speech_rate_arg) ||
           HasOptionValue(opt_speech_pitch_arg)),
          opt_greeting, opt_speech_file, &speechValidationError))
  {
    fprintf(stderr, "%s\n", speechValidationError.c_str());
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
  }

  const auto initializeSpeechIfNeeded = [&]() -> bool {
    if (pSpeechService != nullptr ||
        !(opt_speech || opt_greeting || opt_speech_file))
    {
      return true;
    }

    std::string speechBackendError;
    if (!CreateConfiguredSpeechService(*pAudioOutput, speechBackend,
                                       speechOptions, &pSpeechService,
                                       &speechBackendError))
    {
      fprintf(stderr, "Speech init failed: %s\n",
              speechBackendError.empty() ? "unknown backend error"
                                         : speechBackendError.c_str());
      return false;
    }
    PrintSpeechConfiguration(opt_speech_backend, speechOptions,
                             opt_speech_rate_arg, opt_speech_pitch_arg);

    return true;
  };

  if (opt_greeting)
  {
    std::string speechBackendError;
    if (!CreateConfiguredSpeechService(*pAudioOutput, speechBackend,
                                       speechOptions, &pSpeechService,
                                       &speechBackendError))
    {
      fprintf(stderr, "Speech init failed: %s\n",
              speechBackendError.empty() ? "unknown backend error"
                                         : speechBackendError.c_str());
      return 1;
    }
    PrintSpeechConfiguration(opt_speech_backend, speechOptions,
                             opt_speech_rate_arg, opt_speech_pitch_arg);
    pSpeechService->SpeakText("P P U C, the pinball power-up controller.");
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

    if (opt_translite_attract &&
        !LoadRGB24Image(opt_translite_attract, &transliteAttractImage, &transliteLoadError))
    {
      printf("%s\n", transliteLoadError.c_str());
      return 1;
    }

    const LoadedRGB24Image& initialImage =
        !transliteAttractImage.pixels.empty() ? transliteAttractImage : transliteImage;
    pTransliteDisplay = std::make_unique<DMDUtil::KMSDMD>(
        "PPUC Translite",
        opt_translite_width,
        opt_translite_height,
        initialImage.width,
        initialImage.height,
        opt_translite_screen,
        0,
        0,
        DMDUtil::KMSDMD::RenderingMode::SmoothScaling,
        DMDUtil::KMSDMD::Rotation::Rotate0);

    if (!pTransliteDisplay || !pTransliteDisplay->IsReady())
    {
      printf("KMS translite setup failed: %s\n",
             pTransliteDisplay ? pTransliteDisplay->GetError() : "unknown error");
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

  ppuc = new PPUC();

  // Load config file. But options set via command line are preferred.
  ppuc->LoadConfiguration(config_file);

  if (!opt_debug)
  {
    opt_debug = ppuc->GetDebug();
  }
  ppuc->SetDebug(opt_debug);
  ppuc->SetDebugErrors(opt_debug_errors);
  ppuc->SetForceHardReset(opt_hard_reset);
  ppuc->SetSkippedBoardsCsv(opt_skip_boards);
  ppuc->SetSwitchReplyDelayUs(opt_switch_reply_delay_us);
  ppuc->SetDisableFastFlipForTests(opt_switch_test || opt_coil_test ||
                                   opt_lamp_test || opt_gi_test ||
                                   opt_flasher_test);

  if (opt_debug || opt_debug_errors)
  {
    char logLine[256];
    snprintf(logLine, sizeof(logLine), "%" PRIu64 " INFO: Logging started", CurrentUnixMs());
    PrintFlushedLogLine("", logLine);
  }

  if (opt_rom)
  {
    ppuc->SetRom(opt_rom);
  }
  else
  {
    opt_rom = ppuc->GetRom();
  }

  if (opt_serial)
  {
    ppuc->SetSerial(opt_serial);
  }
  else
  {
    // opt_serial will be ignored by ZeDMD later.
    opt_serial = ppuc->GetSerial();
  }

  if (opt_switch_test || opt_coil_test || opt_lamp_test || opt_gi_test ||
      opt_flasher_test)
  {
    if (!ppuc->Connect())
    {
      printf("Unable to open serial communication to PPUC boards on %s.\n",
             opt_serial ? opt_serial : "(null)");
      return 1;
    }

    if (!initializeSpeechIfNeeded())
    {
      return 1;
    }

    ppuc->StartUpdates();
    const bool testRequiresHighPower = opt_coil_test || opt_flasher_test;
    const bool closeVirtualCoinDoorForTest =
        testRequiresHighPower && opt_close_coin_door &&
        ppuc->IsSwitchVirtualized(ppuc->GetCoinDoorClosedSwitch());

    if (closeVirtualCoinDoorForTest)
    {
      ppuc->SetSwitchState(ppuc->GetCoinDoorClosedSwitch(), 1);
    }
    ppuc->SetSolenoidState(ppuc->GetGameOnSolenoid(),
                           testRequiresHighPower ? 1 : 0);

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

    BenchTestRunner testRunner = CreateBenchTestRunner(ppuc, testMode, opt_number);
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
        const uint32_t cleanChainBaseline =
            ppuc->GetCleanSwitchReplyChainCount();
        WaitForCleanSwitchReplyCycle(ppuc, cleanChainBaseline);
      }
      PrimeBenchSwitchStates(ppuc, testRunner);
      while (running && ServiceBenchTestRunner(ppuc, testRunner))
      {
        std::this_thread::sleep_for(std::chrono::microseconds(MAIN_LOOP_SLEEP_US));
      }
      CleanupBenchTestRunner(ppuc, testRunner);
    }

    ppuc->SetSolenoidState(ppuc->GetGameOnSolenoid(), 0);
    if (closeVirtualCoinDoorForTest)
    {
      ppuc->SetSwitchState(ppuc->GetCoinDoorClosedSwitch(), 0);
    }
    ppuc->StopUpdates();
    ppuc->Disconnect();

    return 0;
  }

  // Initialize displays.
  // ZeDMD messes with USB ports. when searching for the DMD.
  // So it is important to start that search before the RS485 BUS gets
  // initialized.
  DMDUtil::Config* dmdConfig = DMDUtil::Config::GetInstance();
  dmdConfig->SetLogCallback(DMDUtilLogCallback);
  dmdConfig->parseConfigFile(opt_ini_file);
  dmdConfig->SetRoundedCorners(opt_rounded_corners);

  if (opt_pup_triggers)
  {
    pPUPTriggerEngine = std::make_unique<PUPTriggerEngine>();
    pPUPTriggerEngine->SetDebug(opt_debug);
    pPUPTriggerEngine->SetTriggerCallback([](const char source, const uint16_t id, const uint8_t value) {
      if (source == kBoardEffectTriggerSource)
      {
        if (ppuc != nullptr)
        {
          ppuc->TriggerEvent(EVENT_SOURCE_EFFECT, id, value);
        }
        return;
      }

      if (source == kSpeechTriggerSource)
      {
        if (pSpeechService && pSpeechTriggerMap)
        {
          const std::string* text = pSpeechTriggerMap->Find(id);
          if (text)
          {
            pSpeechService->SpeakText(*text);
          }
        }
        return;
      }

      if (pDmd)
      {
        pDmd->SetPUPTrigger(source, id, value);
      }
    });

    std::string error;
    if (!pPUPTriggerEngine->LoadRules(opt_pup_triggers, error))
    {
      printf("%s: %s\n", error.c_str(), opt_pup_triggers);
      return 1;
    }
    printf("Loaded %zu PUP trigger rule(s) from %s\n", pPUPTriggerEngine->GetRuleCount(), opt_pup_triggers);
  }

  if (opt_speech_file)
  {
    pSpeechTriggerMap = std::make_unique<SpeechTriggerMap>();
    std::string error;
    if (!pSpeechTriggerMap->Load(opt_speech_file, error))
    {
      printf("%s: %s\n", error.c_str(), opt_speech_file);
      return 1;
    }
    printf("Loaded %zu speech trigger text entr%s from %s\n",
           pSpeechTriggerMap->GetEntryCount(),
           pSpeechTriggerMap->GetEntryCount() == 1 ? "y" : "ies",
           opt_speech_file);
  }

  PinmameConfig config = {
      PINMAME_AUDIO_FORMAT_INT16,
      44100,
      "",
      PINMAME_CALLBACK_CAST(PinmameOnStateUpdatedCallback, &OnStateUpdated),
      PINMAME_CALLBACK_CAST(PinmameOnDisplayAvailableCallback, &OnDisplayAvailable),
      PINMAME_CALLBACK_CAST(PinmameOnDisplayUpdatedCallback, &OnDisplayUpdated),
      PINMAME_CALLBACK_CAST(PinmameOnAudioAvailableCallback, &OnAudioAvailable),
      PINMAME_CALLBACK_CAST(PinmameOnAudioUpdatedCallback, &OnAudioUpdated),
      PINMAME_CALLBACK_CAST(PinmameOnMechAvailableCallback, &OnMechAvailable),
      PINMAME_CALLBACK_CAST(PinmameOnMechUpdatedCallback, &OnMechUpdated),
      PINMAME_CALLBACK_CAST(PinmameOnSolenoidUpdatedCallback, &OnSolenoidUpdated),
      PINMAME_CALLBACK_CAST(PinmameOnConsoleDataUpdatedCallback, &OnConsoleDataUpdated),
      PINMAME_CALLBACK_CAST(PinmameIsKeyPressedFunction, &IsKeyPressed),
      PINMAME_CALLBACK_CAST(PinmameOnLogMessageCallback, &OnLogMessage),
      NULL,
  };

#if defined(_WIN32) || defined(_WIN64)
  if (opt_pinmame_path != nullptr)
  {
    snprintf((char*)config.vpmPath, PINMAME_MAX_PATH, "%s%s", opt_pinmame_path,
             (opt_pinmame_path[0] != '\0' && opt_pinmame_path[strlen(opt_pinmame_path) - 1] != '\\' &&
              opt_pinmame_path[strlen(opt_pinmame_path) - 1] != '/')
                 ? "\\"
                 : "");
  }
  else
  {
    snprintf((char*)config.vpmPath, PINMAME_MAX_PATH, "%s%s\\pinmame\\", getenv("HOMEDRIVE"), getenv("HOMEPATH"));
  }
#else
  if (opt_pinmame_path != nullptr)
  {
    snprintf((char*)config.vpmPath, PINMAME_MAX_PATH, "%s%s", opt_pinmame_path,
             (opt_pinmame_path[0] != '\0' && opt_pinmame_path[strlen(opt_pinmame_path) - 1] != '/')
                 ? "/"
                 : "");
  }
  else
  {
    snprintf((char*)config.vpmPath, PINMAME_MAX_PATH, "%s/.pinmame/", getenv("HOME"));
  }
#endif

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
    snprintf(altcolorPath, PINMAME_MAX_PATH + 8, "%saltcolor", config.vpmPath);
#else
    snprintf(altcolorPath, PINMAME_MAX_PATH + 8, "%saltcolor", config.vpmPath);
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
    dmdConfig->SetPUPVideosPath(getenv("HOME"));
    dmdConfig->SetPUPCapture(true);
  }

  if (opt_debug)
  {
    printf("Finding displays...\n");
    dmdConfig->SetZeDMDDebug(opt_debug);
    dmdConfig->SetLogLevel(DMDUtil_LogLevel_DEBUG);
  }

  pDmd = new DMDUtil::DMD();
  pDmd->SetRomName(opt_rom);
  pDmd->FindDisplays();

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
      printf("Unsupported virtual DMD renderer '%s'. Use one of: dots, squares, scale2x, scale4x, "
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
    pVirtualDMD = DMDUtil::CreateKMSDMD(*pDmd, "PPUC DMD", opt_virtual_dmd_width, opt_virtual_dmd_height,
                                        opt_virtual_dmd_hd ? 256 : 128, opt_virtual_dmd_hd ? 64 : 32,
                                        opt_virtual_dmd_screen, kmsVirtualDmdX, kmsVirtualDmdY,
                                        virtualDmdRenderingMode, virtualDmdRotation);

    if (!pVirtualDMD)
    {
      printf("KMS couldn't create virtual DMD output.\n");
      return 1;
    }
#else
    DMDUtil::SDLDMD::RenderingMode virtualDmdRenderingMode = DMDUtil::SDLDMD::RenderingMode::Dots;
    if (!DMDUtil::ParseSDLDMDRenderingMode(opt_virtual_dmd_renderer, &virtualDmdRenderingMode))
    {
      printf("Unsupported virtual DMD renderer '%s'. Use one of: dots, squares, scale2x, scale4x, "
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

    pVirtualDMD = DMDUtil::CreateSDLDMD(*pDmd, "PPUC DMD", opt_virtual_dmd_width, opt_virtual_dmd_height,
                                        opt_virtual_dmd_window ? SDL_WINDOW_BORDERLESS : SDL_WINDOW_FULLSCREEN,
                                        opt_virtual_dmd_hd ? 256 : 128, opt_virtual_dmd_hd ? 64 : 32,
                                        opt_virtual_dmd_screen, opt_virtual_dmd_x, opt_virtual_dmd_y,
                                        virtualDmdRenderingMode, virtualDmdRotation);

    if (!pVirtualDMD)
    {
      printf("SDL couldn't create virtual DMD window/renderer: %s\n", SDL_GetError());
      return SDL_APP_FAILURE;
    }
#endif
  }

  while (pDmd->IsFinding()) std::this_thread::sleep_for(std::chrono::milliseconds(100));

  if (!opt_no_serial && !ppuc->Connect())
  {
    printf("Unable to open serial communication to PPUC boards on %s.\n",
           opt_serial ? opt_serial : "(null)");
    return 1;
  }

  if (!initializeSpeechIfNeeded())
  {
    return 1;
  }

  PinmameSetConfig(&config);

  // TODO: Add support for PINMAME_DMD_MODE_BRIGHTNESS in the libdmdutil pipeline.
  // For now, keep using RAW so monochrome DMD ROMs render correctly on ZeDMD and SDLDMD.
  const PINMAME_DMD_MODE dmdMode = PINMAME_DMD_MODE_RAW;
  PinmameSetDmdMode(dmdMode);
  // PinmameSetSoundMode(PINMAME_SOUND_MODE_ALTSOUND);
  PinmameSetHandleKeyboard(0);
  PinmameSetHandleMechanics(0);

#if defined(_WIN32) || defined(_WIN64)
  // Avoid compile error C2131. Use a larger constant value instead.
  PinmameLampState changedLampStates[256];
  PinmameGIState changedGIStates[8];
#else
  PinmameLampState changedLampStates[PinmameGetMaxLamps()];
  PinmameGIState changedGIStates[PinmameGetMaxGIs()];
#endif

  if (PinmameRun(opt_rom) == PINMAME_STATUS_OK)
  {
    // Setup signal handlers to allow graceful termination
    signal(SIGINT, signal_handler_graceful);
    signal(SIGHUP, signal_handler_graceful);
    signal(SIGTERM, signal_handler_graceful);
    signal(SIGQUIT, signal_handler_graceful);
    signal(SIGABRT, signal_handler_graceful);

    int index_recv = 0;

    ppuc->StartUpdates();
    if (opt_close_coin_door) {
      ppuc->SetSwitchState(ppuc->GetCoinDoorClosedSwitch(), 1);
    }

    while (running)
    {
      std::this_thread::sleep_for(std::chrono::microseconds(MAIN_LOOP_SLEEP_US));

      if (game_state.load(std::memory_order_acquire) == 0)
      {
        continue;
      }

      PPUCSwitchState* switchState;
      while ((switchState = ppuc->GetNextSwitchState()) != nullptr)
      {
        const uint8_t newSwitchState = switchState->state == 0 ? 0 : 1;

        if (opt_debug || opt_debug_switches)
        {
          printf("Switch updated: #%d, %d\n", switchState->number, newSwitchState);
        }

        if (pPUPTriggerEngine)
        {
          pPUPTriggerEngine->OnSwitchState(switchState->number, newSwitchState);
        }

        // Switches between 200 and 240 are custom switches within the io-boards which should not be sent to
        // pinmame. Switches above 240 will become negative values, for example 243 => -3.
        if (switchState->number < 200 || switchState->number > 241)
        {
          const int switchNumber = (switchState->number < 241) ? switchState->number : 240 - switchState->number;
          PinmameSetSwitch(switchNumber, newSwitchState);
        }
      }

      int count = PinmameGetChangedLamps(changedLampStates);
      for (int c = 0; c < count; c++)
      {
        uint16_t lampNo = changedLampStates[c].lampNo;
        uint8_t lampState = changedLampStates[c].state == 0 ? 0 : 1;

        if (opt_debug || opt_debug_lamps)
        {
          printf("Lamp updated: #%d, %d\n", lampNo, lampState);
        }

        ppuc->SetLampState(lampNo, lampState);

        if (pPUPTriggerEngine)
        {
          pPUPTriggerEngine->OnLampState(static_cast<int>(lampNo), lampState);
        }
      }

      if (ppuc->GetPlatform() == PLATFORM_WPC)
      {
        count = PinmameGetChangedGIs(changedGIStates);
        for (int c = 0; c < count; c++)
        {
          const uint8_t giNo = static_cast<uint8_t>(changedGIStates[c].giNo);
          const uint8_t giState = static_cast<uint8_t>(changedGIStates[c].state);

          if (opt_debug || opt_debug_lamps)
          {
            printf("GI updated: #%d, %d\n", giNo, giState);
          }

          ppuc->SetGIState(giNo, giState);
        }
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
                PinmameSetSwitch(4, 1);
                break;
              case 13:  // Enter
                // Game Start on Williams Flash
                PinmameSetSwitch(3, 1);
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
      ppuc->StopUpdates();
      if (!opt_no_serial)
      {
        ppuc->Disconnect();
      }
      fflush(stdout);
      _Exit(0);
    }

    ppuc->StopUpdates();
    PinmameStop();
  }

  if (!opt_no_serial)
  {
    // Close the serial device
    ppuc->Disconnect();
  }

  bool quitSDL = false;

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
    SDL_Quit();
  }

  return 0;
}
