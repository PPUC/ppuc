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

#include <cstdint>
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
