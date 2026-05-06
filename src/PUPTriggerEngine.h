#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

class PUPTriggerEngine
{
 public:
  using TriggerCallback = std::function<void(char source, uint16_t id, uint8_t value)>;

  void SetDebug(bool debug);
  void SetTriggerCallback(TriggerCallback callback);
  bool LoadRules(const char* path, std::string& error);
  size_t GetRuleCount() const;
  void Update();

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
    Player
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
    SwitchRising,
    SwitchFalling,
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
  uint64_t GetNowMs() const;
  bool CanTriggerNow(const Rule& rule, uint64_t nowMs) const;
  void CollectDueTriggers(uint64_t nowMs, std::vector<MatchedTrigger>& matched);
  void RecordMatchedTriggers(const std::vector<MatchedTrigger>& matched, uint64_t nowMs);
  void PruneHistoryLocked(uint64_t nowMs);
  void ClearPlayerHistoryLocked(uint8_t player);
  void HandleStateChange(EventType type, int number, uint8_t state, std::unordered_map<int, uint8_t>& states);
  void HandleBallChange(uint8_t currentBall);
  void HandlePlayerChange(uint8_t currentPlayer);

  std::unordered_map<int, uint8_t> m_switchStates;
  std::unordered_map<int, uint8_t> m_lampStates;
  std::unordered_map<int, uint8_t> m_coilStates;
  std::vector<Rule> m_rules;
  std::deque<HistoryEntry> m_history;
  TriggerCallback m_triggerCallback;
  uint8_t m_currentBall = 0;
  uint8_t m_currentPlayer = 0;
  bool m_attractMode = true;
  bool m_debug = false;
  mutable std::mutex m_mutex;
};
