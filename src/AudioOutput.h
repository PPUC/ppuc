#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

#include "SDL3/SDL.h"

class AudioOutput
{
public:
  AudioOutput() = default;
  ~AudioOutput();

  bool Initialize();
  void Shutdown();

  void ConfigureGameFormat(int frequency, int channels);
  void QueueGameFrames(const int16_t* samples, size_t frameCount);
  void QueueSpeechSamples(const int16_t* samples, size_t sampleCount,
                         int frequency, int channels);

private:
  struct PendingBuffer
  {
    std::vector<int16_t> samples;
    size_t offsetSamples = 0;
  };

  static void SDLCALL OnDeviceNeedsAudio(void* userdata,
                                         SDL_AudioStream* stream,
                                         int additionalAmount,
                                         int totalAmount);

  void EnsureStreamLocked(const SDL_AudioSpec& spec);
  void QueueSamplesLocked(std::deque<PendingBuffer>& queue,
                          const int16_t* samples, size_t sampleCount,
                          int frequency, int channels);
  void MixQueueLocked(std::deque<PendingBuffer>& queue,
                      int16_t* mixBuffer, size_t sampleCount);

  std::mutex mutex_;
  SDL_AudioStream* stream_ = nullptr;
  SDL_AudioSpec deviceSpec_{
      .format = SDL_AUDIO_S16LE,
      .channels = 2,
      .freq = 48000,
  };
  int gameFrequency_ = 48000;
  int gameChannels_ = 2;
  std::deque<PendingBuffer> gameQueue_;
  std::deque<PendingBuffer> speechQueue_;
};
