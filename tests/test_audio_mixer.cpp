// Tests for the sample-mixing core.
//
// This logic used to be private to AudioOutput and therefore only reachable
// through an initialised SDL audio device, which meant it had no coverage at
// all. Three behaviours here are load-bearing and each fails silently when
// wrong: the overflow trim (a producer outrunning the device must lose its
// oldest audio, not grow without bound), saturation on mix (two loud sources
// must clip rather than wrap to the opposite sign), and the audibility signal
// that drives music ducking -- and that the AltSound mode-1 fallback in
// docs/PLUGIN_MIGRATION.md will use to decide when an overriding source has
// gone quiet.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <vector>

#include "AudioMixer.h"
#include "doctest.h"

namespace
{

std::vector<int16_t> Block(size_t count, int16_t value) { return std::vector<int16_t>(count, value); }

}  // namespace

TEST_CASE("ClampSample saturates instead of wrapping")
{
  CHECK(AudioMixer::ClampSample(0) == 0);
  CHECK(AudioMixer::ClampSample(1000) == 1000);
  CHECK(AudioMixer::ClampSample(-1000) == -1000);

  CHECK(AudioMixer::ClampSample(std::numeric_limits<int16_t>::max()) == std::numeric_limits<int16_t>::max());
  CHECK(AudioMixer::ClampSample(std::numeric_limits<int16_t>::min()) == std::numeric_limits<int16_t>::min());

  // The case that matters: summing two near-full-scale sources. Wrapping here
  // turns a loud passage into a loud passage of the opposite sign, which is
  // audible as a hard crack.
  CHECK(AudioMixer::ClampSample(40000) == std::numeric_limits<int16_t>::max());
  CHECK(AudioMixer::ClampSample(-40000) == std::numeric_limits<int16_t>::min());
}

TEST_CASE("Enqueue accumulates and BufferedSamples counts what is left")
{
  AudioMixer::Queue queue;
  CHECK(AudioMixer::BufferedSamples(queue) == 0);

  AudioMixer::Enqueue(queue, Block(10, 100));
  AudioMixer::Enqueue(queue, Block(5, 100));
  CHECK(queue.size() == 2);
  CHECK(AudioMixer::BufferedSamples(queue) == 15);
}

TEST_CASE("Enqueue ignores empty blocks")
{
  AudioMixer::Queue queue;
  AudioMixer::Enqueue(queue, {});
  CHECK(queue.empty());
  CHECK(AudioMixer::BufferedSamples(queue) == 0);
}

TEST_CASE("Enqueue drops the oldest audio once over the cap")
{
  AudioMixer::Queue queue;
  const size_t cap = 10;

  AudioMixer::Enqueue(queue, Block(6, 1), cap);
  AudioMixer::Enqueue(queue, Block(6, 2), cap);
  // 12 > 10, so the front block goes and the newest audio survives.
  CHECK(queue.size() == 1);
  CHECK(AudioMixer::BufferedSamples(queue) == 6);
  CHECK(queue.front().samples[0] == 2);
}

TEST_CASE("The overflow trim accounts for already-consumed samples")
{
  AudioMixer::Queue queue;
  const size_t cap = 10;

  AudioMixer::Enqueue(queue, Block(8, 1), cap);

  std::vector<int16_t> mix(6, 0);
  AudioMixer::Mix(queue, mix.data(), mix.size());
  // 2 samples left unconsumed, so a 7-sample block still fits under the cap
  // and must not evict anything.
  CHECK(AudioMixer::BufferedSamples(queue) == 2);

  AudioMixer::Enqueue(queue, Block(7, 2), cap);
  CHECK(queue.size() == 2);
  CHECK(AudioMixer::BufferedSamples(queue) == 9);
}

TEST_CASE("Mix adds into the buffer rather than overwriting it")
{
  AudioMixer::Queue queue;
  AudioMixer::Enqueue(queue, Block(4, 100));

  std::vector<int16_t> mix(4, 50);
  AudioMixer::Mix(queue, mix.data(), mix.size());

  for (int16_t sample : mix)
  {
    CHECK(sample == 150);
  }
  CHECK(queue.empty());
}

TEST_CASE("Mix saturates when summed sources exceed full scale")
{
  AudioMixer::Queue queue;
  AudioMixer::Enqueue(queue, Block(4, 30000));

  std::vector<int16_t> mix(4, 30000);
  AudioMixer::Mix(queue, mix.data(), mix.size());

  for (int16_t sample : mix)
  {
    CHECK(sample == std::numeric_limits<int16_t>::max());
  }
}

TEST_CASE("Mix leaves the tail untouched when the queue runs dry")
{
  AudioMixer::Queue queue;
  AudioMixer::Enqueue(queue, Block(2, 100));

  std::vector<int16_t> mix(5, 7);
  AudioMixer::Mix(queue, mix.data(), mix.size());

  CHECK(mix[0] == 107);
  CHECK(mix[1] == 107);
  // Not zeroed: the caller mixes several queues into this same buffer, so
  // clearing the remainder would silence every source mixed after this one.
  CHECK(mix[2] == 7);
  CHECK(mix[3] == 7);
  CHECK(mix[4] == 7);
}

TEST_CASE("Mix spans block boundaries and consumes exactly what it used")
{
  AudioMixer::Queue queue;
  AudioMixer::Enqueue(queue, Block(3, 10));
  AudioMixer::Enqueue(queue, Block(3, 20));

  std::vector<int16_t> mix(4, 0);
  AudioMixer::Mix(queue, mix.data(), mix.size());

  CHECK(mix[0] == 10);
  CHECK(mix[1] == 10);
  CHECK(mix[2] == 10);
  CHECK(mix[3] == 20);
  CHECK(AudioMixer::BufferedSamples(queue) == 2);
}

TEST_CASE("A partially consumed block resumes where it left off")
{
  AudioMixer::Queue queue;
  std::vector<int16_t> block = {1, 2, 3, 4};
  AudioMixer::Enqueue(queue, block);

  std::vector<int16_t> first(2, 0);
  AudioMixer::Mix(queue, first.data(), first.size());
  CHECK(first[0] == 1);
  CHECK(first[1] == 2);

  std::vector<int16_t> second(2, 0);
  AudioMixer::Mix(queue, second.data(), second.size());
  CHECK(second[0] == 3);
  CHECK(second[1] == 4);
  CHECK(queue.empty());
}

TEST_CASE("Mix reports audibility against the threshold")
{
  SUBCASE("silence is not audible")
  {
    AudioMixer::Queue queue;
    AudioMixer::Enqueue(queue, Block(4, 0));
    std::vector<int16_t> mix(4, 0);
    CHECK_FALSE(AudioMixer::Mix(queue, mix.data(), mix.size()));
  }

  SUBCASE("just below the threshold is not audible")
  {
    AudioMixer::Queue queue;
    AudioMixer::Enqueue(queue, Block(4, AudioMixer::kAudibleSampleThreshold - 1));
    std::vector<int16_t> mix(4, 0);
    CHECK_FALSE(AudioMixer::Mix(queue, mix.data(), mix.size()));
  }

  SUBCASE("the threshold itself is audible")
  {
    AudioMixer::Queue queue;
    AudioMixer::Enqueue(queue, Block(4, AudioMixer::kAudibleSampleThreshold));
    std::vector<int16_t> mix(4, 0);
    CHECK(AudioMixer::Mix(queue, mix.data(), mix.size()));
  }

  SUBCASE("negative samples count on magnitude, not sign")
  {
    AudioMixer::Queue queue;
    AudioMixer::Enqueue(queue, Block(4, -AudioMixer::kAudibleSampleThreshold));
    std::vector<int16_t> mix(4, 0);
    CHECK(AudioMixer::Mix(queue, mix.data(), mix.size()));
  }

  SUBCASE("one loud sample in an otherwise quiet block is audible")
  {
    AudioMixer::Queue queue;
    std::vector<int16_t> block(8, 0);
    block[5] = 20000;
    AudioMixer::Enqueue(queue, block);
    std::vector<int16_t> mix(8, 0);
    CHECK(AudioMixer::Mix(queue, mix.data(), mix.size()));
  }

  SUBCASE("audibility describes what was consumed, not what remains")
  {
    AudioMixer::Queue queue;
    std::vector<int16_t> block(8, 0);
    block[6] = 20000;
    AudioMixer::Enqueue(queue, block);

    std::vector<int16_t> quiet(4, 0);
    CHECK_FALSE(AudioMixer::Mix(queue, quiet.data(), quiet.size()));

    std::vector<int16_t> loud(4, 0);
    CHECK(AudioMixer::Mix(queue, loud.data(), loud.size()));
  }
}

TEST_CASE("Mixing an empty queue is a silent no-op")
{
  AudioMixer::Queue queue;
  std::vector<int16_t> mix(4, 9);
  CHECK_FALSE(AudioMixer::Mix(queue, mix.data(), mix.size()));
  for (int16_t sample : mix)
  {
    CHECK(sample == 9);
  }
}

TEST_CASE("Mix tolerates a null buffer")
{
  AudioMixer::Queue queue;
  AudioMixer::Enqueue(queue, Block(4, 100));
  CHECK_FALSE(AudioMixer::Mix(queue, nullptr, 4));
  // The queue must not have been consumed by a call that mixed nothing.
  CHECK(AudioMixer::BufferedSamples(queue) == 4);
}

TEST_CASE("Discard drains a queue without touching the mix buffer")
{
  // An overridden source keeps producing at the emulator's rate whether or not
  // anyone is listening. Stalling its queue would push it to the overflow cap
  // and then play stale audio the instant the override lifts.
  AudioMixer::Queue queue;
  AudioMixer::Enqueue(queue, Block(8, 4000));
  AudioMixer::Enqueue(queue, Block(8, 4000));

  std::vector<int16_t> mixBuffer(12, 111);
  const bool audible = AudioMixer::Discard(queue, 12);

  CHECK(audible);
  CHECK(AudioMixer::BufferedSamples(queue) == 4);
  for (int16_t sample : mixBuffer)
  {
    CHECK(sample == 111);
  }
}

TEST_CASE("Discard reports audibility on the same threshold as Mix")
{
  AudioMixer::Queue quiet;
  AudioMixer::Enqueue(quiet, Block(16, AudioMixer::kAudibleSampleThreshold - 1));
  CHECK_FALSE(AudioMixer::Discard(quiet, 16));

  AudioMixer::Queue loud;
  AudioMixer::Enqueue(loud, Block(16, AudioMixer::kAudibleSampleThreshold));
  CHECK(AudioMixer::Discard(loud, 16));
}

TEST_CASE("Discard on an empty queue is silent and harmless")
{
  AudioMixer::Queue queue;
  CHECK_FALSE(AudioMixer::Discard(queue, 64));
  CHECK(queue.empty());
}

TEST_CASE("TrimToTarget leaves a queue that is already short alone")
{
  AudioMixer::Queue queue;
  AudioMixer::Enqueue(queue, Block(8, 100));
  CHECK(AudioMixer::TrimToTarget(queue, 32, 64) == 0);
  CHECK(AudioMixer::BufferedSamples(queue) == 8);
}

TEST_CASE("TrimToTarget drops the oldest audio, landing exactly on the target")
{
  // Overshooting into an underrun would be worse than the latency being
  // trimmed, so a partial block is split rather than discarded whole.
  AudioMixer::Queue queue;
  AudioMixer::Enqueue(queue, Block(10, 1));
  AudioMixer::Enqueue(queue, Block(10, 2));
  AudioMixer::Enqueue(queue, Block(10, 3));

  CHECK(AudioMixer::TrimToTarget(queue, 12, 24) == 18);
  CHECK(AudioMixer::BufferedSamples(queue) == 12);

  // What survives is the newest audio: the tail of block 2, then block 3.
  std::vector<int16_t> mixBuffer(12, 0);
  AudioMixer::Mix(queue, mixBuffer.data(), 12);
  CHECK(mixBuffer[0] == 2);
  CHECK(mixBuffer[1] == 2);
  CHECK(mixBuffer[2] == 3);
  CHECK(mixBuffer[11] == 3);
}

TEST_CASE("TrimToTarget copes with a target of zero and an empty queue")
{
  AudioMixer::Queue empty;
  CHECK(AudioMixer::TrimToTarget(empty, 0, 0) == 0);

  AudioMixer::Queue queue;
  AudioMixer::Enqueue(queue, Block(4, 7));
  CHECK(AudioMixer::TrimToTarget(queue, 0, 0) == 4);
  CHECK(queue.empty());
}

TEST_CASE("TrimToTarget ignores a queue between the target and the high-water mark")
{
  // The case that matters in practice: a lane oscillating a little above its
  // target is about to drain anyway, and cutting it would be an audible click
  // for nothing.
  AudioMixer::Queue queue;
  AudioMixer::Enqueue(queue, Block(40, 5));

  CHECK(AudioMixer::TrimToTarget(queue, 20, 100) == 0);
  CHECK(AudioMixer::BufferedSamples(queue) == 40);
}

TEST_CASE("TrimToTarget keeps stereo frames aligned")
{
  // Dropping an odd number of samples from an interleaved lane swaps left and
  // right for the rest of the session -- a permanent fault to save half a
  // sample of latency.
  AudioMixer::Queue queue;
  std::vector<int16_t> stereo(40);
  for (size_t i = 0; i < stereo.size(); ++i)
  {
    stereo[i] = (i % 2 == 0) ? 1000 : -1000;
  }
  AudioMixer::Enqueue(queue, stereo);

  AudioMixer::TrimOptions options;
  options.channels = 2;
  // 40 - 15 = 25, an odd number, so the trim has to round down to 24 and stop
  // one sample short of its target.
  CHECK(AudioMixer::TrimToTarget(queue, 15, 20, options) == 24);
  CHECK(AudioMixer::BufferedSamples(queue) == 16);

  std::vector<int16_t> mixBuffer(16, 0);
  AudioMixer::Mix(queue, mixBuffer.data(), 16);
  CHECK(mixBuffer[0] == 1000);
  CHECK(mixBuffer[1] == -1000);
}

TEST_CASE("TrimToTarget splices on matching waveform instead of cutting anywhere")
{
  // A hard cut lands wherever the arithmetic points, which on periodic
  // programme material is a step of up to twice the amplitude -- a click. The
  // search moves the cut onto a matching part of the waveform, and the
  // crossfade spreads whatever is left over several hundred samples.
  constexpr size_t kPeriod = 100;
  constexpr size_t kTotal = 1000;
  constexpr double kAmplitude = 10000.0;

  AudioMixer::Queue queue;
  std::vector<int16_t> wave(kTotal);
  for (size_t i = 0; i < kTotal; ++i)
  {
    wave[i] = static_cast<int16_t>(kAmplitude * std::sin(2.0 * 3.14159265358979 * static_cast<double>(i) /
                                                         static_cast<double>(kPeriod)));
  }
  AudioMixer::Enqueue(queue, wave);

  AudioMixer::TrimOptions options;
  options.channels = 1;
  options.crossfadeSamples = 50;
  options.searchSamples = 60;

  // 1000 - 430 = 570 samples to drop, which is 70 samples into a period. The
  // nearest cut that lands on a period boundary, and so on identical phase, is
  // 600.
  const size_t dropped = AudioMixer::TrimToTarget(queue, 430, 500, options);
  CHECK(dropped == 600);
  CHECK(AudioMixer::BufferedSamples(queue) == 400);

  std::vector<int16_t> mixBuffer(400, 0);
  AudioMixer::Mix(queue, mixBuffer.data(), 400);

  // One period of this wave steps by at most amplitude * 2pi / period between
  // neighbours. Nothing in the spliced result may exceed that by more than
  // rounding.
  const int maxNaturalStep = static_cast<int>(kAmplitude * 2.0 * 3.14159265358979 / static_cast<double>(kPeriod)) + 2;
  int maxStep = 0;
  for (size_t i = 1; i < mixBuffer.size(); ++i)
  {
    maxStep = std::max(maxStep, std::abs(static_cast<int>(mixBuffer[i]) - static_cast<int>(mixBuffer[i - 1])));
  }
  CHECK(maxStep <= maxNaturalStep);

  // And the join still starts where the unsplit audio would have: the listener
  // hears no step into the crossfade either.
  CHECK(std::abs(static_cast<int>(mixBuffer[0]) - static_cast<int>(wave[0])) <= maxNaturalStep);
}

TEST_CASE("TrimToTarget falls back to a hard cut when a splice will not fit")
{
  // A queue shorter than the crossfade has nothing to blend with; it must
  // still come down to its target rather than refuse.
  AudioMixer::Queue queue;
  AudioMixer::Enqueue(queue, Block(30, 500));

  AudioMixer::TrimOptions options;
  options.channels = 1;
  options.crossfadeSamples = 220;
  options.searchSamples = 220;

  CHECK(AudioMixer::TrimToTarget(queue, 10, 20, options) == 20);
  CHECK(AudioMixer::BufferedSamples(queue) == 10);
}

TEST_CASE("RateRatioFor leaves a lane at its target alone")
{
  CHECK(AudioMixer::RateRatioFor(1000, 1000) == doctest::Approx(1.0f));
}

TEST_CASE("RateRatioFor speeds up a lane running ahead and slows one running behind")
{
  // Greater than 1 consumes input faster, which yields fewer output samples and
  // drains a queue that is too deep.
  CHECK(AudioMixer::RateRatioFor(1200, 1000) > 1.0f);
  CHECK(AudioMixer::RateRatioFor(800, 1000) < 1.0f);
}

TEST_CASE("RateRatioFor stays inaudible however far off the depth is")
{
  // The correction changes pitch as well as speed, so a lane that is empty or
  // wildly overfull must still only be nudged.
  CHECK(AudioMixer::RateRatioFor(0, 1000) == doctest::Approx(1.0f - AudioMixer::kMaxRateDeviation));
  CHECK(AudioMixer::RateRatioFor(1000000, 1000) == doctest::Approx(1.0f + AudioMixer::kMaxRateDeviation));
  CHECK(AudioMixer::kMaxRateDeviation < 0.01f);
}

TEST_CASE("RateRatioFor eases off as a lane approaches its target")
{
  // A correction that stayed saturated until the last moment would overshoot.
  const float far = AudioMixer::RateRatioFor(1300, 1000);
  const float near = AudioMixer::RateRatioFor(1010, 1000);
  CHECK(near > 1.0f);
  CHECK(near < far);
}

TEST_CASE("RateRatioFor copes with no target") { CHECK(AudioMixer::RateRatioFor(500, 0) == doctest::Approx(1.0f)); }
