#include "SpeechService.h"

#include <memory>
#include <string>

#include "AudioOutput.h"

namespace
{
class NullSpeechService final : public SpeechService
{
public:
  void SpeakText(const std::string& /*text*/) override {}
};

bool SupportsFliteVoice(const std::string& voice)
{
  return voice.empty() || voice == "kal" || voice == "cmu_us_kal";
}
}  // namespace

#if defined(PPUC_HAS_FLITE)
#include "FliteSpeechService.h"
#endif

#if defined(PPUC_HAS_ESPEAK_NG)
#include "ESpeakNgSpeechService.h"
#endif

void SpeechService::SpeakSwitchActivated(const PPUCSwitch& ppucSwitch)
{
  std::string text = "Switch " + std::to_string(ppucSwitch.number);
  if (!ppucSwitch.description.empty())
  {
    text += ", " + ppucSwitch.description;
  }
  SpeakText(text);
}

std::unique_ptr<SpeechService> CreateSpeechService(AudioOutput& audioOutput,
                                                   SpeechBackend backend,
                                                   const SpeechOptions& options,
                                                   std::string* errorMessage)
{
  if (errorMessage != nullptr)
  {
    errorMessage->clear();
  }

  switch (backend)
  {
    case SpeechBackend::Auto:
#if defined(PPUC_HAS_ESPEAK_NG)
      return std::make_unique<ESpeakNgSpeechService>(audioOutput, options);
#elif defined(PPUC_HAS_FLITE)
      if (!SupportsFliteVoice(options.voice) || options.rate > 0 ||
          options.pitch > 0)
      {
        if (errorMessage != nullptr)
        {
          *errorMessage =
              "Auto speech backend fell back to Flite, but the requested voice/rate/pitch requires espeak-ng";
        }
        return nullptr;
      }
      return std::make_unique<FliteSpeechService>(audioOutput, options);
#else
      if (errorMessage != nullptr)
      {
        *errorMessage = "No speech backend available in this build";
      }
      return nullptr;
#endif

    case SpeechBackend::Flite:
#if defined(PPUC_HAS_FLITE)
      if (!SupportsFliteVoice(options.voice))
      {
        if (errorMessage != nullptr)
        {
          *errorMessage =
              "Speech backend 'flite' only supports voice 'kal' in this build";
        }
        return nullptr;
      }
      if (options.rate > 0 || options.pitch > 0)
      {
        if (errorMessage != nullptr)
        {
          *errorMessage =
              "Speech backend 'flite' does not support --speech-rate or --speech-pitch";
        }
        return nullptr;
      }
      return std::make_unique<FliteSpeechService>(audioOutput, options);
#else
      if (errorMessage != nullptr)
      {
        *errorMessage = "Speech backend 'flite' is not available in this build";
      }
      return nullptr;
#endif

    case SpeechBackend::ESpeakNg:
#if defined(PPUC_HAS_ESPEAK_NG)
      return std::make_unique<ESpeakNgSpeechService>(audioOutput, options);
#else
      if (errorMessage != nullptr)
      {
        *errorMessage = "Speech backend 'espeak-ng' is not available in this build";
      }
      return nullptr;
#endif
  }

  if (errorMessage != nullptr)
  {
    *errorMessage = "Unknown speech backend";
  }
  return nullptr;
}
