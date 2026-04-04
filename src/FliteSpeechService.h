#pragma once

#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

#include "SpeechService.h"

class AudioOutput;
typedef struct cst_voice_struct cst_voice;

class FliteSpeechService final : public SpeechService
{
public:
  explicit FliteSpeechService(AudioOutput& audioOutput);
  ~FliteSpeechService() override;

  void SpeakText(const std::string& text) override;

private:
  void WorkerLoop();

  AudioOutput& audioOutput_;
  cst_voice* voice_ = nullptr;
  std::mutex mutex_;
  std::condition_variable condition_;
  std::queue<std::string> queue_;
  std::thread worker_;
  bool stopRequested_ = false;
};
