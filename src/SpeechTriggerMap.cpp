#include "SpeechTriggerMap.h"

#include <fstream>
#include <sstream>

namespace
{
std::string Trim(const std::string& input)
{
  const size_t first = input.find_first_not_of(" \t\r\n");
  if (first == std::string::npos)
  {
    return {};
  }

  const size_t last = input.find_last_not_of(" \t\r\n");
  return input.substr(first, last - first + 1);
}

bool ParseUInt16(const std::string& token, uint16_t& value)
{
  try
  {
    const unsigned long parsed = std::stoul(token, nullptr, 10);
    if (parsed > 0xFFFFul)
    {
      return false;
    }
    value = static_cast<uint16_t>(parsed);
    return true;
  }
  catch (...)
  {
    return false;
  }
}
}  // namespace

bool SpeechTriggerMap::Load(const char* path, std::string& error)
{
  std::ifstream input(path);
  if (!input.is_open())
  {
    error = "Unable to open speech trigger file";
    return false;
  }

  std::unordered_map<uint16_t, std::string> loaded;
  std::string line;
  size_t lineNo = 0;
  while (std::getline(input, line))
  {
    lineNo++;
    const size_t comment = line.find('#');
    if (comment != std::string::npos)
    {
      line = line.substr(0, comment);
    }

    line = Trim(line);
    if (line.empty())
    {
      continue;
    }

    const size_t separator = line.find(':');
    if (separator == std::string::npos)
    {
      error = "Invalid speech trigger line " + std::to_string(lineNo) +
              ": missing ':' separator.";
      return false;
    }

    const std::string idPart = Trim(line.substr(0, separator));
    const std::string textPart = Trim(line.substr(separator + 1));
    if (idPart.empty() || textPart.empty())
    {
      error = "Invalid speech trigger line " + std::to_string(lineNo) +
              ": missing id or text.";
      return false;
    }

    uint16_t id = 0;
    if (!ParseUInt16(idPart, id))
    {
      error = "Invalid speech trigger line " + std::to_string(lineNo) +
              ": id must be uint16.";
      return false;
    }

    loaded[id] = textPart;
  }

  m_entries = std::move(loaded);
  return true;
}

const std::string* SpeechTriggerMap::Find(uint16_t id) const
{
  const auto it = m_entries.find(id);
  return it == m_entries.end() ? nullptr : &it->second;
}

size_t SpeechTriggerMap::GetEntryCount() const
{
  return m_entries.size();
}
