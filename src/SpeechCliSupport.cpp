#include "SpeechCliSupport.h"

#include <climits>
#include <cstdlib>
#include <cstring>
#include <string>

#include "AudioOutput.h"

namespace
{
bool ParseIntStrict(const char* text, int* outValue)
{
  if (!text || !outValue || text[0] == '\0')
  {
    return false;
  }

  char* end = nullptr;
  const long value = strtol(text, &end, 10);
  if (end == text || *end != '\0')
  {
    return false;
  }

  if (value < INT_MIN || value > INT_MAX)
  {
    return false;
  }

  *outValue = static_cast<int>(value);
  return true;
}

bool ParseSpeechBackend(const char* text, SpeechBackend* outBackend)
{
  if (!text || !outBackend)
  {
    return false;
  }

  if (strcmp(text, "auto") == 0)
  {
    *outBackend = SpeechBackend::Auto;
    return true;
  }
  if (strcmp(text, "flite") == 0)
  {
    *outBackend = SpeechBackend::Flite;
    return true;
  }
  if (strcmp(text, "espeak") == 0 || strcmp(text, "espeak-ng") == 0)
  {
    *outBackend = SpeechBackend::ESpeakNg;
    return true;
  }

  return false;
}
}  // namespace

bool ParseSpeechCliOptions(const char* backendArg,
                           const char* voiceArg,
                           const char* rateArg,
                           const char* pitchArg,
                           SpeechBackend* outBackend,
                           SpeechOptions* outOptions,
                           std::string* errorMessage)
{
  if (errorMessage != nullptr)
  {
    errorMessage->clear();
  }

  if (outBackend == nullptr || outOptions == nullptr)
  {
    if (errorMessage != nullptr)
    {
      *errorMessage = "speech option output storage is missing";
    }
    return false;
  }

  const char* effectiveBackend = backendArg ? backendArg : "auto";
  if (!ParseSpeechBackend(effectiveBackend, outBackend))
  {
    if (errorMessage != nullptr)
    {
      *errorMessage = std::string("Invalid value for --speech-backend: ") +
                      effectiveBackend;
    }
    return false;
  }

  SpeechOptions options;
  if (voiceArg != nullptr && voiceArg[0] != '\0')
  {
    options.voice = voiceArg;
  }

  if (rateArg != nullptr && !ParseIntStrict(rateArg, &options.rate))
  {
    if (errorMessage != nullptr)
    {
      *errorMessage = std::string("Invalid value for --speech-rate: ") +
                      rateArg;
    }
    return false;
  }

  if (pitchArg != nullptr && !ParseIntStrict(pitchArg, &options.pitch))
  {
    if (errorMessage != nullptr)
    {
      *errorMessage = std::string("Invalid value for --speech-pitch: ") +
                      pitchArg;
    }
    return false;
  }

  if (options.rate < 0)
  {
    if (errorMessage != nullptr)
    {
      *errorMessage = "--speech-rate must be >= 0";
    }
    return false;
  }

  if (options.pitch < 0 || options.pitch > 100)
  {
    if (errorMessage != nullptr)
    {
      *errorMessage = "--speech-pitch must be in the range 0..100";
    }
    return false;
  }

  *outOptions = std::move(options);
  return true;
}

bool ValidateSpeechAudioUsage(bool noSound,
                              bool speechEnabled,
                              bool greetingEnabled,
                              const char* speechFile,
                              std::string* errorMessage)
{
  if (errorMessage != nullptr)
  {
    errorMessage->clear();
  }

  if (speechFile != nullptr && speechFile[0] != '\0' && noSound)
  {
    if (errorMessage != nullptr)
    {
      *errorMessage =
          "--speech-file requires audio output and cannot be used with --no-sound";
    }
    return false;
  }

  if (noSound && (speechEnabled || greetingEnabled))
  {
    if (errorMessage != nullptr)
    {
      *errorMessage =
          "Speech options require audio output and cannot be used with --no-sound";
    }
    return false;
  }

  return true;
}

bool CreateConfiguredSpeechService(AudioOutput& audioOutput,
                                   SpeechBackend backend,
                                   const SpeechOptions& options,
                                   std::unique_ptr<SpeechService>* outService,
                                   std::string* errorMessage)
{
  if (errorMessage != nullptr)
  {
    errorMessage->clear();
  }

  if (outService == nullptr)
  {
    if (errorMessage != nullptr)
    {
      *errorMessage = "speech service output storage is missing";
    }
    return false;
  }

  std::string backendError;
  std::unique_ptr<SpeechService> service =
      CreateSpeechService(audioOutput, backend, options, &backendError);
  if (service == nullptr)
  {
    if (errorMessage != nullptr)
    {
      *errorMessage = backendError.empty() ? "unknown backend error"
                                           : backendError;
    }
    return false;
  }

  *outService = std::move(service);
  return true;
}

void PrintSpeechConfiguration(const char* backendArg,
                              const SpeechOptions& options,
                              const char* rateArg,
                              const char* pitchArg)
{
  printf("Speech backend requested: %s",
         backendArg ? backendArg : "auto");
  if (!options.voice.empty())
  {
    printf(", voice=%s", options.voice.c_str());
  }
  if (rateArg != nullptr)
  {
    printf(", rate=%d", options.rate);
  }
  if (pitchArg != nullptr)
  {
    printf(", pitch=%d", options.pitch);
  }
  printf("\n");
}
