#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

// The sample-mixing core of AudioOutput, with no SDL in it.
//
// AudioOutput owns the SDL device, the format conversion and the SDL3_mixer
// music track. What is left once those are removed is a small amount of logic
// that is easy to get subtly wrong and impossible to test through SDL: queue
// accounting, the overflow trim, additive mixing with saturation, and the
// "was anything audible" signal that drives music ducking.
//
// That signal matters more than it looks. Ducking uses it today; the AltSound
// mode-1 fallback in docs/PLUGIN_MIGRATION.md will use it to decide when an
// overriding source has gone quiet and the overridden one should be let
// through. Both depend on the same threshold behaving predictably.
//
// Everything here operates on samples ALREADY in the output device's format.
// Resampling and channel conversion happen upstream, in AudioOutput.
namespace AudioMixer
{

// A queued block plus how far into it the mixer has consumed.
struct PendingBuffer
{
  std::vector<int16_t> samples;
  size_t offsetSamples = 0;
};

using Queue = std::deque<PendingBuffer>;

// ~30 seconds at 22.05 kHz. A cap rather than a target: a producer that
// outruns the device must lose its oldest audio rather than grow without
// bound.
inline constexpr size_t kMaxBufferedSamples = 22050 * 30;

// A sample counts as audible at roughly 1.5% of full scale. High enough to
// ignore dither and DC offset in an otherwise silent stream, low enough to
// catch a genuine quiet passage.
inline constexpr int kAudibleSampleThreshold = 512;

// Saturating narrow to int16.
int16_t ClampSample(int value);

// Total samples still to be played across the whole queue.
size_t BufferedSamples(const Queue& queue);

// Appends a block, then drops whole blocks from the front until the queue is
// back under `maxBufferedSamples`. Empty blocks are ignored.
void Enqueue(Queue& queue, std::vector<int16_t> samples, size_t maxBufferedSamples = kMaxBufferedSamples);

// Mixes up to `sampleCount` samples additively into `mixBuffer`, consuming the
// queue as it goes. Returns true if any sample mixed reached
// kAudibleSampleThreshold.
//
// Mixes as much as the queue holds and stops; it does NOT zero the remainder,
// because the caller is mixing several queues into one buffer.
bool Mix(Queue& queue, int16_t* mixBuffer, size_t sampleCount);

}  // namespace AudioMixer
