#include "FliteSpeechService.h"

#include <utility>

#include "AudioOutput.h"

extern "C"
{
#if __has_include("flite/flite.h")
#include "flite/flite.h"
#else
#include "flite/flite/flite.h"
#endif
cst_voice* register_cmu_us_kal(const char* voxdir);
void unregister_cmu_us_kal(cst_voice* voice);
}

namespace
{
constexpr size_t kMaxQueuedUtterances = 8;
}

FliteSpeechService::FliteSpeechService(AudioOutput& audioOutput)
    : audioOutput_(audioOutput)
{
  flite_init();
  voice_ = register_cmu_us_kal(nullptr);
  worker_ = std::thread(&FliteSpeechService::WorkerLoop, this);
}

FliteSpeechService::~FliteSpeechService()
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

  if (voice_ != nullptr)
  {
    unregister_cmu_us_kal(voice_);
    voice_ = nullptr;
  }
}

void FliteSpeechService::SpeakText(const std::string& text)
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

void FliteSpeechService::WorkerLoop()
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

    if (voice_ == nullptr)
    {
      continue;
    }

    cst_wave* wave =
        flite_text_to_wave(text.c_str(), voice_);
    if (wave == nullptr)
    {
      continue;
    }

    const size_t sampleCount =
        static_cast<size_t>(wave->num_samples) *
        static_cast<size_t>(wave->num_channels);
    audioOutput_.QueueSpeechSamples(
        reinterpret_cast<const int16_t*>(wave->samples), sampleCount,
        wave->sample_rate, wave->num_channels);
    delete_wave(wave);
  }
}
