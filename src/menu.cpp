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
#include <string>
#include <system_error>
#include <vector>

#include "AudioOutput.h"
#include "SDL3/SDL.h"
#include "SDL3/SDL_filesystem.h"
#include "SDL3_image/SDL_image.h"
#include "SpeechCliSupport.h"
#include "cargs.h"

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
      entry->imagePath.empty() && entry->selectedImagePath.empty())
  {
    return true;
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
  if (!TokenizeCommandLine(entry->commandLine, &entry->commandArgs,
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

std::vector<std::string> WrapText(const std::string& text, size_t maxChars)
{
  std::vector<std::string> lines;
  if (text.empty())
  {
    return lines;
  }

  std::string remaining = Trim(text);
  while (!remaining.empty())
  {
    if (remaining.size() <= maxChars)
    {
      lines.push_back(remaining);
      break;
    }

    size_t split = remaining.rfind(' ', maxChars);
    if (split == std::string::npos || split == 0)
    {
      split = maxChars;
    }

    lines.push_back(Trim(remaining.substr(0, split)));
    remaining = Trim(remaining.substr(split));
  }

  return lines;
}

float MeasureDebugTextWidth(const std::string& text, float scale)
{
  return static_cast<float>(text.size()) *
         static_cast<float>(SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE) * scale;
}

void RenderDebugTextLine(SDL_Renderer* renderer,
                         float x,
                         float y,
                         const std::string& text,
                         SDL_Color color,
                         float scale)
{
  if (renderer == nullptr || text.empty())
  {
    return;
  }

  SDL_SetRenderScale(renderer, scale, scale);
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
  SDL_RenderDebugText(renderer, x / scale, y / scale, text.c_str());
  SDL_SetRenderScale(renderer, 1.0f, 1.0f);
}

void RenderCenteredWrappedText(SDL_Renderer* renderer,
                               const std::string& text,
                               float centerX,
                               float y,
                               size_t maxCharsPerLine,
                               SDL_Color color,
                               float scale,
                               float lineSpacing)
{
  const std::vector<std::string> lines = WrapText(text, maxCharsPerLine);
  for (size_t i = 0; i < lines.size(); ++i)
  {
    const float width = MeasureDebugTextWidth(lines[i], scale);
    RenderDebugTextLine(renderer, centerX - (width / 2.0f),
                        y + static_cast<float>(i) * lineSpacing, lines[i],
                        color, scale);
  }
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

void RenderPlaceholder(SDL_Renderer* renderer, const SDL_FRect& bounds)
{
  SDL_SetRenderDrawColor(renderer, 80, 80, 80, 255);
  SDL_RenderRect(renderer, &bounds);
  RenderCenteredWrappedText(renderer, "NO IMAGE",
                            bounds.x + (bounds.w / 2.0f),
                            bounds.y + (bounds.h / 2.0f) - 10.0f, 10,
                            SDL_Color{220, 220, 220, 255}, 2.0f, 20.0f);
}

void RenderMenu(SDL_Renderer* renderer,
                const std::vector<MenuEntry>& entries,
                const MenuAssets& assets,
                size_t selectedIndex,
                const std::string& slogan)
{
  int renderWidth = 0;
  int renderHeight = 0;
  SDL_GetRenderOutputSize(renderer, &renderWidth, &renderHeight);

  const float screenWidth = static_cast<float>(renderWidth);
  const float screenHeight = static_cast<float>(renderHeight);

  SDL_SetRenderDrawColor(renderer, 9, 12, 18, 255);
  SDL_RenderClear(renderer);

  float currentY = 36.0f;
  if (assets.logoTexture.texture != nullptr)
  {
    const SDL_FRect logoBounds{
        screenWidth * 0.30f,
        currentY,
        screenWidth * 0.40f,
        std::min(screenHeight * 0.18f, 180.0f),
    };
    RenderTextureFitted(renderer, assets.logoTexture, logoBounds);
    currentY = logoBounds.y + logoBounds.h + 18.0f;
  }

  if (!slogan.empty())
  {
    RenderCenteredWrappedText(renderer, slogan, screenWidth / 2.0f, currentY,
                              48, SDL_Color{233, 197, 106, 255}, 2.0f, 20.0f);
    currentY += 56.0f;
  }

  const float sectionTop = currentY + 12.0f;
  const float sectionBottom = screenHeight - 36.0f;
  const float availableHeight = std::max(160.0f, sectionBottom - sectionTop);
  const float cardHeight =
      std::min(200.0f, std::max(150.0f, availableHeight / 3.4f));
  const float cardGap = 16.0f;
  size_t visibleCount =
      static_cast<size_t>(availableHeight / (cardHeight + cardGap));
  visibleCount =
      std::max<size_t>(1, std::min<size_t>(visibleCount, entries.size()));

  size_t startIndex = 0;
  if (selectedIndex >= visibleCount / 2)
  {
    startIndex = selectedIndex - (visibleCount / 2);
  }
  if (startIndex + visibleCount > entries.size())
  {
    startIndex = entries.size() - visibleCount;
  }

  const float cardX = screenWidth * 0.08f;
  const float cardWidth = screenWidth * 0.84f;

  for (size_t visibleSlot = 0; visibleSlot < visibleCount; ++visibleSlot)
  {
    const size_t entryIndex = startIndex + visibleSlot;
    const bool selected = entryIndex == selectedIndex;
    const float cardY = sectionTop + visibleSlot * (cardHeight + cardGap);
    const SDL_FRect cardRect{cardX, cardY, cardWidth, cardHeight};

    SDL_SetRenderDrawColor(renderer, selected ? 35 : 22, selected ? 48 : 28,
                           selected ? 70 : 38, 255);
    SDL_RenderFillRect(renderer, &cardRect);

    SDL_SetRenderDrawColor(renderer, selected ? 233 : 96,
                           selected ? 197 : 110,
                           selected ? 106 : 128, 255);
    SDL_RenderRect(renderer, &cardRect);

    const float imagePadding = 14.0f;
    const SDL_FRect imageRect{
        cardRect.x + imagePadding,
        cardRect.y + imagePadding,
        std::min(260.0f, cardRect.w * 0.28f),
        cardRect.h - (imagePadding * 2.0f),
    };

    const MenuTexture& texture = selected ? entries[entryIndex].selectedTexture
                                          : entries[entryIndex].normalTexture;
    if (texture.texture != nullptr)
    {
      RenderTextureFitted(renderer, texture, imageRect);
    }
    else
    {
      RenderPlaceholder(renderer, imageRect);
    }

    const float textX = imageRect.x + imageRect.w + 24.0f;
    const float textY = cardRect.y + 24.0f;
    const float textWidth = cardRect.x + cardRect.w - textX - 20.0f;
    const size_t maxChars = std::max<size_t>(
        10, static_cast<size_t>(
                textWidth / (SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * 2.0f)));

    const SDL_Color titleColor = selected ? SDL_Color{255, 240, 197, 255}
                                          : SDL_Color{232, 236, 243, 255};
    RenderCenteredWrappedText(renderer, entries[entryIndex].title,
                              textX + (textWidth / 2.0f), textY, maxChars,
                              titleColor, 2.0f, 22.0f);

    if (!entries[entryIndex].info.empty())
    {
      RenderCenteredWrappedText(renderer, entries[entryIndex].info,
                                textX + (textWidth / 2.0f), textY + 50.0f,
                                maxChars * 2,
                                SDL_Color{172, 181, 195, 255}, 1.0f, 14.0f);
    }

    RenderDebugTextLine(renderer, textX, cardRect.y + cardRect.h - 28.0f,
                        selected ? "ENTER: START   ESC: QUIT"
                                 : "ARROWS: MOVE",
                        SDL_Color{150, 160, 175, 255}, 1.0f);
  }

  SDL_RenderPresent(renderer);
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

void PositionWindowOnDisplay(SDL_Window* window, int displayIndex)
{
  if (window == nullptr || displayIndex < 0)
  {
    return;
  }

  int displayCount = 0;
  SDL_DisplayID* displays = SDL_GetDisplays(&displayCount);
  if (displays == nullptr || displayIndex >= displayCount)
  {
    return;
  }

  SDL_Rect bounds{};
  if (SDL_GetDisplayBounds(displays[displayIndex], &bounds))
  {
    SDL_SetWindowPosition(window, bounds.x, bounds.y);
  }
}

static struct cag_option options[] = {
    {.identifier = 'm',
     .access_letters = "m",
     .access_name = "menu-file",
     .value_name = "VALUE",
     .description = "Path to menu definition file (required)"},
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
     .description = "Run the selector in a window instead of fullscreen (optional)"},
    {.identifier = 'G',
     .access_name = "width",
     .value_name = "VALUE",
     .description = "Window width in pixels (optional, default 1920)"},
    {.identifier = 'H',
     .access_name = "height",
     .value_name = "VALUE",
     .description = "Window height in pixels (optional, default 1080)"},
    {.identifier = 'I',
     .access_name = "screen",
     .value_name = "VALUE",
     .description = "Display index for the menu window (optional)"},
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
    {.identifier = 'h',
     .access_letters = "h",
     .access_name = "help",
     .value_name = nullptr,
     .description = "Show this help"},
};
}  // namespace

int main(int argc, char* argv[])
{
  printf("PPUC Menu Version: %s\n", PPUC_EXECUTABLE_VERSION);
  printf("Commit SHA: %s\n", PPUC_EXECUTABLE_SHA);

  char identifier = 0;
  cag_option_context cagContext;
  const char* optMenuFile = nullptr;
  const char* optLogo = nullptr;
  const char* optSlogan = nullptr;
  bool optWindowed = false;
  int optWidth = 1920;
  int optHeight = 1080;
  int optScreen = -1;
  bool optNoSound = false;
  bool optSpeech = false;
  bool optGreeting = false;
  const char* optSpeechBackend = "auto";
  const char* optSpeechVoice = nullptr;
  const char* optSpeechRate = nullptr;
  const char* optSpeechPitch = nullptr;

  cag_option_init(&cagContext, options, CAG_ARRAY_SIZE(options), argc, argv);
  while (cag_option_fetch(&cagContext))
  {
    identifier = cag_option_get_identifier(&cagContext);
    switch (identifier)
    {
      case 'm':
        optMenuFile = cag_option_get_value(&cagContext);
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
        printf("Usage: ppuc-menu [OPTION]...\n");
        cag_option_print(options, CAG_ARRAY_SIZE(options), stdout);
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

  for (int i = cag_option_get_index(&cagContext); i < argc; ++i)
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

  if (optMenuFile == nullptr)
  {
    fprintf(stderr,
            "No menu definition provided. Use option -m /path/to/menu/file.\n");
    return 1;
  }

  if (optWidth <= 0 || optHeight <= 0)
  {
    fprintf(stderr, "--width and --height must be > 0\n");
    return 1;
  }

  std::string speechError;
  if (!ValidateSpeechAudioUsage(optNoSound, optSpeech, optGreeting, nullptr,
                                &speechError))
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

  std::vector<MenuEntry> entries;
  std::string menuError;
  if (!LoadMenuDefinition(optMenuFile, &entries, &menuError))
  {
    fprintf(stderr, "%s\n", menuError.c_str());
    return 1;
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
  const SDL_WindowFlags windowFlags =
      SDL_WINDOW_HIGH_PIXEL_DENSITY |
      (optWindowed ? 0 : SDL_WINDOW_FULLSCREEN);
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

  size_t selectedIndex = 0;
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

  while (running)
  {
    if (needsRender)
    {
      RenderMenu(renderer, entries, assets, selectedIndex, slogan);
      needsRender = false;
    }

    SDL_Event event;
    if (!SDL_WaitEventTimeout(&event, 50))
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
            if (selectedIndex > 0)
            {
              --selectedIndex;
              needsRender = true;
              if (optSpeech && speechService != nullptr)
              {
                SpeakFocusTitle(speechService.get(), entries, selectedIndex);
              }
            }
            break;

          case SDLK_DOWN:
          case SDLK_RIGHT:
            if (selectedIndex + 1 < entries.size())
            {
              ++selectedIndex;
              needsRender = true;
              if (optSpeech && speechService != nullptr)
              {
                SpeakFocusTitle(speechService.get(), entries, selectedIndex);
              }
            }
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

  const fs::path menuDir = fs::path(optMenuFile).parent_path();
  const std::string basePath = GetBaseExecutablePath();

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

  if (!LaunchSelectedEntry(entries[selectedIndex], menuDir, basePath,
                           &menuError))
  {
    fprintf(stderr, "%s\n", menuError.c_str());
    return 1;
  }

  return 0;
}
