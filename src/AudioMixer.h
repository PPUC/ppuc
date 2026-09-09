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

// How far ahead of the device a lane is allowed to sit before the oldest audio
// is discarded to catch up.
//
// This is a target, unlike kMaxBufferedSamples which is a last-resort cap. Any
// transient stall -- a plugin loading, a video decoder starting, a disk hiccup --
// lets a producer run ahead while nothing is draining it, and because production
// and consumption then match exactly, that backlog never shrinks again. Loading
// a PUP pack put Terminator 2's ROM audio 480 ms behind and left it there for
// the whole session. Trimming turns a permanent half second of latency into one
// brief discontinuity.
//
// Trimming only happens once a lane passes kHighWaterBufferedMs, and then it
// goes all the way back to kTargetBufferedMs. The gap between the two is what
// keeps normal playback untouched: a lane's depth oscillates by ~20 ms as
// buffers arrive and drain, and trimming at the target alone clipped every one
// of those peaks -- measured as two dozen discontinuities in forty seconds,
// each an audible click, to save a few milliseconds that were about to drain
// anyway. Only a real stall crosses the high-water mark.
inline constexpr unsigned int kTargetBufferedMs = 100;
inline constexpr unsigned int kHighWaterBufferedMs = 250;

// Saturating narrow to int16.
int16_t ClampSample(int value);

// Total samples still to be played across the whole queue.
size_t BufferedSamples(const Queue& queue);

// Appends a block, then drops whole blocks from the front until the queue is
// back under `maxBufferedSamples`. Empty blocks are ignored.
void Enqueue(Queue& queue, std::vector<int16_t> samples, size_t maxBufferedSamples = kMaxBufferedSamples);

// Drops the oldest samples back to `targetSamples`, but only once the queue has
// passed `highWaterSamples`. Returns how many were dropped, which is zero in
// the ordinary case.
size_t TrimToTarget(Queue& queue, size_t targetSamples, size_t highWaterSamples);

// Mixes up to `sampleCount` samples additively into `mixBuffer`, consuming the
// queue as it goes. Returns true if any sample mixed reached
// kAudibleSampleThreshold.
//
// Mixes as much as the queue holds and stops; it does NOT zero the remainder,
// because the caller is mixing several queues into one buffer.
bool Mix(Queue& queue, int16_t* mixBuffer, size_t sampleCount);

// Consumes up to `sampleCount` samples without mixing them anywhere, returning
// the same audibility answer Mix would have. A source that is currently
// overridden must be drained rather than stalled: it keeps producing at the
// emulator's rate whether or not anyone is listening, and a stalled queue would
// grow to the overflow cap and then play back stale audio the moment the
// override lifts.
bool Discard(Queue& queue, size_t sampleCount);

}  // namespace AudioMixer
