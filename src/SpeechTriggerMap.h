#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

class SpeechTriggerMap
{
public:
  bool Load(const char* path, std::string& error);
  const std::string* Find(uint16_t id) const;
  size_t GetEntryCount() const;

private:
  std::unordered_map<uint16_t, std::string> m_entries;
};
