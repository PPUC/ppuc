#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "LuaArg.h"
#include "RulesAction.h"

class GameCore;
class DmdCanvas;
class Adjustments;
class MachineMemory;
class SwitchReport;
namespace scoreboard
{
class Feed;
}

struct lua_State;

class LuaRulesEngine
{
 public:
  using TriggerCallback = std::function<void(char source, uint16_t id, uint8_t value)>;
  using SpeechCallback = std::function<void(const std::string& text)>;
  // Returns whether the sound started.
  using SoundCallback = std::function<bool(const std::string& file)>;
  // Start a scene in the Serum colorization. Separate from TriggerCallback
  // because a 'D' trigger and a scene are two different requests to two
  // different consumers that happen to share a numbering space: the trigger
  // drives a PUP pack's media, the scene drives the DMD. A pack can want one
  // without the other, so a script has to be able to ask for one without the
  // other.
  using SerumSceneCallback = std::function<void(uint16_t id)>;
  // A sound command for the AltSound pack, as if the CPU board had sent it to
  // sound board `board`. The command is the number the pack is keyed by (the
  // 16-bit DCS command on WPC DCS / WPC-S); splitting it into the bytes the
  // ROM would write is the host's job. Returns whether it was handed over.
  using SoundCommandCallback = std::function<bool(int board, uint32_t cmd)>;
  using ActionCallback = std::function<void(const RulesAction& action)>;
  // The color program of one GI string, from ppuc.setGiProgram. The arguments
  // are checked already: string 1-5, colors 0xRRGGBB, effect one of
  // "default", "static", "cycle", "fade". Returns whether the boards will show
  // it, which the script gets back.
  using GiProgramCallback = std::function<bool(int string, const std::vector<uint32_t>& colors,
                                               const std::string& effect, uint16_t periodMs)>;

  // Source of the millisecond timestamp used for named-state expiry, trigger
  // history windows, onlyOnceEvery and ppuc.after scheduling. Defaults to a
  // steady clock; injectable so that time-dependent rule behaviour can be
  // tested deterministically instead of with sleeps.
  using ClockFn = std::function<uint64_t()>;

  struct SwitchProcessResult
  {
    bool forwardToCpu = true;
  };

  // What the rules decided about a coil the engine just asked for.
  //
  // A rule that takes over a coil has to answer before the board is driven,
  // which is why coil events now reach the rules ahead of the interceptor
  // rather than after it. Flash's multiball needs exactly this: the ROM fires
  // the eject hole the instant the ball arrives, and the rule has to put that
  // ball's neighbour into the shooter lane instead.
  struct CoilProcessResult
  {
    bool forwardToBoard = true;
  };

  LuaRulesEngine();
  ~LuaRulesEngine();

  LuaRulesEngine(const LuaRulesEngine&) = delete;
  LuaRulesEngine& operator=(const LuaRulesEngine&) = delete;

  void SetDebug(bool debug);
  void SetTriggerCallback(TriggerCallback callback);
  void SetSpeechCallback(SpeechCallback callback);
  void SetSoundCallback(SoundCallback callback);
  void SetSerumSceneCallback(SerumSceneCallback callback);
  void SetSoundCommandCallback(SoundCommandCallback callback);
  void SetActionCallback(ActionCallback callback);
  void SetGiProgramCallback(GiProgramCallback callback);
  // Overrides the clock. Passing an empty function restores the default.
  void SetClock(ClockFn clock);
  void SetSwitchGroups(const std::unordered_map<std::string, std::vector<uint16_t>>& switchGroups);
  // Attaches the ROM-less game and its display. Both must be set before
  // LoadScripts: `ppuc.game` and `ppuc.dmd` are registered only when present, and
  // `ppuc.game == nil` is the documented feature test for a script that wants to
  // run under either engine.
  //
  // Deliberately not stubbed with no-ops in ROM mode: a silent no-op is worse
  // than an error naming the line.
  void SetGameCore(GameCore* pGameCore);
  void SetDmdCanvas(DmdCanvas* pCanvas);
  // The operator's adjustments (ppuc.defineAdjustment, ppuc.adjustment), the
  // rules' machine memory (ppuc.nvGet, ppuc.nvSet) and the switch report
  // (ppuc.excludeFromSwitchReport). Without them the functions still exist:
  // an adjustment then reads as its default and machine memory forgets on
  // a restart, so a game runs the same on a bench without a game folder.
  void SetAdjustments(Adjustments* pAdjustments);
  void SetMachineMemory(MachineMemory* pMemory);
  void SetSwitchReport(SwitchReport* pReport);
  // Where ppuc.scoreboardSet(name, value) goes. Without a scoreboard the
  // function exists and does nothing, so one set of rules runs on a machine
  // with a scoreboard and on one without.
  void SetScoreboardFeed(scoreboard::Feed* pFeed);
  GameCore* GetGameCore() const { return m_pGameCore; }
  DmdCanvas* GetDmdCanvas() const { return m_pDmdCanvas; }

  // Dispatches a GameCore event to `ppuc.on<Name>` handlers.
  void CallGameHandler(const char* name, std::vector<LuaArg> args = {});

  // Asks the script for permission. An absent handler returns `defaultResult`:
  // credit policy is a rules decision, but having no rules must never make the
  // machine unplayable.
  bool CallQueryHandler(const char* name, std::vector<LuaArg> args, bool defaultResult);

  bool LoadScript(const char* path, std::string& error);
  bool LoadScripts(const std::vector<std::string>& paths, std::string& error);
  void Update();

  SwitchProcessResult ProcessSwitchState(int number, uint8_t state);
  void OnLampState(int number, uint8_t state);
  // Replaces OnCoilState: same notification, but its answer decides whether
  // the coil reaches the board.
  CoilProcessResult ProcessCoilState(int number, uint8_t state);

  // The switches the configuration marks as buttons -- flippers, coin door,
  // service. Rules ask so that "the player hit something on the playfield" can
  // be told from "the player pressed a button", using the same definition the
  // ball search already uses rather than a second list that can disagree.
  void SetButtonSwitches(std::unordered_set<int> buttons);
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
  bool CallHandler(const char* name, std::vector<LuaArg> args = {});
  bool CallRegisteredHandler(int handlerRef, const char* name, const std::vector<LuaArg>& args);
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
  static int LuaPlaySound(lua_State* L);
  static int LuaSerumScene(lua_State* L);
  static int LuaSoundCommand(lua_State* L);
  static int LuaEffectTrigger(lua_State* L);
  static int LuaSuppressSwitch(lua_State* L);
  static int LuaSuppressCoil(lua_State* L);
  static int LuaIsButtonSwitch(lua_State* L);
  static int LuaHoldBallSearch(lua_State* L);
  static int LuaSendSwitchToCpu(lua_State* L);
  static int LuaPulseCoil(lua_State* L);
  static int LuaSetCoil(lua_State* L);
  static int LuaBlinkLamp(lua_State* L);
  static int LuaStopBlinkLamp(lua_State* L);
  static int LuaBallSave(lua_State* L);
  static int LuaSetGiProgram(lua_State* L);
  static int LuaDefineAdjustment(lua_State* L);
  static int LuaAdjustment(lua_State* L);
  static int LuaNvGet(lua_State* L);
  static int LuaNvSet(lua_State* L);
  static int LuaExcludeFromSwitchReport(lua_State* L);
  static int LuaScoreboardSet(lua_State* L);

  lua_State* m_lua = nullptr;
  std::unordered_map<int, uint8_t> m_switchStates;
  std::unordered_map<int, uint8_t> m_lampStates;
  std::unordered_map<int, uint8_t> m_coilStates;
  std::unordered_map<std::string, uint64_t> m_namedStates;
  std::unordered_map<std::string, std::vector<uint16_t>> m_switchGroups;
  std::unordered_map<std::string, std::vector<int>> m_handlers;
  std::unordered_set<int> m_suppressedSwitchOpen;
  std::unordered_set<int> m_buttonSwitches;
  bool m_suppressCurrentCoil = false;
  std::deque<HistoryEntry> m_history;
  std::deque<ScheduledCallback> m_scheduledCallbacks;
  CurrentEvent m_currentEvent;
  TriggerCallback m_triggerCallback;
  SpeechCallback m_speechCallback;
  SoundCallback m_soundCallback;
  SerumSceneCallback m_serumSceneCallback;
  SoundCommandCallback m_soundCommandCallback;
  ActionCallback m_actionCallback;
  GiProgramCallback m_giProgramCallback;
  ClockFn m_clock;
  uint8_t m_currentBall = 0;
  uint8_t m_currentPlayer = 0;
  bool m_attractMode = true;
  bool m_debug = false;
  bool m_fatalError = false;
  std::string m_fatalErrorMessage;
  uint64_t m_nextScheduledSequence = 0;
  GameCore* m_pGameCore = nullptr;
  DmdCanvas* m_pDmdCanvas = nullptr;
  Adjustments* m_pAdjustments = nullptr;
  MachineMemory* m_pMemory = nullptr;
  SwitchReport* m_pSwitchReport = nullptr;
  scoreboard::Feed* m_pScoreboardFeed = nullptr;
  // What the rules defined when there is no store, so ppuc.adjustment still
  // answers with the default and machine memory lasts for the session.
  std::unique_ptr<Adjustments> m_ownAdjustments;
  std::unique_ptr<MachineMemory> m_ownMemory;
  // Recursive because a Lua handler may call ppuc.game.* which re-enters through
  // a query handler -- ppuc.game.startGame() reaching canStartGame is the
  // obvious case. Recursive costs nothing and removes a class of deadlock that
  // is very hard to reproduce.
  //
  // The lock is far from vestigial: rules run on the switch thread (switches,
  // timers, a ROM-less engine's events), on PinMAME's coil poll thread
  // (onCoilChanged) and on the main thread (PinMAME's lamps, ball and player).
  // Whoever holds it holds up the other two, which is why a callback out of
  // a rule never waits for a disk, a screen or the plugin bus: those are
  // posted to the main thread instead (see ppuc.cpp).
  mutable std::recursive_mutex m_mutex;
};
