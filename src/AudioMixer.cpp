#include "AudioMixer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
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

bool Mix(Queue& queue, int16_t* mixBuffer, size_t sampleCount, float gain)
{
  if (mixBuffer == nullptr)
  {
    return false;
  }

  // A silent source is still drained, on purpose. Its producer runs at the
  // emulator's rate whether anyone listens or not, so holding the queue back
  // would fill it to the overflow cap and then release stale audio the moment
  // the volume came back up.
  if (gain <= 0.0f)
  {
    Discard(queue, sampleCount);
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
      const int scaled =
          static_cast<int>(std::lround(static_cast<float>(front.samples[front.offsetSamples + i]) * gain));
      if (!hadAudibleSamples && std::abs(scaled) >= kAudibleSampleThreshold)
      {
        hadAudibleSamples = true;
      }
      const int mixedValue = static_cast<int>(mixBuffer[mixedSamples + i]) + scaled;
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

namespace
{

size_t AbsDifference(size_t a, size_t b) { return (a > b) ? a - b : b - a; }

// Reads `count` samples starting `skip` samples into the queue, without
// consuming anything. Returns how many it could read.
size_t PeekSamples(const Queue& queue, size_t skip, size_t count, int16_t* out)
{
  size_t copied = 0;
  for (const auto& entry : queue)
  {
    const size_t available = entry.samples.size() - entry.offsetSamples;
    if (skip >= available)
    {
      skip -= available;
      continue;
    }
    const size_t chunk = std::min(count - copied, available - skip);
    std::copy_n(entry.samples.data() + entry.offsetSamples + skip, chunk, out + copied);
    copied += chunk;
    skip = 0;
    if (copied == count)
    {
      break;
    }
  }
  return copied;
}

// Consumes `count` samples from the front, splitting a block if it has to.
void DropSamples(Queue& queue, size_t count)
{
  while (count > 0 && !queue.empty())
  {
    PendingBuffer& front = queue.front();
    const size_t available = front.samples.size() - front.offsetSamples;
    if (available <= count)
    {
      count -= available;
      queue.pop_front();
      continue;
    }
    front.offsetSamples += count;
    count = 0;
  }
}

// Of the cuts within `searchSamples` of `idealDrop`, the one whose audio after
// the cut best matches the audio that plays before it. Sum of absolute
// differences: no normalisation, no floating point, and it agrees with
// correlation on the only thing being asked -- which of these candidates makes
// the smallest step.
size_t BestCut(const Queue& queue, size_t idealDrop, size_t searchSamples, size_t fadeSamples, size_t channels,
               size_t buffered)
{
  const size_t maxDrop = std::min(idealDrop + searchSamples, buffered - fadeSamples);
  const size_t minDrop = (idealDrop > searchSamples + channels) ? idealDrop - searchSamples : channels;
  if (maxDrop <= minDrop)
  {
    return std::min(idealDrop, maxDrop);
  }

  std::vector<int16_t> before(fadeSamples);
  if (PeekSamples(queue, 0, fadeSamples, before.data()) != fadeSamples)
  {
    return idealDrop;
  }

  std::vector<int16_t> candidates(maxDrop - minDrop + fadeSamples);
  const size_t readable = PeekSamples(queue, minDrop, candidates.size(), candidates.data());
  if (readable < fadeSamples)
  {
    return idealDrop;
  }

  size_t bestDrop = idealDrop;
  uint64_t bestScore = std::numeric_limits<uint64_t>::max();
  for (size_t drop = minDrop; drop + fadeSamples <= minDrop + readable; drop += channels)
  {
    const int16_t* candidate = candidates.data() + (drop - minDrop);
    uint64_t score = 0;
    for (size_t i = 0; i < fadeSamples; ++i)
    {
      score += static_cast<uint64_t>(std::abs(static_cast<int>(before[i]) - static_cast<int>(candidate[i])));
    }
    // Ties go to the cut nearest the one asked for, so the lane still lands
    // close to its target.
    if (score < bestScore || (score == bestScore && AbsDifference(drop, idealDrop) < AbsDifference(bestDrop, idealDrop)))
    {
      bestScore = score;
      bestDrop = drop;
    }
  }
  return bestDrop;
}

}  // namespace

size_t TrimToTarget(Queue& queue, size_t targetSamples, size_t highWaterSamples, const TrimOptions& options)
{
  const size_t buffered = BufferedSamples(queue);
  if (buffered <= highWaterSamples || buffered <= targetSamples)
  {
    return 0;
  }

  const size_t channels = std::max(1u, options.channels);
  size_t drop = buffered - targetSamples;
  drop -= drop % channels;
  if (drop == 0)
  {
    return 0;
  }

  const size_t fadeSamples = options.crossfadeSamples - (options.crossfadeSamples % channels);
  if (fadeSamples != 0 && drop + fadeSamples <= buffered)
  {
    drop = BestCut(queue, drop, options.searchSamples - (options.searchSamples % channels), fadeSamples, channels,
                   buffered);

    std::vector<int16_t> before(fadeSamples);
    std::vector<int16_t> after(fadeSamples);
    if (PeekSamples(queue, 0, fadeSamples, before.data()) == fadeSamples &&
        PeekSamples(queue, drop, fadeSamples, after.data()) == fadeSamples)
    {
      // The join replaces the first fadeSamples of what survives, so the queue
      // still ends up exactly `drop` samples shorter.
      PendingBuffer join;
      join.samples.resize(fadeSamples);
      const size_t fadeFrames = fadeSamples / channels;
      for (size_t i = 0; i < fadeSamples; ++i)
      {
        const float weight = static_cast<float>(i / channels + 1) / static_cast<float>(fadeFrames + 1);
        join.samples[i] = ClampSample(static_cast<int>(static_cast<float>(before[i]) * (1.0f - weight) +
                                                      static_cast<float>(after[i]) * weight));
      }
      DropSamples(queue, drop + fadeSamples);
      queue.push_front(std::move(join));
      return drop;
    }
  }

  DropSamples(queue, drop);
  return drop;
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
