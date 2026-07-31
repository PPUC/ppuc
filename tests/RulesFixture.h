#pragma once

// Test support for LuaRulesEngine.
//
// The engine is well suited to unit testing: every output leaves through a
// std::function callback, and it has no dependency on PPUC, libppuc or the
// serial transport. A test loads a rules script from a temporary file, drives
// switch/lamp/coil events, and asserts on what came back out.

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "LuaRulesEngine.h"
#include "doctest.h"

namespace ppuc_test {

// Writes a Lua rules script to a uniquely named temporary file and removes it
// again on destruction.
class TempLua {
 public:
  explicit TempLua(const std::string& contents) {
    static int counter = 0;
    m_path = std::string(P_tmpdir) + "/ppuc_rules_" +
             std::to_string(reinterpret_cast<uintptr_t>(this)) + "_" +
             std::to_string(counter++) + ".lua";

    std::ofstream out(m_path);
    REQUIRE_MESSAGE(out.is_open(), "could not create temp file " << m_path);
    out << contents;
    out.close();
  }

  ~TempLua() { std::remove(m_path.c_str()); }

  TempLua(const TempLua&) = delete;
  TempLua& operator=(const TempLua&) = delete;

  const char* path() const { return m_path.c_str(); }

 private:
  std::string m_path;
};

// Captures everything a rules script emits, so tests can assert on behaviour
// rather than on internal state.
struct RecordedTrigger {
  char source;
  uint16_t id;
  uint8_t value;
};

class RulesHarness {
 public:
  explicit RulesHarness(const std::string& script) : m_script(script) {
    // Drive the engine from a clock the test controls, so time-dependent
    // behaviour can be asserted exactly instead of with sleeps. Starts well
    // away from 0, since 0 is the "never expires" sentinel for named states.
    m_engine.SetClock([this]() { return m_nowMs; });

    m_engine.SetTriggerCallback([this](char source, uint16_t id, uint8_t value) {
      triggers.push_back(RecordedTrigger{source, id, value});
    });
    m_engine.SetSpeechCallback(
        [this](const std::string& text) { speech.push_back(text); });
    m_engine.SetActionCallback(
        [this](const RulesAction& action) { actions.push_back(action); });
  }

  // Loads the script. Returns the error message, or empty on success.
  std::string Load() {
    std::string error;
    if (!m_engine.LoadScript(m_script.path(), error)) {
      return error.empty() ? "load failed without a message" : error;
    }
    return {};
  }

  // Loads and requires success, for tests whose subject is not loading.
  void LoadOrFail() {
    const auto error = Load();
    REQUIRE_MESSAGE(error.empty(), "rules script failed to load: " << error);
  }

  LuaRulesEngine& engine() { return m_engine; }

  // Moves the injected clock forward. Nothing time-driven happens until the
  // engine is asked to do work, so most tests advance then call Update().
  void Advance(uint64_t ms) { m_nowMs += ms; }

  // Advances and pumps the engine, which is what the ppuc-pinmame main loop
  // does every tick.
  void AdvanceAndUpdate(uint64_t ms) {
    Advance(ms);
    m_engine.Update();
  }

  uint64_t now() const { return m_nowMs; }

  std::vector<RecordedTrigger> triggers;
  std::vector<std::string> speech;
  std::vector<RulesAction> actions;

 private:
  TempLua m_script;
  uint64_t m_nowMs = 10'000;
  LuaRulesEngine m_engine;
};

}  // namespace ppuc_test
