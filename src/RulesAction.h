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
  GrantBallSave,
  // Holds the ball search off, or lets it run again. For the stretch of a
  // multiball start where a ball waits in the shooter lane and another sits in
  // a kickout hole: nothing is lost, the machine is simply waiting for the
  // player, and a search would fire coils under a ball that is exactly where it
  // should be. `state` is 1 to hold it off.
  HoldBallSearch
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
