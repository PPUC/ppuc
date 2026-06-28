#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "RulesAction.h"

struct lua_State;

class LuaRulesEngine
{
 public:
  using TriggerCallback = std::function<void(char source, uint16_t id, uint8_t value)>;
  using SpeechCallback = std::function<void(const std::string& text)>;
  using ActionCallback = std::function<void(const RulesAction& action)>;

  struct SwitchProcessResult
  {
    bool forwardToCpu = true;
  };

  LuaRulesEngine();
  ~LuaRulesEngine();

  LuaRulesEngine(const LuaRulesEngine&) = delete;
  LuaRulesEngine& operator=(const LuaRulesEngine&) = delete;

  void SetDebug(bool debug);
  void SetTriggerCallback(TriggerCallback callback);
  void SetSpeechCallback(SpeechCallback callback);
  void SetActionCallback(ActionCallback callback);
  void SetSwitchGroups(const std::unordered_map<std::string, std::vector<uint16_t>>& switchGroups);
  bool LoadScript(const char* path, std::string& error);
  bool LoadScripts(const std::vector<std::string>& paths, std::string& error);
  void Update();

  SwitchProcessResult ProcessSwitchState(int number, uint8_t state);
  void OnLampState(int number, uint8_t state);
  void OnCoilState(int number, uint8_t state);
  void SetCurrentBall(uint8_t currentBall);
  void SetCurrentPlayer(uint8_t currentPlayer);
  void SetAttractMode(bool attractMode);

  bool HasFatalError() const;
  const std::string& GetFatalError() const;

 private:
  enum class EventType
  {
    None,
    Switch,
    Lamp,
    Coil
  };

  struct CurrentEvent
  {
    EventType type = EventType::None;
    int number = 0;
    uint8_t oldValue = 0;
    uint8_t newValue = 0;
  };

  struct HistoryEntry
  {
    uint16_t id = 0;
    uint8_t player = 0;
    uint64_t timestampMs = 0;
  };

  struct ScheduledCallback
  {
    uint64_t dueMs = 0;
    uint64_t sequence = 0;
    int ref = 0;
  };

  uint8_t GetState(const std::unordered_map<int, uint8_t>& states, int number) const;
  bool IsRising(EventType type, int number) const;
  bool IsFalling(EventType type, int number) const;
  bool SwitchGroupStateActive(const std::string& name) const;
  bool SwitchGroupEdge(const std::string& name, bool rising) const;
  bool HistoryContains(uint16_t id, uint32_t windowMs, uint64_t nowMs) const;
  bool SequenceOccurred(const std::vector<uint16_t>& ids, uint32_t windowMs, uint64_t nowMs) const;
  bool NamedStateActive(const std::string& name, uint64_t nowMs) const;
  bool StartOnceEvery(const std::string& name, uint32_t durationMs, uint64_t nowMs);
  uint64_t GetNowMs() const;
  void PruneHistoryLocked(uint64_t nowMs);
  void PruneNamedStatesLocked(uint64_t nowMs);
  void RecordTriggerLocked(uint16_t id, uint64_t nowMs);

  bool InitializeLua(std::string& error);
  void RegisterApi();
  void ClearRegisteredHandlers();
  void ClearScheduledCallbacks();
  void ClearPpucHandlers();
  void CapturePpucHandlers();
  bool LoadScriptIntoState(const char* path, std::string& error);
  bool CallHandler(const char* name);
  bool CallHandler(const char* name, int arg1);
  bool CallHandler(const char* name, int arg1, int arg2);
  bool CallRegisteredHandler(int handlerRef, const char* name, const std::vector<int>& args);
  bool CallScheduledCallback(int callbackRef);
  void RunDueScheduledCallbacks(uint64_t nowMs);
  void SetFatalError(const std::string& error);

  static LuaRulesEngine* FromLua(lua_State* L);
  static int LuaSwitchState(lua_State* L);
  static int LuaLampState(lua_State* L);
  static int LuaCoilState(lua_State* L);
  static int LuaCurrentBall(lua_State* L);
  static int LuaCurrentPlayer(lua_State* L);
  static int LuaAttractMode(lua_State* L);
  static int LuaSwitchGroupState(lua_State* L);
  static int LuaSwitchGroupClosing(lua_State* L);
  static int LuaSwitchGroupOpening(lua_State* L);
  static int LuaSetState(lua_State* L);
  static int LuaClearState(lua_State* L);
  static int LuaStateActive(lua_State* L);
  static int LuaTriggerHistory(lua_State* L);
  static int LuaTriggerSequence(lua_State* L);
  static int LuaOnlyOnceEvery(lua_State* L);
  static int LuaAfter(lua_State* L);
  static int LuaPupTrigger(lua_State* L);
  static int LuaSpeech(lua_State* L);
  static int LuaEffectTrigger(lua_State* L);
  static int LuaSuppressSwitch(lua_State* L);
  static int LuaSendSwitchToCpu(lua_State* L);
  static int LuaPulseCoil(lua_State* L);
  static int LuaBlinkLamp(lua_State* L);
  static int LuaStopBlinkLamp(lua_State* L);

  lua_State* m_lua = nullptr;
  std::unordered_map<int, uint8_t> m_switchStates;
  std::unordered_map<int, uint8_t> m_lampStates;
  std::unordered_map<int, uint8_t> m_coilStates;
  std::unordered_map<std::string, uint64_t> m_namedStates;
  std::unordered_map<std::string, std::vector<uint16_t>> m_switchGroups;
  std::unordered_map<std::string, std::vector<int>> m_handlers;
  std::unordered_set<int> m_suppressedSwitchOpen;
  std::deque<HistoryEntry> m_history;
  std::deque<ScheduledCallback> m_scheduledCallbacks;
  CurrentEvent m_currentEvent;
  TriggerCallback m_triggerCallback;
  SpeechCallback m_speechCallback;
  ActionCallback m_actionCallback;
  uint8_t m_currentBall = 0;
  uint8_t m_currentPlayer = 0;
  bool m_attractMode = true;
  bool m_debug = false;
  bool m_fatalError = false;
  std::string m_fatalErrorMessage;
  uint64_t m_nextScheduledSequence = 0;
  mutable std::mutex m_mutex;
};
