#pragma once

#include <memory>
#include <string>

#include "PPUC_structs.h"

class AudioOutput;

enum class SpeechBackend
{
  Auto,
  Flite,
  ESpeakNg
};

struct SpeechOptions
{
  std::string voice;
  int rate = 0;
  int pitch = 0;
};

class SpeechService
{
public:
  virtual ~SpeechService() = default;

  virtual void SpeakText(const std::string& text) = 0;

  void SpeakSwitchActivated(const PPUCSwitch& ppucSwitch);
};

std::unique_ptr<SpeechService> CreateSpeechService(AudioOutput& audioOutput,
                                                   SpeechBackend backend,
                                                   const SpeechOptions& options,
                                                   std::string* errorMessage = nullptr);
