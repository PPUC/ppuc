#include "AudioOutput.h"

#include <algorithm>
#include <limits>

namespace
{
constexpr size_t kMaxBufferedSamples = 22050 * 30;

static int16_t ClampMixedSample(int value)
{
  if (value > std::numeric_limits<int16_t>::max())
  {
    return std::numeric_limits<int16_t>::max();
  }
  if (value < std::numeric_limits<int16_t>::min())
  {
    return std::numeric_limits<int16_t>::min();
  }
  return static_cast<int16_t>(value);
}
}  // namespace

AudioOutput::~AudioOutput()
{
  Shutdown();
}

bool AudioOutput::Initialize()
{
  std::lock_guard<std::mutex> lock(mutex_);
  EnsureStreamLocked(deviceSpec_);
  return stream_ != nullptr;
}

void AudioOutput::Shutdown()
{
  std::lock_guard<std::mutex> lock(mutex_);
  gameQueue_.clear();
  speechQueue_.clear();
  if (stream_ != nullptr)
  {
    SDL_DestroyAudioStream(stream_);
    stream_ = nullptr;
  }
}

void AudioOutput::ConfigureGameFormat(int frequency, int channels)
{
  if (frequency <= 0 || channels <= 0)
  {
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  gameFrequency_ = frequency;
  gameChannels_ = channels;

  SDL_AudioSpec desiredSpec{
      .format = SDL_AUDIO_S16LE,
      .channels = static_cast<Uint8>(channels),
      .freq = frequency,
  };
  EnsureStreamLocked(desiredSpec);
}

void AudioOutput::QueueGameFrames(const int16_t* samples, size_t frameCount)
{
  if (samples == nullptr || frameCount == 0)
  {
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  QueueSamplesLocked(gameQueue_, samples, frameCount * gameChannels_,
                     gameFrequency_, gameChannels_);
}

void AudioOutput::QueueSpeechSamples(const int16_t* samples, size_t sampleCount,
                                     int frequency, int channels)
{
  if (samples == nullptr || sampleCount == 0 || frequency <= 0 || channels <= 0)
  {
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  QueueSamplesLocked(speechQueue_, samples, sampleCount, frequency, channels);
}

void SDLCALL AudioOutput::OnDeviceNeedsAudio(void* userdata,
                                             SDL_AudioStream* stream,
                                             int additionalAmount,
                                             int /*totalAmount*/)
{
  auto* self = static_cast<AudioOutput*>(userdata);
  if (self == nullptr || additionalAmount <= 0)
  {
    return;
  }

  const size_t sampleCount = static_cast<size_t>(additionalAmount) / sizeof(int16_t);
  std::vector<int16_t> mixBuffer(sampleCount, 0);

  {
    std::lock_guard<std::mutex> lock(self->mutex_);
    self->MixQueueLocked(self->gameQueue_, mixBuffer.data(), sampleCount);
    self->MixQueueLocked(self->speechQueue_, mixBuffer.data(), sampleCount);
  }

  SDL_PutAudioStreamData(stream, mixBuffer.data(), additionalAmount);
}

void AudioOutput::EnsureStreamLocked(const SDL_AudioSpec& spec)
{
  if (stream_ != nullptr && deviceSpec_.freq == spec.freq &&
      deviceSpec_.channels == spec.channels &&
      deviceSpec_.format == spec.format)
  {
    return;
  }

  if (stream_ != nullptr)
  {
    SDL_DestroyAudioStream(stream_);
    stream_ = nullptr;
  }

  deviceSpec_ = spec;
  stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                      &deviceSpec_, &AudioOutput::OnDeviceNeedsAudio,
                                      this);
  if (stream_ != nullptr)
  {
    SDL_ResumeAudioStreamDevice(stream_);
  }
}

void AudioOutput::QueueSamplesLocked(std::deque<PendingBuffer>& queue,
                                     const int16_t* samples,
                                     size_t sampleCount, int frequency,
                                     int channels)
{
  if (stream_ == nullptr)
  {
    EnsureStreamLocked(deviceSpec_);
    if (stream_ == nullptr)
    {
      return;
    }
  }

  PendingBuffer pending;
  const bool requiresConversion = frequency != deviceSpec_.freq ||
                                  channels != deviceSpec_.channels;

  if (!requiresConversion)
  {
    pending.samples.assign(samples, samples + sampleCount);
  }
  else
  {
    const SDL_AudioSpec sourceSpec{
        .format = SDL_AUDIO_S16LE,
        .channels = static_cast<Uint8>(channels),
        .freq = frequency,
    };

    Uint8* convertedData = nullptr;
    int convertedLength = 0;
    if (!SDL_ConvertAudioSamples(
            &sourceSpec, reinterpret_cast<const Uint8*>(samples),
            static_cast<int>(sampleCount * sizeof(int16_t)), &deviceSpec_,
            &convertedData, &convertedLength))
    {
      return;
    }

    const auto* convertedSamples =
        reinterpret_cast<const int16_t*>(convertedData);
    const size_t convertedSampleCount =
        static_cast<size_t>(convertedLength) / sizeof(int16_t);
    pending.samples.assign(convertedSamples,
                           convertedSamples + convertedSampleCount);
    SDL_free(convertedData);
  }

  if (pending.samples.empty())
  {
    return;
  }

  queue.push_back(std::move(pending));

  size_t bufferedSamples = 0;
  for (const auto& entry : queue)
  {
    bufferedSamples += entry.samples.size() - entry.offsetSamples;
  }
  while (bufferedSamples > kMaxBufferedSamples && !queue.empty())
  {
    bufferedSamples -= queue.front().samples.size() - queue.front().offsetSamples;
    queue.pop_front();
  }
}

void AudioOutput::MixQueueLocked(std::deque<PendingBuffer>& queue,
                                 int16_t* mixBuffer, size_t sampleCount)
{
  size_t mixedSamples = 0;
  while (mixedSamples < sampleCount && !queue.empty())
  {
    PendingBuffer& front = queue.front();
    const size_t availableSamples = front.samples.size() - front.offsetSamples;
    const size_t chunkSamples =
        std::min(sampleCount - mixedSamples, availableSamples);

    for (size_t i = 0; i < chunkSamples; ++i)
    {
      const int mixedValue = static_cast<int>(mixBuffer[mixedSamples + i]) +
                             static_cast<int>(front.samples[front.offsetSamples + i]);
      mixBuffer[mixedSamples + i] = ClampMixedSample(mixedValue);
    }

    mixedSamples += chunkSamples;
    front.offsetSamples += chunkSamples;
    if (front.offsetSamples >= front.samples.size())
    {
      queue.pop_front();
    }
  }
}
