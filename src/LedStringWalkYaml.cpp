#include "LedStringWalkYaml.h"

#include <yaml-cpp/yaml.h>

#include <exception>

namespace
{

template <typename T>
T Read(const YAML::Node& node, const char* key, T fallback)
{
  const YAML::Node value = node[key];
  return value && value.IsScalar() ? value.as<T>() : fallback;
}

// An empty block is written as `{  }`, not as an empty list.
void ReadAssignments(const YAML::Node& stripe, const char* key, LedStringWalk::Kind kind,
                     std::vector<LedStringWalk::Assignment>* pAssignments)
{
  const YAML::Node items = stripe[key];
  if (!items || !items.IsSequence())
  {
    return;
  }
  for (const YAML::Node& item : items)
  {
    LedStringWalk::Assignment assignment;
    assignment.kind = kind;
    assignment.number = Read<int>(item, "number", 0);
    assignment.ledNumber = Read<int>(item, "ledNumber", -1);
    assignment.description = Read<std::string>(item, "description", "");
    const std::string color = Read<std::string>(item, "color", "0");
    try
    {
      assignment.color = static_cast<uint32_t>(std::stoul(color, nullptr, 16));
    }
    catch (const std::exception&)
    {
      assignment.color = 0;
    }
    pAssignments->push_back(std::move(assignment));
  }
}

}  // namespace

bool LoadLedStringsFromYaml(const std::string& path, std::vector<LedStringWalk::String>* pStrings, std::string* pError)
{
  pStrings->clear();
  try
  {
    const YAML::Node root = YAML::LoadFile(path);
    const YAML::Node stripes = root["ledStripes"];
    if (!stripes || !stripes.IsSequence())
    {
      return true;
    }
    for (const YAML::Node& stripe : stripes)
    {
      LedStringWalk::String string;
      string.board = Read<int>(stripe, "board", 0);
      string.port = Read<int>(stripe, "port", 0);
      string.amount = Read<int>(stripe, "amount", 0);
      string.description = Read<std::string>(stripe, "description", "");
      ReadAssignments(stripe, "lamps", LedStringWalk::Kind::Lamp, &string.assignments);
      ReadAssignments(stripe, "flashers", LedStringWalk::Kind::Flasher, &string.assignments);
      ReadAssignments(stripe, "gi", LedStringWalk::Kind::Gi, &string.assignments);
      pStrings->push_back(std::move(string));
    }
  }
  catch (const std::exception& ex)
  {
    pStrings->clear();
    if (pError) *pError = std::string("failed to read ledStripes from ") + path + ": " + ex.what();
    return false;
  }
  return true;
}
