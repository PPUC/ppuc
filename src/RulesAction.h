#pragma once

#include <cstdint>

enum class RulesActionType
{
  SendSwitchToCpu,
  PulseCoil,
  StartBlinkLamp,
  StopBlinkLamp
};

struct RulesAction
{
  RulesActionType type;
  int number = 0;
  uint8_t state = 0;
  uint32_t durationMs = 0;
  uint32_t onMs = 0;
  uint32_t offMs = 0;
};
