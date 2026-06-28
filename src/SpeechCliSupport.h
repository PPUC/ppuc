#pragma once

#include <memory>
#include <string>

#include "SpeechService.h"

class AudioOutput;

bool ParseSpeechCliOptions(const char* backendArg,
                           const char* voiceArg,
                           const char* rateArg,
                           const char* pitchArg,
                           SpeechBackend* outBackend,
                           SpeechOptions* outOptions,
                           std::string* errorMessage);

bool ValidateSpeechAudioUsage(bool noSound,
                              bool speechEnabled,
                              bool greetingEnabled,
                              std::string* errorMessage);

bool CreateConfiguredSpeechService(AudioOutput& audioOutput,
                                   SpeechBackend backend,
                                   const SpeechOptions& options,
                                   std::unique_ptr<SpeechService>* outService,
                                   std::string* errorMessage);

void PrintSpeechConfiguration(const char* backendArg,
                              const SpeechOptions& options,
                              const char* rateArg,
                              const char* pitchArg);
