#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
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

  void OnSwitchState(int number, uint8_t state);
  void OnLampState(int number, uint8_t state);
  void OnCoilState(int number, uint8_t state);
  void SetAttractMode(bool attractMode);

  // Internal types are public to keep parser implementation simple in .cpp.
  enum class EventType
  {
    Switch,
    Lamp,
    Coil
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
    uint64_t lastTriggeredMs = 0;
    std::unique_ptr<ExprNode> expression;
  };

 private:
  uint8_t GetState(const std::unordered_map<int, uint8_t>& states, int number) const;
  bool EvaluateExpression(const ExprNode* node, const TriggerEvent& event) const;
  void HandleStateChange(EventType type, int number, uint8_t state, std::unordered_map<int, uint8_t>& states);

  std::unordered_map<int, uint8_t> m_switchStates;
  std::unordered_map<int, uint8_t> m_lampStates;
  std::unordered_map<int, uint8_t> m_coilStates;
  std::vector<Rule> m_rules;
  TriggerCallback m_triggerCallback;
  bool m_attractMode = true;
  bool m_debug = false;
  mutable std::mutex m_mutex;
};
