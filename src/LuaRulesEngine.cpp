#include "LuaRulesEngine.h"

#include "scoreboard/ScoreboardModel.h"

#include "Adjustments.h"
#include "LuaApiSupport.h"
#include "LuaGameApi.h"
#include "MachineState.h"
#include "SwitchReport.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
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
    // ROM-less game events. Harmless under PinMAME: nothing ever emits them.
    "onAttractStart",
    "onAttractEnd",
    "onGameStart",
    "onPlayerAdded",
    "onStartRejected",
    "onBallStart",
    "onBallServed",
    "onBallServeFailed",
    "onBallStuck",
    "onBallEnd",
    "onBonusCount",
    "onExtraBall",
    "onScore",
    "onTiltWarning",
    "onTilt",
    "onSlamTilt",
    "onReplay",
    "onMatch",
    "onGameEnd",
    "onCreditsChanged",
    "onBallSaved",
    "onDmdFrame",
    // An operator changed one of the game's adjustments at the machine.
    "onAdjustmentChanged",
    // Query handlers: return a boolean.
    "canStartGame",
    "canAddPlayer",
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
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  m_debug = debug;
}

void LuaRulesEngine::SetTriggerCallback(TriggerCallback callback)
{
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  m_triggerCallback = std::move(callback);
}

void LuaRulesEngine::SetGiProgramCallback(GiProgramCallback callback)
{
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  m_giProgramCallback = std::move(callback);
}

void LuaRulesEngine::SetSerumSceneCallback(SerumSceneCallback callback)
{
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  m_serumSceneCallback = std::move(callback);
}

void LuaRulesEngine::SetSoundCommandCallback(SoundCommandCallback callback)
{
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  m_soundCommandCallback = std::move(callback);
}

void LuaRulesEngine::SetSpeechCallback(SpeechCallback callback)
{
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  m_speechCallback = std::move(callback);
}

void LuaRulesEngine::SetSoundCallback(SoundCallback callback)
{
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  m_soundCallback = std::move(callback);
}

void LuaRulesEngine::SetActionCallback(ActionCallback callback)
{
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  m_actionCallback = std::move(callback);
}

void LuaRulesEngine::SetClock(ClockFn clock)
{
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  m_clock = std::move(clock);
}

void LuaRulesEngine::SetSwitchGroups(const std::unordered_map<std::string, std::vector<uint16_t>>& switchGroups)
{
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
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
  SetPpucFunction(m_lua, this, "serumScene", LuaSerumScene);
  SetPpucFunction(m_lua, this, "soundCommand", LuaSoundCommand);
  SetPpucFunction(m_lua, this, "speech", LuaSpeech);
  SetPpucFunction(m_lua, this, "playSound", LuaPlaySound);
  SetPpucFunction(m_lua, this, "effectTrigger", LuaEffectTrigger);
  SetPpucFunction(m_lua, this, "suppressSwitch", LuaSuppressSwitch);
  SetPpucFunction(m_lua, this, "sendSwitchToCpu", LuaSendSwitchToCpu);
  SetPpucFunction(m_lua, this, "pulseCoil", LuaPulseCoil);
  SetPpucFunction(m_lua, this, "setCoil", LuaSetCoil);
  SetPpucFunction(m_lua, this, "blinkLamp", LuaBlinkLamp);
  SetPpucFunction(m_lua, this, "stopBlinkLamp", LuaStopBlinkLamp);
  SetPpucFunction(m_lua, this, "ballSave", LuaBallSave);
  SetPpucFunction(m_lua, this, "suppressCoil", LuaSuppressCoil);
  SetPpucFunction(m_lua, this, "isButtonSwitch", LuaIsButtonSwitch);
  SetPpucFunction(m_lua, this, "holdBallSearch", LuaHoldBallSearch);
  SetPpucFunction(m_lua, this, "setGiProgram", LuaSetGiProgram);
  SetPpucFunction(m_lua, this, "defineAdjustment", LuaDefineAdjustment);
  SetPpucFunction(m_lua, this, "adjustment", LuaAdjustment);
  SetPpucFunction(m_lua, this, "nvGet", LuaNvGet);
  SetPpucFunction(m_lua, this, "nvSet", LuaNvSet);
  SetPpucFunction(m_lua, this, "excludeFromSwitchReport", LuaExcludeFromSwitchReport);
  SetPpucFunction(m_lua, this, "scoreboardSet", LuaScoreboardSet);
  SetPpucFunction(m_lua, this, "scoreboardCallout", LuaScoreboardCallout);
  SetPpucFunction(m_lua, this, "gameVersion", LuaGameVersion);

  // Registered only when the ROM-less game is running, so `ppuc.game == nil` is
  // a reliable feature test for a script that must work under either engine.
  RegisterLuaGameApi(m_lua, m_pGameCore);
  RegisterLuaDmdApi(m_lua, m_pDmdCanvas);

  lua_setglobal(m_lua, "ppuc");
}

bool LuaRulesEngine::LoadScript(const char* path, std::string& error)
{
  return LoadScripts(std::vector<std::string>{path}, error);
}

bool LuaRulesEngine::LoadScripts(const std::vector<std::string>& paths, std::string& error)
{
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
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

bool LuaRulesEngine::CallHandler(const char* name, std::vector<LuaArg> args)
{
  const auto it = m_handlers.find(name);
  if (it == m_handlers.end())
  {
    return true;
  }

  for (const int handlerRef : it->second)
  {
    if (!CallRegisteredHandler(handlerRef, name, args))
    {
      return false;
    }
  }
  return true;
}

void LuaRulesEngine::SetGameCore(GameCore* pGameCore) { m_pGameCore = pGameCore; }

void LuaRulesEngine::SetDmdCanvas(DmdCanvas* pCanvas) { m_pDmdCanvas = pCanvas; }

void LuaRulesEngine::SetAdjustments(Adjustments* pAdjustments)
{
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  m_pAdjustments = pAdjustments;
}

void LuaRulesEngine::SetMachineMemory(MachineMemory* pMemory)
{
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  m_pMemory = pMemory;
}

void LuaRulesEngine::SetSwitchReport(SwitchReport* pReport)
{
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  m_pSwitchReport = pReport;
}

void LuaRulesEngine::SetScoreboardFeed(scoreboard::Feed* pFeed)
{
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  m_pScoreboardFeed = pFeed;
}

void LuaRulesEngine::CallGameHandler(const char* name, std::vector<LuaArg> args)
{
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (m_lua == nullptr || m_fatalError)
  {
    return;
  }
  // The current-event helpers (ppuc.switchGroupClosing and friends) only mean
  // something during a switch dispatch; a game event is not one.
  m_currentEvent = CurrentEvent{};
  CallHandler(name, std::move(args));
}

bool LuaRulesEngine::CallQueryHandler(const char* name, std::vector<LuaArg> args, bool defaultResult)
{
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (m_lua == nullptr || m_fatalError)
  {
    return defaultResult;
  }

  const auto it = m_handlers.find(name);
  if (it == m_handlers.end() || it->second.empty())
  {
    return defaultResult;
  }

  // Every registered handler must agree. One veto is a veto: a script that says
  // no should not be overruled by another that forgot to.
  for (const int handlerRef : it->second)
  {
    lua_rawgeti(m_lua, LUA_REGISTRYINDEX, handlerRef);
    for (const LuaArg& arg : args)
    {
      PushLuaArg(m_lua, arg);
    }

    if (lua_pcall(m_lua, static_cast<int>(args.size()), 1, 0) != LUA_OK)
    {
      std::string error = lua_tostring(m_lua, -1);
      SetFatalError(std::string(name) + ": " + error);
      lua_pop(m_lua, 1);
      return defaultResult;
    }

    // A handler that returns nothing means "no opinion", not "no".
    const bool allowed = lua_isnoneornil(m_lua, -1) ? defaultResult : (lua_toboolean(m_lua, -1) != 0);
    lua_pop(m_lua, 1);
    if (!allowed)
    {
      return false;
    }
  }
  return true;
}

bool LuaRulesEngine::CallRegisteredHandler(int handlerRef, const char* name, const std::vector<LuaArg>& args)
{
  lua_rawgeti(m_lua, LUA_REGISTRYINDEX, handlerRef);
  for (const LuaArg& arg : args)
  {
    PushLuaArg(m_lua, arg);
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
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
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
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  SwitchProcessResult result;
  const uint8_t normalized = state == 0 ? 0 : 1;
  const uint8_t old = GetState(m_switchStates, number);
  m_switchStates[number] = normalized;
  m_currentEvent = CurrentEvent{EventType::Switch, number, old, normalized};

  if (m_lua != nullptr && !m_fatalError)
  {
    CallHandler("onSwitchChanged", {number, normalized});
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
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  const uint8_t normalized = state == 0 ? 0 : 1;
  const uint8_t old = GetState(m_lampStates, number);
  m_lampStates[number] = normalized;
  // onLampChanged means what it says. The engine can report a lamp as on
  // again without it having gone off, and every such report used to run the
  // rule: an award that speaks when its lamp lights said its line several
  // times over, on a lamp that never visibly blinked.
  //
  // A lamp that really does blink still calls the rule on each edge, because
  // the state genuinely changes. A rule that should act once per ball has to
  // say so itself.
  if (old == normalized)
  {
    return;
  }
  m_currentEvent = CurrentEvent{EventType::Lamp, number, old, normalized};
  if (m_lua != nullptr && !m_fatalError)
  {
    CallHandler("onLampChanged", {number, normalized});
  }
  m_currentEvent = CurrentEvent{};
}

LuaRulesEngine::CoilProcessResult LuaRulesEngine::ProcessCoilState(int number, uint8_t state)
{
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  CoilProcessResult result;
  const uint8_t normalized = state == 0 ? 0 : 1;
  const uint8_t old = GetState(m_coilStates, number);
  m_coilStates[number] = normalized;
  m_currentEvent = CurrentEvent{EventType::Coil, number, old, normalized};
  m_suppressCurrentCoil = false;
  if (m_lua != nullptr && !m_fatalError)
  {
    CallHandler("onCoilChanged", {number, normalized});
  }
  result.forwardToBoard = !m_suppressCurrentCoil;
  m_suppressCurrentCoil = false;
  m_currentEvent = CurrentEvent{};
  return result;
}

void LuaRulesEngine::SetButtonSwitches(std::unordered_set<int> buttons)
{
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  m_buttonSwitches = std::move(buttons);
}

void LuaRulesEngine::SetCurrentBall(uint8_t currentBall)
{
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (m_currentBall == currentBall)
  {
    return;
  }
  m_currentBall = currentBall;
  if (m_lua != nullptr && !m_fatalError)
  {
    CallHandler("onBallChanged", {currentBall});
  }
}

void LuaRulesEngine::SetCurrentPlayer(uint8_t currentPlayer)
{
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (m_currentPlayer == currentPlayer)
  {
    return;
  }
  m_currentPlayer = currentPlayer;
  if (m_lua != nullptr && !m_fatalError)
  {
    CallHandler("onPlayerChanged", {currentPlayer});
  }
}

void LuaRulesEngine::SetAttractMode(bool attractMode)
{
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  m_attractMode = attractMode;
}

bool LuaRulesEngine::HasFatalError() const
{
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
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
  if (m_clock)
  {
    return m_clock();
  }

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

// ppuc.playSound(file) -- play an audio file from the game folder once, over
// the game's own sound. Returns whether it started: false without sound, for a
// file that is missing or unreadable, or for a path outside the game folder.
int LuaRulesEngine::LuaPlaySound(lua_State* L)
{
  auto* engine = FromLua(L);
  const char* file = luaL_checkstring(L, 1);
  const bool started = engine->m_soundCallback && file != nullptr && file[0] != '\0' && engine->m_soundCallback(file);
  lua_pushboolean(L, started ? 1 : 0);
  return 1;
}

// ppuc.serumScene(id) -- play scene `id` in the Serum colorization.
//
// Deliberately not folded into pupTrigger. Serum takes scenes on its own
// message and reads no 'D' event; a pack that wants both a video and a scene
// asks for both, and one that wants only a video is no longer forced into a
// scene it never authored.
//
// Scene ids live in libserum's 50000..62000 window. The window is checked in
// the Serum plugin and in libdmdutil, so this passes the id through as given
// rather than duplicating the constant a third time.
int LuaRulesEngine::LuaSerumScene(lua_State* L)
{
  auto* engine = FromLua(L);
  const uint16_t id = static_cast<uint16_t>(luaL_checkinteger(L, 1));
  if (engine->m_serumSceneCallback)
  {
    engine->m_serumSceneCallback(id);
  }
  engine->RecordTriggerLocked(id, engine->GetNowMs());
  return 0;
}

// ppuc.soundCommand(cmd [, board]) -- send sound command `cmd` to the AltSound
// pack, exactly as if the CPU board had sent it to sound board `board`
// (default 0). `cmd` is the number the pack is keyed by: on WPC DCS / WPC-S the
// 16-bit command, which the host splits into the two bytes the ROM would write.
// Works under both engines; with a ROM it comes on top of the ROM's own
// commands. Returns whether the command was handed over: false without
// AltSound's host, or when the game's hardware generation has no way to say
// that number (the host logs why).
//
// Out-of-range arguments are errors rather than a false: a command past 16 bits
// is a typo in the script, not a state of the machine.
int LuaRulesEngine::LuaSoundCommand(lua_State* L)
{
  auto* engine = FromLua(L);
  const lua_Integer cmd = luaL_checkinteger(L, 1);
  const lua_Integer board = luaL_optinteger(L, 2, 0);
  if (cmd < 0 || cmd > 0xFFFF)
  {
    return luaL_error(L, "ppuc.soundCommand: command %d is not 0x0000-0xFFFF", static_cast<int>(cmd));
  }
  if (board < 0 || board > 0xFF)
  {
    return luaL_error(L, "ppuc.soundCommand: board %d is not 0-255", static_cast<int>(board));
  }
  const bool sent = engine->m_soundCommandCallback &&
                    engine->m_soundCommandCallback(static_cast<int>(board), static_cast<uint32_t>(cmd));
  lua_pushboolean(L, sent ? 1 : 0);
  return 1;
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

namespace
{
// "RRGGBB" or "#RRGGBB", or a number 0xRRGGBB.
uint32_t CheckGiColor(lua_State* L, int index)
{
  if (lua_type(L, index) == LUA_TNUMBER)
  {
    const lua_Integer value = luaL_checkinteger(L, index);
    if (value < 0 || value > 0xFFFFFF)
    {
      luaL_error(L, "ppuc.setGiProgram: color %d is not 0xRRGGBB", static_cast<int>(value));
    }
    return static_cast<uint32_t>(value);
  }
  const char* text = luaL_checkstring(L, index);
  const char* hex = text[0] == '#' ? text + 1 : text;
  uint32_t color = 0;
  bool ok = std::strlen(hex) == 6;
  for (int i = 0; ok && i < 6; ++i)
  {
    const char c = hex[i];
    uint32_t nibble = 0;
    if (c >= '0' && c <= '9')
      nibble = static_cast<uint32_t>(c - '0');
    else if (c >= 'a' && c <= 'f')
      nibble = static_cast<uint32_t>(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F')
      nibble = static_cast<uint32_t>(c - 'A' + 10);
    else
      ok = false;
    color = (color << 4) | nibble;
  }
  if (!ok)
  {
    luaL_error(L, "ppuc.setGiProgram: color '%s' is not RRGGBB", text);
  }
  return color;
}
}  // namespace

// ppuc.setGiProgram(string, colors, effect, periodMs) -- the color of a GI
// string while custom modes run. colors is a list of "RRGGBB" (or a single
// one), highest priority first; effect "static" (default), "cycle", "fade" or
// "default"; periodMs the time per color for cycle and fade. The boards run
// the program themselves, so this is sent once, not per step. Returns false
// when the boards cannot show it (no boards, or firmware before 0.5.9).
int LuaRulesEngine::LuaSetGiProgram(lua_State* L)
{
  auto* engine = FromLua(L);
  const lua_Integer string = luaL_checkinteger(L, 1);
  if (string < 1 || string > 5)
  {
    return luaL_error(L, "ppuc.setGiProgram: GI string %d is not 1-5", static_cast<int>(string));
  }

  std::vector<uint32_t> colors;
  if (lua_istable(L, 2))
  {
    const lua_Integer count = static_cast<lua_Integer>(lua_rawlen(L, 2));
    for (lua_Integer i = 1; i <= count; ++i)
    {
      lua_rawgeti(L, 2, i);
      colors.push_back(CheckGiColor(L, lua_gettop(L)));
      lua_pop(L, 1);
    }
  }
  else if (!lua_isnoneornil(L, 2))
  {
    colors.push_back(CheckGiColor(L, 2));
  }

  const std::string effect = luaL_optstring(L, 3, "static");
  if (effect != "default" && effect != "static" && effect != "cycle" && effect != "fade")
  {
    return luaL_error(L, "ppuc.setGiProgram: effect '%s' is not default, static, cycle or fade", effect.c_str());
  }

  lua_Integer period = luaL_optinteger(L, 4, 0);
  if (period < 0)
  {
    period = 0;
  }
  if (period > 0xFFFF)
  {
    period = 0xFFFF;
  }

  bool accepted = false;
  if (engine->m_giProgramCallback)
  {
    accepted = engine->m_giProgramCallback(static_cast<int>(string), colors, effect, static_cast<uint16_t>(period));
  }
  lua_pushboolean(L, accepted ? 1 : 0);
  return 1;
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

// Only for the coil event being handled, like suppressSwitch. A rule cannot
// disable a coil from a distance: it answers for the activation in front of it
// and nothing else, so a rule that stops running leaves the machine whole.
int LuaRulesEngine::LuaSuppressCoil(lua_State* L)
{
  auto* engine = FromLua(L);
  const int number = static_cast<int>(luaL_checkinteger(L, 1));
  if (engine->m_currentEvent.type == EventType::Coil && engine->m_currentEvent.number == number)
  {
    engine->m_suppressCurrentCoil = true;
  }
  return 0;
}

int LuaRulesEngine::LuaIsButtonSwitch(lua_State* L)
{
  auto* engine = FromLua(L);
  const int number = static_cast<int>(luaL_checkinteger(L, 1));
  lua_pushboolean(L, engine->m_buttonSwitches.find(number) != engine->m_buttonSwitches.end() ? 1 : 0);
  return 1;
}

int LuaRulesEngine::LuaHoldBallSearch(lua_State* L)
{
  auto* engine = FromLua(L);
  if (engine->m_actionCallback)
  {
    const bool hold = lua_isnoneornil(L, 1) ? true : lua_toboolean(L, 1) != 0;
    engine->m_actionCallback(
        RulesAction{RulesActionType::HoldBallSearch, 0, hold ? static_cast<uint8_t>(1) : static_cast<uint8_t>(0), 0, 0,
                    0});
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

// ppuc.setCoil(number, on) -- hold a coil on until ppuc.setCoil(number, false).
//
// `on` is a boolean or a number (0 = off). For coils a rule keeps on for a
// while: a diverter, a gate, a motor. How the copper is driven while it is
// held -- power, hold power, maximum on-time -- stays the coil's configuration,
// which the board enforces.
int LuaRulesEngine::LuaSetCoil(lua_State* L)
{
  auto* engine = FromLua(L);
  const int number = static_cast<int>(luaL_checkinteger(L, 1));
  bool on;
  if (lua_isboolean(L, 2))
  {
    on = lua_toboolean(L, 2) != 0;
  }
  else
  {
    on = luaL_checkinteger(L, 2) != 0;
  }
  if (engine->m_actionCallback)
  {
    engine->m_actionCallback(RulesAction{RulesActionType::SetCoil, number,
                                         on ? static_cast<uint8_t>(1) : static_cast<uint8_t>(0), 0, 0, 0});
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

// ppuc.ballSave(durationMs) -- start or extend a ball save right now.
//
// Works under both engines and regardless of the configured start trigger,
// because ball save lives in the switch path rather than in the engine. A rule
// that has just started a mode can hand out a few seconds of protection with it.
// Extends an active save; never shortens one.
int LuaRulesEngine::LuaBallSave(lua_State* L)
{
  auto* engine = FromLua(L);
  if (engine->m_actionCallback)
  {
    engine->m_actionCallback(RulesAction{RulesActionType::GrantBallSave, 0, 0,
                                         static_cast<uint32_t>(luaL_checkinteger(L, 1)), 0, 0});
  }
  return 0;
}

// ---- Adjustments, machine memory, switch report --------------------------

static void PushStateValue(lua_State* L, const StateValue& value)
{
  switch (value.type)
  {
    case StateValue::Type::Number:
    {
      // An integer where it is one, so `== 3` and string.format("%d") behave.
      if (std::floor(value.number) == value.number && std::fabs(value.number) < 9.0e15)
      {
        lua_pushinteger(L, static_cast<lua_Integer>(value.number));
      }
      else
      {
        lua_pushnumber(L, value.number);
      }
      return;
    }
    case StateValue::Type::Boolean:
      lua_pushboolean(L, value.boolean ? 1 : 0);
      return;
    case StateValue::Type::String:
      lua_pushlstring(L, value.text.data(), value.text.size());
      return;
  }
  lua_pushnil(L);
}

// False for anything but a number, boolean or string.
static bool ToStateValue(lua_State* L, int index, StateValue* value)
{
  switch (lua_type(L, index))
  {
    case LUA_TNUMBER:
      *value = StateValue::Number(lua_tonumber(L, index));
      return true;
    case LUA_TBOOLEAN:
      *value = StateValue::Boolean(lua_toboolean(L, index) != 0);
      return true;
    case LUA_TSTRING:
    {
      size_t length = 0;
      const char* text = lua_tolstring(L, index, &length);
      *value = StateValue::String(std::string(text, length));
      return true;
    }
    default:
      return false;
  }
}

static std::string OptionalStringField(lua_State* L, int table, const char* field)
{
  lua_getfield(L, table, field);
  std::string text;
  if (lua_type(L, -1) == LUA_TSTRING)
  {
    text = lua_tostring(L, -1);
  }
  else if (!lua_isnil(L, -1))
  {
    lua_pop(L, 1);
    luaL_error(L, "ppuc.defineAdjustment: '%s' must be a string", field);
    return {};
  }
  lua_pop(L, 1);
  return text;
}

static bool OptionalNumberField(lua_State* L, int table, const char* field, double* out)
{
  lua_getfield(L, table, field);
  bool present = false;
  if (lua_type(L, -1) == LUA_TNUMBER)
  {
    *out = lua_tonumber(L, -1);
    present = true;
  }
  else if (!lua_isnil(L, -1))
  {
    lua_pop(L, 1);
    luaL_error(L, "ppuc.defineAdjustment: '%s' must be a number", field);
    return false;
  }
  lua_pop(L, 1);
  return present;
}

// ppuc.defineAdjustment{name = "gun_difficulty", label = "Gun difficulty",
//   default = "Normal", choices = {"Easy", "Normal", "Hard"}, about = "..."}
// ppuc.defineAdjustment{name = "ball_save", label = "Ball save", default = 6,
//   min = 0, max = 15, step = 1, unit = "s"}
// ppuc.defineAdjustment{name = "gun_broken", label = "Gun out of order",
//   default = false}
//
// Returns the value in force: the operator's, or the default. The kind follows
// from the default: a number, a boolean, or a string (which needs choices).
int LuaRulesEngine::LuaDefineAdjustment(lua_State* L)
{
  auto* engine = FromLua(L);
  luaL_checktype(L, 1, LUA_TTABLE);

  Adjustments::Definition d;
  d.name = OptionalStringField(L, 1, "name");
  d.label = OptionalStringField(L, 1, "label");
  d.about = OptionalStringField(L, 1, "about");
  d.unit = OptionalStringField(L, 1, "unit");
  d.group = "Game";
  if (d.label.empty())
  {
    d.label = d.name;
  }

  lua_getfield(L, 1, "default");
  if (!ToStateValue(L, -1, &d.defaultValue))
  {
    return luaL_error(L, "ppuc.defineAdjustment: '%s' needs a default (a number, true/false, or a choice)",
                      d.name.c_str());
  }
  lua_pop(L, 1);

  switch (d.defaultValue.type)
  {
    case StateValue::Type::Number:
    {
      d.kind = Adjustments::Kind::Number;
      d.min = d.defaultValue.number;
      d.max = d.defaultValue.number;
      OptionalNumberField(L, 1, "min", &d.min);
      OptionalNumberField(L, 1, "max", &d.max);
      OptionalNumberField(L, 1, "step", &d.step);
      break;
    }
    case StateValue::Type::Boolean:
      d.kind = Adjustments::Kind::Boolean;
      break;
    case StateValue::Type::String:
    {
      d.kind = Adjustments::Kind::Choice;
      lua_getfield(L, 1, "choices");
      if (lua_istable(L, -1))
      {
        const lua_Integer count = static_cast<lua_Integer>(lua_rawlen(L, -1));
        for (lua_Integer i = 1; i <= count; ++i)
        {
          lua_rawgeti(L, -1, i);
          if (lua_type(L, -1) != LUA_TSTRING)
          {
            return luaL_error(L, "ppuc.defineAdjustment: the choices of '%s' must be strings", d.name.c_str());
          }
          d.choices.emplace_back(lua_tostring(L, -1));
          lua_pop(L, 1);
        }
      }
      lua_pop(L, 1);
      break;
    }
  }

  if (engine->m_pAdjustments == nullptr)
  {
    if (!engine->m_ownAdjustments) engine->m_ownAdjustments = std::make_unique<Adjustments>();
    engine->m_pAdjustments = engine->m_ownAdjustments.get();
  }
  // A name PPUC already defines for the machine is the same setting, not a
  // second one: a game that declares "balls_per_game" reads PPUC's, and the
  // service menu shows it once.
  if (engine->m_pAdjustments->GroupOf(d.name) == "Machine")
  {
    PushStateValue(L, engine->m_pAdjustments->Get(d.name));
    return 1;
  }
  std::string error;
  if (!engine->m_pAdjustments->Define(d, &error))
  {
    return luaL_error(L, "ppuc.defineAdjustment: %s", error.c_str());
  }
  PushStateValue(L, engine->m_pAdjustments->Get(d.name));
  return 1;
}

// ppuc.adjustment(name): the value in force, or nil for a name nobody defined.
// Also reads PPUC's own machine adjustments ("ball_save_seconds", ...).
int LuaRulesEngine::LuaAdjustment(lua_State* L)
{
  auto* engine = FromLua(L);
  const std::string name = luaL_checkstring(L, 1);
  if (engine->m_pAdjustments == nullptr || !engine->m_pAdjustments->Has(name))
  {
    lua_pushnil(L);
    return 1;
  }
  PushStateValue(L, engine->m_pAdjustments->Get(name));
  return 1;
}

// ppuc.nvGet(name): what ppuc.nvSet stored, or nil.
int LuaRulesEngine::LuaNvGet(lua_State* L)
{
  auto* engine = FromLua(L);
  const std::string name = luaL_checkstring(L, 1);
  StateValue value;
  if (engine->m_pMemory == nullptr || !engine->m_pMemory->Get(name, &value))
  {
    lua_pushnil(L);
    return 1;
  }
  PushStateValue(L, value);
  return 1;
}

// ppuc.nvSet(name, value): a number, a boolean or a string, kept over power
// cycles; nil forgets it. Written to the USB stick by the main thread, never
// here: this runs on a rules thread.
int LuaRulesEngine::LuaNvSet(lua_State* L)
{
  auto* engine = FromLua(L);
  const std::string name = luaL_checkstring(L, 1);
  if (engine->m_pMemory == nullptr)
  {
    if (!engine->m_ownMemory) engine->m_ownMemory = std::make_unique<MachineMemory>();
    engine->m_pMemory = engine->m_ownMemory.get();
  }
  if (lua_isnoneornil(L, 2))
  {
    engine->m_pMemory->Remove(name);
    return 0;
  }
  StateValue value;
  if (!ToStateValue(L, 2, &value))
  {
    return luaL_error(L, "ppuc.nvSet: '%s' takes a number, a boolean or a string", name.c_str());
  }
  std::string error;
  if (!engine->m_pMemory->Set(name, value, &error))
  {
    return luaL_error(L, "ppuc.nvSet: %s", error.c_str());
  }
  return 0;
}

// ppuc.scoreboardSet(name, value): what a scoreboard text shows as
// {value.name}. A number is shown as a number; nil or "" takes it away. The
// scoreboard is drawn on another thread: this only hands the text over.
int LuaRulesEngine::LuaScoreboardSet(lua_State* L)
{
  auto* engine = FromLua(L);
  const char* name = luaL_checkstring(L, 1);
  std::string value;
  if (!lua_isnoneornil(L, 2))
  {
    if (lua_type(L, 2) == LUA_TBOOLEAN)
    {
      value = lua_toboolean(L, 2) ? "1" : "0";
    }
    else
    {
      size_t length = 0;
      const char* text = luaL_tolstring(L, 2, &length);
      value.assign(text, length);
      lua_pop(L, 1);
    }
  }
  if (engine->m_pScoreboardFeed != nullptr)
  {
    engine->m_pScoreboardFeed->SetValue(name, value);
  }
  return 0;
}

// MAJOR.MINOR.PATCH, numbers without leading zeros, then an optional
// -prerelease and +build of dot-separated [0-9A-Za-z-] parts (semver.org).
bool LuaRulesEngine::IsSemanticVersion(const std::string& version)
{
  size_t i = 0;
  const auto number = [&]() -> bool
  {
    const size_t start = i;
    while (i < version.size() && version[i] >= '0' && version[i] <= '9') ++i;
    if (i == start) return false;
    return !(version[start] == '0' && i - start > 1);
  };
  const auto identifiers = [&]() -> bool
  {
    bool any = false;
    while (true)
    {
      const size_t start = i;
      while (i < version.size() && (std::isalnum(static_cast<unsigned char>(version[i])) || version[i] == '-')) ++i;
      if (i == start) return false;
      any = true;
      if (i < version.size() && version[i] == '.')
      {
        ++i;
        continue;
      }
      return any;
    }
  };
  if (!number() || i >= version.size() || version[i++] != '.') return false;
  if (!number() || i >= version.size() || version[i++] != '.') return false;
  if (!number()) return false;
  if (i < version.size() && version[i] == '-')
  {
    ++i;
    if (!identifiers()) return false;
  }
  if (i < version.size() && version[i] == '+')
  {
    ++i;
    if (!identifiers()) return false;
  }
  return i == version.size();
}

LuaRulesEngine::GameIdentity LuaRulesEngine::GetGameIdentity() const
{
  std::lock_guard<std::mutex> lock(m_identityMutex);
  return m_identity;
}

// ppuc.gameVersion(name, version): the game says what it is, for the log and
// the service tools. The version is a semantic version; anything else is an
// error naming the line, so "0.1" or "v0.1.0" is caught on the bench.
int LuaRulesEngine::LuaGameVersion(lua_State* L)
{
  auto* engine = FromLua(L);
  const char* name = luaL_checkstring(L, 1);
  const char* version = luaL_checkstring(L, 2);
  if (!IsSemanticVersion(version))
  {
    return luaL_error(L, "ppuc.gameVersion: '%s' is not a semantic version (MAJOR.MINOR.PATCH, e.g. 0.1.0)",
                      version);
  }
  {
    std::lock_guard<std::mutex> lock(engine->m_identityMutex);
    engine->m_identity = GameIdentity{name, version};
  }
  printf("Game: %s %s\n", name, version);
  return 0;
}

// ppuc.scoreboardCallout(text) or ppuc.scoreboardCallout{text = ..., spot =
// "center", style = "ember", size = 120, ms = 1500, effect = "pop" | "fade" |
// "slide" | "shake", priority = 0}: a few words or a number over the scene for
// a moment. A number gets its thousands separators. Queued for the drawing
// thread like scoreboardSet; nothing is drawn here.
int LuaRulesEngine::LuaScoreboardCallout(lua_State* L)
{
  auto* engine = FromLua(L);
  scoreboard::Callout callout;
  auto textOf = [L](int index) -> std::string
  {
    if (lua_type(L, index) == LUA_TNUMBER)
    {
      const lua_Integer n = lua_isinteger(L, index) ? lua_tointeger(L, index)
                                                    : static_cast<lua_Integer>(lua_tonumber(L, index));
      return scoreboard::FormatScore(n < 0 ? 0 : static_cast<uint64_t>(n));
    }
    size_t length = 0;
    const char* text = luaL_tolstring(L, index, &length);
    std::string out(text, length);
    lua_pop(L, 1);
    return out;
  };
  if (lua_istable(L, 1))
  {
    lua_getfield(L, 1, "text");
    if (lua_isnil(L, -1)) return luaL_error(L, "scoreboardCallout: no text");
    callout.text = textOf(lua_gettop(L));
    lua_pop(L, 1);
    lua_getfield(L, 1, "spot");
    if (lua_isstring(L, -1)) callout.spot = lua_tostring(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 1, "style");
    if (lua_isstring(L, -1)) callout.style = lua_tostring(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 1, "size");
    if (lua_isnumber(L, -1)) callout.size = static_cast<int>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    lua_getfield(L, 1, "ms");
    if (lua_isnumber(L, -1)) callout.durationMs = static_cast<uint32_t>(std::max<lua_Integer>(1, lua_tointeger(L, -1)));
    lua_pop(L, 1);
    lua_getfield(L, 1, "priority");
    if (lua_isnumber(L, -1)) callout.priority = static_cast<int>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    lua_getfield(L, 1, "effect");
    if (lua_isstring(L, -1) && !scoreboard::Callout::ParseEffect(lua_tostring(L, -1), &callout.effect))
    {
      return luaL_error(L, "scoreboardCallout: effect must be pop, fade, slide, shake or brand");
    }
    lua_pop(L, 1);
  }
  else
  {
    callout.text = textOf(1);
  }
  if (engine->m_pScoreboardFeed != nullptr && !callout.text.empty())
  {
    engine->m_pScoreboardFeed->PushCallout(callout);
  }
  return 0;
}

// ppuc.excludeFromSwitchReport(n, ...): switches that may sit still for many
// games without being broken -- a coin switch on free play, a service button,
// a target only a wizard mode uses.
int LuaRulesEngine::LuaExcludeFromSwitchReport(lua_State* L)
{
  auto* engine = FromLua(L);
  const int count = lua_gettop(L);
  for (int i = 1; i <= count; ++i)
  {
    const int number = static_cast<int>(luaL_checkinteger(L, i));
    if (engine->m_pSwitchReport != nullptr)
    {
      engine->m_pSwitchReport->Ignore(number);
    }
  }
  return 0;
}
