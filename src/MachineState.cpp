#include "MachineState.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

bool StateValue::operator==(const StateValue& other) const
{
  if (type != other.type) return false;
  switch (type)
  {
    case Type::Number:
      return number == other.number;
    case Type::Boolean:
      return boolean == other.boolean;
    case Type::String:
      return text == other.text;
  }
  return false;
}

namespace MachineState
{

bool IsValidName(const std::string& name)
{
  if (name.empty() || name.size() > 64) return false;
  for (const char c : name)
  {
    const bool ok =
        (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
    if (!ok) return false;
  }
  return true;
}

static std::string Trim(const std::string& s)
{
  size_t begin = 0;
  size_t end = s.size();
  while (begin < end && (s[begin] == ' ' || s[begin] == '\t' || s[begin] == '\r')) ++begin;
  while (end > begin && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r')) --end;
  return s.substr(begin, end - begin);
}

static bool ParseQuoted(const std::string& raw, std::string* out)
{
  if (raw.size() < 2 || raw.front() != '"' || raw.back() != '"') return false;
  std::string text;
  for (size_t i = 1; i + 1 < raw.size(); ++i)
  {
    char c = raw[i];
    if (c == '\\')
    {
      if (i + 2 >= raw.size()) return false;
      const char next = raw[++i];
      switch (next)
      {
        case 'n':
          c = '\n';
          break;
        case 't':
          c = '\t';
          break;
        case '"':
        case '\\':
          c = next;
          break;
        default:
          return false;
      }
    }
    else if (c == '"')
    {
      return false;
    }
    text.push_back(c);
  }
  *out = std::move(text);
  return true;
}

static bool ParseValue(const std::string& raw, StateValue* value)
{
  if (raw.empty()) return false;
  if (raw == "true" || raw == "false")
  {
    *value = StateValue::Boolean(raw == "true");
    return true;
  }
  if (raw.front() == '"')
  {
    std::string text;
    if (!ParseQuoted(raw, &text)) return false;
    *value = StateValue::String(std::move(text));
    return true;
  }
  char* end = nullptr;
  errno = 0;
  const double number = std::strtod(raw.c_str(), &end);
  if (errno != 0 || end == raw.c_str() || *end != '\0' || !std::isfinite(number)) return false;
  *value = StateValue::Number(number);
  return true;
}

StateEntries Parse(const std::string& text, int* badLines)
{
  StateEntries entries;
  int bad = 0;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line))
  {
    const std::string trimmed = Trim(line);
    if (trimmed.empty() || trimmed[0] == '#') continue;
    const size_t eq = trimmed.find('=');
    if (eq == std::string::npos)
    {
      ++bad;
      continue;
    }
    const std::string name = Trim(trimmed.substr(0, eq));
    StateValue value;
    if (!IsValidName(name) || !ParseValue(Trim(trimmed.substr(eq + 1)), &value))
    {
      ++bad;
      continue;
    }
    // The last one wins, as a person editing the file would expect.
    bool replaced = false;
    for (auto& entry : entries)
    {
      if (entry.first == name)
      {
        entry.second = value;
        replaced = true;
        break;
      }
    }
    if (!replaced) entries.emplace_back(name, std::move(value));
  }
  if (badLines != nullptr) *badLines = bad;
  return entries;
}

std::string FormatNumber(double value)
{
  if (std::floor(value) == value && std::fabs(value) < 9.0e15)
  {
    return std::to_string(static_cast<long long>(value));
  }
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.17g", value);
  return buffer;
}

static std::string Quote(const std::string& text)
{
  std::string out = "\"";
  for (const char c : text)
  {
    switch (c)
    {
      case '\n':
        out += "\\n";
        break;
      case '\t':
        out += "\\t";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      default:
        out.push_back(c);
    }
  }
  out.push_back('"');
  return out;
}

std::string Format(const StateEntries& entries, const std::string& header)
{
  std::string out;
  if (!header.empty())
  {
    std::istringstream in(header);
    std::string line;
    while (std::getline(in, line))
    {
      out += line.empty() ? "#\n" : "# " + line + "\n";
    }
  }
  for (const auto& [name, value] : entries)
  {
    out += name;
    out += " = ";
    switch (value.type)
    {
      case StateValue::Type::Number:
        out += FormatNumber(value.number);
        break;
      case StateValue::Type::Boolean:
        out += value.boolean ? "true" : "false";
        break;
      case StateValue::Type::String:
        out += Quote(value.text);
        break;
    }
    out += "\n";
  }
  return out;
}

bool ReadFile(const std::string& path, std::string* content, std::string* error)
{
  std::ifstream in(path, std::ios::binary);
  if (!in)
  {
    if (error) *error = "cannot open " + path;
    return false;
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  if (in.bad())
  {
    if (error) *error = "cannot read " + path;
    return false;
  }
  *content = buffer.str();
  return true;
}

bool WriteFileAtomically(const std::string& path, const std::string& content, std::string* error)
{
  std::error_code ec;
  const std::filesystem::path target(path);
  if (target.has_parent_path())
  {
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec)
    {
      if (error) *error = "cannot create " + target.parent_path().string() + ": " + ec.message();
      return false;
    }
  }

  const std::string tmp = path + ".tmp";
#ifndef _WIN32
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
  {
    if (error) *error = "cannot write " + tmp + ": " + std::strerror(errno);
    return false;
  }
  size_t written = 0;
  while (written < content.size())
  {
    const ssize_t n = ::write(fd, content.data() + written, content.size() - written);
    if (n < 0)
    {
      if (errno == EINTR) continue;
      if (error) *error = "cannot write " + tmp + ": " + std::strerror(errno);
      ::close(fd);
      return false;
    }
    written += static_cast<size_t>(n);
  }
  // The data on the stick before the name points at it.
  if (::fsync(fd) != 0)
  {
    if (error) *error = "cannot flush " + tmp + ": " + std::strerror(errno);
    ::close(fd);
    return false;
  }
  ::close(fd);
#else
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out << content;
    out.flush();
    if (!out)
    {
      if (error) *error = "cannot write " + tmp;
      return false;
    }
  }
#endif

  std::filesystem::rename(tmp, target, ec);
  if (ec)
  {
    if (error) *error = "cannot rename " + tmp + ": " + ec.message();
    return false;
  }

#ifndef _WIN32
  // And the rename itself. On FAT, which a USB stick usually is, this is what
  // writes the directory entry.
  if (target.has_parent_path())
  {
    const int dirFd = ::open(target.parent_path().c_str(), O_RDONLY);
    if (dirFd >= 0)
    {
      ::fsync(dirFd);
      ::close(dirFd);
    }
  }
#endif
  return true;
}

std::string StateDirectory(const std::string& gameDir)
{
  if (gameDir.empty()) return {};
  return (std::filesystem::path(gameDir) / "state").string();
}

}  // namespace MachineState

bool MachineMemory::Set(const std::string& name, const StateValue& value, std::string* error)
{
  if (!MachineState::IsValidName(name))
  {
    if (error) *error = "'" + name + "' is not a valid name (letters, digits, '_', '.', '-')";
    return false;
  }
  if (value.type == StateValue::Type::String && value.text.size() > kMaxStringLength)
  {
    if (error) *error = "the value of '" + name + "' is longer than " + std::to_string(kMaxStringLength);
    return false;
  }
  if (value.type == StateValue::Type::Number && !std::isfinite(value.number))
  {
    if (error) *error = "the value of '" + name + "' is not a finite number";
    return false;
  }
  std::lock_guard<std::mutex> lock(m_mutex);
  auto it = m_values.find(name);
  if (it == m_values.end())
  {
    if (m_values.size() >= kMaxEntries)
    {
      if (error) *error = "machine memory is full (" + std::to_string(kMaxEntries) + " values)";
      return false;
    }
    m_values.emplace(name, value);
    m_dirty = true;
  }
  else if (it->second != value)
  {
    it->second = value;
    m_dirty = true;
  }
  return true;
}

bool MachineMemory::Remove(const std::string& name)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_values.erase(name) == 0) return false;
  m_dirty = true;
  return true;
}

bool MachineMemory::Get(const std::string& name, StateValue* value) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  auto it = m_values.find(name);
  if (it == m_values.end()) return false;
  *value = it->second;
  return true;
}

void MachineMemory::Load(const StateEntries& entries)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  for (const auto& [name, value] : entries)
  {
    if (m_values.size() >= kMaxEntries) break;
    m_values[name] = value;
  }
}

StateEntries MachineMemory::Save() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return StateEntries(m_values.begin(), m_values.end());
}

bool MachineMemory::TakeDirty()
{
  std::lock_guard<std::mutex> lock(m_mutex);
  const bool dirty = m_dirty;
  m_dirty = false;
  return dirty;
}
