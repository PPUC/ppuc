#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "SpeechService.h"

#if __has_include("espeak-ng/speak_lib.h")
#include "espeak-ng/speak_lib.h"
#elif __has_include("espeak-ng/espeak-ng/speak_lib.h")
#include "espeak-ng/espeak-ng/speak_lib.h"
#else
#error "espeak-ng headers not found"
#endif

class AudioOutput;

class ESpeakNgSpeechService final : public SpeechService
{
public:
  ESpeakNgSpeechService(AudioOutput& audioOutput, const SpeechOptions& options);
  ~ESpeakNgSpeechService() override;

  void SpeakText(const std::string& text) override;

private:
  static int SynthCallback(short* wav, int numSamples, espeak_EVENT* events);
  void WorkerLoop();

  AudioOutput& audioOutput_;
  std::mutex mutex_;
  std::condition_variable condition_;
  std::queue<std::string> queue_;
  std::thread worker_;
  bool stopRequested_ = false;

  std::mutex synthMutex_;
  std::vector<int16_t> synthSamples_;
  int sampleRate_ = 0;

  static ESpeakNgSpeechService* activeInstance_;
};
