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
