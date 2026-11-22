#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "MachineState.h"

// The operator's adjustments: values somebody who owns a machine may change at
// the machine, through the coin door buttons, and that are kept on the USB stick
// until changed again.
//
// Two kinds share one list:
//
// - Machine adjustments, defined by PPUC itself for what PPUC does whatever the
//   game: ball save and tilt warnings (both engines), the ROM-less game core's
//   balls per game and free play, the switch report. Group "Machine".
// - Game adjustments, defined by the game's Lua rules with
//   ppuc.defineAdjustment{...}: a mode's difficulty, a device marked broken.
//   Group "Game". A rule that defines one reads it back with
//   ppuc.adjustment(name) and hears changes through ppuc.onAdjustmentChanged.
//
// A value is stored by name, not by position, so a game that adds, removes or
// reorders its adjustments keeps everything the operator already set. A stored
// value whose adjustment is not (yet) defined is kept as it is: the rules define
// theirs after the file is read, and a rule that is commented out for a day must
// not cost the operator its setting.
//
// Thread-safe: the rules define and read adjustments on their threads, the
// tools change them on the main thread.
class Adjustments
{
 public:
  enum class Kind
  {
    Number,
    Boolean,
    Choice,
  };

  struct Definition
  {
    std::string name;
    std::string label;
    std::string group;
    std::string about;
    Kind kind = Kind::Number;
    // Number
    double min = 0.0;
    double max = 100.0;
    double step = 1.0;
    std::string unit;
    // Choice
    std::vector<std::string> choices;
    StateValue defaultValue;
  };

  struct Entry
  {
    Definition definition;
    StateValue value;
  };

  // A definition that is not valid is refused with a reason. Defining the same
  // name again replaces the definition (the rules being loaded again) and keeps
  // the value if it still fits; otherwise the value goes back to the default.
  bool Define(Definition definition, std::string* error);

  bool Has(const std::string& name) const;
  // The default when not defined: callers check Has() first.
  StateValue Get(const std::string& name) const;
  double GetNumber(const std::string& name, double fallback) const;
  bool GetBoolean(const std::string& name, bool fallback) const;

  // Clamped to the range, rounded to the step, refused when it does not fit at
  // all (a string for a number, a choice that is not offered). `applied` is the
  // value actually stored.
  bool Set(const std::string& name, const StateValue& value, StateValue* applied, std::string* error);

  // The next value up (direction > 0) or down: a step for a number, the
  // neighbour for a choice, the other one for a boolean. Numbers stop at their
  // ends; choices and booleans go round.
  StateValue Stepped(const std::string& name, int direction) const;
  // The same from `from`: a value being edited that is not stored yet.
  StateValue Stepped(const std::string& name, const StateValue& from, int direction) const;

  bool ResetToDefault(const std::string& name);

  // For a screen: "6 s", "Yes", "Easy".
  std::string Display(const std::string& name) const;
  static std::string Display(const Definition& definition, const StateValue& value);

  // In the order they were first defined, which is the order a game defines
  // them in: the screen lists them that way.
  std::vector<Entry> List() const;

  // The stored values. Values for names that are not defined stay pending until
  // they are.
  void Load(const StateEntries& entries);
  // Every value that differs from its default, and every pending one.
  StateEntries Save() const;

  // Set when a value changes, cleared by TakeDirty(): the caller writes the
  // file when it is true.
  bool TakeDirty();

  static bool Validate(const Definition& definition, std::string* error);

 private:
  static bool Fit(const Definition& definition, const StateValue& in, StateValue* out);

  mutable std::mutex m_mutex;
  std::vector<std::string> m_order;
  std::unordered_map<std::string, Entry> m_entries;
  std::unordered_map<std::string, StateValue> m_pending;
  bool m_dirty = false;
};
