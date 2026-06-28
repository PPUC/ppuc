#include "LuaRulesEngine.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <utility>

extern "C"
{
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

namespace
{
constexpr uint32_t kHistoryRetentionMs = 60000;
constexpr char kBoardEffectTriggerSource = 'F';
constexpr const char* kHandlerNames[] = {
    "onSwitchChanged",
    "onLampChanged",
    "onCoilChanged",
    "onBallChanged",
    "onPlayerChanged",
    "onRulesUpdate",
};

using LuaFn = int (*)(lua_State*);

void OpenLuaLibrary(lua_State* L, const char* name, lua_CFunction fn)
{
  luaL_requiref(L, name, fn, 1);
  lua_pop(L, 1);
}

void SetPpucFunction(lua_State* L, LuaRulesEngine* engine, const char* name, LuaFn fn)
{
  lua_pushlightuserdata(L, engine);
  lua_pushcclosure(L, fn, 1);
  lua_setfield(L, -2, name);
}

uint16_t HashNamedTriggerId(const char* name)
{
  if (name == nullptr)
  {
    return 0;
  }

  uint32_t hash = 2166136261u;
  while (*name != '\0')
  {
    hash ^= static_cast<uint8_t>(*name++);
    hash *= 16777619u;
  }

  hash ^= (hash >> 16);
  const uint16_t reduced = static_cast<uint16_t>(hash & 0xFFFFu);
  return reduced == 0 ? 1 : reduced;
}
}  // namespace

LuaRulesEngine::LuaRulesEngine() = default;

LuaRulesEngine::~LuaRulesEngine()
{
  if (m_lua != nullptr)
  {
    ClearRegisteredHandlers();
    ClearScheduledCallbacks();
    lua_close(m_lua);
    m_lua = nullptr;
  }
}

void LuaRulesEngine::SetDebug(bool debug)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_debug = debug;
}

void LuaRulesEngine::SetTriggerCallback(TriggerCallback callback)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_triggerCallback = std::move(callback);
}

void LuaRulesEngine::SetSpeechCallback(SpeechCallback callback)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_speechCallback = std::move(callback);
}

void LuaRulesEngine::SetActionCallback(ActionCallback callback)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_actionCallback = std::move(callback);
}

void LuaRulesEngine::SetSwitchGroups(const std::unordered_map<std::string, std::vector<uint16_t>>& switchGroups)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_switchGroups = switchGroups;
}

bool LuaRulesEngine::InitializeLua(std::string& error)
{
  if (m_lua != nullptr)
  {
    lua_close(m_lua);
    m_lua = nullptr;
  }

  m_lua = luaL_newstate();
  if (m_lua == nullptr)
  {
    error = "Unable to create Lua state";
    return false;
  }

  OpenLuaLibrary(m_lua, LUA_GNAME, luaopen_base);
  OpenLuaLibrary(m_lua, LUA_TABLIBNAME, luaopen_table);
  OpenLuaLibrary(m_lua, LUA_STRLIBNAME, luaopen_string);
  OpenLuaLibrary(m_lua, LUA_MATHLIBNAME, luaopen_math);
  lua_pushnil(m_lua);
  lua_setglobal(m_lua, "dofile");
  lua_pushnil(m_lua);
  lua_setglobal(m_lua, "loadfile");
  RegisterApi();
  return true;
}

void LuaRulesEngine::RegisterApi()
{
  lua_newtable(m_lua);

  SetPpucFunction(m_lua, this, "switchState", LuaSwitchState);
  SetPpucFunction(m_lua, this, "lampState", LuaLampState);
  SetPpucFunction(m_lua, this, "coilState", LuaCoilState);
  SetPpucFunction(m_lua, this, "currentBall", LuaCurrentBall);
  SetPpucFunction(m_lua, this, "currentPlayer", LuaCurrentPlayer);
  SetPpucFunction(m_lua, this, "attractMode", LuaAttractMode);

  SetPpucFunction(m_lua, this, "switchGroupState", LuaSwitchGroupState);
  SetPpucFunction(m_lua, this, "switchGroupClosing", LuaSwitchGroupClosing);
  SetPpucFunction(m_lua, this, "switchGroupOpening", LuaSwitchGroupOpening);

  SetPpucFunction(m_lua, this, "setState", LuaSetState);
  SetPpucFunction(m_lua, this, "clearState", LuaClearState);
  SetPpucFunction(m_lua, this, "stateActive", LuaStateActive);
  SetPpucFunction(m_lua, this, "triggerHistory", LuaTriggerHistory);
  SetPpucFunction(m_lua, this, "triggerSequence", LuaTriggerSequence);
  SetPpucFunction(m_lua, this, "onlyOnceEvery", LuaOnlyOnceEvery);
  SetPpucFunction(m_lua, this, "after", LuaAfter);

  SetPpucFunction(m_lua, this, "pupTrigger", LuaPupTrigger);
  SetPpucFunction(m_lua, this, "speech", LuaSpeech);
  SetPpucFunction(m_lua, this, "effectTrigger", LuaEffectTrigger);
  SetPpucFunction(m_lua, this, "suppressSwitch", LuaSuppressSwitch);
  SetPpucFunction(m_lua, this, "sendSwitchToCpu", LuaSendSwitchToCpu);
  SetPpucFunction(m_lua, this, "pulseCoil", LuaPulseCoil);
  SetPpucFunction(m_lua, this, "blinkLamp", LuaBlinkLamp);
  SetPpucFunction(m_lua, this, "stopBlinkLamp", LuaStopBlinkLamp);

  lua_setglobal(m_lua, "ppuc");
}

bool LuaRulesEngine::LoadScript(const char* path, std::string& error)
{
  return LoadScripts(std::vector<std::string>{path}, error);
}

bool LuaRulesEngine::LoadScripts(const std::vector<std::string>& paths, std::string& error)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_fatalError = false;
  m_fatalErrorMessage.clear();
  ClearRegisteredHandlers();
  ClearScheduledCallbacks();

  if (!InitializeLua(error))
  {
    return false;
  }

  for (const std::string& path : paths)
  {
    if (!LoadScriptIntoState(path.c_str(), error))
    {
      return false;
    }
  }

  return true;
}

void LuaRulesEngine::ClearRegisteredHandlers()
{
  if (m_lua == nullptr)
  {
    m_handlers.clear();
    return;
  }

  for (auto& entry : m_handlers)
  {
    for (const int ref : entry.second)
    {
      luaL_unref(m_lua, LUA_REGISTRYINDEX, ref);
    }
  }
  m_handlers.clear();
}

void LuaRulesEngine::ClearScheduledCallbacks()
{
  if (m_lua != nullptr)
  {
    for (const ScheduledCallback& callback : m_scheduledCallbacks)
    {
      luaL_unref(m_lua, LUA_REGISTRYINDEX, callback.ref);
    }
  }
  m_scheduledCallbacks.clear();
  m_nextScheduledSequence = 0;
}

void LuaRulesEngine::ClearPpucHandlers()
{
  lua_getglobal(m_lua, "ppuc");
  for (const char* name : kHandlerNames)
  {
    lua_pushnil(m_lua);
    lua_setfield(m_lua, -2, name);
  }
  lua_pop(m_lua, 1);
}

void LuaRulesEngine::CapturePpucHandlers()
{
  lua_getglobal(m_lua, "ppuc");
  for (const char* name : kHandlerNames)
  {
    lua_getfield(m_lua, -1, name);
    if (lua_isfunction(m_lua, -1))
    {
      m_handlers[name].push_back(luaL_ref(m_lua, LUA_REGISTRYINDEX));
    }
    else
    {
      lua_pop(m_lua, 1);
    }
  }
  lua_pop(m_lua, 1);
}

bool LuaRulesEngine::LoadScriptIntoState(const char* path, std::string& error)
{
  ClearPpucHandlers();

  if (luaL_loadfile(m_lua, path) != LUA_OK)
  {
    error = lua_tostring(m_lua, -1);
    lua_pop(m_lua, 1);
    return false;
  }

  if (lua_pcall(m_lua, 0, 0, 0) != LUA_OK)
  {
    error = lua_tostring(m_lua, -1);
    lua_pop(m_lua, 1);
    return false;
  }

  CapturePpucHandlers();
  return true;
}

bool LuaRulesEngine::CallHandler(const char* name)
{
  const auto it = m_handlers.find(name);
  if (it == m_handlers.end())
  {
    return true;
  }

  for (const int handlerRef : it->second)
  {
    if (!CallRegisteredHandler(handlerRef, name, {}))
    {
      return false;
    }
  }
  return true;
}

bool LuaRulesEngine::CallHandler(const char* name, int arg1)
{
  const auto it = m_handlers.find(name);
  if (it == m_handlers.end())
  {
    return true;
  }

  for (const int handlerRef : it->second)
  {
    if (!CallRegisteredHandler(handlerRef, name, {arg1}))
    {
      return false;
    }
  }
  return true;
}

bool LuaRulesEngine::CallHandler(const char* name, int arg1, int arg2)
{
  const auto it = m_handlers.find(name);
  if (it == m_handlers.end())
  {
    return true;
  }

  for (const int handlerRef : it->second)
  {
    if (!CallRegisteredHandler(handlerRef, name, {arg1, arg2}))
    {
      return false;
    }
  }
  return true;
}

bool LuaRulesEngine::CallRegisteredHandler(int handlerRef, const char* name, const std::vector<int>& args)
{
  lua_rawgeti(m_lua, LUA_REGISTRYINDEX, handlerRef);
  for (const int arg : args)
  {
    lua_pushinteger(m_lua, arg);
  }

  if (lua_pcall(m_lua, static_cast<int>(args.size()), 0, 0) != LUA_OK)
  {
    std::string error = lua_tostring(m_lua, -1);
    if (name != nullptr && name[0] != '\0')
    {
      error = std::string(name) + ": " + error;
    }
    SetFatalError(error);
    lua_pop(m_lua, 1);
    return false;
  }
  return true;
}

bool LuaRulesEngine::CallScheduledCallback(int callbackRef)
{
  lua_rawgeti(m_lua, LUA_REGISTRYINDEX, callbackRef);
  luaL_unref(m_lua, LUA_REGISTRYINDEX, callbackRef);

  if (lua_pcall(m_lua, 0, 0, 0) != LUA_OK)
  {
    std::string error = lua_tostring(m_lua, -1);
    SetFatalError(std::string("scheduled callback: ") + error);
    lua_pop(m_lua, 1);
    return false;
  }
  return true;
}

void LuaRulesEngine::RunDueScheduledCallbacks(uint64_t nowMs)
{
  std::vector<ScheduledCallback> due;
  for (auto it = m_scheduledCallbacks.begin(); it != m_scheduledCallbacks.end();)
  {
    if (it->dueMs <= nowMs)
    {
      due.push_back(*it);
      it = m_scheduledCallbacks.erase(it);
    }
    else
    {
      ++it;
    }
  }

  std::sort(due.begin(), due.end(), [](const ScheduledCallback& a, const ScheduledCallback& b) {
    if (a.dueMs != b.dueMs)
    {
      return a.dueMs < b.dueMs;
    }
    return a.sequence < b.sequence;
  });

  for (const ScheduledCallback& callback : due)
  {
    if (!CallScheduledCallback(callback.ref) || m_fatalError)
    {
      return;
    }
  }
}

void LuaRulesEngine::Update()
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_lua == nullptr || m_fatalError)
  {
    return;
  }

  const uint64_t nowMs = GetNowMs();
  PruneHistoryLocked(nowMs);
  PruneNamedStatesLocked(nowMs);
  m_currentEvent = CurrentEvent{};
  RunDueScheduledCallbacks(nowMs);
  if (m_fatalError)
  {
    return;
  }
  CallHandler("onRulesUpdate");
}

LuaRulesEngine::SwitchProcessResult LuaRulesEngine::ProcessSwitchState(int number, uint8_t state)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  SwitchProcessResult result;
  const uint8_t normalized = state == 0 ? 0 : 1;
  const uint8_t old = GetState(m_switchStates, number);
  m_switchStates[number] = normalized;
  m_currentEvent = CurrentEvent{EventType::Switch, number, old, normalized};

  if (m_lua != nullptr && !m_fatalError)
  {
    CallHandler("onSwitchChanged", number, normalized);
  }

  if (normalized == 0 && m_suppressedSwitchOpen.erase(number) > 0)
  {
    result.forwardToCpu = false;
  }
  else if (m_suppressedSwitchOpen.find(number) != m_suppressedSwitchOpen.end())
  {
    result.forwardToCpu = false;
  }

  m_currentEvent = CurrentEvent{};
  return result;
}

void LuaRulesEngine::OnLampState(int number, uint8_t state)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  const uint8_t normalized = state == 0 ? 0 : 1;
  const uint8_t old = GetState(m_lampStates, number);
  m_lampStates[number] = normalized;
  m_currentEvent = CurrentEvent{EventType::Lamp, number, old, normalized};
  if (m_lua != nullptr && !m_fatalError)
  {
    CallHandler("onLampChanged", number, normalized);
  }
  m_currentEvent = CurrentEvent{};
}

void LuaRulesEngine::OnCoilState(int number, uint8_t state)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  const uint8_t normalized = state == 0 ? 0 : 1;
  const uint8_t old = GetState(m_coilStates, number);
  m_coilStates[number] = normalized;
  m_currentEvent = CurrentEvent{EventType::Coil, number, old, normalized};
  if (m_lua != nullptr && !m_fatalError)
  {
    CallHandler("onCoilChanged", number, normalized);
  }
  m_currentEvent = CurrentEvent{};
}

void LuaRulesEngine::SetCurrentBall(uint8_t currentBall)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_currentBall == currentBall)
  {
    return;
  }
  m_currentBall = currentBall;
  if (m_lua != nullptr && !m_fatalError)
  {
    CallHandler("onBallChanged", currentBall);
  }
}

void LuaRulesEngine::SetCurrentPlayer(uint8_t currentPlayer)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_currentPlayer == currentPlayer)
  {
    return;
  }
  m_currentPlayer = currentPlayer;
  if (m_lua != nullptr && !m_fatalError)
  {
    CallHandler("onPlayerChanged", currentPlayer);
  }
}

void LuaRulesEngine::SetAttractMode(bool attractMode)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_attractMode = attractMode;
}

bool LuaRulesEngine::HasFatalError() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_fatalError;
}

const std::string& LuaRulesEngine::GetFatalError() const
{
  return m_fatalErrorMessage;
}

uint8_t LuaRulesEngine::GetState(const std::unordered_map<int, uint8_t>& states, int number) const
{
  const auto it = states.find(number);
  return it == states.end() ? 0 : it->second;
}

bool LuaRulesEngine::IsRising(EventType type, int number) const
{
  return m_currentEvent.type == type && m_currentEvent.number == number && m_currentEvent.oldValue == 0 &&
         m_currentEvent.newValue != 0;
}

bool LuaRulesEngine::IsFalling(EventType type, int number) const
{
  return m_currentEvent.type == type && m_currentEvent.number == number && m_currentEvent.oldValue != 0 &&
         m_currentEvent.newValue == 0;
}

bool LuaRulesEngine::SwitchGroupStateActive(const std::string& name) const
{
  const auto it = m_switchGroups.find(name);
  if (it == m_switchGroups.end())
  {
    return false;
  }

  for (const uint16_t number : it->second)
  {
    if (GetState(m_switchStates, number) != 0)
    {
      return true;
    }
  }
  return false;
}

bool LuaRulesEngine::SwitchGroupEdge(const std::string& name, bool rising) const
{
  if (m_currentEvent.type != EventType::Switch)
  {
    return false;
  }

  const auto it = m_switchGroups.find(name);
  if (it == m_switchGroups.end())
  {
    return false;
  }

  if (std::find(it->second.begin(), it->second.end(), static_cast<uint16_t>(m_currentEvent.number)) == it->second.end())
  {
    return false;
  }
  return rising ? IsRising(EventType::Switch, m_currentEvent.number) : IsFalling(EventType::Switch, m_currentEvent.number);
}

bool LuaRulesEngine::HistoryContains(uint16_t id, uint32_t windowMs, uint64_t nowMs) const
{
  for (auto it = m_history.rbegin(); it != m_history.rend(); ++it)
  {
    if (it->player != m_currentPlayer || it->id != id)
    {
      continue;
    }
    if (windowMs == 0 || nowMs - it->timestampMs <= windowMs)
    {
      return true;
    }
  }
  return false;
}

bool LuaRulesEngine::SequenceOccurred(const std::vector<uint16_t>& ids, uint32_t windowMs, uint64_t nowMs) const
{
  if (ids.empty())
  {
    return false;
  }

  size_t next = ids.size();
  uint64_t newest = 0;
  uint64_t oldest = 0;
  for (auto it = m_history.rbegin(); it != m_history.rend(); ++it)
  {
    if (it->player != m_currentPlayer)
    {
      continue;
    }
    if (it->id != ids[next - 1])
    {
      continue;
    }
    if (newest == 0)
    {
      newest = it->timestampMs;
    }
    oldest = it->timestampMs;
    next--;
    if (next == 0)
    {
      return windowMs == 0 || (nowMs - newest <= windowMs && newest - oldest <= windowMs);
    }
  }
  return false;
}

bool LuaRulesEngine::NamedStateActive(const std::string& name, uint64_t nowMs) const
{
  const auto it = m_namedStates.find(name);
  return it != m_namedStates.end() && (it->second == 0 || it->second > nowMs);
}

bool LuaRulesEngine::StartOnceEvery(const std::string& name, uint32_t durationMs, uint64_t nowMs)
{
  PruneNamedStatesLocked(nowMs);
  if (NamedStateActive(name, nowMs))
  {
    return false;
  }

  m_namedStates[name] = durationMs == 0 ? 0 : nowMs + durationMs;
  return true;
}

uint64_t LuaRulesEngine::GetNowMs() const
{
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

void LuaRulesEngine::PruneHistoryLocked(uint64_t nowMs)
{
  while (!m_history.empty() && nowMs - m_history.front().timestampMs > kHistoryRetentionMs)
  {
    m_history.pop_front();
  }
}

void LuaRulesEngine::PruneNamedStatesLocked(uint64_t nowMs)
{
  for (auto it = m_namedStates.begin(); it != m_namedStates.end();)
  {
    if (it->second != 0 && it->second <= nowMs)
    {
      it = m_namedStates.erase(it);
    }
    else
    {
      ++it;
    }
  }
}

void LuaRulesEngine::RecordTriggerLocked(uint16_t id, uint64_t nowMs)
{
  m_history.push_back(HistoryEntry{id, m_currentPlayer, nowMs});
  PruneHistoryLocked(nowMs);
}

void LuaRulesEngine::SetFatalError(const std::string& error)
{
  m_fatalError = true;
  m_fatalErrorMessage = error.empty() ? "Lua rules runtime error" : error;
}

LuaRulesEngine* LuaRulesEngine::FromLua(lua_State* L)
{
  return static_cast<LuaRulesEngine*>(lua_touserdata(L, lua_upvalueindex(1)));
}

int LuaRulesEngine::LuaSwitchState(lua_State* L)
{
  auto* engine = FromLua(L);
  lua_pushboolean(L, engine->GetState(engine->m_switchStates, static_cast<int>(luaL_checkinteger(L, 1))) != 0);
  return 1;
}

int LuaRulesEngine::LuaLampState(lua_State* L)
{
  auto* engine = FromLua(L);
  lua_pushboolean(L, engine->GetState(engine->m_lampStates, static_cast<int>(luaL_checkinteger(L, 1))) != 0);
  return 1;
}

int LuaRulesEngine::LuaCoilState(lua_State* L)
{
  auto* engine = FromLua(L);
  lua_pushboolean(L, engine->GetState(engine->m_coilStates, static_cast<int>(luaL_checkinteger(L, 1))) != 0);
  return 1;
}

int LuaRulesEngine::LuaCurrentBall(lua_State* L)
{
  lua_pushinteger(L, FromLua(L)->m_currentBall);
  return 1;
}

int LuaRulesEngine::LuaCurrentPlayer(lua_State* L)
{
  lua_pushinteger(L, FromLua(L)->m_currentPlayer);
  return 1;
}

int LuaRulesEngine::LuaAttractMode(lua_State* L)
{
  lua_pushboolean(L, FromLua(L)->m_attractMode);
  return 1;
}

int LuaRulesEngine::LuaSwitchGroupState(lua_State* L)
{
  lua_pushboolean(L, FromLua(L)->SwitchGroupStateActive(luaL_checkstring(L, 1)));
  return 1;
}

int LuaRulesEngine::LuaSwitchGroupClosing(lua_State* L)
{
  lua_pushboolean(L, FromLua(L)->SwitchGroupEdge(luaL_checkstring(L, 1), true));
  return 1;
}

int LuaRulesEngine::LuaSwitchGroupOpening(lua_State* L)
{
  lua_pushboolean(L, FromLua(L)->SwitchGroupEdge(luaL_checkstring(L, 1), false));
  return 1;
}

int LuaRulesEngine::LuaSetState(lua_State* L)
{
  auto* engine = FromLua(L);
  const std::string name = luaL_checkstring(L, 1);
  const uint32_t durationMs = lua_gettop(L) >= 2 ? static_cast<uint32_t>(luaL_checkinteger(L, 2)) : 0;
  engine->m_namedStates[name] = durationMs == 0 ? 0 : engine->GetNowMs() + durationMs;
  return 0;
}

int LuaRulesEngine::LuaClearState(lua_State* L)
{
  FromLua(L)->m_namedStates.erase(luaL_checkstring(L, 1));
  return 0;
}

int LuaRulesEngine::LuaStateActive(lua_State* L)
{
  auto* engine = FromLua(L);
  lua_pushboolean(L, engine->NamedStateActive(luaL_checkstring(L, 1), engine->GetNowMs()));
  return 1;
}

int LuaRulesEngine::LuaTriggerHistory(lua_State* L)
{
  auto* engine = FromLua(L);
  const uint16_t id = static_cast<uint16_t>(luaL_checkinteger(L, 1));
  const uint32_t windowMs = lua_gettop(L) >= 2 ? static_cast<uint32_t>(luaL_checkinteger(L, 2)) : 0;
  lua_pushboolean(L, engine->HistoryContains(id, windowMs, engine->GetNowMs()));
  return 1;
}

int LuaRulesEngine::LuaTriggerSequence(lua_State* L)
{
  auto* engine = FromLua(L);
  const int argc = lua_gettop(L);
  const uint32_t windowMs = static_cast<uint32_t>(luaL_checkinteger(L, 1));
  std::vector<uint16_t> ids;
  ids.reserve(argc > 1 ? static_cast<size_t>(argc - 1) : 0);
  for (int i = 2; i <= argc; ++i)
  {
    ids.push_back(static_cast<uint16_t>(luaL_checkinteger(L, i)));
  }
  lua_pushboolean(L, engine->SequenceOccurred(ids, windowMs, engine->GetNowMs()));
  return 1;
}

int LuaRulesEngine::LuaOnlyOnceEvery(lua_State* L)
{
  auto* engine = FromLua(L);
  const std::string name = luaL_checkstring(L, 1);
  const uint32_t durationMs = lua_gettop(L) >= 2 ? static_cast<uint32_t>(luaL_checkinteger(L, 2)) : 0;
  lua_pushboolean(L, engine->StartOnceEvery(name, durationMs, engine->GetNowMs()));
  return 1;
}

int LuaRulesEngine::LuaAfter(lua_State* L)
{
  auto* engine = FromLua(L);
  const lua_Integer delayArg = luaL_checkinteger(L, 1);
  luaL_checktype(L, 2, LUA_TFUNCTION);
  const uint64_t delayMs = delayArg <= 0 ? 0 : static_cast<uint64_t>(delayArg);
  lua_pushvalue(L, 2);
  const int callbackRef = luaL_ref(L, LUA_REGISTRYINDEX);
  engine->m_scheduledCallbacks.push_back(
      ScheduledCallback{engine->GetNowMs() + delayMs, engine->m_nextScheduledSequence++, callbackRef});
  return 0;
}

int LuaRulesEngine::LuaPupTrigger(lua_State* L)
{
  auto* engine = FromLua(L);
  const char* source = luaL_checkstring(L, 1);
  const uint16_t id = static_cast<uint16_t>(luaL_checkinteger(L, 2));
  const uint8_t value = lua_gettop(L) >= 3 ? static_cast<uint8_t>(luaL_checkinteger(L, 3)) : 1;
  if (engine->m_triggerCallback)
  {
    engine->m_triggerCallback(source != nullptr && source[0] != '\0' ? source[0] : 'P', id, value);
  }
  engine->RecordTriggerLocked(id, engine->GetNowMs());
  return 0;
}

int LuaRulesEngine::LuaSpeech(lua_State* L)
{
  auto* engine = FromLua(L);
  const char* text = luaL_checkstring(L, 1);
  if (engine->m_speechCallback && text != nullptr && text[0] != '\0')
  {
    engine->m_speechCallback(text);
  }
  return 0;
}

int LuaRulesEngine::LuaEffectTrigger(lua_State* L)
{
  auto* engine = FromLua(L);
  uint16_t id = 0;
  if (lua_type(L, 1) == LUA_TSTRING)
  {
    id = HashNamedTriggerId(lua_tostring(L, 1));
  }
  else
  {
    id = static_cast<uint16_t>(luaL_checkinteger(L, 1));
  }
  const uint8_t value = lua_gettop(L) >= 2 ? static_cast<uint8_t>(luaL_checkinteger(L, 2)) : 1;
  if (engine->m_triggerCallback)
  {
    engine->m_triggerCallback(kBoardEffectTriggerSource, id, value);
  }
  engine->RecordTriggerLocked(id, engine->GetNowMs());
  return 0;
}

int LuaRulesEngine::LuaSuppressSwitch(lua_State* L)
{
  auto* engine = FromLua(L);
  const int number = static_cast<int>(luaL_checkinteger(L, 1));
  if (engine->m_currentEvent.type == EventType::Switch && engine->m_currentEvent.number == number &&
      engine->m_currentEvent.newValue != 0)
  {
    engine->m_suppressedSwitchOpen.insert(number);
  }
  return 0;
}

int LuaRulesEngine::LuaSendSwitchToCpu(lua_State* L)
{
  auto* engine = FromLua(L);
  if (engine->m_actionCallback)
  {
    engine->m_actionCallback(RulesAction{RulesActionType::SendSwitchToCpu,
                                         static_cast<int>(luaL_checkinteger(L, 1)),
                                         luaL_checkinteger(L, 2) == 0 ? static_cast<uint8_t>(0) : static_cast<uint8_t>(1),
                                         0,
                                         0,
                                         0});
  }
  return 0;
}

int LuaRulesEngine::LuaPulseCoil(lua_State* L)
{
  auto* engine = FromLua(L);
  if (engine->m_actionCallback)
  {
    engine->m_actionCallback(RulesAction{RulesActionType::PulseCoil,
                                         static_cast<int>(luaL_checkinteger(L, 1)),
                                         0,
                                         static_cast<uint32_t>(luaL_checkinteger(L, 2)),
                                         0,
                                         0});
  }
  return 0;
}

int LuaRulesEngine::LuaBlinkLamp(lua_State* L)
{
  auto* engine = FromLua(L);
  if (engine->m_actionCallback)
  {
    engine->m_actionCallback(RulesAction{RulesActionType::StartBlinkLamp,
                                         static_cast<int>(luaL_checkinteger(L, 1)),
                                         0,
                                         0,
                                         static_cast<uint32_t>(luaL_checkinteger(L, 2)),
                                         static_cast<uint32_t>(luaL_checkinteger(L, 3))});
  }
  return 0;
}

int LuaRulesEngine::LuaStopBlinkLamp(lua_State* L)
{
  auto* engine = FromLua(L);
  if (engine->m_actionCallback)
  {
    engine->m_actionCallback(
        RulesAction{RulesActionType::StopBlinkLamp, static_cast<int>(luaL_checkinteger(L, 1)), 0, 0, 0, 0});
  }
  return 0;
}
