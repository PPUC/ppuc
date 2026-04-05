#include "ESpeakNgSpeechService.h"

#include <cstdlib>
#include <filesystem>
#include <utility>

#include "AudioOutput.h"
#include "SDL3/SDL.h"

namespace
{
constexpr size_t kMaxQueuedUtterances = 8;
constexpr const char* kDefaultVoice = "en-us+f3";
constexpr int kDefaultRate = 165;
constexpr int kDefaultPitch = 70;

std::string FindESpeakDataPath()
{
  namespace fs = std::filesystem;

  std::vector<fs::path> candidates;
  if (const char* envPath = std::getenv("ESPEAK_DATA_PATH"))
  {
    if (envPath[0] != '\0')
    {
      candidates.emplace_back(envPath);
    }
  }

  const char* basePathRaw = SDL_GetBasePath();
  if (basePathRaw != nullptr)
  {
    fs::path basePath(basePathRaw);

    candidates.push_back(basePath / "espeak-ng-data");
    candidates.push_back(basePath / "third-party" / "runtime-libs" /
                         PPUC_RUNTIME_LIB_SUBDIR / "espeak-ng-data");
    candidates.push_back(basePath.parent_path() / "third-party" /
                         "runtime-libs" / PPUC_RUNTIME_LIB_SUBDIR /
                         "espeak-ng-data");
  }

  const fs::path cwd = fs::current_path();
  candidates.push_back(cwd / "espeak-ng-data");
  candidates.push_back(cwd / "third-party" / "runtime-libs" /
                       PPUC_RUNTIME_LIB_SUBDIR / "espeak-ng-data");

  for (const fs::path& candidate : candidates)
  {
    if (candidate.empty())
    {
      continue;
    }

    std::error_code ec;
    if (fs::exists(candidate, ec) && fs::is_directory(candidate, ec))
    {
      return candidate.string();
    }
  }

  return std::string();
}
}

ESpeakNgSpeechService* ESpeakNgSpeechService::activeInstance_ = nullptr;

ESpeakNgSpeechService::ESpeakNgSpeechService(AudioOutput& audioOutput,
                                             const SpeechOptions& options)
    : audioOutput_(audioOutput)
{
  const std::string dataPath = FindESpeakDataPath();
  sampleRate_ = espeak_Initialize(
      AUDIO_OUTPUT_RETRIEVAL, 0, dataPath.empty() ? nullptr : dataPath.c_str(),
      0);
  if (sampleRate_ > 0)
  {
    activeInstance_ = this;
    espeak_SetSynthCallback(&ESpeakNgSpeechService::SynthCallback);
    const std::string voice =
        options.voice.empty() ? kDefaultVoice : options.voice;
    espeak_SetVoiceByName(voice.c_str());
    espeak_SetParameter(espeakRATE,
                        options.rate > 0 ? options.rate : kDefaultRate, 0);
    espeak_SetParameter(espeakPITCH,
                        options.pitch > 0 ? options.pitch : kDefaultPitch, 0);
  }
  worker_ = std::thread(&ESpeakNgSpeechService::WorkerLoop, this);
}

ESpeakNgSpeechService::~ESpeakNgSpeechService()
{
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopRequested_ = true;
  }
  condition_.notify_all();
  if (worker_.joinable())
  {
    worker_.join();
  }

  if (activeInstance_ == this)
  {
    activeInstance_ = nullptr;
    espeak_Terminate();
  }
}

void ESpeakNgSpeechService::SpeakText(const std::string& text)
{
  if (text.empty())
  {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.size() >= kMaxQueuedUtterances)
    {
      return;
    }
    queue_.push(text);
  }
  condition_.notify_one();
}

int ESpeakNgSpeechService::SynthCallback(short* wav, int numSamples,
                                         espeak_EVENT* /*events*/)
{
  ESpeakNgSpeechService* self = activeInstance_;
  if (self == nullptr || wav == nullptr || numSamples <= 0)
  {
    return 0;
  }

  std::lock_guard<std::mutex> lock(self->synthMutex_);
  self->synthSamples_.insert(self->synthSamples_.end(), wav, wav + numSamples);
  return 0;
}

void ESpeakNgSpeechService::WorkerLoop()
{
  for (;;)
  {
    std::string text;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      condition_.wait(lock, [this] {
        return stopRequested_ || !queue_.empty();
      });

      if (stopRequested_ && queue_.empty())
      {
        return;
      }

      text = std::move(queue_.front());
      queue_.pop();
    }

    if (sampleRate_ <= 0 || activeInstance_ != this)
    {
      continue;
    }

    {
      std::lock_guard<std::mutex> lock(synthMutex_);
      synthSamples_.clear();
    }

    unsigned int utteranceId = 0;
    const espeak_ERROR synthResult =
        espeak_Synth(text.c_str(), text.size() + 1, 0, POS_CHARACTER, 0,
                     espeakCHARS_AUTO, &utteranceId, nullptr);
    if (synthResult != EE_OK)
    {
      continue;
    }

    if (espeak_Synchronize() != EE_OK)
    {
      continue;
    }

    std::vector<int16_t> samples;
    {
      std::lock_guard<std::mutex> lock(synthMutex_);
      samples.swap(synthSamples_);
    }
    if (samples.empty())
    {
      continue;
    }

    audioOutput_.QueueSpeechSamples(samples.data(), samples.size(), sampleRate_,
                                    1);
  }
}
