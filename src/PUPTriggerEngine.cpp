#include "PUPTriggerEngine.h"

#include <ctype.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>
#include <chrono>

#include "io-boards/Event.h"

namespace
{
std::string Trim(const std::string& input)
{
  size_t start = 0;
  while (start < input.size() && ::isspace(static_cast<unsigned char>(input[start])))
  {
    start++;
  }

  size_t end = input.size();
  while (end > start && ::isspace(static_cast<unsigned char>(input[end - 1])))
  {
    end--;
  }

  return input.substr(start, end - start);
}

bool ParseUInt16Token(const std::string& token, uint16_t& out)
{
  if (token.empty())
  {
    return false;
  }

  int value = 0;
  for (char c : token)
  {
    if (!::isdigit(static_cast<unsigned char>(c)))
    {
      return false;
    }

    value = (value * 10) + (c - '0');
    if (value > 65535)
    {
      return false;
    }
  }

  out = static_cast<uint16_t>(value);
  return true;
}

bool ParseTriggerIdToken(const std::string& token, uint16_t& out)
{
  if (ParseUInt16Token(token, out))
  {
    return true;
  }

  if (token.empty())
  {
    return false;
  }

  for (char c : token)
  {
    const unsigned char uc = static_cast<unsigned char>(c);
    if (!(::isalnum(uc) || c == '_' || c == '-' || c == '.'))
    {
      return false;
    }
  }

  out = HashNamedTriggerId(token.c_str());
  return true;
}

bool ParseUInt8Token(const std::string& token, uint8_t& out)
{
  if (token.empty())
  {
    return false;
  }

  int value = 0;
  for (char c : token)
  {
    if (!::isdigit(static_cast<unsigned char>(c)))
    {
      return false;
    }

    value = (value * 10) + (c - '0');
    if (value > 255)
    {
      return false;
    }
  }

  out = static_cast<uint8_t>(value);
  return true;
}

bool ParseUInt32Token(const std::string& token, uint32_t& out)
{
  if (token.empty())
  {
    return false;
  }

  uint64_t value = 0;
  for (char c : token)
  {
    if (!::isdigit(static_cast<unsigned char>(c)))
    {
      return false;
    }

    value = (value * 10) + static_cast<uint64_t>(c - '0');
    if (value > 4294967295ULL)
    {
      return false;
    }
  }

  out = static_cast<uint32_t>(value);
  return true;
}

class TriggerExpressionParser
{
 public:
  explicit TriggerExpressionParser(const std::string& input) : m_input(input), m_position(0) {}

  std::unique_ptr<PUPTriggerEngine::ExprNode> Parse()
  {
    auto root = ParseOr();
    if (!root)
    {
      return nullptr;
    }

    SkipWhitespace();
    if (!IsEnd())
    {
      m_error = "unexpected trailing token";
      return nullptr;
    }

    return root;
  }

  const std::string& Error() const { return m_error; }

 private:
  std::unique_ptr<PUPTriggerEngine::ExprNode> ParseOr()
  {
    auto left = ParseAnd();
    if (!left)
    {
      return nullptr;
    }

    while (true)
    {
      SkipWhitespace();
      if (!Match("||"))
      {
        break;
      }

      auto right = ParseAnd();
      if (!right)
      {
        m_error = "expected expression after ||";
        return nullptr;
      }

      auto node = std::make_unique<PUPTriggerEngine::ExprNode>();
      node->type = PUPTriggerEngine::ExprNodeType::Or;
      node->left = std::move(left);
      node->right = std::move(right);
      left = std::move(node);
    }

    return left;
  }

  std::unique_ptr<PUPTriggerEngine::ExprNode> ParseAnd()
  {
    auto left = ParseUnary();
    if (!left)
    {
      return nullptr;
    }

    while (true)
    {
      SkipWhitespace();
      if (!Match("&&"))
      {
        break;
      }

      auto right = ParseUnary();
      if (!right)
      {
        m_error = "expected expression after &&";
        return nullptr;
      }

      auto node = std::make_unique<PUPTriggerEngine::ExprNode>();
      node->type = PUPTriggerEngine::ExprNodeType::And;
      node->left = std::move(left);
      node->right = std::move(right);
      left = std::move(node);
    }

    return left;
  }

  std::unique_ptr<PUPTriggerEngine::ExprNode> ParseUnary()
  {
    SkipWhitespace();
    if (Match("!"))
    {
      auto operand = ParseUnary();
      if (!operand)
      {
        m_error = "expected expression after !";
        return nullptr;
      }

      auto node = std::make_unique<PUPTriggerEngine::ExprNode>();
      node->type = PUPTriggerEngine::ExprNodeType::Not;
      node->left = std::move(operand);
      return node;
    }

    return ParsePrimary();
  }

  std::unique_ptr<PUPTriggerEngine::ExprNode> ParsePrimary()
  {
    SkipWhitespace();
    if (Match("("))
    {
      auto expression = ParseOr();
      if (!expression)
      {
        return nullptr;
      }

      SkipWhitespace();
      if (!Match(")"))
      {
        m_error = "missing closing )";
        return nullptr;
      }

      return expression;
    }

    std::string identifier = ParseIdentifier();
    if (identifier.empty())
    {
      m_error = "expected expression";
      return nullptr;
    }

    for (char& character : identifier)
    {
      character = static_cast<char>(::tolower(character));
    }

    if (identifier == "true" || identifier == "false")
    {
      auto node = std::make_unique<PUPTriggerEngine::ExprNode>();
      node->type = PUPTriggerEngine::ExprNodeType::Literal;
      node->literal = identifier == "true";
      return node;
    }

    if (identifier == "attract")
    {
      SkipWhitespace();
      if (Match("("))
      {
        SkipWhitespace();
        if (!Match(")"))
        {
          m_error = "missing ) in function 'attract'";
          return nullptr;
        }
      }

      auto node = std::make_unique<PUPTriggerEngine::ExprNode>();
      node->type = PUPTriggerEngine::ExprNodeType::AttractState;
      return node;
    }

    PUPTriggerEngine::ExprNodeType functionType;
    if (identifier == "ball")
    {
      functionType = PUPTriggerEngine::ExprNodeType::BallState;
    }
    else if (identifier == "switch")
    {
      functionType = PUPTriggerEngine::ExprNodeType::SwitchState;
    }
    else if (identifier == "lamp")
    {
      functionType = PUPTriggerEngine::ExprNodeType::LampState;
    }
    else if (identifier == "coil")
    {
      functionType = PUPTriggerEngine::ExprNodeType::CoilState;
    }
    else if (identifier == "switch_rising")
    {
      functionType = PUPTriggerEngine::ExprNodeType::SwitchRising;
    }
    else if (identifier == "switch_falling")
    {
      functionType = PUPTriggerEngine::ExprNodeType::SwitchFalling;
    }
    else if (identifier == "lamp_rising")
    {
      functionType = PUPTriggerEngine::ExprNodeType::LampRising;
    }
    else if (identifier == "lamp_falling")
    {
      functionType = PUPTriggerEngine::ExprNodeType::LampFalling;
    }
    else if (identifier == "coil_rising")
    {
      functionType = PUPTriggerEngine::ExprNodeType::CoilRising;
    }
    else if (identifier == "coil_falling")
    {
      functionType = PUPTriggerEngine::ExprNodeType::CoilFalling;
    }
    else
    {
      m_error = "unknown identifier '" + identifier + "'";
      return nullptr;
    }

    SkipWhitespace();
    if (!Match("("))
    {
      m_error = "missing ( after function '" + identifier + "'";
      return nullptr;
    }

    int number = 0;
    if (!ParseUnsignedNumber(number))
    {
      m_error = "expected numeric argument in function '" + identifier + "'";
      return nullptr;
    }

    SkipWhitespace();
    if (!Match(")"))
    {
      m_error = "missing ) in function '" + identifier + "'";
      return nullptr;
    }

    auto node = std::make_unique<PUPTriggerEngine::ExprNode>();
    node->type = functionType;
    node->number = number;
    return node;
  }

  std::string ParseIdentifier()
  {
    SkipWhitespace();
    const size_t start = m_position;
    while (!IsEnd())
    {
      const char c = m_input[m_position];
      if (::isalnum(static_cast<unsigned char>(c)) || c == '_')
      {
        m_position++;
      }
      else
      {
        break;
      }
    }

    if (m_position == start)
    {
      return "";
    }

    return m_input.substr(start, m_position - start);
  }

  bool ParseUnsignedNumber(int& out)
  {
    SkipWhitespace();
    if (IsEnd() || !::isdigit(static_cast<unsigned char>(m_input[m_position])))
    {
      return false;
    }

    int value = 0;
    while (!IsEnd() && ::isdigit(static_cast<unsigned char>(m_input[m_position])))
    {
      value = (value * 10) + (m_input[m_position] - '0');
      m_position++;
    }

    out = value;
    return true;
  }

  bool Match(const char* token)
  {
    size_t idx = 0;
    while (token[idx] != '\0')
    {
      if (m_position + idx >= m_input.size() || m_input[m_position + idx] != token[idx])
      {
        return false;
      }
      idx++;
    }

    m_position += idx;
    return true;
  }

  void SkipWhitespace()
  {
    while (!IsEnd() && ::isspace(static_cast<unsigned char>(m_input[m_position])))
    {
      m_position++;
    }
  }

  bool IsEnd() const { return m_position >= m_input.size(); }

  std::string m_input;
  size_t m_position;
  std::string m_error;
};
}  // namespace

bool PUPTriggerEngine::LoadRules(const char* path, std::string& error)
{
  std::ifstream input(path);
  if (!input.is_open())
  {
    error = "Unable to open PUP trigger file";
    return false;
  }

  std::vector<Rule> loadedRules;
  std::string line;
  size_t lineNo = 0;

  while (std::getline(input, line))
  {
    lineNo++;
    const size_t comment = line.find('#');
    if (comment != std::string::npos)
    {
      line = line.substr(0, comment);
    }

    line = Trim(line);
    if (line.empty())
    {
      continue;
    }

    const size_t separator = line.find(':');
    if (separator == std::string::npos)
    {
      error = "Invalid PUP trigger line " + std::to_string(lineNo) + ": missing ':' separator.";
      return false;
    }

    const std::string triggerPart = Trim(line.substr(0, separator));
    const std::string expressionPart = Trim(line.substr(separator + 1));
    if (triggerPart.empty() || expressionPart.empty())
    {
      error = "Invalid PUP trigger line " + std::to_string(lineNo) + ": missing trigger or expression.";
      return false;
    }

    std::istringstream triggerStream(triggerPart);
    std::vector<std::string> tokens;
    std::string token;
    while (triggerStream >> token)
    {
      tokens.push_back(token);
    }

    if (tokens.size() < 2)
    {
      error = "Invalid PUP trigger line " + std::to_string(lineNo) +
              ": expected '<source> <id> [value] : <expression>'.";
      return false;
    }

    const std::string& sourceToken = tokens[0];
    const std::string& idToken = tokens[1];

    if (sourceToken.size() != 1)
    {
      error = "Invalid PUP trigger line " + std::to_string(lineNo) + ": source must be a single character.";
      return false;
    }

    Rule rule;
    rule.source = sourceToken[0];
    rule.line = lineNo;

    if (!ParseTriggerIdToken(idToken, rule.id))
    {
      error = "Invalid PUP trigger line " + std::to_string(lineNo) + ": id must be uint16 or a named token.";
      return false;
    }

    bool hasValue = false;
    for (size_t i = 2; i < tokens.size(); i++)
    {
      const std::string& option = tokens[i];
      if (option.rfind("cooldown=", 0) == 0)
      {
        const std::string msToken = option.substr(9);
        if (!ParseUInt32Token(msToken, rule.cooldownMs))
        {
          error = "Invalid PUP trigger line " + std::to_string(lineNo) + ": cooldown must be uint32 milliseconds.";
          return false;
        }
        continue;
      }

      if (option.rfind("delay=", 0) == 0)
      {
        const std::string msToken = option.substr(6);
        if (!ParseUInt32Token(msToken, rule.delayMs))
        {
          error = "Invalid PUP trigger line " + std::to_string(lineNo) + ": delay must be uint32 milliseconds.";
          return false;
        }
        continue;
      }

      if (!hasValue)
      {
        if (!ParseUInt8Token(option, rule.value))
        {
          error = "Invalid PUP trigger line " + std::to_string(lineNo) + ": value must be uint8.";
          return false;
        }
        hasValue = true;
        continue;
      }

      error = "Invalid PUP trigger line " + std::to_string(lineNo) + ": unknown option '" + option + "'.";
      return false;
    }

    TriggerExpressionParser parser(expressionPart);
    rule.expression = parser.Parse();
    if (!rule.expression)
    {
      error = "Invalid PUP trigger expression on line " + std::to_string(lineNo) + ": " + parser.Error();
      return false;
    }

    rule.eventTriggered = UsesEventEdges(rule.expression.get());

    loadedRules.push_back(std::move(rule));
  }

  std::lock_guard<std::mutex> lock(m_mutex);
  m_rules = std::move(loadedRules);
  m_switchStates.clear();
  m_lampStates.clear();
  m_coilStates.clear();
  m_currentBall = 0;
  return true;
}

void PUPTriggerEngine::SetDebug(bool debug)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_debug = debug;
}

void PUPTriggerEngine::SetTriggerCallback(TriggerCallback callback)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_triggerCallback = std::move(callback);
}

void PUPTriggerEngine::SetAttractMode(bool attractMode)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_attractMode = attractMode;
}

size_t PUPTriggerEngine::GetRuleCount() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_rules.size();
}

void PUPTriggerEngine::Update()
{
  const uint64_t nowMs = GetNowMs();
  std::vector<std::tuple<char, uint16_t, uint8_t, size_t>> matched;
  TriggerCallback triggerCallback;
  bool debug = false;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    CollectDueTriggers(nowMs, matched);
    triggerCallback = m_triggerCallback;
    debug = m_debug;
  }

  for (const auto& match : matched)
  {
    if (debug)
    {
      printf("PUP trigger matched (line=%zu): source=%c id=%u value=%u\n", std::get<3>(match), std::get<0>(match),
             std::get<1>(match), std::get<2>(match));
    }

    if (triggerCallback)
    {
      triggerCallback(std::get<0>(match), std::get<1>(match), std::get<2>(match));
    }
  }
}

uint8_t PUPTriggerEngine::GetState(const std::unordered_map<int, uint8_t>& states, int number) const
{
  const auto it = states.find(number);
  return it == states.end() ? 0 : it->second;
}

bool PUPTriggerEngine::EvaluateExpression(const ExprNode* node, const TriggerEvent& event) const
{
  if (!node)
  {
    return false;
  }

  switch (node->type)
  {
    case ExprNodeType::Literal:
      return node->literal;
    case ExprNodeType::AttractState:
      return m_attractMode;
    case ExprNodeType::Not:
      return !EvaluateExpression(node->left.get(), event);
    case ExprNodeType::And:
      return EvaluateExpression(node->left.get(), event) && EvaluateExpression(node->right.get(), event);
    case ExprNodeType::Or:
      return EvaluateExpression(node->left.get(), event) || EvaluateExpression(node->right.get(), event);
    case ExprNodeType::BallState:
      return m_currentBall == static_cast<uint8_t>(node->number);
    case ExprNodeType::SwitchState:
      return GetState(m_switchStates, node->number) != 0;
    case ExprNodeType::LampState:
      return GetState(m_lampStates, node->number) != 0;
    case ExprNodeType::CoilState:
      return GetState(m_coilStates, node->number) != 0;
    case ExprNodeType::SwitchRising:
      return event.type == EventType::Switch && event.number == node->number && event.oldValue == 0 &&
             event.newValue != 0;
    case ExprNodeType::SwitchFalling:
      return event.type == EventType::Switch && event.number == node->number && event.oldValue != 0 &&
             event.newValue == 0;
    case ExprNodeType::LampRising:
      return event.type == EventType::Lamp && event.number == node->number && event.oldValue == 0 &&
             event.newValue != 0;
    case ExprNodeType::LampFalling:
      return event.type == EventType::Lamp && event.number == node->number && event.oldValue != 0 &&
             event.newValue == 0;
    case ExprNodeType::CoilRising:
      return event.type == EventType::Coil && event.number == node->number && event.oldValue == 0 &&
             event.newValue != 0;
    case ExprNodeType::CoilFalling:
      return event.type == EventType::Coil && event.number == node->number && event.oldValue != 0 &&
             event.newValue == 0;
  }

  return false;
}

bool PUPTriggerEngine::UsesEventEdges(const ExprNode* node) const
{
  if (!node)
  {
    return false;
  }

  switch (node->type)
  {
    case ExprNodeType::SwitchRising:
    case ExprNodeType::SwitchFalling:
    case ExprNodeType::LampRising:
    case ExprNodeType::LampFalling:
    case ExprNodeType::CoilRising:
    case ExprNodeType::CoilFalling:
      return true;
    default:
      return UsesEventEdges(node->left.get()) || UsesEventEdges(node->right.get());
  }
}

uint64_t PUPTriggerEngine::GetNowMs() const
{
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

bool PUPTriggerEngine::CanTriggerNow(const Rule& rule, uint64_t nowMs) const
{
  return rule.cooldownMs == 0 || nowMs >= (rule.lastTriggeredMs + static_cast<uint64_t>(rule.cooldownMs));
}

void PUPTriggerEngine::CollectDueTriggers(uint64_t nowMs,
                                          std::vector<std::tuple<char, uint16_t, uint8_t, size_t>>& matched)
{
  for (auto& rule : m_rules)
  {
    if (!rule.pending || nowMs < rule.pendingTriggerMs)
    {
      continue;
    }

    if (!CanTriggerNow(rule, nowMs))
    {
      continue;
    }

    rule.lastTriggeredMs = nowMs;
    rule.pending = false;
    rule.pendingTriggerMs = 0;
    matched.emplace_back(rule.source, rule.id, rule.value, rule.line);
  }
}

void PUPTriggerEngine::HandleStateChange(EventType type, int number, uint8_t state, std::unordered_map<int, uint8_t>& states)
{
  const uint64_t nowMs = GetNowMs();
  std::vector<std::tuple<char, uint16_t, uint8_t, size_t>> matched;
  TriggerCallback triggerCallback;
  bool debug = false;
  {
    std::lock_guard<std::mutex> lock(m_mutex);

    const uint8_t oldState = GetState(states, number);
    states[number] = state;

    const TriggerEvent event = {type, number, oldState, state};
    for (auto& rule : m_rules)
    {
      const bool expressionMatched = EvaluateExpression(rule.expression.get(), event);

      if (rule.delayMs > 0)
      {
        if (rule.eventTriggered)
        {
          if (expressionMatched && !rule.pending)
          {
            rule.pending = true;
            rule.pendingTriggerMs = nowMs + static_cast<uint64_t>(rule.delayMs);
          }
        }
        else
        {
          if (expressionMatched)
          {
            if (!rule.conditionActive)
            {
              rule.conditionActive = true;
              if (!rule.pending)
              {
                rule.pending = true;
                rule.pendingTriggerMs = nowMs + static_cast<uint64_t>(rule.delayMs);
              }
            }
          }
          else
          {
            rule.conditionActive = false;
            rule.pending = false;
            rule.pendingTriggerMs = 0;
          }
        }
        continue;
      }

      if (!expressionMatched)
      {
        continue;
      }

      if (!CanTriggerNow(rule, nowMs))
      {
        continue;
      }

      rule.lastTriggeredMs = nowMs;
      matched.emplace_back(rule.source, rule.id, rule.value, rule.line);
    }

    CollectDueTriggers(nowMs, matched);

    triggerCallback = m_triggerCallback;
    debug = m_debug;
  }

  for (const auto& match : matched)
  {
    if (debug)
    {
      printf("PUP trigger matched (line=%zu): source=%c id=%u value=%u\n", std::get<3>(match), std::get<0>(match),
             std::get<1>(match), std::get<2>(match));
    }

    if (triggerCallback)
    {
      triggerCallback(std::get<0>(match), std::get<1>(match), std::get<2>(match));
    }
  }
}

void PUPTriggerEngine::HandleBallChange(uint8_t currentBall)
{
  const uint64_t nowMs = GetNowMs();
  std::vector<std::tuple<char, uint16_t, uint8_t, size_t>> matched;
  TriggerCallback triggerCallback;
  bool debug = false;
  {
    std::lock_guard<std::mutex> lock(m_mutex);

    const uint8_t oldBall = m_currentBall;
    if (oldBall == currentBall)
    {
      return;
    }

    m_currentBall = currentBall;

    const TriggerEvent event = {EventType::Ball, static_cast<int>(currentBall), oldBall, currentBall};
    for (auto& rule : m_rules)
    {
      const bool expressionMatched = EvaluateExpression(rule.expression.get(), event);

      if (rule.delayMs > 0)
      {
        if (rule.eventTriggered)
        {
          if (expressionMatched && !rule.pending)
          {
            rule.pending = true;
            rule.pendingTriggerMs = nowMs + static_cast<uint64_t>(rule.delayMs);
          }
        }
        else
        {
          if (expressionMatched)
          {
            if (!rule.conditionActive)
            {
              rule.conditionActive = true;
              if (!rule.pending)
              {
                rule.pending = true;
                rule.pendingTriggerMs = nowMs + static_cast<uint64_t>(rule.delayMs);
              }
            }
          }
          else
          {
            rule.conditionActive = false;
            rule.pending = false;
            rule.pendingTriggerMs = 0;
          }
        }
        continue;
      }

      if (!expressionMatched)
      {
        continue;
      }

      if (!CanTriggerNow(rule, nowMs))
      {
        continue;
      }

      rule.lastTriggeredMs = nowMs;
      matched.emplace_back(rule.source, rule.id, rule.value, rule.line);
    }

    CollectDueTriggers(nowMs, matched);

    triggerCallback = m_triggerCallback;
    debug = m_debug;
  }

  for (const auto& match : matched)
  {
    if (debug)
    {
      printf("PUP trigger matched (line=%zu): source=%c id=%u value=%u\n", std::get<3>(match), std::get<0>(match),
             std::get<1>(match), std::get<2>(match));
    }

    if (triggerCallback)
    {
      triggerCallback(std::get<0>(match), std::get<1>(match), std::get<2>(match));
    }
  }
}

void PUPTriggerEngine::OnSwitchState(int number, uint8_t state)
{
  HandleStateChange(EventType::Switch, number, state == 0 ? 0 : 1, m_switchStates);
}

void PUPTriggerEngine::OnLampState(int number, uint8_t state)
{
  HandleStateChange(EventType::Lamp, number, state == 0 ? 0 : 1, m_lampStates);
}

void PUPTriggerEngine::OnCoilState(int number, uint8_t state)
{
  HandleStateChange(EventType::Coil, number, state == 0 ? 0 : 1, m_coilStates);
}

void PUPTriggerEngine::SetCurrentBall(uint8_t currentBall)
{
  HandleBallChange(currentBall);
}
