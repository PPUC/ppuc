#include "PPUC.h"
#include "ppuc_version.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "AudioOutput.h"
#include "MenuConfigYaml.h"
#include "MenuGames.h"
#include "SDL3/SDL.h"
#include "SDL3/SDL_filesystem.h"
#include "SDL3_image/SDL_image.h"
#include "SpeechCliSupport.h"
#include "UiStyle.h"
#include "cargs.h"
#ifdef PPUC_HAS_SDL3_TTF
#include "SDL3_ttf/SDL_ttf.h"
#endif

#if defined(_WIN32) || defined(_WIN64)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace
{
struct MenuTexture
{
  SDL_Texture* texture = nullptr;
  float width = 0.0f;
  float height = 0.0f;
};

struct MenuEntry
{
  std::string title;
  std::string info;
  std::string commandLine;
  std::vector<std::string> commandArgs;
  // Set for a game folder: launched as `ppuc-pinmame --game <folder>`, with
  // the folder's [Menu] Arguments (or the menu file's `arguments:`) appended.
  std::optional<menugames::GameFolder> game;
  std::string arguments;
  std::string imagePath;
  std::string selectedImagePath;
  MenuTexture normalTexture;
  MenuTexture selectedTexture;
};

struct MenuAssets
{
  MenuTexture logoTexture;
};

std::string Trim(const std::string& input)
{
  size_t start = 0;
  while (start < input.size() &&
         std::isspace(static_cast<unsigned char>(input[start])) != 0)
  {
    ++start;
  }

  size_t end = input.size();
  while (end > start &&
         std::isspace(static_cast<unsigned char>(input[end - 1])) != 0)
  {
    --end;
  }

  return input.substr(start, end - start);
}

std::string ToLower(std::string value)
{
  for (char& c : value)
  {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return value;
}

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

bool FileExists(const fs::path& path)
{
  std::error_code ec;
  return fs::exists(path, ec) && !fs::is_directory(path, ec);
}

std::string ResolvePathRelativeToCurrentWorkingDirectory(const std::string& value)
{
  fs::path candidate(value);
  if (candidate.is_absolute())
  {
    return candidate.lexically_normal().string();
  }
  return (fs::current_path() / candidate).lexically_normal().string();
}

bool ParseKeyValueLine(const std::string& line,
                       std::string* outKey,
                       std::string* outValue)
{
  const size_t separator = line.find(':');
  if (separator == std::string::npos)
  {
    return false;
  }

  if (outKey != nullptr)
  {
    *outKey = Trim(line.substr(0, separator));
  }
  if (outValue != nullptr)
  {
    *outValue = Trim(line.substr(separator + 1));
  }
  return true;
}

bool TokenizeCommandLine(const std::string& commandLine,
                         std::vector<std::string>* outArgs,
                         std::string* errorMessage)
{
  if (errorMessage != nullptr)
  {
    errorMessage->clear();
  }

  if (outArgs == nullptr)
  {
    if (errorMessage != nullptr)
    {
      *errorMessage = "missing command argument output storage";
    }
    return false;
  }

  std::vector<std::string> tokens;
  std::string current;
  bool inSingleQuote = false;
  bool inDoubleQuote = false;
  bool escaping = false;

  for (char c : commandLine)
  {
    if (escaping)
    {
      current.push_back(c);
      escaping = false;
      continue;
    }

    if (c == '\\' && !inSingleQuote)
    {
      escaping = true;
      continue;
    }

    if (c == '\'' && !inDoubleQuote)
    {
      inSingleQuote = !inSingleQuote;
      continue;
    }

    if (c == '"' && !inSingleQuote)
    {
      inDoubleQuote = !inDoubleQuote;
      continue;
    }

    if (!inSingleQuote && !inDoubleQuote &&
        std::isspace(static_cast<unsigned char>(c)) != 0)
    {
      if (!current.empty())
      {
        tokens.push_back(std::move(current));
        current.clear();
      }
      continue;
    }

    current.push_back(c);
  }

  if (escaping || inSingleQuote || inDoubleQuote)
  {
    if (errorMessage != nullptr)
    {
      *errorMessage = "unterminated quote or escape sequence in command";
    }
    return false;
  }

  if (!current.empty())
  {
    tokens.push_back(std::move(current));
  }

  if (tokens.empty())
  {
    if (errorMessage != nullptr)
    {
      *errorMessage = "command is empty";
    }
    return false;
  }

  *outArgs = std::move(tokens);
  return true;
}

bool FinalizeMenuEntry(MenuEntry* entry,
                       std::vector<MenuEntry>* outEntries,
                       size_t lineNumber,
                       std::string* errorMessage)
{
  if (entry == nullptr || outEntries == nullptr)
  {
    if (errorMessage != nullptr)
    {
      *errorMessage = "menu parser internal state is invalid";
    }
    return false;
  }

  if (entry->title.empty() && entry->commandLine.empty() &&
      entry->imagePath.empty() && entry->selectedImagePath.empty() &&
      !entry->game.has_value() && entry->arguments.empty())
  {
    return true;
  }

  if (entry->game.has_value())
  {
    if (!entry->commandLine.empty())
    {
      if (errorMessage != nullptr)
      {
        *errorMessage = "line " + std::to_string(lineNumber) +
                        ": menu entry has both game and command";
      }
      return false;
    }

    // The game folder's own [Menu] settings fill in what the entry leaves
    // out, so one folder can be listed twice with different arguments.
    const menugames::GameFolder& game = *entry->game;
    if (entry->title.empty())
    {
      entry->title = game.title;
    }
    if (entry->info.empty())
    {
      entry->info = game.info;
    }
    if (entry->imagePath.empty() && entry->selectedImagePath.empty())
    {
      entry->imagePath = game.image.string();
      entry->selectedImagePath = game.selectedImage.string();
    }
    if (entry->arguments.empty())
    {
      entry->arguments = game.arguments;
    }
    // Built as arguments rather than as a command line: a Windows path is
    // full of backslashes, which the command line tokenizer reads as escapes.
    std::vector<std::string> extraArgs;
    if (!entry->arguments.empty())
    {
      std::string tokenizeError;
      if (!TokenizeCommandLine(entry->arguments, &extraArgs, &tokenizeError))
      {
        if (errorMessage != nullptr)
        {
          *errorMessage = "line " + std::to_string(lineNumber) +
                          ": arguments: " + tokenizeError;
        }
        return false;
      }
    }
    entry->commandArgs = {"ppuc-pinmame", "--game", game.folder.string()};
    entry->commandArgs.insert(entry->commandArgs.end(), extraArgs.begin(),
                              extraArgs.end());
    entry->commandLine = "ppuc-pinmame --game " + game.folder.string() +
                         (entry->arguments.empty() ? "" : " " + entry->arguments);
  }
  else if (!entry->arguments.empty())
  {
    if (errorMessage != nullptr)
    {
      *errorMessage = "line " + std::to_string(lineNumber) +
                      ": arguments belong to a game entry; put them in the command";
    }
    return false;
  }

  if (entry->title.empty())
  {
    if (errorMessage != nullptr)
    {
      *errorMessage = "line " + std::to_string(lineNumber) +
                      ": menu entry is missing title";
    }
    return false;
  }

  if (entry->commandLine.empty())
  {
    if (errorMessage != nullptr)
    {
      *errorMessage = "line " + std::to_string(lineNumber) +
                      ": menu entry is missing command";
    }
    return false;
  }

  std::string tokenizeError;
  if (!entry->game.has_value() &&
      !TokenizeCommandLine(entry->commandLine, &entry->commandArgs,
                           &tokenizeError))
  {
    if (errorMessage != nullptr)
    {
      *errorMessage = "line " + std::to_string(lineNumber) + ": " +
                      tokenizeError;
    }
    return false;
  }

  if (entry->selectedImagePath.empty())
  {
    entry->selectedImagePath = entry->imagePath;
  }
  if (entry->imagePath.empty())
  {
    entry->imagePath = entry->selectedImagePath;
  }

  outEntries->push_back(std::move(*entry));
  *entry = MenuEntry{};
  return true;
}

bool LoadMenuDefinition(const char* path,
                        std::vector<MenuEntry>* outEntries,
                        std::string* errorMessage)
{
  if (errorMessage != nullptr)
  {
    errorMessage->clear();
  }

  if (path == nullptr || outEntries == nullptr)
  {
    if (errorMessage != nullptr)
    {
      *errorMessage = "missing menu definition path";
    }
    return false;
  }

  std::ifstream input(path);
  if (!input)
  {
    if (errorMessage != nullptr)
    {
      *errorMessage = std::string("failed to open menu definition: ") + path;
    }
    return false;
  }

  outEntries->clear();
  MenuEntry currentEntry;
  std::string rawLine;
  size_t lineNumber = 0;
  size_t entryStartLine = 1;

  while (std::getline(input, rawLine))
  {
    ++lineNumber;
    const std::string line = Trim(rawLine);

    if (line.empty())
    {
      if (!FinalizeMenuEntry(&currentEntry, outEntries, entryStartLine,
                             errorMessage))
      {
        return false;
      }
      entryStartLine = lineNumber + 1;
      continue;
    }

    if (line[0] == '#')
    {
      continue;
    }

    std::string key;
    std::string value;
    if (!ParseKeyValueLine(line, &key, &value))
    {
      if (errorMessage != nullptr)
      {
        *errorMessage = "line " + std::to_string(lineNumber) +
                        ": expected <key>: <value>";
      }
      return false;
    }

    key = ToLower(key);
    if (key == "title")
    {
      currentEntry.title = value;
    }
    else if (key == "info")
    {
      currentEntry.info = value;
    }
    else if (key == "command")
    {
      currentEntry.commandLine = value;
    }
    else if (key == "game")
    {
      menugames::GameFolder game;
      std::string gameError;
      if (!menugames::ReadGameFolder(
              ResolvePathRelativeToCurrentWorkingDirectory(value), &game,
              &gameError))
      {
        if (errorMessage != nullptr)
        {
          *errorMessage = "line " + std::to_string(lineNumber) + ": " +
                          gameError;
        }
        return false;
      }
      currentEntry.game = std::move(game);
    }
    else if (key == "arguments")
    {
      currentEntry.arguments = value;
    }
    else if (key == "image" || key == "normal-image")
    {
      currentEntry.imagePath =
          ResolvePathRelativeToCurrentWorkingDirectory(value);
    }
    else if (key == "selected-image" || key == "image-selected")
    {
      currentEntry.selectedImagePath =
          ResolvePathRelativeToCurrentWorkingDirectory(value);
    }
    else
    {
      if (errorMessage != nullptr)
      {
        *errorMessage = "line " + std::to_string(lineNumber) +
                        ": unknown key '" + key + "'";
      }
      return false;
    }
  }

  if (!FinalizeMenuEntry(&currentEntry, outEntries,
                         std::max(entryStartLine, lineNumber), errorMessage))
  {
    return false;
  }

  if (outEntries->empty())
  {
    if (errorMessage != nullptr)
    {
      *errorMessage = "menu definition does not contain any entries";
    }
    return false;
  }

  return true;
}

// Adds one entry per game folder in gamesDir.
bool AddGamesDirectoryEntries(const std::string& gamesDir,
                              std::vector<MenuEntry>* outEntries,
                              std::string* errorMessage)
{
  std::vector<std::string> skipped;
  const std::vector<menugames::GameFolder> games =
      menugames::ScanGamesDirectory(
          ResolvePathRelativeToCurrentWorkingDirectory(gamesDir), &skipped);
  for (const std::string& reason : skipped)
  {
    printf("Menu: skipping %s\n", reason.c_str());
  }

  for (const menugames::GameFolder& game : games)
  {
    MenuEntry entry;
    entry.game = game;
    std::string finalizeError;
    if (!FinalizeMenuEntry(&entry, outEntries, 0, &finalizeError))
    {
      if (errorMessage != nullptr)
      {
        *errorMessage = game.folder.string() + ": " + finalizeError;
      }
      return false;
    }
  }

  if (games.empty())
  {
    if (errorMessage != nullptr)
    {
      *errorMessage = "no game folders found in " + gamesDir;
    }
    return false;
  }
  return true;
}

bool LoadTextureFromPath(SDL_Renderer* renderer,
                         const std::string& path,
                         MenuTexture* outTexture,
                         std::string* errorMessage)
{
  if (errorMessage != nullptr)
  {
    errorMessage->clear();
  }

  if (path.empty())
  {
    if (outTexture != nullptr)
    {
      *outTexture = MenuTexture{};
    }
    return true;
  }

  if (renderer == nullptr || outTexture == nullptr)
  {
    if (errorMessage != nullptr)
    {
      *errorMessage = "renderer or texture output is missing";
    }
    return false;
  }

  SDL_Texture* texture = IMG_LoadTexture(renderer, path.c_str());
  if (texture == nullptr)
  {
    if (errorMessage != nullptr)
    {
      *errorMessage = "failed to load texture '" + path + "': " +
                      SDL_GetError();
    }
    return false;
  }

  float width = 0.0f;
  float height = 0.0f;
  if (!SDL_GetTextureSize(texture, &width, &height))
  {
    if (errorMessage != nullptr)
    {
      *errorMessage = "failed to query texture size for '" + path + "': " +
                      SDL_GetError();
    }
    SDL_DestroyTexture(texture);
    return false;
  }

  outTexture->texture = texture;
  outTexture->width = width;
  outTexture->height = height;
  return true;
}

void DestroyTexture(MenuTexture* texture)
{
  if (texture != nullptr && texture->texture != nullptr)
  {
    SDL_DestroyTexture(texture->texture);
    texture->texture = nullptr;
    texture->width = 0.0f;
    texture->height = 0.0f;
  }
}

void DestroyMenuTextures(std::vector<MenuEntry>* entries)
{
  if (entries == nullptr)
  {
    return;
  }

  for (MenuEntry& entry : *entries)
  {
    if (entry.selectedTexture.texture != entry.normalTexture.texture)
    {
      DestroyTexture(&entry.selectedTexture);
    }
    DestroyTexture(&entry.normalTexture);
  }
}

bool LoadMenuTextures(SDL_Renderer* renderer,
                      std::vector<MenuEntry>* entries,
                      MenuAssets* assets,
                      const std::string& logoPath,
                      std::string* errorMessage)
{
  if (errorMessage != nullptr)
  {
    errorMessage->clear();
  }

  if (renderer == nullptr || entries == nullptr || assets == nullptr)
  {
    if (errorMessage != nullptr)
    {
      *errorMessage = "menu texture loader received invalid input";
    }
    return false;
  }

  if (!logoPath.empty() &&
      !LoadTextureFromPath(renderer, logoPath, &assets->logoTexture,
                           errorMessage))
  {
    return false;
  }

  for (MenuEntry& entry : *entries)
  {
    if (!LoadTextureFromPath(renderer, entry.imagePath, &entry.normalTexture,
                             errorMessage))
    {
      return false;
    }

    if (entry.selectedImagePath == entry.imagePath)
    {
      entry.selectedTexture = entry.normalTexture;
      continue;
    }

    if (!LoadTextureFromPath(renderer, entry.selectedImagePath,
                             &entry.selectedTexture, errorMessage))
    {
      return false;
    }
  }

  return true;
}

// The fonts of PPUC's own screens. Each one is null when SDL_ttf or a font is
// missing; text then falls back to SDL's built-in debug font, which is crude
// but never absent, so the menu always says what it is.
struct MenuFonts
{
#ifdef PPUC_HAS_SDL3_TTF
  TTF_Font* large = nullptr;
  TTF_Font* small = nullptr;
  TTF_Font* compact = nullptr;
#endif
};

enum class FontSize
{
  Large,
  Small,
  Compact,
};

// Opened once the renderer exists, scaled to its height: the tools menu is
// laid out for a 1080-line backbox, and the menu keeps those proportions on
// whatever screen it gets.
void OpenMenuFonts(MenuFonts* fonts, int outputHeight)
{
#ifdef PPUC_HAS_SDL3_TTF
  if (!TTF_Init())
  {
    printf("Menu: plain text only: TTF_Init failed: %s\n", SDL_GetError());
    return;
  }
  const float scale = outputHeight > 0 ? static_cast<float>(outputHeight) / 1080.0f : 1.0f;
  for (const char* path : uistyle::kFontCandidates)
  {
    fonts->large = TTF_OpenFont(path, uistyle::kFontLargePt * scale);
    if (fonts->large != nullptr)
    {
      fonts->small = TTF_OpenFont(path, uistyle::kFontSmallPt * scale);
      fonts->compact = TTF_OpenFont(path, uistyle::kFontCompactPt * scale);
      printf("Menu: font %s\n", path);
      return;
    }
  }
  printf("Menu: plain text only: no usable font found\n");
#else
  (void)fonts;
  (void)outputHeight;
  printf("Menu: plain text only: built without SDL_ttf\n");
#endif
}

void CloseMenuFonts(MenuFonts* fonts)
{
#ifdef PPUC_HAS_SDL3_TTF
  for (TTF_Font** font : {&fonts->large, &fonts->small, &fonts->compact})
  {
    if (*font != nullptr)
    {
      TTF_CloseFont(*font);
      *font = nullptr;
    }
  }
  if (TTF_WasInit() > 0)
  {
    TTF_Quit();
  }
#else
  (void)fonts;
#endif
}

#ifdef PPUC_HAS_SDL3_TTF
TTF_Font* FontFor(const MenuFonts& fonts, FontSize size)
{
  switch (size)
  {
    case FontSize::Large:
      return fonts.large;
    case FontSize::Small:
      return fonts.small;
    case FontSize::Compact:
      return fonts.compact;
  }
  return nullptr;
}
#endif

// The debug font is 8 pixels square; scaled to roughly the TTF sizes.
float DebugScale(FontSize size, float outputHeight)
{
  const float base = size == FontSize::Large ? 4.0f : size == FontSize::Small ? 2.5f : 2.0f;
  return std::max(1.0f, base * outputHeight / 1080.0f);
}

float LineHeight(const MenuFonts& fonts, FontSize size, float outputHeight)
{
#ifdef PPUC_HAS_SDL3_TTF
  if (TTF_Font* font = FontFor(fonts, size); font != nullptr)
  {
    return static_cast<float>(TTF_GetFontLineSkip(font));
  }
#else
  (void)fonts;
#endif
  return DebugScale(size, outputHeight) * SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * 1.4f;
}

float MeasureText(const MenuFonts& fonts, FontSize size, const std::string& text, float outputHeight)
{
#ifdef PPUC_HAS_SDL3_TTF
  if (TTF_Font* font = FontFor(fonts, size); font != nullptr)
  {
    int w = 0;
    int h = 0;
    return TTF_GetStringSize(font, text.c_str(), 0, &w, &h) ? static_cast<float>(w) : 0.0f;
  }
#else
  (void)fonts;
#endif
  return static_cast<float>(text.size()) * SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * DebugScale(size, outputHeight);
}

// Shortens text until it fits, so a long title never runs off its card.
std::string FitText(const MenuFonts& fonts, FontSize size, std::string text, float maxWidth, float outputHeight)
{
  if (MeasureText(fonts, size, text, outputHeight) <= maxWidth)
  {
    return text;
  }
  while (!text.empty() && MeasureText(fonts, size, text + "...", outputHeight) > maxWidth)
  {
    text.pop_back();
  }
  return text + "...";
}

// Splits text into lines no wider than maxWidth, at spaces.
std::vector<std::string> WrapText(const MenuFonts& fonts, FontSize size, const std::string& text, float maxWidth,
                                  float outputHeight)
{
  std::vector<std::string> lines;
  std::string line;
  size_t start = 0;
  const std::string trimmed = Trim(text);
  while (start < trimmed.size())
  {
    size_t end = trimmed.find(' ', start);
    if (end == std::string::npos)
    {
      end = trimmed.size();
    }
    const std::string word = trimmed.substr(start, end - start);
    start = end + 1;
    if (word.empty())
    {
      continue;
    }
    const std::string candidate = line.empty() ? word : line + " " + word;
    if (!line.empty() && MeasureText(fonts, size, candidate, outputHeight) > maxWidth)
    {
      lines.push_back(line);
      line = word;
    }
    else
    {
      line = candidate;
    }
  }
  if (!line.empty())
  {
    lines.push_back(line);
  }
  return lines;
}

enum class Align
{
  Left,
  Center,
};

void DrawText(SDL_Renderer* renderer, const MenuFonts& fonts, FontSize size, const std::string& text, float x, float y,
              SDL_Color color, Align align, float outputHeight)
{
  if (renderer == nullptr || text.empty())
  {
    return;
  }
  const float width = MeasureText(fonts, size, text, outputHeight);
  const float left = align == Align::Center ? x - width / 2.0f : x;

#ifdef PPUC_HAS_SDL3_TTF
  if (TTF_Font* font = FontFor(fonts, size); font != nullptr)
  {
    SDL_Surface* surface = TTF_RenderText_Blended(font, text.c_str(), 0, color);
    if (surface == nullptr)
    {
      return;
    }
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    const SDL_FRect dst{left, y, static_cast<float>(surface->w), static_cast<float>(surface->h)};
    SDL_DestroySurface(surface);
    if (texture != nullptr)
    {
      SDL_RenderTexture(renderer, texture, nullptr, &dst);
      SDL_DestroyTexture(texture);
    }
    return;
  }
#else
  (void)fonts;
#endif

  const float scale = DebugScale(size, outputHeight);
  SDL_SetRenderScale(renderer, scale, scale);
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
  SDL_RenderDebugText(renderer, left / scale, y / scale, text.c_str());
  SDL_SetRenderScale(renderer, 1.0f, 1.0f);
}

void SetDrawColor(SDL_Renderer* renderer, SDL_Color color)
{
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
}

void RenderTextureFitted(SDL_Renderer* renderer,
                         const MenuTexture& texture,
                         const SDL_FRect& bounds)
{
  if (renderer == nullptr || texture.texture == nullptr ||
      texture.width <= 0.0f || texture.height <= 0.0f)
  {
    return;
  }

  const float scale = std::min(bounds.w / texture.width,
                               bounds.h / texture.height);
  const float drawWidth = texture.width * scale;
  const float drawHeight = texture.height * scale;
  const SDL_FRect dst{
      bounds.x + ((bounds.w - drawWidth) / 2.0f),
      bounds.y + ((bounds.h - drawHeight) / 2.0f),
      drawWidth,
      drawHeight,
  };
  SDL_RenderTexture(renderer, texture.texture, nullptr, &dst);
}

void RenderPlaceholder(SDL_Renderer* renderer, const MenuFonts& fonts, const SDL_FRect& bounds, float outputHeight)
{
  SetDrawColor(renderer, uistyle::kPanelBorder);
  SDL_RenderRect(renderer, &bounds);
  DrawText(renderer, fonts, FontSize::Compact, "NO IMAGE", bounds.x + bounds.w / 2.0f,
           bounds.y + (bounds.h - LineHeight(fonts, FontSize::Compact, outputHeight)) / 2.0f, uistyle::kDim,
           Align::Center, outputHeight);
}

// Draws one frame without presenting it, so the same frame can be read back
// for --screenshot.
//
// Laid out like the tools menu in ppuc-pinmame, in its colours and fonts: one
// card per game, the chosen one lifted and outlined in amber, and the keys that
// work spelled out at the bottom.
void DrawMenu(SDL_Renderer* renderer,
              const MenuFonts& fonts,
              const std::vector<MenuEntry>& entries,
              const MenuAssets& assets,
              size_t selectedIndex,
              const std::string& slogan,
              bool playfieldButtons)
{
  int renderWidth = 0;
  int renderHeight = 0;
  SDL_GetCurrentRenderOutputSize(renderer, &renderWidth, &renderHeight);
  const float screenWidth = static_cast<float>(renderWidth);
  const float screenHeight = static_cast<float>(renderHeight);
  const float unit = screenHeight / 1080.0f;

  SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
  SDL_RenderClear(renderer);

  float currentY = 36.0f * unit;
  if (assets.logoTexture.texture != nullptr)
  {
    const SDL_FRect logoBounds{
        screenWidth * 0.30f,
        currentY,
        screenWidth * 0.40f,
        screenHeight * 0.16f,
    };
    RenderTextureFitted(renderer, assets.logoTexture, logoBounds);
    currentY = logoBounds.y + logoBounds.h + 18.0f * unit;
  }

  const std::string heading = slogan.empty() ? "CHOOSE A GAME" : slogan;
  DrawText(renderer, fonts, FontSize::Large, FitText(fonts, FontSize::Large, heading, screenWidth * 0.9f, screenHeight),
           screenWidth / 2.0f, currentY, slogan.empty() ? uistyle::kText : uistyle::kAmber, Align::Center,
           screenHeight);
  currentY += LineHeight(fonts, FontSize::Large, screenHeight) + 20.0f * unit;

  const float footerHeight = 2.0f * LineHeight(fonts, FontSize::Small, screenHeight) + 24.0f * unit;
  const float sectionTop = currentY;
  const float sectionBottom = screenHeight - footerHeight;
  const float availableHeight = std::max(120.0f * unit, sectionBottom - sectionTop);
  const float cardGap = 16.0f * unit;
  const float cardHeight = std::min(220.0f * unit, std::max(140.0f * unit, availableHeight / 3.4f));
  size_t visibleCount = static_cast<size_t>((availableHeight + cardGap) / (cardHeight + cardGap));
  visibleCount = std::max<size_t>(1, std::min<size_t>(visibleCount, entries.size()));

  size_t startIndex = 0;
  if (selectedIndex >= visibleCount / 2)
  {
    startIndex = selectedIndex - (visibleCount / 2);
  }
  if (startIndex + visibleCount > entries.size())
  {
    startIndex = entries.size() - visibleCount;
  }

  const float cardWidth = std::min(screenWidth - 48.0f * unit, std::max(screenWidth * 0.6f, 1100.0f * unit));
  const float cardX = (screenWidth - cardWidth) / 2.0f;

  for (size_t visibleSlot = 0; visibleSlot < visibleCount; ++visibleSlot)
  {
    const size_t entryIndex = startIndex + visibleSlot;
    const MenuEntry& entry = entries[entryIndex];
    const bool selected = entryIndex == selectedIndex;
    const float cardY = sectionTop + static_cast<float>(visibleSlot) * (cardHeight + cardGap);
    const SDL_FRect cardRect{cardX, cardY, cardWidth, cardHeight};

    SetDrawColor(renderer, selected ? uistyle::kRowSelected : uistyle::kPanel);
    SDL_RenderFillRect(renderer, &cardRect);
    SetDrawColor(renderer, selected ? uistyle::kAmber : uistyle::kPanelBorder);
    SDL_RenderRect(renderer, &cardRect);
    if (selected)
    {
      // Two pixels on a 1080-line screen: the amber has to be seen from where
      // the player stands, not from where the technician sits.
      const SDL_FRect inner{cardRect.x + 1.0f, cardRect.y + 1.0f, cardRect.w - 2.0f, cardRect.h - 2.0f};
      SDL_RenderRect(renderer, &inner);
    }

    const float padding = 14.0f * unit;
    const SDL_FRect imageRect{
        cardRect.x + padding,
        cardRect.y + padding,
        std::min(cardRect.w * 0.3f, (cardRect.h - 2.0f * padding) * 1.6f),
        cardRect.h - 2.0f * padding,
    };
    const MenuTexture& texture = selected ? entry.selectedTexture : entry.normalTexture;
    if (texture.texture != nullptr)
    {
      RenderTextureFitted(renderer, texture, imageRect);
    }
    else
    {
      RenderPlaceholder(renderer, fonts, imageRect, screenHeight);
    }

    const float textX = imageRect.x + imageRect.w + 28.0f * unit;
    const float textWidth = cardRect.x + cardRect.w - textX - 24.0f * unit;
    float textY = cardRect.y + 22.0f * unit;

    DrawText(renderer, fonts, FontSize::Large, FitText(fonts, FontSize::Large, entry.title, textWidth, screenHeight),
             textX, textY, selected ? uistyle::kText : uistyle::kDim, Align::Left, screenHeight);
    textY += LineHeight(fonts, FontSize::Large, screenHeight) + 6.0f * unit;

    const float infoLine = LineHeight(fonts, FontSize::Small, screenHeight);
    for (const std::string& line : WrapText(fonts, FontSize::Small, entry.info, textWidth, screenHeight))
    {
      if (textY + infoLine > cardRect.y + cardRect.h - padding)
      {
        break;
      }
      DrawText(renderer, fonts, FontSize::Small, line, textX, textY, uistyle::kDim, Align::Left, screenHeight);
      textY += infoLine;
    }
  }

  const float legendY = screenHeight - footerHeight + 12.0f * unit;
  const std::string position = std::to_string(selectedIndex + 1) + " / " + std::to_string(entries.size());
  DrawText(renderer, fonts, FontSize::Small, position, screenWidth / 2.0f, legendY, uistyle::kDim, Align::Center,
           screenHeight);
  const char* legend = playfieldButtons ? "FLIPPERS choose    START plays"
                                        : "cursor keys choose    ENTER plays    ESC quits";
  DrawText(renderer, fonts, FontSize::Small, legend, screenWidth / 2.0f,
           legendY + LineHeight(fonts, FontSize::Small, screenHeight), uistyle::kDim, Align::Center, screenHeight);
}

void RenderMenu(SDL_Renderer* renderer,
                const MenuFonts& fonts,
                const std::vector<MenuEntry>& entries,
                const MenuAssets& assets,
                size_t selectedIndex,
                const std::string& slogan,
                bool playfieldButtons)
{
  DrawMenu(renderer, fonts, entries, assets, selectedIndex, slogan, playfieldButtons);
  SDL_RenderPresent(renderer);
}

// Writes the frame DrawMenu has just drawn, before it is presented.
bool SaveMenuScreenshot(SDL_Renderer* renderer, const std::string& path)
{
  SDL_Surface* surface = SDL_RenderReadPixels(renderer, nullptr);
  if (surface == nullptr)
  {
    fprintf(stderr, "Menu: screenshot failed: %s\n", SDL_GetError());
    return false;
  }
  const bool saved = IMG_SavePNG(surface, path.c_str());
  if (!saved)
  {
    fprintf(stderr, "Menu: screenshot failed: %s\n", SDL_GetError());
  }
  SDL_DestroySurface(surface);
  return saved;
}

void SpeakFocusTitle(SpeechService* speechService,
                     const std::vector<MenuEntry>& entries,
                     size_t selectedIndex)
{
  if (speechService == nullptr || selectedIndex >= entries.size())
  {
    return;
  }

  speechService->SpeakText(entries[selectedIndex].title);
}

std::string GetBaseExecutablePath()
{
  const char* basePath = SDL_GetBasePath();
  if (basePath == nullptr || basePath[0] == '\0')
  {
    return std::string();
  }
  return std::string(basePath);
}

std::string ResolveExecutablePath(const std::string& executable,
                                  const fs::path& menuDir,
                                  const std::string& basePath)
{
  if (executable.empty())
  {
    return executable;
  }

  const bool hasPathSeparator =
      executable.find('/') != std::string::npos ||
      executable.find('\\') != std::string::npos;

  fs::path candidate(executable);
  if (candidate.is_absolute())
  {
    return candidate.string();
  }

  if (hasPathSeparator)
  {
    const fs::path cwdCandidate = fs::current_path() / candidate;
    if (FileExists(cwdCandidate))
    {
      return cwdCandidate.lexically_normal().string();
    }

    const fs::path menuCandidate = menuDir / candidate;
    return menuCandidate.lexically_normal().string();
  }

  if (!basePath.empty())
  {
    const fs::path baseCandidate = fs::path(basePath) / candidate;
    if (FileExists(baseCandidate))
    {
      return baseCandidate.string();
    }
  }

  const fs::path cwdCandidate = fs::current_path() / candidate;
  if (FileExists(cwdCandidate))
  {
    return cwdCandidate.string();
  }

  const fs::path menuCandidate = menuDir / candidate;
  if (FileExists(menuCandidate))
  {
    return menuCandidate.string();
  }

  return executable;
}

bool LaunchSelectedEntry(const MenuEntry& entry,
                         const fs::path& menuDir,
                         const std::string& basePath,
                         std::string* errorMessage)
{
  if (errorMessage != nullptr)
  {
    errorMessage->clear();
  }

  if (entry.commandArgs.empty())
  {
    if (errorMessage != nullptr)
    {
      *errorMessage = "selected menu entry has no command arguments";
    }
    return false;
  }

  std::vector<std::string> commandArgs = entry.commandArgs;
  commandArgs[0] = ResolveExecutablePath(commandArgs[0], menuDir, basePath);

  std::vector<char*> argv;
  argv.reserve(commandArgs.size() + 1);
  for (std::string& arg : commandArgs)
  {
    argv.push_back(arg.data());
  }
  argv.push_back(nullptr);

#if defined(_WIN32) || defined(_WIN64)
  const intptr_t processId =
      _spawnvp(_P_NOWAIT, commandArgs[0].c_str(), argv.data());
  if (processId == -1)
  {
    if (errorMessage != nullptr)
    {
      *errorMessage = "failed to launch '" + entry.commandLine + "': " +
                      std::strerror(errno);
    }
    return false;
  }
  return true;
#else
  execvp(commandArgs[0].c_str(), argv.data());
  if (errorMessage != nullptr)
  {
    *errorMessage = "failed to launch '" + entry.commandLine + "': " +
                    std::strerror(errno);
  }
  return false;
#endif
}

// As ppuc-pinmame places its translite: on the display's origin, then waits
// for the window manager (where there is one) to have done it.
void PositionWindowOnDisplay(SDL_Window* window, int displayIndex)
{
  if (window == nullptr || displayIndex < 0)
  {
    return;
  }

  int displayCount = 0;
  SDL_DisplayID* displays = SDL_GetDisplays(&displayCount);
  if (displays == nullptr)
  {
    return;
  }
  const SDL_DisplayID display =
      displayIndex < displayCount ? displays[displayIndex] : 0;
  SDL_free(displays);
  if (display == 0)
  {
    printf("Menu: there is no screen %d, staying on the first\n",
           displayIndex);
    return;
  }

  SDL_Rect bounds{};
  if (SDL_GetDisplayBounds(display, &bounds))
  {
    SDL_SetWindowPosition(window, bounds.x, bounds.y);
  }
}

// The playfield buttons, read from the boards with a configuration that holds
// nothing else.
struct MenuHardware
{
  std::unique_ptr<PPUC> ppuc;
  menugames::MenuButtons buttons;
  // Switch reports arriving before this are the boards' initial state, not
  // presses: a button held while the machine boots must not start a game.
  uint64_t acceptSwitchesFromMs = 0;
  // Last reported state per switch: a press is an open-to-closed edge, never
  // a closed state reported again by a switch refresh.
  std::map<uint32_t, bool> closed;
};

constexpr uint64_t kInitialSwitchStateMs = 1000;

void PrintButton(const char* role, const menugames::SwitchInfo& sw)
{
  printf("Menu: %s is switch %u '%s' (board %u, %s %u)\n", role, sw.number,
         sw.description.c_str(), sw.board, sw.matrix ? "matrix position" : "port",
         sw.port);
}

// Finds the three buttons in the first game folder that names them all, and
// checks every other game folder agrees about where they are wired. The source
// game is returned through pSource.
bool FindMenuButtons(const std::vector<MenuEntry>& entries,
                     menugames::MenuButtons* pButtons,
                     const menugames::GameFolder** pSource)
{
  bool found = false;
  for (const MenuEntry& entry : entries)
  {
    if (!entry.game.has_value())
    {
      continue;
    }
    const menugames::GameFolder& game = *entry.game;

    std::vector<menugames::SwitchInfo> switches;
    menugames::ButtonOverrides overrides = game.buttons;
    std::string error;
    menugames::MenuButtons buttons;
    if (!LoadMenuSwitches(game.configFile.string(), &switches,
                          &overrides.emGameStart, &error) ||
        !menugames::ResolveMenuButtons(switches, overrides, &buttons, &error))
    {
      printf("Menu: buttons of %s: %s\n", game.folder.string().c_str(),
             error.c_str());
      continue;
    }

    if (!found)
    {
      *pButtons = buttons;
      *pSource = &game;
      found = true;
      continue;
    }

    // The same playfield wired the same way: anything else means one of the
    // game folders is for another machine, or its io-boards.yaml is wrong.
    const std::pair<const char*, std::pair<const menugames::SwitchInfo*,
                                           const menugames::SwitchInfo*>>
        pairs[] = {
            {"left flipper button", {&pButtons->leftFlipper, &buttons.leftFlipper}},
            {"right flipper button", {&pButtons->rightFlipper, &buttons.rightFlipper}},
            {"start button", {&pButtons->start, &buttons.start}},
        };
    for (const auto& [role, pair] : pairs)
    {
      if (!menugames::SameInput(*pair.first, *pair.second))
      {
        printf("Menu: WARNING: %s wires the %s to board %u port %u, but "
               "the menu reads it on board %u port %u as %s does\n",
               game.folder.string().c_str(), role, pair.second->board,
               pair.second->port, pair.first->board, pair.first->port,
               (*pSource)->folder.string().c_str());
      }
    }
  }
  return found;
}

uint64_t NowMs()
{
  return SDL_GetTicks();
}

bool ConnectMenuHardware(const std::vector<MenuEntry>& entries,
                         const char* optSerial,
                         MenuHardware* pHardware)
{
  const menugames::GameFolder* source = nullptr;
  if (!FindMenuButtons(entries, &pHardware->buttons, &source))
  {
    printf("Menu: no game folder names its flipper and start buttons; "
           "keyboard only\n");
    return false;
  }

  printf("Menu: buttons from %s\n", source->folder.string().c_str());
  PrintButton("left flipper button", pHardware->buttons.leftFlipper);
  PrintButton("right flipper button", pHardware->buttons.rightFlipper);
  PrintButton("start button", pHardware->buttons.start);

  std::error_code ec;
  fs::path configPath = fs::temp_directory_path(ec);
  if (ec)
  {
    configPath = fs::current_path();
  }
  configPath /= "ppuc-menu-io-boards.yaml";

  std::string error;
  if (!WriteMenuConfig(source->configFile.string(), configPath.string(),
                       pHardware->buttons, &error))
  {
    printf("Menu: %s; keyboard only\n", error.c_str());
    return false;
  }
  printf("Menu: board configuration written to %s\n",
         configPath.string().c_str());

  auto ppuc = std::make_unique<PPUC>();
  try
  {
    ppuc->LoadConfiguration(configPath.string().c_str());
  }
  catch (const std::exception& ex)
  {
    printf("Menu: %s; keyboard only\n", ex.what());
    return false;
  }

  // The source game's own connection settings: the boards answer the menu
  // the way they answer the game.
  if (!source->skipBoards.empty())
  {
    ppuc->SetSkippedBoardsCsv(source->skipBoards.c_str());
  }
  if (source->switchReplyDelayUs > 0)
  {
    ppuc->SetSwitchReplyDelayUs(source->switchReplyDelayUs);
  }
  const std::string serial =
      optSerial != nullptr && optSerial[0] != '\0' ? optSerial : source->serial;
  if (!serial.empty())
  {
    ppuc->SetSerial(serial.c_str());
  }

  if (!ppuc->Connect())
  {
    printf("Menu: unable to open the PPUC boards on %s; keyboard only\n",
           ppuc->GetSerial());
    ppuc->Disconnect();
    return false;
  }
  ppuc->StartUpdates();
  printf("Menu: playfield buttons active on %s\n", ppuc->GetSerial());

  pHardware->ppuc = std::move(ppuc);
  pHardware->acceptSwitchesFromMs = NowMs() + kInitialSwitchStateMs;
  return true;
}

// Releases the serial port for the game about to start.
void DisconnectMenuHardware(MenuHardware* pHardware)
{
  if (pHardware->ppuc == nullptr)
  {
    return;
  }
  pHardware->ppuc->StopUpdates();
  pHardware->ppuc->Disconnect();
  pHardware->ppuc.reset();
}

static struct cag_option options[] = {
    {.identifier = 'm',
     .access_letters = "m",
     .access_name = "menu-file",
     .value_name = "VALUE",
     .description = "Path to menu definition file (optional with --games-dir)"},
    {.identifier = 'd',
     .access_letters = "d",
     .access_name = "games-dir",
     .value_name = "VALUE",
     .description = "Directory of game folders, one menu entry each (optional)"},
    {.identifier = 's',
     .access_name = "serial",
     .value_name = "VALUE",
     .description = "Serial device of the PPUC boards; overrides the game's (optional)"},
    {.identifier = 'n',
     .access_name = "no-serial",
     .value_name = nullptr,
     .description = "Do not read the playfield buttons; keyboard only (optional)"},
    {.identifier = 'p',
     .access_name = "screenshot",
     .value_name = "FILE",
     .description = "Render the first frame to a PNG and exit, touching no boards (optional)"},
    {.identifier = 'l',
     .access_letters = "l",
     .access_name = "logo",
     .value_name = "VALUE",
     .description = "Path to menu logo image (optional)"},
    {.identifier = 'g',
     .access_letters = "g",
     .access_name = "slogan",
     .value_name = "VALUE",
     .description = "Menu slogan text shown below the logo (optional)"},
    {.identifier = 'F',
     .access_name = "window",
     .value_name = nullptr,
     .description = "Run the selector in a borderless window instead of fullscreen (optional, default the game's [Translite] Window)"},
    {.identifier = 'G',
     .access_name = "width",
     .value_name = "VALUE",
     .description = "Window width in pixels (optional, default the game's [Translite] Width, else 1920)"},
    {.identifier = 'H',
     .access_name = "height",
     .value_name = "VALUE",
     .description = "Window height in pixels (optional, default the game's [Translite] Height, else 1080)"},
    {.identifier = 'I',
     .access_name = "screen",
     .value_name = "VALUE",
     .description = "Display index for the menu window (optional, default the game's [Translite] Screen)"},
    {.identifier = 'M',
     .access_name = "no-sound",
     .value_name = nullptr,
     .description = "Turn off menu sound output (optional)"},
    {.identifier = 'W',
     .access_name = "speech",
     .value_name = nullptr,
     .description = "Speak the focused game title (optional)"},
    {.identifier = 'Y',
     .access_name = "greeting",
     .value_name = nullptr,
     .description = "Speak a startup greeting before the initial title (optional)"},
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
    // Not an option cargs parses: arguments after "--" are appended to every
    // game folder's ppuc-pinmame command.
    {.identifier = 'h',
     .access_letters = "h",
     .access_name = "help",
     .value_name = nullptr,
     .description = "Show this help"},
};
}  // namespace

int main(int argc, char* argv[])
{
  // Line by line, so a supervisor's log has what the menu found about the
  // buttons even when nobody quits it cleanly.
  setvbuf(stdout, nullptr, _IOLBF, BUFSIZ);
  printf("PPUC Menu Version: %s\n", PPUC_EXECUTABLE_VERSION);
  printf("Commit SHA: %s\n", PPUC_EXECUTABLE_SHA);

  char identifier = 0;
  cag_option_context cagContext;
  const char* optMenuFile = nullptr;
  const char* optGamesDir = nullptr;
  const char* optSerial = nullptr;
  bool optNoSerial = false;
  const char* optLogo = nullptr;
  const char* optSlogan = nullptr;
  // Unset until given: the first game's [Translite] fills in what the command
  // line does not, so the menu comes up on the screen the game will use.
  bool optWindowed = false;
  int optWidth = 0;
  int optHeight = 0;
  int optScreen = -2;
  const char* optScreenshot = nullptr;
  bool optNoSound = false;
  bool optSpeech = false;
  bool optGreeting = false;
  const char* optSpeechBackend = "auto";
  const char* optSpeechVoice = nullptr;
  const char* optSpeechRate = nullptr;
  const char* optSpeechPitch = nullptr;

  // Everything after "--" belongs to the game, not to the menu: the OS image
  // passes --plugin-dir and --firmware-path this way, whichever game is
  // chosen.
  std::vector<std::string> gameArgs;
  int menuArgc = argc;
  for (int i = 1; i < argc; ++i)
  {
    if (std::strcmp(argv[i], "--") == 0)
    {
      menuArgc = i;
      gameArgs.assign(argv + i + 1, argv + argc);
      break;
    }
  }

  cag_option_init(&cagContext, options, CAG_ARRAY_SIZE(options), menuArgc,
                  argv);
  while (cag_option_fetch(&cagContext))
  {
    identifier = cag_option_get_identifier(&cagContext);
    switch (identifier)
    {
      case 'm':
        optMenuFile = cag_option_get_value(&cagContext);
        break;
      case 'd':
        optGamesDir = cag_option_get_value(&cagContext);
        break;
      case 's':
        optSerial = cag_option_get_value(&cagContext);
        break;
      case 'n':
        optNoSerial = true;
        break;
      case 'p':
        optScreenshot = cag_option_get_value(&cagContext);
        break;
      case 'l':
        optLogo = cag_option_get_value(&cagContext);
        break;
      case 'g':
        optSlogan = cag_option_get_value(&cagContext);
        break;
      case 'F':
        optWindowed = true;
        break;
      case 'G':
        optWidth = atoi(cag_option_get_value(&cagContext));
        break;
      case 'H':
        optHeight = atoi(cag_option_get_value(&cagContext));
        break;
      case 'I':
        optScreen = atoi(cag_option_get_value(&cagContext));
        break;
      case 'M':
        optNoSound = true;
        break;
      case 'W':
        optSpeech = true;
        break;
      case 'Y':
        optGreeting = true;
        break;
      case 'U':
        optSpeechBackend = cag_option_get_value(&cagContext);
        break;
      case 'v':
        optSpeechVoice = cag_option_get_value(&cagContext);
        break;
      case 'w':
        optSpeechRate = cag_option_get_value(&cagContext);
        break;
      case 'x':
        optSpeechPitch = cag_option_get_value(&cagContext);
        break;
      case 'h':
        printf("Usage: ppuc-menu [OPTION]... [-- PPUC-PINMAME ARGUMENT...]\n");
        cag_option_print(options, CAG_ARRAY_SIZE(options), stdout);
        printf("Arguments after -- are passed to ppuc-pinmame for every game "
               "folder. With exactly one entry the game starts directly.\n");
        return 0;
      default:
        if (cag_option_get_error_index(&cagContext) >= 0)
        {
          fprintf(stderr, "Unknown command line option: ");
          cag_option_print_error(&cagContext, stderr);
          fprintf(stderr, "Usage: ppuc-menu [OPTION]...\n");
          cag_option_print(options, CAG_ARRAY_SIZE(options), stderr);
          return 1;
        }
        break;
    }
  }

  for (int i = cag_option_get_index(&cagContext); i < menuArgc; ++i)
  {
    const char* arg = argv[i];
    if (arg != nullptr && arg[0] == '-' && arg[1] != '\0')
    {
      fprintf(stderr, "Unknown command line option: %s\n", arg);
      fprintf(stderr, "Usage: ppuc-menu [OPTION]...\n");
      cag_option_print(options, CAG_ARRAY_SIZE(options), stderr);
      return 1;
    }
  }

  if (optMenuFile == nullptr && optGamesDir == nullptr)
  {
    fprintf(stderr,
            "Nothing to offer. Use --games-dir /path/to/games and/or "
            "-m /path/to/menu/file.\n");
    return 1;
  }

  std::string speechError;
  if (!ValidateSpeechAudioUsage(optNoSound, optSpeech, optGreeting, &speechError))
  {
    fprintf(stderr, "%s\n", speechError.c_str());
    return 1;
  }

  SpeechBackend speechBackend = SpeechBackend::Auto;
  SpeechOptions speechOptions;
  if (!ParseSpeechCliOptions(optSpeechBackend, optSpeechVoice, optSpeechRate,
                             optSpeechPitch, &speechBackend, &speechOptions,
                             &speechError))
  {
    fprintf(stderr, "%s\n", speechError.c_str());
    return 1;
  }

  // Menu file entries first, in the order written there, then the game
  // folders found in --games-dir.
  std::vector<MenuEntry> entries;
  std::string menuError;
  if (optMenuFile != nullptr &&
      !LoadMenuDefinition(optMenuFile, &entries, &menuError))
  {
    fprintf(stderr, "%s\n", menuError.c_str());
    return 1;
  }
  if (optGamesDir != nullptr)
  {
    std::vector<MenuEntry> gameEntries;
    if (!AddGamesDirectoryEntries(optGamesDir, &gameEntries, &menuError))
    {
      fprintf(stderr, "%s\n", menuError.c_str());
      return 1;
    }
    for (MenuEntry& entry : gameEntries)
    {
      entries.push_back(std::move(entry));
    }
  }

  for (MenuEntry& entry : entries)
  {
    if (entry.game.has_value())
    {
      entry.commandArgs.insert(entry.commandArgs.end(), gameArgs.begin(),
                               gameArgs.end());
      for (const std::string& arg : gameArgs)
      {
        entry.commandLine += " " + arg;
      }
    }
  }

  // The screen the game draws its translite, B2S backglass, tools and firmware
  // screen on: [Translite] of the game folders. Every game on one cabinet has
  // the same backbox, so each setting comes from the first folder that makes
  // it -- one folder carrying the cabinet's screen settings is enough. The
  // command line still wins.
  int iniWindowed = -1;
  for (const MenuEntry& entry : entries)
  {
    if (!entry.game.has_value())
    {
      continue;
    }
    const menugames::GameFolder& game = *entry.game;
    if (optWidth == 0 && game.transliteWidth > 0)
    {
      optWidth = game.transliteWidth;
    }
    if (optHeight == 0 && game.transliteHeight > 0)
    {
      optHeight = game.transliteHeight;
    }
    if (optScreen == -2 && game.transliteScreen >= 0)
    {
      optScreen = game.transliteScreen;
    }
    if (iniWindowed < 0)
    {
      iniWindowed = game.transliteWindow;
    }
  }
  optWindowed = optWindowed || iniWindowed == 1;
  if (optWidth == 0)
  {
    optWidth = 1920;
  }
  if (optHeight == 0)
  {
    optHeight = 1080;
  }
  if (optScreen == -2)
  {
    optScreen = -1;
  }
  if (optWidth <= 0 || optHeight <= 0)
  {
    fprintf(stderr, "--width and --height must be > 0\n");
    return 1;
  }

  // Nothing to choose between: start the game straight away, without a
  // window and without touching the boards. A machine with one game folder
  // boots exactly as if ppuc-pinmame had been started directly.
  if (entries.size() == 1 && optScreenshot == nullptr)
  {
    printf("Menu: one game, starting %s directly\n",
           entries.front().title.c_str());
    const fs::path menuDir = optMenuFile != nullptr
                                 ? fs::path(optMenuFile).parent_path()
                                 : fs::current_path();
    if (!LaunchSelectedEntry(entries.front(), menuDir, GetBaseExecutablePath(),
                             &menuError))
    {
      fprintf(stderr, "%s\n", menuError.c_str());
      return 1;
    }
    return 0;
  }

  const SDL_InitFlags initFlags = SDL_INIT_VIDEO |
                                  (optNoSound ? 0 : SDL_INIT_AUDIO);
  ConfigureSDLVideoDriverForHeadlessLinux();
  if (!SDL_Init(initFlags))
  {
    fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }

  std::unique_ptr<AudioOutput> audioOutput;
  std::unique_ptr<SpeechService> speechService;
  if (!optNoSound)
  {
    audioOutput = std::make_unique<AudioOutput>();
    if (!audioOutput->Initialize())
    {
      fprintf(stderr, "Audio output init failed: %s\n", SDL_GetError());
      audioOutput.reset();
      SDL_Quit();
      return 1;
    }
  }

  if (optSpeech || optGreeting)
  {
    if (!CreateConfiguredSpeechService(*audioOutput, speechBackend,
                                       speechOptions, &speechService,
                                       &speechError))
    {
      fprintf(stderr, "Speech init failed: %s\n", speechError.c_str());
      speechService.reset();
      audioOutput.reset();
      SDL_Quit();
      return 1;
    }
    PrintSpeechConfiguration(optSpeechBackend, speechOptions, optSpeechRate,
                             optSpeechPitch);
  }

  SDL_Window* window = nullptr;
  SDL_Renderer* renderer = nullptr;
  // The same window as ppuc-pinmame's translite, so whatever puts the
  // translite on the right panel puts the menu there too: fullscreen, or
  // borderless with Window=true; no high-DPI scaling, which the translite
  // does not ask for either.
  const SDL_WindowFlags windowFlags =
      optWindowed ? SDL_WINDOW_BORDERLESS : SDL_WINDOW_FULLSCREEN;
  // A cabinet has no mouse, and SDL's KMS/DRM backend draws a cursor by
  // default. Kept in a window, as ppuc-pinmame does.
  if (!optWindowed)
  {
    SDL_HideCursor();
  }
  if (!SDL_CreateWindowAndRenderer("PPUC Menu", optWidth, optHeight,
                                   windowFlags, &window, &renderer))
  {
    fprintf(stderr, "SDL couldn't create menu window/renderer: %s\n",
            SDL_GetError());
    speechService.reset();
    audioOutput.reset();
    SDL_Quit();
    return 1;
  }

  PositionWindowOnDisplay(window, optScreen);
  while (!SDL_SyncWindow(window))
  {
  }
  SDL_ShowWindow(window);
  SDL_RaiseWindow(window);
  {
    int outputWidth = 0;
    int outputHeight = 0;
    SDL_GetCurrentRenderOutputSize(renderer, &outputWidth, &outputHeight);
    printf("Menu: %s renderer, %dx%d on screen %d, %s, video driver %s\n",
           SDL_GetRendererName(renderer), outputWidth, outputHeight, optScreen,
           optWindowed ? "window" : "fullscreen", SDL_GetCurrentVideoDriver());
  }

  MenuAssets assets;
  if (!LoadMenuTextures(renderer, &entries, &assets, optLogo ? optLogo : "",
                        &menuError))
  {
    fprintf(stderr, "%s\n", menuError.c_str());
    DestroyMenuTextures(&entries);
    DestroyTexture(&assets.logoTexture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    speechService.reset();
    audioOutput.reset();
    SDL_Quit();
    return 1;
  }

  MenuFonts fonts;
  {
    int outputWidth = 0;
    int outputHeight = 0;
    SDL_GetCurrentRenderOutputSize(renderer, &outputWidth, &outputHeight);
    OpenMenuFonts(&fonts, outputHeight);
  }

  size_t selectedIndex = 0;

  if (optScreenshot != nullptr)
  {
    DrawMenu(renderer, fonts, entries, assets, selectedIndex,
             optSlogan ? optSlogan : "", !optNoSerial);
    const bool saved = SaveMenuScreenshot(renderer, optScreenshot);
    if (saved)
    {
      printf("Menu: screenshot written to %s\n", optScreenshot);
    }
    CloseMenuFonts(&fonts);
    DestroyMenuTextures(&entries);
    DestroyTexture(&assets.logoTexture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    speechService.reset();
    audioOutput.reset();
    SDL_Quit();
    return saved ? 0 : 1;
  }

  if (optGreeting && speechService != nullptr)
  {
    speechService->SpeakText("Welcome to the P P U C game menu.");
  }
  if (optSpeech && speechService != nullptr)
  {
    SpeakFocusTitle(speechService.get(), entries, selectedIndex);
  }

  const std::string slogan = optSlogan ? optSlogan : "";
  bool running = true;
  bool launchRequested = false;
  bool needsRender = true;

  // Shown before connecting: resetting and configuring the boards takes a
  // moment, and a black screen meanwhile looks like a machine that is broken.
  RenderMenu(renderer, fonts, entries, assets, selectedIndex, slogan, false);
  MenuHardware hardware;
  const bool playfieldButtons =
      !optNoSerial && ConnectMenuHardware(entries, optSerial, &hardware);

  const auto moveSelection = [&](int delta)
  {
    if ((delta < 0 && selectedIndex == 0) ||
        (delta > 0 && selectedIndex + 1 >= entries.size()))
    {
      return;
    }
    selectedIndex = delta < 0 ? selectedIndex - 1 : selectedIndex + 1;
    needsRender = true;
    if (optSpeech && speechService != nullptr)
    {
      SpeakFocusTitle(speechService.get(), entries, selectedIndex);
    }
  };

  while (running)
  {
    if (needsRender)
    {
      RenderMenu(renderer, fonts, entries, assets, selectedIndex, slogan,
                 playfieldButtons);
      needsRender = false;
    }

    if (hardware.ppuc != nullptr)
    {
      const bool accept = NowMs() >= hardware.acceptSwitchesFromMs;
      PPUCSwitchState* switchState = nullptr;
      while (running &&
             (switchState = hardware.ppuc->GetNextSwitchState()) != nullptr)
      {
        const uint32_t number = static_cast<uint32_t>(switchState->number);
        const bool pressed = switchState->state != 0;
        delete switchState;
        bool& wasClosed = hardware.closed[number];
        const bool edge = pressed && !wasClosed;
        wasClosed = pressed;
        if (!accept || !edge)
        {
          continue;
        }
        if (number == hardware.buttons.leftFlipper.number)
        {
          moveSelection(-1);
        }
        else if (number == hardware.buttons.rightFlipper.number)
        {
          moveSelection(1);
        }
        else if (number == hardware.buttons.start.number)
        {
          launchRequested = true;
          running = false;
        }
      }
      if (!running)
      {
        break;
      }
      if (needsRender)
      {
        continue;
      }
    }

    SDL_Event event;
    if (!SDL_WaitEventTimeout(&event, hardware.ppuc != nullptr ? 10 : 50))
    {
      continue;
    }

    switch (event.type)
    {
      case SDL_EVENT_QUIT:
        running = false;
        break;

      case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
      case SDL_EVENT_WINDOW_RESIZED:
      case SDL_EVENT_WINDOW_EXPOSED:
        needsRender = true;
        break;

      case SDL_EVENT_KEY_DOWN:
        if (event.key.repeat)
        {
          break;
        }

        switch (event.key.key)
        {
          case SDLK_ESCAPE:
            running = false;
            break;

          case SDLK_UP:
          case SDLK_LEFT:
            moveSelection(-1);
            break;

          case SDLK_DOWN:
          case SDLK_RIGHT:
            moveSelection(1);
            break;

          case SDLK_RETURN:
          case SDLK_RETURN2:
          case SDLK_KP_ENTER:
            launchRequested = true;
            running = false;
            break;

          default:
            break;
        }
        break;

      default:
        break;
    }
  }

  // Before anything starts: the game opens the same serial port.
  DisconnectMenuHardware(&hardware);

  const fs::path menuDir = optMenuFile != nullptr
                               ? fs::path(optMenuFile).parent_path()
                               : fs::current_path();
  const std::string basePath = GetBaseExecutablePath();

  CloseMenuFonts(&fonts);
  DestroyMenuTextures(&entries);
  DestroyTexture(&assets.logoTexture);
  if (renderer != nullptr)
  {
    SDL_DestroyRenderer(renderer);
  }
  if (window != nullptr)
  {
    SDL_DestroyWindow(window);
  }
  speechService.reset();
  audioOutput.reset();
  SDL_Quit();

  if (!launchRequested)
  {
    return 0;
  }

  printf("Menu: starting %s: %s\n", entries[selectedIndex].title.c_str(),
         entries[selectedIndex].commandLine.c_str());

  if (!LaunchSelectedEntry(entries[selectedIndex], menuDir, basePath,
                           &menuError))
  {
    fprintf(stderr, "%s\n", menuError.c_str());
    return 1;
  }

  return 0;
}
