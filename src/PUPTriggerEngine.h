#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class PUPTriggerEngine
{
 public:
  using TriggerCallback = std::function<void(char source, uint16_t id, uint8_t value)>;

  enum class ActionType
  {
    PulseCoil,
    StartBlinkLamp,
    StopBlinkLamp
  };

  struct RuleAction
  {
    ActionType type;
    int number = 0;
    uint32_t durationMs = 0;
    uint32_t onMs = 0;
    uint32_t offMs = 0;
  };

  struct SwitchProcessResult
  {
    bool forwardToCpu = true;
  };

  using ActionCallback = std::function<void(const RuleAction& action)>;

  void SetDebug(bool debug);
  void SetTriggerCallback(TriggerCallback callback);
  void SetActionCallback(ActionCallback callback);
  void SetSwitchGroups(const std::unordered_map<std::string, std::vector<uint16_t>>& switchGroups);
  bool LoadRules(const char* path, std::string& error);
  size_t GetRuleCount() const;
  void Update();

  SwitchProcessResult ProcessSwitchState(int number, uint8_t state);
  void OnSwitchState(int number, uint8_t state);
  void OnLampState(int number, uint8_t state);
  void OnCoilState(int number, uint8_t state);
  void SetCurrentBall(uint8_t currentBall);
  void SetCurrentPlayer(uint8_t currentPlayer);
  void SetAttractMode(bool attractMode);

  // Internal types are public to keep parser implementation simple in .cpp.
  enum class EventType
  {
    Switch,
    Lamp,
    Coil,
    Ball,
    Player,
    Timer
  };

  struct TriggerEvent
  {
    EventType type;
    int number;
    uint8_t oldValue;
    uint8_t newValue;
  };

  enum class ExprNodeType
  {
    Literal,
    AttractState,
    Not,
    And,
    Or,
    BallState,
    PlayerState,
    HistoryState,
    SequenceState,
    SwitchState,
    LampState,
    CoilState,
    NamedState,
    SwitchGroupState,
    SwitchRising,
    SwitchFalling,
    SwitchRisingGroup,
    SwitchFallingGroup,
    LampRising,
    LampFalling,
    CoilRising,
    CoilFalling
  };

  struct ExprNode
  {
    ExprNodeType type;
    int number = 0;
    bool literal = false;
    uint32_t windowMs = 0;
    std::string name;
    std::vector<int> numbers;
    std::vector<uint16_t> triggerIds;
    std::unique_ptr<ExprNode> left;
    std::unique_ptr<ExprNode> right;
  };

  struct Rule
  {
    char source = 'P';
    uint16_t id = 0;
    uint8_t value = 1;
    size_t line = 0;
    uint32_t cooldownMs = 0;
    uint32_t delayMs = 0;
    uint64_t lastTriggeredMs = 0;
    uint64_t pendingTriggerMs = 0;
    bool pending = false;
    bool conditionActive = false;
    bool eventTriggered = false;
    uint8_t setPlayer = 0;
    uint8_t clearPlayerHistory = 0;
    std::string setState;
    std::string clearState;
    uint32_t stateMs = 0;
    int suppressSwitch = -1;
    int pulseCoil = -1;
    uint32_t pulseMs = 120;
    int blinkLamp = -1;
    uint32_t blinkOnMs = 250;
    uint32_t blinkOffMs = 250;
    bool blinkActive = false;
    std::unique_ptr<ExprNode> expression;
  };

  struct MatchedTrigger
  {
    char source = 'P';
    uint16_t id = 0;
    uint8_t value = 1;
    size_t line = 0;
    uint8_t player = 0;
    uint8_t ball = 0;
  };

  struct HistoryEntry
  {
    char source = 'P';
    uint16_t id = 0;
    uint8_t value = 1;
    uint8_t player = 0;
    uint64_t timestampMs = 0;
  };

 private:
  uint8_t GetState(const std::unordered_map<int, uint8_t>& states, int number) const;
  bool EvaluateExpression(const ExprNode* node, const TriggerEvent& event) const;
  bool UsesEventEdges(const ExprNode* node) const;
  bool HistoryContains(uint16_t triggerId, uint32_t windowMs, uint64_t nowMs) const;
  bool SequenceOccurred(const std::vector<uint16_t>& triggerIds, uint32_t windowMs, uint64_t nowMs) const;
  bool NamedStateActive(const std::string& name, uint64_t nowMs) const;
  bool SwitchGroupStateActive(const std::string& name) const;
  bool SwitchGroupContainsEvent(const std::string& name, const TriggerEvent& event) const;
  uint64_t GetNowMs() const;
  bool CanTriggerNow(const Rule& rule, uint64_t nowMs) const;
  void CollectDueTriggers(uint64_t nowMs, std::vector<MatchedTrigger>& matched, std::vector<RuleAction>& actions);
  void CollectBlinkActions(uint64_t nowMs, const TriggerEvent& event, std::vector<RuleAction>& actions);
  void RecordMatchedTriggers(const std::vector<MatchedTrigger>& matched, uint64_t nowMs);
  void ApplyRuleSideEffects(Rule& rule, uint64_t nowMs, std::vector<RuleAction>& actions);
  void PruneHistoryLocked(uint64_t nowMs);
  void PruneNamedStatesLocked(uint64_t nowMs);
  void ClearPlayerHistoryLocked(uint8_t player);
  SwitchProcessResult HandleStateChange(EventType type, int number, uint8_t state, std::unordered_map<int, uint8_t>& states);
  void HandleBallChange(uint8_t currentBall);
  void HandlePlayerChange(uint8_t currentPlayer);
  void DispatchMatchesAndActions(const std::vector<MatchedTrigger>& matched,
                                 const std::vector<RuleAction>& actions,
                                 TriggerCallback triggerCallback,
                                 ActionCallback actionCallback,
                                 bool debug);

  std::unordered_map<int, uint8_t> m_switchStates;
  std::unordered_map<int, uint8_t> m_lampStates;
  std::unordered_map<int, uint8_t> m_coilStates;
  std::unordered_map<std::string, uint64_t> m_namedStates;
  std::unordered_map<std::string, std::vector<uint16_t>> m_switchGroups;
  std::unordered_set<int> m_suppressedSwitchOpen;
  std::vector<Rule> m_rules;
  std::deque<HistoryEntry> m_history;
  TriggerCallback m_triggerCallback;
  ActionCallback m_actionCallback;
  uint8_t m_currentBall = 0;
  uint8_t m_currentPlayer = 0;
  bool m_attractMode = true;
  bool m_debug = false;
  mutable std::mutex m_mutex;
};
