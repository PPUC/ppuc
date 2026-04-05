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

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>

#include "DMDUtil/Config.h"
#include "DMDUtil/ConsoleDMD.h"
#include "DMDUtil/DMDUtil.h"
#include "AudioOutput.h"
#include "PUPTriggerEngine.h"
#include "SDL3/SDL.h"
#include "SDL3_image/SDL_image.h"
#include "SpeechService.h"
#include "SpeechTriggerMap.h"
#include "VirtualDMD.h"
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

SDL_Window* pTransliteWindow;
SDL_Renderer* pTransliteRenderer;
SDL_Texture* pTransliteTexture;
SDL_Texture* pTransliteAttractTexture;
SDL_Window* pVirtualDMDWindow;
SDL_Renderer* pVirtualDMDRenderer;

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
const char* opt_rom = NULL;
int game_state = 0;
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

static bool ParseIntStrict(const char* text, int* outValue)
{
  if (!text || !outValue || text[0] == '\0')
  {
    return false;
  }

  char* end = nullptr;
  const long value = strtol(text, &end, 10);
  if (end == text || *end != '\0')
  {
    return false;
  }

  if (value < INT_MIN || value > INT_MAX)
  {
    return false;
  }

  *outValue = static_cast<int>(value);
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

static bool ParseSpeechBackend(const char* text, SpeechBackend* outBackend)
{
  if (!text || !outBackend)
  {
    return false;
  }

  if (strcmp(text, "auto") == 0)
  {
    *outBackend = SpeechBackend::Auto;
    return true;
  }
  if (strcmp(text, "flite") == 0)
  {
    *outBackend = SpeechBackend::Flite;
    return true;
  }
  if (strcmp(text, "espeak") == 0 || strcmp(text, "espeak-ng") == 0)
  {
    *outBackend = SpeechBackend::ESpeakNg;
    return true;
  }

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
    {.identifier = 'h', .access_letters = "h", .access_name = "help", .description = "Show help"}};

void PINMAMECALLBACK Game(PinmameGame* game)
{
  printf(
      "Game(): name=%s, description=%s, manufacturer=%s, year=%s, "
      "flags=%lu, found=%d\n",
      game->name, game->description, game->manufacturer, game->year, (unsigned long)game->flags, game->found);
}

void PINMAMECALLBACK OnStateUpdated(int state, void* p_userData)
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

    game_state = state;
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
                                  void* p_userData)
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
                                        void* p_userData)
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
                                      void* p_userData)
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
      case PINMAME_DISPLAY_TYPE_VIDEO_ROT90:
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
    }
  }
}

int PINMAMECALLBACK OnAudioAvailable(PinmameAudioInfo* p_audioInfo, void* p_userData)
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

int PINMAMECALLBACK OnAudioUpdated(void* p_buffer, int samples, void* p_userData)
{
  if (pAudioOutput != nullptr)
  {
    pAudioOutput->QueueGameFrames(reinterpret_cast<const int16_t*>(p_buffer),
                                  static_cast<size_t>(samples));
  }
  return samples;
}

void PINMAMECALLBACK OnSolenoidUpdated(PinmameSolenoidState* p_solenoidState, void* p_userData)
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

void PINMAMECALLBACK OnMechAvailable(int mechNo, PinmameMechInfo* p_mechInfo, void* p_userData)
{
  if (opt_debug)
  {
    printf(
        "OnMechAvailable: mechNo=%d, type=%d, length=%d, steps=%d, pos=%d, "
        "speed=%d\n",
        mechNo, p_mechInfo->type, p_mechInfo->length, p_mechInfo->steps, p_mechInfo->pos, p_mechInfo->speed);
  }
}

void PINMAMECALLBACK OnMechUpdated(int mechNo, PinmameMechInfo* p_mechInfo, void* p_userData)
{
  if (opt_debug)
  {
    printf(
        "OnMechUpdated: mechNo=%d, type=%d, length=%d, steps=%d, pos=%d, "
        "speed=%d\n",
        mechNo, p_mechInfo->type, p_mechInfo->length, p_mechInfo->steps, p_mechInfo->pos, p_mechInfo->speed);
  }
}

void PINMAMECALLBACK OnConsoleDataUpdated(void* p_data, int size, void* p_userData)
{
  if (opt_debug)
  {
    printf("OnConsoleDataUpdated: size=%d\n", size);
  }
}

int PINMAMECALLBACK IsKeyPressed(PINMAME_KEYCODE keycode, void* p_userData) { return 0; }

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
  VirtualDMD* pVirtualDMD = nullptr;

  cag_option_init(&cag_context, options, CAG_ARRAY_SIZE(options), argc, argv);
  while (cag_option_fetch(&cag_context))
  {
    identifier = cag_option_get_identifier(&cag_context);
    switch (identifier)
    {
      case 'c':
        config_file = cag_option_get_value(&cag_context);
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
  if (!ParseSpeechBackend(opt_speech_backend, &speechBackend))
  {
    fprintf(stderr, "Invalid value for --speech-backend: %s\n",
            opt_speech_backend ? opt_speech_backend : "(null)");
    return 1;
  }

  SpeechOptions speechOptions;
  if (opt_speech_voice && opt_speech_voice[0] != '\0')
  {
    speechOptions.voice = opt_speech_voice;
  }
  if (opt_speech_rate_arg &&
      !ParseIntStrict(opt_speech_rate_arg, &speechOptions.rate))
  {
    fprintf(stderr, "Invalid value for --speech-rate: %s\n",
            opt_speech_rate_arg);
    return 1;
  }
  if (opt_speech_pitch_arg &&
      !ParseIntStrict(opt_speech_pitch_arg, &speechOptions.pitch))
  {
    fprintf(stderr, "Invalid value for --speech-pitch: %s\n",
            opt_speech_pitch_arg);
    return 1;
  }
  if (speechOptions.rate < 0)
  {
    fprintf(stderr, "--speech-rate must be >= 0\n");
    return 1;
  }
  if (speechOptions.pitch < 0 || speechOptions.pitch > 100)
  {
    fprintf(stderr, "--speech-pitch must be in the range 0..100\n");
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

  if (opt_speech_file && opt_no_sound)
  {
    fprintf(stderr,
            "--speech-file requires audio output and cannot be used with --no-sound\n");
    return 1;
  }
  if ((opt_speech || opt_greeting || opt_speech_voice || opt_speech_rate_arg ||
       opt_speech_pitch_arg) &&
      opt_no_sound)
  {
    fprintf(stderr,
            "Speech options require audio output and cannot be used with --no-sound\n");
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
    pSpeechService = CreateSpeechService(*pAudioOutput, speechBackend,
                                         speechOptions, &speechBackendError);
    if (pSpeechService == nullptr)
    {
      fprintf(stderr, "Speech init failed: %s\n",
              speechBackendError.empty() ? "unknown backend error"
                                         : speechBackendError.c_str());
      return false;
    }

    printf("Speech backend requested: %s",
           opt_speech_backend ? opt_speech_backend : "auto");
    if (!speechOptions.voice.empty())
    {
      printf(", voice=%s", speechOptions.voice.c_str());
    }
    if (opt_speech_rate_arg != nullptr)
    {
      printf(", rate=%d", speechOptions.rate);
    }
    if (opt_speech_pitch_arg != nullptr)
    {
      printf(", pitch=%d", speechOptions.pitch);
    }
    printf("\n");

    return true;
  };

  if (opt_greeting)
  {
    std::string speechBackendError;
    pSpeechService = CreateSpeechService(*pAudioOutput, speechBackend,
                                         speechOptions, &speechBackendError);
    if (pSpeechService == nullptr)
    {
      fprintf(stderr, "Speech init failed: %s\n",
              speechBackendError.empty() ? "unknown backend error"
                                         : speechBackendError.c_str());
      return 1;
    }
    pSpeechService->SpeakText("P P U C, the pinball power-up controller.");
  }

  if (opt_translite || opt_virtual_dmd)
  {
// Set the SDL video driver for Linux framebuffer
#ifdef __linux__
    setenv("SDL_VIDEODRIVER", "KMSDRM", 1);
#endif

    if (!SDL_Init(SDL_INIT_VIDEO))
    {
      printf("SDL_Init Error: %s\n", SDL_GetError());
      return 1;
    }
  }

  if (opt_translite)
  {
    if (!SDL_CreateWindowAndRenderer("PPUC Translite", opt_translite_width, opt_translite_height,
                                     opt_translite_window ? SDL_WINDOW_BORDERLESS : SDL_WINDOW_FULLSCREEN,
                                     &pTransliteWindow, &pTransliteRenderer))
    {
      printf("SDL couldn't create translite window/renderer: %s\n", SDL_GetError());
      return SDL_APP_FAILURE;
    }

    SDL_SetWindowPosition(pTransliteWindow, 0, 0);
    while (!SDL_SyncWindow(pTransliteWindow));

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
  }

  if (opt_virtual_dmd)
  {
    if (!SDL_CreateWindowAndRenderer("PPUC DMD", opt_virtual_dmd_width, opt_virtual_dmd_height,
                                     opt_virtual_dmd_window ? SDL_WINDOW_BORDERLESS : SDL_WINDOW_FULLSCREEN,
                                     &pVirtualDMDWindow, &pVirtualDMDRenderer))
    {
      printf("SDL couldn't create virtual DMD window/renderer: %s\n", SDL_GetError());
      return SDL_APP_FAILURE;
    }

    SDL_SetWindowPosition(pVirtualDMDWindow, 0, 0);
    while (!SDL_SyncWindow(pVirtualDMDWindow));
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

  if (opt_pup_triggers)
  {
    pPUPTriggerEngine = std::make_unique<PUPTriggerEngine>();
    pPUPTriggerEngine->SetDebug(opt_debug);
    pPUPTriggerEngine->SetTriggerCallback([](const char source, const uint16_t id, const uint8_t value) {
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
      &OnStateUpdated,
      &OnDisplayAvailable,
      &OnDisplayUpdated,
      &OnAudioAvailable,
      &OnAudioUpdated,
      &OnMechAvailable,
      &OnMechUpdated,
      &OnSolenoidUpdated,
      &OnConsoleDataUpdated,
      &IsKeyPressed,
      &OnLogMessage,
      NULL,
  };

#if defined(_WIN32) || defined(_WIN64)
  snprintf((char*)config.vpmPath, PINMAME_MAX_PATH, "%s%s\\pinmame\\", getenv("HOMEDRIVE"), getenv("HOMEPATH"));
#else
  snprintf((char*)config.vpmPath, PINMAME_MAX_PATH, "%s/.pinmame/", getenv("HOME"));
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
    if (opt_virtual_dmd_hd)
    {
      pVirtualDMD = new VirtualDMD(pVirtualDMDRenderer, 256, 64);
    }
    else
    {
      pVirtualDMD = new VirtualDMD(pVirtualDMDRenderer, 128, 32);
    }

    pDmd->AddRGB24DMD((DMDUtil::RGB24DMD* const)pVirtualDMD);
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

  PinmameSetDmdMode(PINMAME_DMD_MODE_RAW);
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

      if (!game_state)
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
              if (!SDL_SetRenderDrawColor(pTransliteRenderer, 0, 0, 0, 255) || !SDL_RenderClear(pTransliteRenderer) ||
                  !SDL_RenderTexture(pTransliteRenderer, pTransliteTexture, nullptr, nullptr) ||
                  !SDL_RenderPresent(pTransliteRenderer) || !SDL_FlushRenderer(pTransliteRenderer))
              {
                printf("Failed to render translite: %s\n", SDL_GetError());
              }
              break;

            case RenderCommand::RENDER_ATTRACT:
              printf("Rendering attract translite\n");
              if (!SDL_SetRenderDrawColor(pTransliteRenderer, 0, 0, 0, 255) || !SDL_RenderClear(pTransliteRenderer) ||
                  !SDL_RenderTexture(pTransliteRenderer, pTransliteAttractTexture, nullptr, nullptr) ||
                  !SDL_RenderPresent(pTransliteRenderer) || !SDL_FlushRenderer(pTransliteRenderer))
              {
                printf("Failed to render attract translite: %s\n", SDL_GetError());
              }
              break;
          }
          renderQueue.pop();
        }
      }

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

  if (pVirtualDMD)
  {
    SDL_DestroyRenderer(pVirtualDMDRenderer);
    SDL_DestroyWindow(pVirtualDMDWindow);
    quitSDL = true;
  }

  if (quitSDL)
  {
    SDL_Quit();
  }

  return 0;
}
