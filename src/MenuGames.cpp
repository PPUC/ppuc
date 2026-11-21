#include "MenuGames.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <system_error>

namespace fs = std::filesystem;

namespace menugames
{
namespace
{

std::string Trim(const std::string& value)
{
  size_t start = 0;
  while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0)
  {
    ++start;
  }
  size_t end = value.size();
  while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0)
  {
    --end;
  }
  return value.substr(start, end - start);
}

std::string Lower(std::string value)
{
  for (char& c : value)
  {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return value;
}

// The words of a description, lower case. "Credit (Start) Button" is credit,
// start, button: matching words rather than substrings keeps "eos" out of
// "videos" and "start" out of "restart".
std::vector<std::string> Words(const std::string& text)
{
  std::vector<std::string> words;
  std::string current;
  for (const char c : text)
  {
    if (std::isalnum(static_cast<unsigned char>(c)) != 0)
    {
      current.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    else if (!current.empty())
    {
      words.push_back(std::move(current));
      current.clear();
    }
  }
  if (!current.empty())
  {
    words.push_back(std::move(current));
  }
  return words;
}

bool HasWord(const std::vector<std::string>& words, const char* word)
{
  return std::find(words.begin(), words.end(), word) != words.end();
}

bool IsEndOfStroke(const std::vector<std::string>& words)
{
  return HasWord(words, "eos") || (HasWord(words, "end") && HasWord(words, "stroke"));
}

bool IsUpper(const std::vector<std::string>& words) { return HasWord(words, "upper") || HasWord(words, "top"); }

bool IsFlipperButton(const SwitchInfo& sw, const std::vector<std::string>& words)
{
  if (IsEndOfStroke(words))
  {
    return false;
  }
  if (!HasWord(words, "flipper") && !HasWord(words, "flippers"))
  {
    return false;
  }
  return HasWord(words, "button") || sw.button || sw.drivesOutput;
}

std::string Describe(const SwitchInfo& sw)
{
  return "switch " + std::to_string(sw.number) + " '" + sw.description + "' (board " + std::to_string(sw.board) +
         ", port " + std::to_string(sw.port) + ")";
}

std::string DescribeAll(const std::vector<const SwitchInfo*>& candidates)
{
  std::string text;
  for (const SwitchInfo* sw : candidates)
  {
    text += (text.empty() ? "" : ", ") + Describe(*sw);
  }
  return text;
}

const SwitchInfo* FindByNumber(const std::vector<SwitchInfo>& switches, uint32_t number)
{
  for (const SwitchInfo& sw : switches)
  {
    if (sw.number == number)
    {
      return &sw;
    }
  }
  return nullptr;
}

const char* OverrideKey(ButtonRole role)
{
  switch (role)
  {
    case ButtonRole::LeftFlipper:
      return "LeftFlipperSwitch";
    case ButtonRole::RightFlipper:
      return "RightFlipperSwitch";
    case ButtonRole::Start:
      return "StartSwitch";
  }
  return "";
}

// Narrows candidates to one, or explains why not.
const SwitchInfo* PickOne(ButtonRole role, std::vector<const SwitchInfo*> candidates, std::string* pError)
{
  if (role != ButtonRole::Start && candidates.size() > 1)
  {
    // A machine with an upper flipper has two left (or right) flipper buttons
    // on the bus: the upper one mirrors the main one. The main one is the
    // button.
    std::vector<const SwitchInfo*> main;
    for (const SwitchInfo* sw : candidates)
    {
      if (!IsUpper(Words(sw->description)))
      {
        main.push_back(sw);
      }
    }
    if (!main.empty())
    {
      candidates = main;
    }
  }
  if (candidates.size() > 1)
  {
    // Prefer the one marked as a cabinet button.
    std::vector<const SwitchInfo*> buttons;
    for (const SwitchInfo* sw : candidates)
    {
      if (sw->button)
      {
        buttons.push_back(sw);
      }
    }
    if (buttons.size() == 1)
    {
      candidates = buttons;
    }
  }

  if (candidates.size() == 1)
  {
    return candidates.front();
  }

  if (pError != nullptr)
  {
    if (candidates.empty())
    {
      *pError = std::string("no ") + ButtonRoleName(role) + " found in io-boards.yaml; set [Menu] " +
                OverrideKey(role) + " in ppuc.ini";
    }
    else
    {
      *pError = std::string("more than one ") + ButtonRoleName(role) + " in io-boards.yaml (" +
                DescribeAll(candidates) + "); set [Menu] " + OverrideKey(role) + " in ppuc.ini";
    }
  }
  return nullptr;
}

const SwitchInfo* Resolve(ButtonRole role, const std::vector<SwitchInfo>& switches, uint32_t overrideNumber,
                          uint32_t hint, std::string* pError)
{
  if (overrideNumber != 0)
  {
    const SwitchInfo* sw = FindByNumber(switches, overrideNumber);
    if (sw == nullptr && pError != nullptr)
    {
      *pError = std::string("[Menu] ") + OverrideKey(role) + " = " + std::to_string(overrideNumber) +
                " names no switch in io-boards.yaml";
    }
    return sw;
  }
  if (hint != 0)
  {
    if (const SwitchInfo* sw = FindByNumber(switches, hint); sw != nullptr)
    {
      return sw;
    }
  }

  std::vector<const SwitchInfo*> candidates;
  for (const SwitchInfo& sw : switches)
  {
    const std::vector<std::string> words = Words(sw.description);
    switch (role)
    {
      case ButtonRole::LeftFlipper:
        if (IsFlipperButton(sw, words) && HasWord(words, "left") && !HasWord(words, "right"))
        {
          candidates.push_back(&sw);
        }
        break;
      case ButtonRole::RightFlipper:
        if (IsFlipperButton(sw, words) && HasWord(words, "right") && !HasWord(words, "left"))
        {
          candidates.push_back(&sw);
        }
        break;
      case ButtonRole::Start:
        if (HasWord(words, "start") && !IsEndOfStroke(words))
        {
          candidates.push_back(&sw);
        }
        break;
    }
  }
  return PickOne(role, std::move(candidates), pError);
}

}  // namespace

const char* ButtonRoleName(ButtonRole role)
{
  switch (role)
  {
    case ButtonRole::LeftFlipper:
      return "left flipper button";
    case ButtonRole::RightFlipper:
      return "right flipper button";
    case ButtonRole::Start:
      return "start button";
  }
  return "button";
}

bool ResolveMenuButtons(const std::vector<SwitchInfo>& switches, const ButtonOverrides& overrides,
                        MenuButtons* pButtons, std::string* pError)
{
  const SwitchInfo* left = Resolve(ButtonRole::LeftFlipper, switches, overrides.leftFlipper, 0, pError);
  if (left == nullptr)
  {
    return false;
  }
  const SwitchInfo* right = Resolve(ButtonRole::RightFlipper, switches, overrides.rightFlipper, 0, pError);
  if (right == nullptr)
  {
    return false;
  }
  const SwitchInfo* start = Resolve(ButtonRole::Start, switches, overrides.start, overrides.emGameStart, pError);
  if (start == nullptr)
  {
    return false;
  }

  if (SameInput(*left, *right) || SameInput(*left, *start) || SameInput(*right, *start))
  {
    if (pError != nullptr)
    {
      *pError = "the menu buttons are not three different inputs: left " + Describe(*left) + ", right " +
                Describe(*right) + ", start " + Describe(*start);
    }
    return false;
  }

  if (pButtons != nullptr)
  {
    pButtons->leftFlipper = *left;
    pButtons->rightFlipper = *right;
    pButtons->start = *start;
  }
  return true;
}

bool SameInput(const SwitchInfo& a, const SwitchInfo& b)
{
  return a.board == b.board && a.port == b.port && a.matrix == b.matrix;
}

bool ParseIni(const fs::path& path, IniFile* pIni, std::string* pError)
{
  std::ifstream file(path);
  if (!file.is_open())
  {
    if (pError != nullptr)
    {
      *pError = "unable to open " + path.string();
    }
    return false;
  }

  IniFile ini;
  std::string section;
  std::string line;
  while (std::getline(file, line))
  {
    const std::string trimmed = Trim(line);
    if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';')
    {
      continue;
    }
    if (trimmed.front() == '[' && trimmed.back() == ']')
    {
      section = Trim(trimmed.substr(1, trimmed.size() - 2));
      continue;
    }
    const size_t equals = trimmed.find('=');
    if (equals == std::string::npos)
    {
      continue;
    }
    const std::string key = Trim(trimmed.substr(0, equals));
    const std::string value = Trim(trimmed.substr(equals + 1));
    if (!value.empty())
    {
      ini[section][key] = value;
    }
  }

  if (pIni != nullptr)
  {
    *pIni = std::move(ini);
  }
  return true;
}

std::string IniValue(const IniFile& ini, const std::string& section, const std::string& key,
                     const std::string& fallback)
{
  const auto sectionIt = ini.find(section);
  if (sectionIt == ini.end())
  {
    return fallback;
  }
  const auto keyIt = sectionIt->second.find(key);
  return keyIt == sectionIt->second.end() ? fallback : keyIt->second;
}

uint32_t IniUnsigned(const IniFile& ini, const std::string& section, const std::string& key, uint32_t fallback)
{
  const std::string value = IniValue(ini, section, key);
  if (value.empty())
  {
    return fallback;
  }
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
  return end != nullptr && *end == '\0' ? static_cast<uint32_t>(parsed) : fallback;
}

bool IniBool(const IniFile& ini, const std::string& section, const std::string& key, bool fallback)
{
  const std::string value = Lower(IniValue(ini, section, key));
  if (value == "1" || value == "true" || value == "yes" || value == "on")
  {
    return true;
  }
  if (value == "0" || value == "false" || value == "no" || value == "off")
  {
    return false;
  }
  return fallback;
}

namespace
{

bool IsFile(const fs::path& path)
{
  std::error_code ec;
  return fs::is_regular_file(path, ec);
}

// The first of <folder>/<stem>.<ext> that exists, for each stem in turn.
fs::path FindImage(const fs::path& folder, std::initializer_list<const char*> stems)
{
  static const char* const kExtensions[] = {".png", ".jpg", ".jpeg", ".webp", ".bmp"};
  for (const char* stem : stems)
  {
    for (const char* extension : kExtensions)
    {
      const fs::path candidate = folder / (std::string(stem) + extension);
      if (IsFile(candidate))
      {
        return candidate;
      }
    }
  }
  return fs::path();
}

fs::path ResolveInFolder(const fs::path& folder, const std::string& value)
{
  const fs::path path(value);
  return path.is_absolute() ? path : (folder / path).lexically_normal();
}

std::string DefaultInfo(const IniFile& ini)
{
  const std::string engine = Lower(IniValue(ini, "Game", "Engine"));
  const std::string rom = IniValue(ini, "Game", "Rom", IniValue(ini, "Paths", "Rom"));
  if (engine == "script")
  {
    return "Lua game";
  }
  return rom.empty() ? std::string() : "ROM " + rom;
}

}  // namespace

bool ReadGameFolder(const fs::path& folder, GameFolder* pGame, std::string* pError)
{
  GameFolder game;
  std::error_code ec;
  game.folder = fs::absolute(folder, ec).lexically_normal();
  if (ec)
  {
    game.folder = folder.lexically_normal();
  }
  // "games/tmwrp/" has an empty filename; its parent path is the folder.
  if (game.folder.filename().empty())
  {
    game.folder = game.folder.parent_path();
  }

  IniFile ini;
  const fs::path iniPath = game.folder / "ppuc.ini";
  if (IsFile(iniPath) && !ParseIni(iniPath, &ini, pError))
  {
    return false;
  }

  if (IniBool(ini, "Menu", "Hidden", false))
  {
    if (pError != nullptr)
    {
      *pError = game.folder.string() + ": hidden by [Menu] Hidden";
    }
    return false;
  }

  const std::string configFile = IniValue(ini, "Paths", "ConfigFile");
  game.configFile = configFile.empty() ? game.folder / "io-boards.yaml" : ResolveInFolder(game.folder, configFile);
  if (!IsFile(game.configFile))
  {
    if (pError != nullptr)
    {
      *pError = game.folder.string() + ": no game configuration (" + game.configFile.string() + ")";
    }
    return false;
  }

  game.title = IniValue(ini, "Menu", "Title", game.folder.filename().string());
  game.info = IniValue(ini, "Menu", "Info", DefaultInfo(ini));
  game.arguments = IniValue(ini, "Menu", "Arguments");
  game.order = std::atoi(IniValue(ini, "Menu", "Order", "0").c_str());

  const std::string image = IniValue(ini, "Menu", "Image");
  const std::string selectedImage = IniValue(ini, "Menu", "SelectedImage");
  game.image = image.empty() ? FindImage(game.folder, {"translite-off", "translite-attract"})
                             : ResolveInFolder(game.folder, image);
  game.selectedImage = selectedImage.empty() ? FindImage(game.folder, {"translite-on", "translite"})
                                             : ResolveInFolder(game.folder, selectedImage);

  game.buttons.leftFlipper = IniUnsigned(ini, "Menu", "LeftFlipperSwitch");
  game.buttons.rightFlipper = IniUnsigned(ini, "Menu", "RightFlipperSwitch");
  game.buttons.start = IniUnsigned(ini, "Menu", "StartSwitch");

  game.serial = IniValue(ini, "Paths", "Serial");
  game.skipBoards = IniValue(ini, "Runtime", "SkipBoards");
  game.switchReplyDelayUs = IniUnsigned(ini, "Runtime", "SwitchReplyDelayUs");
  game.transliteWidth = static_cast<int>(IniUnsigned(ini, "Translite", "Width"));
  game.transliteHeight = static_cast<int>(IniUnsigned(ini, "Translite", "Height"));
  game.transliteScreen = std::atoi(IniValue(ini, "Translite", "Screen", "-1").c_str());
  if (const std::string window = IniValue(ini, "Translite", "Window"); !window.empty())
  {
    game.transliteWindow = IniBool(ini, "Translite", "Window", false) ? 1 : 0;
  }

  if (pGame != nullptr)
  {
    *pGame = std::move(game);
  }
  return true;
}

std::vector<GameFolder> ScanGamesDirectory(const fs::path& gamesDir, std::vector<std::string>* pSkipped)
{
  std::vector<GameFolder> games;
  std::error_code ec;
  for (fs::directory_iterator it(gamesDir, fs::directory_options::follow_directory_symlink, ec), end; !ec && it != end;
       it.increment(ec))
  {
    std::error_code typeError;
    if (!it->is_directory(typeError))
    {
      continue;
    }
    const std::string name = it->path().filename().string();
    if (name.empty() || name[0] == '.')
    {
      continue;
    }

    GameFolder game;
    std::string error;
    if (ReadGameFolder(it->path(), &game, &error))
    {
      games.push_back(std::move(game));
    }
    else if (pSkipped != nullptr)
    {
      pSkipped->push_back(error);
    }
  }
  if (ec && pSkipped != nullptr)
  {
    pSkipped->push_back("unable to read " + gamesDir.string() + ": " + ec.message());
  }

  std::sort(games.begin(), games.end(),
            [](const GameFolder& a, const GameFolder& b)
            {
              if (a.order != b.order)
              {
                return a.order < b.order;
              }
              const std::string titleA = Lower(a.title);
              const std::string titleB = Lower(b.title);
              if (titleA != titleB)
              {
                return titleA < titleB;
              }
              return a.folder < b.folder;
            });
  return games;
}

}  // namespace menugames
