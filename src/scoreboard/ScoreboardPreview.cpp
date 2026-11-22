// ppuc-scoreboard-preview: a game's scoreboard drawn into a PNG, with scores
// made up on the command line. No machine, no window, no PUP.
//
//   ppuc-scoreboard-preview <game folder> <out.png> [options]
//
//   --size WxH          output size (default: the layout's)
//   --players N         1-4 (default 4)
//   --current P         whose turn (default 2)
//   --ball B            (default 2)
//   --credits C         (default 3)
//   --score P=N         a player's score (defaults are made up)
//   --var name=N        a player variable of the current player
//   --ball-var name=N   a ball variable
//   --set name=text     a value the rules would set with ppuc.scoreboardSet
//   --attract           as after a game, no game running
//   --scene picture     shown in the window, standing in for a video frame
//   --callout text      a callout, drawn as it is --callout-at ms after it
//                       started (default 400: just landed)
//   --callout-at ms
//   --effect name       pop, fade, slide, shake
//   --spot name         a callout spot of the layout (default center)
//
// Prints the PUP CustomPos that puts a video into the window.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include "SDL3/SDL.h"
#include "SDL3_image/SDL_image.h"
#include "SDL3_ttf/SDL_ttf.h"
#include "scoreboard/Scoreboard.h"
#include "scoreboard/ScoreboardYaml.h"

using namespace scoreboard;

static bool SplitAssign(const char* text, std::string* name, std::string* value)
{
  const char* eq = std::strchr(text, '=');
  if (eq == nullptr) return false;
  *name = std::string(text, eq);
  *value = eq + 1;
  return !name->empty();
}

static int Usage()
{
  std::fprintf(stderr,
               "usage: ppuc-scoreboard-preview <game folder> <out.png> [--size WxH] [--players N] [--current P]\n"
               "       [--ball B] [--credits C] [--score P=N] [--var name=N] [--ball-var name=N]\n"
               "       [--set name=text] [--attract] [--scene picture]\n"
               "       [--callout text] [--callout-at ms] [--effect pop|fade|slide|shake] [--spot name]\n");
  return 2;
}

int main(int argc, char** argv)
{
  if (argc < 3) return Usage();
  const std::filesystem::path gameFolder = argv[1];
  const std::string outPath = argv[2];
  const std::filesystem::path folder = gameFolder / "scoreboard";

  Layout layout;
  std::string error;
  if (!LoadLayout((folder / "scoreboard.yaml").string(), &layout, &error))
  {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }

  int width = layout.width;
  int height = layout.height;
  Snapshot snapshot;
  Facts& facts = snapshot.facts;
  facts.hasGame = true;
  facts.inGame = true;
  facts.players = 4;
  facts.current = 2;
  facts.ball = 2;
  facts.credits = 3;
  facts.scores = {12450000, 38720500, 9880000, 1250000};
  facts.playerVars.assign(layout.playerVarNames.size(), 0);
  facts.ballVars.assign(layout.ballVarNames.size(), 0);
  std::string scene;
  Callout callout;
  bool hasCallout = false;
  uint64_t calloutAt = 400;

  for (int i = 3; i < argc; ++i)
  {
    const std::string arg = argv[i];
    const bool hasValue = i + 1 < argc;
    std::string name;
    std::string value;
    if (arg == "--size" && hasValue)
    {
      if (std::sscanf(argv[++i], "%dx%d", &width, &height) != 2) return Usage();
    }
    else if (arg == "--players" && hasValue)
      facts.players = static_cast<uint8_t>(std::atoi(argv[++i]));
    else if (arg == "--current" && hasValue)
      facts.current = static_cast<uint8_t>(std::atoi(argv[++i]));
    else if (arg == "--ball" && hasValue)
      facts.ball = static_cast<uint8_t>(std::atoi(argv[++i]));
    else if (arg == "--credits" && hasValue)
      facts.credits = static_cast<uint16_t>(std::atoi(argv[++i]));
    else if (arg == "--score" && hasValue && SplitAssign(argv[++i], &name, &value))
    {
      const int p = std::atoi(name.c_str());
      if (p < 1 || p > 4) return Usage();
      facts.scores[p - 1] = std::strtoull(value.c_str(), nullptr, 10);
    }
    else if ((arg == "--var" || arg == "--ball-var") && hasValue && SplitAssign(argv[++i], &name, &value))
    {
      const auto& names = arg == "--var" ? layout.playerVarNames : layout.ballVarNames;
      auto& values = arg == "--var" ? facts.playerVars : facts.ballVars;
      bool found = false;
      for (size_t k = 0; k < names.size(); ++k)
      {
        if (names[k] == name)
        {
          values[k] = std::strtoll(value.c_str(), nullptr, 0);
          found = true;
        }
      }
      if (!found) std::fprintf(stderr, "note: no field shows %s\n", name.c_str());
    }
    else if (arg == "--set" && hasValue && SplitAssign(argv[++i], &name, &value))
      snapshot.values[name] = value;
    else if (arg == "--attract")
      facts.inGame = false;
    else if (arg == "--scene" && hasValue)
      scene = argv[++i];
    else if (arg == "--callout" && hasValue)
    {
      callout.text = argv[++i];
      hasCallout = true;
    }
    else if (arg == "--callout-at" && hasValue)
      calloutAt = std::strtoull(argv[++i], nullptr, 10);
    else if (arg == "--effect" && hasValue)
    {
      if (!Callout::ParseEffect(argv[++i], &callout.effect)) return Usage();
    }
    else if (arg == "--spot" && hasValue)
      callout.spot = argv[++i];
    else
      return Usage();
  }
  for (int p = facts.players + 1; p <= 4; ++p) facts.scores[p - 1] = 0;
  snapshot.version = 1;

  if (!SDL_Init(0))
  {
    std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 1;
  }
  SDL_Surface* surface = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_RGBA32);
  SDL_Renderer* renderer = surface ? SDL_CreateSoftwareRenderer(surface) : nullptr;
  if (renderer == nullptr)
  {
    std::fprintf(stderr, "software renderer: %s\n", SDL_GetError());
    return 1;
  }
  SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
  SDL_RenderClear(renderer);

  Board board;
  if (!board.Load(layout, folder.string(), renderer, &error))
  {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  SDL_Texture* sceneTexture = nullptr;
  if (!scene.empty() && (sceneTexture = IMG_LoadTexture(renderer, scene.c_str())) == nullptr)
  {
    std::fprintf(stderr, "%s: %s\n", scene.c_str(), SDL_GetError());
    return 1;
  }
  auto drawFrame = [&]()
  {
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    if (sceneTexture)
    {
      const float sx = static_cast<float>(width) / layout.width;
      const float sy = static_cast<float>(height) / layout.height;
      const SDL_FRect window{layout.windowX * sx, layout.windowY * sy, layout.windowW * sx, layout.windowH * sy};
      SDL_RenderTexture(renderer, sceneTexture, nullptr, &window);
    }
    board.Draw(width, height, snapshot, sceneTexture == nullptr, calloutAt);
    // SDL batches draw calls; without this the time below is only the batching.
    SDL_FlushRenderer(renderer);
  };
  if (hasCallout) board.StartCallout(callout, 0);
  const uint64_t started = SDL_GetTicksNS();
  drawFrame();
  const uint64_t firstNs = SDL_GetTicksNS() - started;
  // Again, as every frame after the first is: nothing left to prepare.
  const uint64_t again = SDL_GetTicksNS();
  drawFrame();
  const uint64_t secondNs = SDL_GetTicksNS() - again;
  if (sceneTexture) SDL_DestroyTexture(sceneTexture);
  SDL_RenderPresent(renderer);

  if (!IMG_SavePNG(surface, outPath.c_str()))
  {
    std::fprintf(stderr, "%s: %s\n", outPath.c_str(), SDL_GetError());
    return 1;
  }
  std::printf("wrote %s (%dx%d), %zu glyphs prepared in %.1f ms, a frame after that %.2f ms (software renderer, no GPU)\n",
              outPath.c_str(), width, height, board.GlyphCount(), firstNs / 1e6, secondNs / 1e6);
  std::printf("PUP CustomPos for the scene screen: %s\n", PupCustomPos(layout, 2).c_str());
  board.Unload();
  SDL_DestroyRenderer(renderer);
  SDL_DestroySurface(surface);
  TTF_Quit();
  SDL_Quit();
  return 0;
}
