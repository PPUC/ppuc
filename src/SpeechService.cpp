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
}  // namespace

#if defined(PPUC_HAS_FLITE)
#include "FliteSpeechService.h"
#endif

void SpeechService::SpeakSwitchActivated(const PPUCSwitch& ppucSwitch)
{
  std::string text = "Switch " + std::to_string(ppucSwitch.number);
  if (!ppucSwitch.description.empty())
  {
    text += ", " + ppucSwitch.description;
  }
  text += ", activated";
  SpeakText(text);
}

std::unique_ptr<SpeechService> CreateSpeechService(AudioOutput& audioOutput)
{
#if defined(PPUC_HAS_FLITE)
  return std::make_unique<FliteSpeechService>(audioOutput);
#else
  return std::make_unique<NullSpeechService>();
#endif
}
