#include <ctype.h>
#include <inttypes.h>
#include <stdlib.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "DMDUtil/Config.h"
#include "DMDUtil/DMDServer.h"
#include "DMDUtil/DMDUtil.h"
#include "SDL3/SDL.h"
#include "SDL3_image/SDL_image.h"
#include "VirtualDMD.h"
#include "cargs.h"

SDL_AudioStream* m_pstream = nullptr;
SDL_AudioSpec audioSpec;

DMDUtil::DMD* pDmd;
DMDUtil::DMDServer* pServer;

SDL_Window* pTransliteWindow;
SDL_Renderer* pTransliteRenderer;
SDL_Texture* pTransliteTexture;
SDL_Texture* pTransliteAttractTexture;
SDL_Window* pVirtualDMDWindow;
SDL_Renderer* pVirtualDMDRenderer;
SDL_Event quitEvent;

bool opt_debug = false;
bool opt_no_sound = false;
bool opt_serum = false;
bool opt_pup = false;
const char* opt_rom = NULL;
int game_state = 0;
bool running = true;

uint32_t currentThreadId = 0;
std::mutex threadMutex;
uint32_t disconnectOtherClients = 0;
std::vector<uint32_t> threads;

static struct cag_option options[] = {
    {.identifier = 'c',
     .access_letters = "c",
     .access_name = "config",
     .value_name = "VALUE",
     .description = "Path to config file"},
    {.identifier = 'M', .access_name = "no-sound", .value_name = NULL, .description = "Turn off sound (optional)"},
    {.identifier = 'm',
     .access_letters = "m",
     .access_name = "dump-display",
     .value_name = NULL,
     .description = "Enable display dump (optional)"},
    {.identifier = 'd',
     .access_letters = "d",
     .access_name = "debug",
     .value_name = NULL,
     .description = "Enable all debug output (optional)"},
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
    {.identifier = 'S',
     .access_name = "virtual-dmd-scale",
     .value_name = NULL,
     .description = "Scale virtual DMD instead or rendering dots."},
    {.identifier = 'h', .access_letters = "h", .access_name = "help", .description = "Show help"}};

void DMDUTILCALLBACK LogCallback(DMDUtil_LogLevel logLevel, const char* format, va_list args)
{
  char buffer[1024];
  vsnprintf(buffer, sizeof(buffer), format, args);
  uint32_t now =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count();

  FILE* output = stderr;

  if (logLevel == DMDUtil_LogLevel_INFO)
  {
    fprintf(output, "%lu INFO: %s\n", now, buffer);
  }
  else if (logLevel == DMDUtil_LogLevel_DEBUG)
  {
    fprintf(output, "%lu DEBUG: %s\n", now, buffer);
  }
  else if (logLevel == DMDUtil_LogLevel_ERROR)
  {
    fprintf(output, "%lu ERROR: %s\n", now, buffer);
  }

  fflush(output);
}

void signal_handler(int sig)
{
  printf("\nReceived signal %d, shutting down...\n", sig);
  running = false;
  quitEvent.type = SDL_EVENT_QUIT;
  SDL_PushEvent(&quitEvent);

  // CTRL-C should terminate the process hard after 2 seconds if normal shutdown fails.
  if (sig == SIGINT)
  {
    std::thread(
        []
        {
          std::this_thread::sleep_for(std::chrono::seconds(2));
          exit(0);
        })
        .detach();
  }
}

int main(int argc, char* argv[])
{
  signal(SIGHUP, signal_handler);
  signal(SIGINT, signal_handler);
  signal(SIGKILL, signal_handler);
  signal(SIGTERM, signal_handler);
  signal(SIGQUIT, signal_handler);
  signal(SIGABRT, signal_handler);

  char identifier;
  cag_option_context cag_context;
  const char* config_file = NULL;
  bool opt_dump = false;
  const char* opt_translite = NULL;
  const char* opt_translite_attract = NULL;
  bool opt_translite_window = false;
  uint16_t opt_translite_width = 1920;
  uint16_t opt_translite_height = 1080;
  int8_t opt_translite_screen = -1;
  bool opt_virtual_dmd = false;
  bool opt_virtual_dmd_hd = false;
  bool opt_virtual_dmd_window = false;
  bool opt_virtual_dmd_scale = false;
  uint16_t opt_virtual_dmd_width = 1280;
  uint16_t opt_virtual_dmd_height = 320;
  int8_t opt_virtual_dmd_screen = -1;
  VirtualDMD* pVirtualDMD;
  uint32_t threadId = 0;

  cag_option_init(&cag_context, options, CAG_ARRAY_SIZE(options), argc, argv);
  while (cag_option_fetch(&cag_context))
  {
    identifier = cag_option_get_identifier(&cag_context);
    switch (identifier)
    {
      case 'c':
        config_file = cag_option_get_value(&cag_context);
        break;
      case 'M':
        opt_no_sound = true;
        break;
      case 'm':
        opt_dump = true;
        break;
      case 'd':
        opt_debug = true;
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
      case 'S':
        opt_virtual_dmd_scale = true;
        break;
      case 'h':
        printf("Usage: ppuc [OPTION]...\n");
        cag_option_print(options, CAG_ARRAY_SIZE(options), stdout);
        return 0;
    }
  }

  if (!config_file)
  {
    printf("Config file missing.\n");
    return -1;
  }

  if (!opt_no_sound)
  {
    // Initialize the sound device
    if (!SDL_Init(SDL_INIT_AUDIO))
    {
      printf("SDL_Init failed: %s\n", SDL_GetError());
      return 1;
    }
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
      printf("SDL couldn't create translite window/renderer: %s", SDL_GetError());
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
  }

  if (opt_virtual_dmd)
  {
    if (!SDL_CreateWindowAndRenderer("PPUC DMD", opt_virtual_dmd_width, opt_virtual_dmd_height,
                                     opt_virtual_dmd_window ? SDL_WINDOW_BORDERLESS : SDL_WINDOW_FULLSCREEN,
                                     &pVirtualDMDWindow, &pVirtualDMDRenderer))
    {
      printf("SDL couldn't create virtual DMD window/renderer: %s", SDL_GetError());
      return SDL_APP_FAILURE;
    }

    SDL_SetWindowPosition(pVirtualDMDWindow, 0, 0);
    while (!SDL_SyncWindow(pVirtualDMDWindow));
  }

  DMDUtil::Config* dmdConfig = DMDUtil::Config::GetInstance();
  dmdConfig->parseConfigFile(config_file);

  dmdConfig->SetDMDServer(false);  // This is the server. It must not connect to a different server!
  dmdConfig->SetLogLevel(DMDUtil_LogLevel_INFO);
  dmdConfig->SetLogCallback(LogCallback);

  if (opt_debug)
  {
    dmdConfig->SetZeDMDDebug(opt_debug);
    dmdConfig->SetLogLevel(DMDUtil_LogLevel_DEBUG);
  }

  pDmd = new DMDUtil::DMD();

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

    if (opt_virtual_dmd_scale)
    {
      pVirtualDMD->SetRenderingMode(VirtualDMD::RenderingMode::XBRZ);
    }

    pDmd->AddRGB24DMD((DMDUtil::RGB24DMD* const)pVirtualDMD);
  }

  pDmd->FindDisplays();
  while (pDmd->IsFinding()) std::this_thread::sleep_for(std::chrono::milliseconds(100));

  pServer = new DMDUtil::DMDServer(pDmd);

  if (!pServer->Start(dmdConfig->GetDMDServerAddr(), dmdConfig->GetDMDServerPort()))
  {
    return 1;
  }

  while (running)
  {
    SDL_Event event;

    // Use SDL_WaitEventTimeout instead of SDL_PollEvent to reduce CPU usage
    // and ensure we can still process signals
    if (SDL_WaitEventTimeout(&event, 100))  // Wait up to 100ms for an event
    {
      switch (event.type)
      {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
          running = false;
          break;
      }
    }

    SDL_Delay(10);
  }

  pServer->Stop();
  delete pServer;

  std::unique_lock<std::mutex> lock(threadMutex);
  currentThreadId = 0;
  disconnectOtherClients = 0;

  while (!threads.empty())
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  delete pDmd;

  bool quitSDL = false;

  if (m_pstream)
  {
    SDL_DestroyAudioStream(m_pstream);
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
