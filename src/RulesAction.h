#pragma once

#include <cstdint>

enum class RulesActionType
{
  SendSwitchToCpu,
  PulseCoil,
  StartBlinkLamp,
  StopBlinkLamp,
  // Starts or extends a ball save for durationMs. Works under either engine:
  // ball save lives in PlayfieldAssist, in the switch path, not in the engine.
  GrantBallSave
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
