#include "AudioMixer.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <utility>

namespace AudioMixer
{

int16_t ClampSample(int value)
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

size_t BufferedSamples(const Queue& queue)
{
  size_t buffered = 0;
  for (const auto& entry : queue)
  {
    buffered += entry.samples.size() - entry.offsetSamples;
  }
  return buffered;
}

void Enqueue(Queue& queue, std::vector<int16_t> samples, size_t maxBufferedSamples)
{
  if (samples.empty())
  {
    return;
  }

  PendingBuffer pending;
  pending.samples = std::move(samples);
  queue.push_back(std::move(pending));

  size_t buffered = BufferedSamples(queue);
  while (buffered > maxBufferedSamples && !queue.empty())
  {
    buffered -= queue.front().samples.size() - queue.front().offsetSamples;
    queue.pop_front();
  }
}

bool Mix(Queue& queue, int16_t* mixBuffer, size_t sampleCount)
{
  if (mixBuffer == nullptr)
  {
    return false;
  }

  size_t mixedSamples = 0;
  bool hadAudibleSamples = false;
  while (mixedSamples < sampleCount && !queue.empty())
  {
    PendingBuffer& front = queue.front();
    const size_t availableSamples = front.samples.size() - front.offsetSamples;
    const size_t chunkSamples = std::min(sampleCount - mixedSamples, availableSamples);

    for (size_t i = 0; i < chunkSamples; ++i)
    {
      const int16_t sample = front.samples[front.offsetSamples + i];
      if (!hadAudibleSamples && std::abs(static_cast<int>(sample)) >= kAudibleSampleThreshold)
      {
        hadAudibleSamples = true;
      }
      const int mixedValue = static_cast<int>(mixBuffer[mixedSamples + i]) + static_cast<int>(sample);
      mixBuffer[mixedSamples + i] = ClampSample(mixedValue);
    }

    mixedSamples += chunkSamples;
    front.offsetSamples += chunkSamples;
    if (front.offsetSamples >= front.samples.size())
    {
      queue.pop_front();
    }
  }

  return hadAudibleSamples;
}

float RateRatioFor(size_t bufferedSamples, size_t targetSamples)
{
  if (targetSamples == 0)
  {
    return 1.0f;
  }
  const float error =
      (static_cast<float>(bufferedSamples) - static_cast<float>(targetSamples)) / static_cast<float>(targetSamples);
  // Gain chosen so the correction saturates once a lane is about a third away
  // from target, and eases off smoothly as it closes in.
  const float correction = std::clamp(error * 0.01f, -kMaxRateDeviation, kMaxRateDeviation);
  return 1.0f + correction;
}

size_t TrimToTarget(Queue& queue, size_t targetSamples, size_t highWaterSamples)
{
  size_t buffered = BufferedSamples(queue);
  if (buffered <= highWaterSamples || buffered <= targetSamples)
  {
    return 0;
  }

  size_t dropped = 0;
  while (buffered > targetSamples && !queue.empty())
  {
    PendingBuffer& front = queue.front();
    const size_t available = front.samples.size() - front.offsetSamples;
    const size_t excess = buffered - targetSamples;
    if (available <= excess)
    {
      queue.pop_front();
      buffered -= available;
      dropped += available;
      continue;
    }
    // Drop part of a block rather than all of it, so trimming lands on the
    // target instead of overshooting into an underrun.
    front.offsetSamples += excess;
    buffered -= excess;
    dropped += excess;
  }
  return dropped;
}

bool Discard(Queue& queue, size_t sampleCount)
{
  bool hadAudibleSamples = false;
  size_t droppedSamples = 0;

  while (droppedSamples < sampleCount && !queue.empty())
  {
    PendingBuffer& front = queue.front();
    const size_t available = front.samples.size() - front.offsetSamples;
    const size_t chunkSamples = std::min(available, sampleCount - droppedSamples);
    const int16_t* source = front.samples.data() + front.offsetSamples;

    for (size_t i = 0; i < chunkSamples && !hadAudibleSamples; ++i)
    {
      if (std::abs(static_cast<int>(source[i])) >= kAudibleSampleThreshold)
      {
        hadAudibleSamples = true;
      }
    }

    droppedSamples += chunkSamples;
    front.offsetSamples += chunkSamples;
    if (front.offsetSamples >= front.samples.size())
    {
      queue.pop_front();
    }
  }

  return hadAudibleSamples;
}

}  // namespace AudioMixer
