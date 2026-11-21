#include "MenuConfigYaml.h"

#include <yaml-cpp/yaml.h>

#include <exception>
#include <fstream>
#include <set>

using menugames::MenuButtons;
using menugames::SwitchInfo;

namespace
{

template <typename T>
T Read(const YAML::Node& node, const char* key, T fallback)
{
  const YAML::Node value = node[key];
  return value && value.IsScalar() ? value.as<T>() : fallback;
}

void ReadSwitches(const YAML::Node& items, bool matrix, std::vector<SwitchInfo>* pSwitches)
{
  if (!items || !items.IsSequence())
  {
    return;
  }
  for (const YAML::Node& item : items)
  {
    SwitchInfo sw;
    sw.number = Read<uint32_t>(item, "number", 0);
    sw.board = Read<uint32_t>(item, "board", 0);
    sw.port = Read<uint32_t>(item, "port", 0);
    sw.debounce = Read<uint32_t>(item, "debounce", 0);
    sw.description = Read<std::string>(item, "description", "");
    sw.button = Read<bool>(item, "button", false);
    sw.matrix = matrix;
    pSwitches->push_back(std::move(sw));
  }
}

bool IsButton(const SwitchInfo& sw, const MenuButtons& buttons)
{
  for (const SwitchInfo* button : {&buttons.leftFlipper, &buttons.rightFlipper, &buttons.start})
  {
    if (button->number == sw.number && button->matrix == sw.matrix)
    {
      return true;
    }
  }
  return false;
}

// The source item, reduced to what a switch input needs. Its debounce stays as
// configured; fastFlip debouncing does not, because no flipper is configured
// for it to serve.
YAML::Node MenuSwitchNode(const YAML::Node& source)
{
  YAML::Node node;
  for (const char* key : {"description", "number", "board", "port", "debounce"})
  {
    if (source[key])
    {
      node[key] = source[key];
    }
  }
  if (source["debounce"])
  {
    node["debounceMode"] = "standard";
  }
  node["button"] = true;
  return node;
}

}  // namespace

bool LoadMenuSwitches(const std::string& configPath, std::vector<SwitchInfo>* pSwitches, uint32_t* pEmGameStart,
                      std::string* pError)
{
  pSwitches->clear();
  if (pEmGameStart != nullptr)
  {
    *pEmGameStart = 0;
  }
  try
  {
    const YAML::Node root = YAML::LoadFile(configPath);
    ReadSwitches(root["switches"], false, pSwitches);
    if (const YAML::Node matrix = root["switchMatrix"]; matrix && matrix.IsMap())
    {
      ReadSwitches(matrix["switches"], true, pSwitches);
    }

    std::set<uint32_t> fastFlipSwitches;
    if (const YAML::Node outputs = root["pwmOutput"]; outputs && outputs.IsSequence())
    {
      for (const YAML::Node& output : outputs)
      {
        if (const uint32_t number = Read<uint32_t>(output, "fastFlipSwitch", 0); number != 0)
        {
          fastFlipSwitches.insert(number);
        }
      }
    }
    for (SwitchInfo& sw : *pSwitches)
    {
      sw.drivesOutput = fastFlipSwitches.count(sw.number) != 0;
    }

    if (const YAML::Node emGame = root["emGame"]; emGame && emGame.IsMap() && pEmGameStart != nullptr)
    {
      *pEmGameStart = Read<uint32_t>(emGame, "startSwitch", 0);
    }
  }
  catch (const std::exception& ex)
  {
    pSwitches->clear();
    if (pError != nullptr)
    {
      *pError = "failed to read switches from " + configPath + ": " + ex.what();
    }
    return false;
  }
  return true;
}

bool WriteMenuConfig(const std::string& sourcePath, const std::string& targetPath, const MenuButtons& buttons,
                     std::string* pError)
{
  try
  {
    const YAML::Node source = YAML::LoadFile(sourcePath);

    YAML::Node root;
    if (source["ppucVersion"])
    {
      root["ppucVersion"] = source["ppucVersion"];
    }
    // libppuc requires a ROM name; nothing loads one.
    root["rom"] = "ppuc-menu";
    root["serialPort"] = Read<std::string>(source, "serialPort", "dummy");
    root["platform"] = Read<std::string>(source, "platform", "WPC");
    // No coin door switch is configured; zero is "no coin door".
    root["coinDoorClosedSwitch"] = 0;
    // Kept from the game: no output is configured to switch it, so where it
    // gates high power, high power stays off for as long as the menu runs.
    root["gameOnSolenoid"] = Read<uint32_t>(source, "gameOnSolenoid", 0);
    root["debug"] = false;

    std::set<uint32_t> boardNumbers;
    for (const SwitchInfo* button : {&buttons.leftFlipper, &buttons.rightFlipper, &buttons.start})
    {
      boardNumbers.insert(button->board);
    }
    const YAML::Node sourceMatrix = source["switchMatrix"];
    const bool usesMatrix = buttons.leftFlipper.matrix || buttons.rightFlipper.matrix || buttons.start.matrix;
    if (usesMatrix && sourceMatrix && sourceMatrix.IsMap())
    {
      boardNumbers.insert(Read<uint32_t>(sourceMatrix, "board", 0));
    }

    YAML::Node boards(YAML::NodeType::Sequence);
    for (const YAML::Node& board : source["boards"])
    {
      const uint32_t number = Read<uint32_t>(board, "number", 0);
      if (boardNumbers.erase(number) == 0)
      {
        continue;
      }
      YAML::Node copy;
      for (const char* key : {"description", "number", "type", "slowSwitches", "firstInChain", "virtual"})
      {
        if (board[key])
        {
          copy[key] = board[key];
        }
      }
      copy["pollEvents"] = true;
      boards.push_back(copy);
    }
    if (!boardNumbers.empty())
    {
      if (pError != nullptr)
      {
        *pError = sourcePath + ": a menu button sits on board " + std::to_string(*boardNumbers.begin()) +
                  ", which is not listed under boards";
      }
      return false;
    }
    root["boards"] = boards;

    YAML::Node switches(YAML::NodeType::Sequence);
    for (const YAML::Node& item : source["switches"])
    {
      SwitchInfo sw;
      sw.number = Read<uint32_t>(item, "number", 0);
      if (IsButton(sw, buttons))
      {
        switches.push_back(MenuSwitchNode(item));
      }
    }
    if (switches.size() > 0)
    {
      root["switches"] = switches;
    }

    if (usesMatrix && sourceMatrix && sourceMatrix.IsMap())
    {
      YAML::Node matrix;
      for (const char* key : {"board", "activeLow", "rows"})
      {
        if (sourceMatrix[key])
        {
          matrix[key] = sourceMatrix[key];
        }
      }
      YAML::Node matrixSwitches(YAML::NodeType::Sequence);
      for (const YAML::Node& item : sourceMatrix["switches"])
      {
        SwitchInfo sw;
        sw.number = Read<uint32_t>(item, "number", 0);
        sw.matrix = true;
        if (IsButton(sw, buttons))
        {
          YAML::Node node;
          for (const char* key : {"description", "number", "board", "port"})
          {
            if (item[key])
            {
              node[key] = item[key];
            }
          }
          node["button"] = true;
          matrixSwitches.push_back(node);
        }
      }
      matrix["switches"] = matrixSwitches;
      root["switchMatrix"] = matrix;
    }

    YAML::Emitter out;
    out << YAML::Comment("Written by ppuc-menu from " + sourcePath +
                         ": the menu buttons and nothing else. Regenerated at every start.");
    out << YAML::Newline << root;
    std::ofstream file(targetPath, std::ios::trunc);
    file << out.c_str() << "\n";
    if (!file)
    {
      if (pError != nullptr)
      {
        *pError = "failed to write " + targetPath;
      }
      return false;
    }
  }
  catch (const std::exception& ex)
  {
    if (pError != nullptr)
    {
      *pError = std::string("failed to write the menu configuration: ") + ex.what();
    }
    return false;
  }
  return true;
}
