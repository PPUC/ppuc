#pragma once

#include <memory>
#include <string>

#include "PPUC_structs.h"

class AudioOutput;

class SpeechService
{
public:
  virtual ~SpeechService() = default;

  virtual void SpeakText(const std::string& text) = 0;

  void SpeakSwitchActivated(const PPUCSwitch& ppucSwitch);
};

std::unique_ptr<SpeechService> CreateSpeechService(AudioOutput& audioOutput);
