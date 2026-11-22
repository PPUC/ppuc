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
//   --free-play         the machine runs free play
//   --scene picture     shown in the window, standing in for a video frame
//   --callout text      a callout, drawn as it is --callout-at ms after it
//                       started (default 400: just landed)
//   --callout-at ms
//   --effect name       pop, fade, slide, shake, brand
//   --spot name         a callout spot of the layout (default center)
//   --replay timeline   plays a recorded game instead: one PNG per frame
//   --frames folder     where the replay's frames go (frame00001.png, ...); "-"
//                       writes them raw to stdout instead, RGBA, for a pipe into
//                       ffmpeg -f rawvideo -pix_fmt rgba -s WxH -r FPS -i -
//                       (a long game as PNGs is tens of gigabytes)
//   --fps n             replay frame rate (default 30)
//   --transparent       leave the window see-through, to lay the frames over
//                       a video afterwards
//
// A timeline has one change per line, "<ms> <what> <value>", in time order:
//   0     players 2          current, ball, credits, ingame (0/1) alike
//   1200  score1 5000000     score1..score4
//   1300  var.bullets 3      a player variable; ballvar.<name> a ball one
//   1300  value.mode Car Chase
//   1400  callout text=SILVER BULLET|effect=slide|spot=center|priority=2|ms=1500
//
// Prints the PUP CustomPos that puts a video into the window.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

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

namespace
{

struct TimelineEvent
{
  uint64_t ms = 0;
  std::string what;
  std::string value;
};

bool ReadTimeline(const std::string& path, std::vector<TimelineEvent>* out, std::string* error)
{
  std::ifstream in(path);
  if (!in)
  {
    *error = path + ": cannot read";
    return false;
  }
  std::string line;
  int lineNo = 0;
  while (std::getline(in, line))
  {
    ++lineNo;
    if (line.empty() || line[0] == '#') continue;
    std::istringstream fields(line);
    TimelineEvent event;
    if (!(fields >> event.ms >> event.what))
    {
      *error = path + " line " + std::to_string(lineNo) + ": expected '<ms> <what> <value>'";
      return false;
    }
    std::getline(fields, event.value);
    const size_t start = event.value.find_first_not_of(' ');
    event.value = start == std::string::npos ? std::string() : event.value.substr(start);
    out->push_back(event);
  }
  return true;
}

void SetVar(const std::vector<std::string>& names, std::vector<int64_t>* values, const std::string& name, int64_t value)
{
  for (size_t i = 0; i < names.size(); ++i)
    if (names[i] == name) (*values)[i] = value;
}

// One timeline change, applied to what the board shows. False for a line
// that means nothing.
bool Apply(const TimelineEvent& event, const Layout& layout, Snapshot* snapshot, Board* board)
{
  Facts& facts = snapshot->facts;
  const int64_t number = std::strtoll(event.value.c_str(), nullptr, 10);
  const std::string& w = event.what;
  if (w == "players")
    facts.players = static_cast<uint8_t>(number);
  else if (w == "current")
    facts.current = static_cast<uint8_t>(number);
  else if (w == "ball")
    facts.ball = static_cast<uint8_t>(number);
  else if (w == "credits")
    facts.credits = static_cast<uint16_t>(number);
  else if (w == "freeplay")
    facts.freePlay = number != 0;
  else if (w == "ingame")
    facts.inGame = number != 0;
  else if (w.size() == 6 && w.compare(0, 5, "score") == 0 && w[5] >= '1' && w[5] <= '4')
    facts.scores[w[5] - '1'] = static_cast<uint64_t>(number < 0 ? 0 : number);
  else if (w.compare(0, 4, "var.") == 0)
    SetVar(layout.playerVarNames, &facts.playerVars, w.substr(4), number);
  else if (w.compare(0, 8, "ballvar.") == 0)
    SetVar(layout.ballVarNames, &facts.ballVars, w.substr(8), number);
  else if (w.compare(0, 6, "value.") == 0)
  {
    if (event.value.empty())
      snapshot->values.erase(w.substr(6));
    else
      snapshot->values[w.substr(6)] = event.value;
  }
  else if (w == "callout")
  {
    Callout callout;
    if (!ParseTimelineCallout(event.value, &callout)) return false;
    board->StartCallout(callout, event.ms);
    return true;
  }
  else
    return false;
  ++snapshot->version;
  return true;
}

}  // namespace

static int Usage()
{
  std::fprintf(stderr,
               "usage: ppuc-scoreboard-preview <game folder> <out.png> [--size WxH] [--players N] [--current P]\n"
               "       [--ball B] [--credits C] [--free-play] [--score P=N] [--var name=N] [--ball-var name=N]\n"
               "       [--set name=text] [--attract] [--scene picture]\n"
               "       [--callout text] [--callout-at ms] [--effect pop|fade|slide|shake|brand] [--spot name]\n"
               "       [--replay timeline --frames folder [--fps n] [--transparent]]\n");
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
  std::string replayPath;
  std::string framesDir;
  int fps = 30;
  bool transparent = false;

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
    else if (arg == "--free-play")
      facts.freePlay = true;
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
    else if (arg == "--replay" && hasValue)
      replayPath = argv[++i];
    else if (arg == "--frames" && hasValue)
      framesDir = argv[++i];
    else if (arg == "--fps" && hasValue)
      fps = std::max(1, std::atoi(argv[++i]));
    else if (arg == "--transparent")
      transparent = true;
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
  if (!replayPath.empty())
  {
    std::vector<TimelineEvent> events;
    if (!ReadTimeline(replayPath, &events, &error))
    {
      std::fprintf(stderr, "%s\n", error.c_str());
      return 1;
    }
    if (framesDir.empty())
    {
      std::fprintf(stderr, "--replay needs --frames\n");
      return 2;
    }
    const bool toStdout = framesDir == "-";
    if (!toStdout) std::filesystem::create_directories(framesDir);
    // A replay starts from nothing: no game, the timeline says what there is.
    snapshot = Snapshot{};
    snapshot.facts.hasGame = true;
    snapshot.facts.playerVars.assign(layout.playerVarNames.size(), 0);
    snapshot.facts.ballVars.assign(layout.ballVarNames.size(), 0);
    const uint64_t endMs = events.empty() ? 0 : events.back().ms + 2500;
    size_t next = 0;
    int frame = 0;
    const uint64_t started = SDL_GetTicksNS();
    for (uint64_t t = 0; t <= endMs; t = static_cast<uint64_t>(++frame) * 1000 / fps)
    {
      while (next < events.size() && events[next].ms <= t)
      {
        if (!Apply(events[next], layout, &snapshot, &board))
        {
          std::fprintf(stderr, "%s: cannot use '%s %s'\n", replayPath.c_str(), events[next].what.c_str(),
                       events[next].value.c_str());
        }
        ++next;
      }
      if (transparent)
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
      else
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
      SDL_RenderClear(renderer);
      board.Draw(width, height, snapshot, !transparent, t);
      SDL_FlushRenderer(renderer);
      if (toStdout)
      {
        // Row by row: the surface may pad its rows.
        bool written = true;
        for (int y = 0; y < height && written; ++y)
        {
          const uint8_t* row = static_cast<const uint8_t*>(surface->pixels) + static_cast<size_t>(y) * surface->pitch;
          written = std::fwrite(row, 4, static_cast<size_t>(width), stdout) == static_cast<size_t>(width);
        }
        if (!written)
        {
          std::fprintf(stderr, "frame %d: stdout closed\n", frame + 1);
          return 1;
        }
        continue;
      }
      char name[32];
      std::snprintf(name, sizeof(name), "frame%05d.png", frame + 1);
      if (!IMG_SavePNG(surface, (std::filesystem::path(framesDir) / name).string().c_str()))
      {
        std::fprintf(stderr, "%s: %s\n", name, SDL_GetError());
        return 1;
      }
    }
    std::fflush(stdout);
    IMG_SavePNG(surface, outPath.c_str());
    // On stderr: stdout may be the video.
    std::fprintf(stderr, "replayed %zu changes into %d frames at %d fps in %s (%.1f s to draw and save)\n",
                 events.size(), frame, fps, toStdout ? "stdout" : framesDir.c_str(),
                 (SDL_GetTicksNS() - started) / 1e9);
    board.Unload();
    SDL_DestroyRenderer(renderer);
    SDL_DestroySurface(surface);
    TTF_Quit();
    SDL_Quit();
    return 0;
  }

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
  std::printf(
      "wrote %s (%dx%d), %zu glyphs prepared in %.1f ms, a frame after that %.2f ms (software renderer, no GPU)\n",
      outPath.c_str(), width, height, board.GlyphCount(), firstNs / 1e6, secondNs / 1e6);
  std::printf("PUP CustomPos for the scene screen: %s\n", PupCustomPos(layout, 2, width, height).c_str());
  board.Unload();
  SDL_DestroyRenderer(renderer);
  SDL_DestroySurface(surface);
  TTF_Quit();
  SDL_Quit();
  return 0;
}
