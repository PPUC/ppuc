#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// What a machine keeps between power cycles that is not the ROM's NVRAM: the
// operator's adjustments, the switch report, the rules' machine memory.
//
// Kept in the game folder, under state/. On a cabinet that folder is on the USB
// stick zedmdos mounts writeable, so it travels with the game and survives a
// reflash of the SD card. Machines are switched off at the wall, never shut
// down, so every file here is written whole to a temporary name, flushed to the
// stick and only then renamed over the old one: a power cut leaves either the
// old file or the new one, never half of either.
//
// The format is meant to be read and, at a pinch, edited by a person:
//
//   # comment
//   ball_save_seconds = 6
//   free_play = true
//   language = "German"
//
// Depends on nothing but the standard library, so ppuc_tests links it.

struct StateValue
{
  enum class Type
  {
    Number,
    Boolean,
    String,
  };

  Type type = Type::Number;
  double number = 0.0;
  bool boolean = false;
  std::string text;

  static StateValue Number(double value)
  {
    StateValue v;
    v.type = Type::Number;
    v.number = value;
    return v;
  }
  static StateValue Boolean(bool value)
  {
    StateValue v;
    v.type = Type::Boolean;
    v.boolean = value;
    return v;
  }
  static StateValue String(std::string value)
  {
    StateValue v;
    v.type = Type::String;
    v.text = std::move(value);
    return v;
  }

  bool operator==(const StateValue& other) const;
  bool operator!=(const StateValue& other) const { return !(*this == other); }
};

using StateEntries = std::vector<std::pair<std::string, StateValue>>;

namespace MachineState
{

// A name is what a script or a person types: letters, digits, '_', '.', '-'.
bool IsValidName(const std::string& name);

// Lines that cannot be read are skipped and counted in `badLines`, if given:
// one mangled line must not cost the operator every other setting.
StateEntries Parse(const std::string& text, int* badLines = nullptr);

// `header` is written as comment lines above the entries.
std::string Format(const StateEntries& entries, const std::string& header = {});

// A number as it is written: integers without a decimal point.
std::string FormatNumber(double value);

// False, with `error` set, when the file does not exist or cannot be read.
bool ReadFile(const std::string& path, std::string* content, std::string* error);

// Creates the directory if needed, writes to `path`.tmp, flushes it to the
// device and renames it over `path`.
bool WriteFileAtomically(const std::string& path, const std::string& content, std::string* error);

// <gameDir>/state, or empty when there is no game folder.
std::string StateDirectory(const std::string& gameDir);

}  // namespace MachineState

// Values the rules keep between games and power cycles, beside what a ROM keeps
// in NVRAM: an auto-adjusted extra-ball level, a champion's record.
// ppuc.nvGet(name) / ppuc.nvSet(name, value) from Lua.
//
// Set from the rules' threads and only ever kept in memory there: writing a
// file on the switch thread would hold up every switch behind the USB stick.
// The main thread writes it out when TakeDirty() says so.
class MachineMemory
{
 public:
  // False with `error` for a bad name, a value that is not a number, boolean or
  // string, or a string longer than kMaxStringLength. Setting nil removes it.
  bool Set(const std::string& name, const StateValue& value, std::string* error);
  bool Remove(const std::string& name);
  bool Get(const std::string& name, StateValue* value) const;

  void Load(const StateEntries& entries);
  StateEntries Save() const;
  bool TakeDirty();

  static constexpr size_t kMaxStringLength = 1024;
  static constexpr size_t kMaxEntries = 1024;

 private:
  mutable std::mutex m_mutex;
  std::map<std::string, StateValue> m_values;
  bool m_dirty = false;
};
